#include "qvgraphicsview.h"
#include <QThread>
#include "hfauthdialog.h"
#include "retouchpromptbar.h"
#include "qvapplication.h"
#include "qvinfodialog.h"
#include "qvcocoafunctions.h"
#include "settingsmanager.h"
#include <QWheelEvent>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QSettings>
#include <QMessageBox>
#include <QMovie>
#include <QtMath>
#include <QGestureEvent>
#include <QScrollBar>
#include <QApplication>
#include <QProcess>
#include <QDir>
#include <QCoreApplication>
#include <QPainter>
#include <QPen>

QVGraphicsView::QVGraphicsView(QWidget *parent) : QGraphicsView(parent)
{
    // GraphicsView setup
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setFrameShape(QFrame::NoFrame);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    viewport()->setAutoFillBackground(false);

    // part of a pathetic attempt at gesture support
    grabGesture(Qt::PinchGesture);

    // Scene setup
    auto *scene = new QGraphicsScene(-1000000.0, -1000000.0, 2000000.0, 2000000.0, this);
    setScene(scene);

    // Initialize other variables
    currentScale = 1.0;
    scaledSize = QSize();
    isOriginalSize = false;
    lastZoomEventPos = QPoint(-1, -1);
    lastZoomRoundingError = QPointF();
    lastScrollRoundingError = QPointF();
    mousePressButton = Qt::MouseButton::NoButton;
    mousePressModifiers = Qt::KeyboardModifier::NoModifier;
    mousePressPosition = QPoint();

    zoomBasisScaleFactor = 1.0;

    connect(&imageCore, &QVImageCore::animatedFrameChanged, this,
            &QVGraphicsView::animatedFrameChanged);
    connect(&imageCore, &QVImageCore::fileChanged, this, &QVGraphicsView::postLoad);
    connect(&imageCore, &QVImageCore::updateLoadedPixmapItem, this,
            &QVGraphicsView::updateLoadedPixmapItem);

    // Should replace the other timer eventually
    expensiveScaleTimerNew = new QTimer(this);
    expensiveScaleTimerNew->setSingleShot(true);
    expensiveScaleTimerNew->setInterval(50);
    connect(expensiveScaleTimerNew, &QTimer::timeout, this, [this] { scaleExpensively(); });

    loadedPixmapItem = new QGraphicsPixmapItem();
    scene->addItem(loadedPixmapItem);

    maskItem = new QGraphicsPixmapItem(loadedPixmapItem);
    maskItem->setOpacity(0.5); // 50% transparency for the red mask
    maskItem->setZValue(1);    // Above the image

    // Connect to settings signal
    connect(&qvApp->getSettingsManager(), &SettingsManager::settingsUpdated, this,
            &QVGraphicsView::settingsUpdated);
    settingsUpdated();

    promptBar = new RetouchPromptBar(this);
    promptBar->hide();
    connect(promptBar, &RetouchPromptBar::generateRequested, this, &QVGraphicsView::applyCreativeFill);
}

// Events

void QVGraphicsView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    if (!isOriginalSize)
        resetScale();
    else
        centerOn(loadedPixmapItem);
}

void QVGraphicsView::dropEvent(QDropEvent *event)
{
    QGraphicsView::dropEvent(event);
    loadMimeData(event->mimeData());
}

void QVGraphicsView::dragEnterEvent(QDragEnterEvent *event)
{
    QGraphicsView::dragEnterEvent(event);
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void QVGraphicsView::dragMoveEvent(QDragMoveEvent *event)
{
    QGraphicsView::dragMoveEvent(event);
    event->acceptProposedAction();
}

void QVGraphicsView::dragLeaveEvent(QDragLeaveEvent *event)
{
    QGraphicsView::dragLeaveEvent(event);
    event->accept();
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
void QVGraphicsView::enterEvent(QEvent *event)
#else
void QVGraphicsView::enterEvent(QEnterEvent *event)
#endif
{
    QGraphicsView::enterEvent(event);
    viewport()->setCursor(Qt::ArrowCursor);
}

void QVGraphicsView::mousePressEvent(QMouseEvent *event)
{
    const auto startWindowMove = [this, event]() {
#ifdef COCOA_LOADED
        return QVCocoaFunctions::startSystemMove(window());
#else
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        return window()->windowHandle()->startSystemMove();
#else
        Q_UNUSED(event)
        return false;
#endif
#endif
    };

    const auto startFallbackWindowMove = [this, event]() {
        mousePressButton = event->button();
        mousePressModifiers = event->modifiers();
        mousePressPosition = event->pos();
    };

    // Check for Ctrl/Cmd drag
    if (event->button() == Qt::LeftButton &&
        event->modifiers().testFlag(Qt::ControlModifier) &&
        qvApp->getSettingsManager().getBool(SettingsManager::Setting::CtrlDragWindow)) {
        const auto windowState = window()->windowState();
        if (!windowState.testFlag(Qt::WindowFullScreen)
            && !windowState.testFlag(Qt::WindowMaximized)) {
            if (!startWindowMove()) {
                startFallbackWindowMove();
            }
            return;
        }
    }

    // Check for titlebar region drag
    if (event->button() == Qt::LeftButton) {
        const auto windowState = window()->windowState();
        if (!windowState.testFlag(Qt::WindowFullScreen)
            && !windowState.testFlag(Qt::WindowMaximized)) {
#ifdef COCOA_LOADED
            // Check if click is in titlebar region
            int titlebarHeight = QVCocoaFunctions::getTitlebarHeight(window()->windowHandle());
            if (event->pos().y() <= titlebarHeight) {
                if (!startWindowMove()) {
                    startFallbackWindowMove();
                }
                return;
            }
#endif
        }
    }

    if (retouchTool != RetouchTool::Off) {
        if (event->button() == Qt::LeftButton) {
            isDrawing = true;
            if (retouchTool == RetouchTool::Lasso)
                lassoPolygon.clear();
            paintOnMask(mapToScene(event->pos()));
            return;
        } else if (event->button() == Qt::MiddleButton) {
            applyRetouch();
            return;
        } else if (event->button() == Qt::RightButton) {
            // Cancel/Exit if in retouch mode
            toggleRetouchMode();
            retouchTool = RetouchTool::Off; // Force off if right click 
            // Wait, maybe right click should just exit the mode?
            // "cancel with right mouse button"
            return;
        }
    }

    QGraphicsView::mousePressEvent(event);
}

void QVGraphicsView::mouseMoveEvent(QMouseEvent *event)
{
    if (mousePressButton == Qt::LeftButton) {
        if (mousePressModifiers.testFlag(Qt::ControlModifier)
            && !event->modifiers().testFlag(Qt::ControlModifier)) {
            mousePressButton = Qt::NoButton;
            mousePressModifiers = Qt::NoModifier;
            QGraphicsView::mouseMoveEvent(event);
            return;
        }

        const QPoint delta = event->pos() - mousePressPosition;
        window()->move(window()->pos() + delta);
        return;
    }

    lastMouseScenePos = mapToScene(event->pos());

    if (retouchTool != RetouchTool::Off && isDrawing) {
        paintOnMask(lastMouseScenePos);
        return;
    }

    if (retouchTool != RetouchTool::Off) {
        viewport()->update();
        return;
    }

    QGraphicsView::mouseMoveEvent(event);
}

void QVGraphicsView::mouseReleaseEvent(QMouseEvent *event)
{
    if (retouchTool != RetouchTool::Off && event->button() == Qt::LeftButton) {
        isDrawing = false;
        if (retouchTool == RetouchTool::Lasso)
            finalizeLasso();
        return;
    }

    mousePressButton = Qt::NoButton;
    mousePressModifiers = Qt::NoModifier;
    QGraphicsView::mouseReleaseEvent(event);
    viewport()->setCursor(Qt::ArrowCursor);
}

bool QVGraphicsView::event(QEvent *event)
{
    // this is for touchpad pinch gestures
    if (event->type() == QEvent::Gesture) {
        auto *gestureEvent = static_cast<QGestureEvent *>(event);
        if (QGesture *pinch = gestureEvent->gesture(Qt::PinchGesture)) {
            auto *pinchGesture = static_cast<QPinchGesture *>(pinch);
            QPinchGesture::ChangeFlags changeFlags = pinchGesture->changeFlags();

            if (changeFlags & QPinchGesture::ScaleFactorChanged) {
                const QPoint hotPoint = mapFromGlobal(pinchGesture->hotSpot().toPoint());
                zoom(pinchGesture->scaleFactor(), hotPoint);
            }

            // Fun rotation stuff maybe later
            //            if (changeFlags & QPinchGesture::RotationAngleChanged) {
            //                qreal rotationDelta = pinchGesture->rotationAngle() -
            //                pinchGesture->lastRotationAngle(); rotate(rotationDelta);
            //                centerOn(loadedPixmapItem);
            //            }
            return true;
        }
    } else if (event->type() == QEvent::NativeGesture) {
        auto *nativeEvent = static_cast<QNativeGestureEvent *>(event);
        if (nativeEvent->gestureType() == Qt::ZoomNativeGesture) {
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
            const QPoint eventPos = nativeEvent->position().toPoint();
#else
            const QPoint eventPos = nativeEvent->pos();
#endif
            zoom(nativeEvent->value() + 1, eventPos);
            return true;
        }
    }
    return QGraphicsView::event(event);
}

void QVGraphicsView::wheelEvent(QWheelEvent *event)
{
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    const QPoint eventPos = event->position().toPoint();
#else
    const QPoint eventPos = event->pos();
#endif

    const bool modifierPressed = event->modifiers().testFlag(Qt::ControlModifier);
    bool dontZoom = qvGetSettingInt(ScrollZoom) == 2;
    if (modifierPressed) {
        dontZoom = !dontZoom;
    }

    bool touchDeviceDetected = false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Auto-detect touchpad
    touchDeviceDetected = event->device()->type() == QInputDevice::DeviceType::TouchPad
            || event->device()->type() == QInputDevice::DeviceType::TouchScreen;
    // Real touchpads are likely to exhibit these characteristics in empirical testing
    touchDeviceDetected = touchDeviceDetected && event->phase() != Qt::NoScrollPhase;
    if (touchDeviceDetected && qvGetSettingInt(ScrollZoom) == 1) {
        // If this is a touch device, override setting
        dontZoom = !modifierPressed;
    }
#endif

    if (dontZoom) {
        const qreal scrollDivisor = 2.0; // To make scrolling less sensitive
        qreal scrollX = event->angleDelta().x() * (isRightToLeft() ? 1 : -1) / scrollDivisor;
        qreal scrollY = event->angleDelta().y() * -1 / scrollDivisor;

        if (event->modifiers() & Qt::ShiftModifier)
            std::swap(scrollX, scrollY);

        QPointF targetScrollDelta = QPointF(scrollX, scrollY) - lastScrollRoundingError;
        QPoint roundedScrollDelta = targetScrollDelta.toPoint();

        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + roundedScrollDelta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() + roundedScrollDelta.y());

        lastScrollRoundingError = roundedScrollDelta - targetScrollDelta;

        return;
    }

    const int yDelta = event->angleDelta().y();
    const qreal yScale = 120.0;

    if (yDelta == 0)
        return;

    const qreal zoomAmountPerWheelClick = qvGetSettingInt(ScaleFactor)/100.0;
    qreal zoomFactor = zoomAmountPerWheelClick;
    if (qvGetSettingBool(FractionalZoom) || touchDeviceDetected) {
        const qreal fractionalWheelClicks = qFabs(yDelta) / yScale;
        zoomFactor *= fractionalWheelClicks;
    }
    zoomFactor += 1.0;

    if (yDelta < 0)
        zoomFactor = qPow(zoomFactor, -1);

    zoom(zoomFactor, eventPos);
}

// Functions

QMimeData *QVGraphicsView::getMimeData() const
{
    auto *mimeData = new QMimeData();
    if (!getCurrentFileDetails().isPixmapLoaded)
        return mimeData;

    mimeData->setUrls(
            { QUrl::fromLocalFile(imageCore.getCurrentFileDetails().fileInfo.absoluteFilePath()) });
    mimeData->setImageData(imageCore.getLoadedPixmap().toImage());
    return mimeData;
}

void QVGraphicsView::loadMimeData(const QMimeData *mimeData)
{
    if (mimeData == nullptr)
        return;

    if (!mimeData->hasUrls())
        return;

    const QList<QUrl> urlList = mimeData->urls();

    bool first = true;
    for (const auto &url : urlList) {
        if (first) {
            loadFile(url.toString());
            emit cancelSlideshow();
            first = false;
            continue;
        }
        QVApplication::openFile(url.toString());
    }
}

void QVGraphicsView::loadFile(const QString &fileName)
{
    imageCore.loadFile(fileName);
}

void QVGraphicsView::reloadFile()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    imageCore.loadFile(getCurrentFileDetails().fileInfo.absoluteFilePath(), true);
}

void QVGraphicsView::postLoad()
{
    updateLoadedPixmapItem();
    qvApp->getActionManager().addFileToRecentsList(getCurrentFileDetails().fileInfo);

    emit fileChanged();
}

void QVGraphicsView::zoomIn(const QPoint &pos)
{
    zoom(qvGetSettingInt(ScaleFactor)/100.0 + 1, pos);
}

void QVGraphicsView::zoomOut(const QPoint &pos)
{
    zoom(qPow(qvGetSettingInt(ScaleFactor)/100.0 + 1, -1), pos);
}

void QVGraphicsView::zoom(qreal scaleFactor, const QPoint &pos)
{
    // don't zoom too far out, dude
    currentScale *= scaleFactor;
    if (currentScale >= 500 || currentScale <= 0.01) {
        currentScale *= qPow(scaleFactor, -1);
        return;
    }

    updateFilteringMode();

    if (pos != lastZoomEventPos) {
        lastZoomEventPos = pos;
        lastZoomRoundingError = QPointF();
    }
    const QPointF scenePos = mapToScene(pos) - lastZoomRoundingError;

    zoomBasisScaleFactor *= scaleFactor;
    setTransform(QTransform(zoomBasis).scale(zoomBasisScaleFactor, zoomBasisScaleFactor));
    absoluteTransform.scale(scaleFactor, scaleFactor);

    // If we are zooming in, we have a point to zoom towards, the mouse is on top of the viewport,
    // and cursor zooming is enabled
    if (currentScale > 1.00001 && pos != QPoint(-1, -1) && underMouse()
        && qvGetSettingBool(CursorZoom)) {
        const QPointF p1mouse = mapFromScene(scenePos);
        const QPointF move = p1mouse - pos;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value()
                                        + (move.x() * (isRightToLeft() ? -1 : 1)));
        verticalScrollBar()->setValue(verticalScrollBar()->value() + move.y());
        lastZoomRoundingError = mapToScene(pos) - scenePos;
    } else {
        centerOn(loadedPixmapItem);
    }
    emit zoomChanged(qFabs(absoluteTransform.m11()));

    if (qvGetSettingBool(ScalingEnabled) && !isOriginalSize) {
        expensiveScaleTimerNew->start();
    }
}

void QVGraphicsView::scaleExpensively()
{
    if (retouchTool != RetouchTool::Off) return;

    // Determine if mirrored or flipped
    bool mirrored = false;
    if (transform().m11() < 0)
        mirrored = true;

    bool flipped = false;
    if (transform().m22() < 0)
        flipped = true;

    // If we are above maximum scaling size
    if ((currentScale >= MAX_EXPENSIVE_SCALING_SIZE)
        || (!qvGetSettingBool(ScalingTwoEnabled) && currentScale > 1.00001)) {
        // Return to original size
        makeUnscaled();
        return;
    }

    // Map size of the original pixmap to the scale acquired in fitting with modification from
    // zooming percentage
    const QRectF mappedRect =
            absoluteTransform.mapRect(QRectF({}, getCurrentFileDetails().loadedPixmapSize));
    const QSizeF mappedPixmapSize = mappedRect.size() * devicePixelRatioF();

    // Undo mirror/flip before new transform
    if (mirrored)
        scale(-1, 1);

    if (flipped)
        scale(1, -1);

    // Set image to scaled version
    loadedPixmapItem->setPixmap(imageCore.scaleExpensively(mappedPixmapSize));

    // Reset transformation
    setTransform(
            QTransform::fromScale(qPow(devicePixelRatioF(), -1), qPow(devicePixelRatioF(), -1)));

    // Redo mirror/flip after new transform
    if (mirrored)
        scale(-1, 1);

    if (flipped)
        scale(1, -1);

    // Set zoombasis
    zoomBasis = transform();
    zoomBasisScaleFactor = 1.0;
}

void QVGraphicsView::makeUnscaled()
{
    // Determine if mirrored or flipped
    bool mirrored = false;
    if (transform().m11() < 0)
        mirrored = true;

    bool flipped = false;
    if (transform().m22() < 0)
        flipped = true;

    // Return to original size
    if (getCurrentFileDetails().isMovieLoaded)
        loadedPixmapItem->setPixmap(getLoadedMovie().currentPixmap());
    else
        loadedPixmapItem->setPixmap(getLoadedPixmap());

    setTransform(absoluteTransform);

    // Redo mirror/flip after new transform
    if (mirrored)
        scale(-1, 1);

    if (flipped)
        scale(1, -1);

    // Reset retouch undo state for the new image
    undoPixmap = QPixmap();

    // Reset transformation
    zoomBasis = transform();
    zoomBasisScaleFactor = 1.0;
}

void QVGraphicsView::updateFilteringMode()
{
    const bool exceededSmoothScaleLimit = currentScale >= MAX_FILTERING_SIZE;
    loadedPixmapItem->setTransformationMode(!exceededSmoothScaleLimit
                                                            && qvGetSettingBool(FilteringEnabled)
                                                    ? Qt::SmoothTransformation
                                                    : Qt::FastTransformation);
}

void QVGraphicsView::animatedFrameChanged(QRect rect)
{
    Q_UNUSED(rect)

    if (qvGetSettingBool(ScalingEnabled)) {
        scaleExpensively();
    } else {
        loadedPixmapItem->setPixmap(getLoadedMovie().currentPixmap());
    }
}

void QVGraphicsView::updateLoadedPixmapItem()
{
    // set pixmap and offset
    loadedPixmapItem->setPixmap(getLoadedPixmap());
    scaledSize = loadedPixmapItem->boundingRect().size().toSize();

    resetScale();

    emit updatedLoadedPixmapItem();
}

void QVGraphicsView::resetScale()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    fitInViewMarginless(loadedPixmapItem);

    if (qvGetSettingBool(ScalingEnabled))
        expensiveScaleTimerNew->start();
}

void QVGraphicsView::originalSize()
{
    if (isOriginalSize) {
        // If we are at the actual original size
        if (transform() == QTransform()) {
            resetScale(); // back to normal mode
            return;
        }
    }
    makeUnscaled();

    resetTransform();
    centerOn(loadedPixmapItem);

    zoomBasis = transform();
    zoomBasisScaleFactor = 1.0;
    absoluteTransform = transform();
    emit zoomChanged(qFabs(absoluteTransform.m11()));

    isOriginalSize = true;
}

void QVGraphicsView::goToFile(const GoToFileMode &mode, int index)
{
    bool shouldRetryFolderInfoUpdate = false;

    // Update folder info only after a little idle time as an optimization for when
    // the user is rapidly navigating through files.
    if (!getCurrentFileDetails().timeSinceLoaded.isValid()
        || getCurrentFileDetails().timeSinceLoaded.hasExpired(3000)) {
        // Make sure the file still exists because if it disappears from the file listing we'll lose
        // track of our index within the folder. Use the static 'exists' method to avoid caching.
        // If we skip updating now, flag it for retry later once we locate a new file.
        if (QFile::exists(getCurrentFileDetails().fileInfo.absoluteFilePath()))
            imageCore.updateFolderInfo();
        else
            shouldRetryFolderInfoUpdate = true;
    }

    const auto &fileList = getCurrentFileDetails().folderFileInfoList;
    if (fileList.isEmpty())
        return;

    int newIndex = getCurrentFileDetails().loadedIndexInFolder;
    int searchDirection = 0;

    switch (mode) {
    case GoToFileMode::constant: {
        newIndex = index;
        break;
    }
    case GoToFileMode::first: {
        newIndex = 0;
        searchDirection = 1;
        break;
    }
    case GoToFileMode::previous: {
        if (newIndex == 0) {
            if (qvGetSettingBool(LoopFoldersEnabled))
                newIndex = fileList.size() - 1;
            else
                emit cancelSlideshow();
        } else
            newIndex--;
        searchDirection = -1;
        break;
    }
    case GoToFileMode::next: {
        if (fileList.size() - 1 == newIndex) {
            if (qvGetSettingBool(LoopFoldersEnabled))
                newIndex = 0;
            else
                emit cancelSlideshow();
        } else
            newIndex++;
        searchDirection = 1;
        break;
    }
    case GoToFileMode::last: {
        newIndex = fileList.size() - 1;
        searchDirection = -1;
        break;
    }
    }

    if (searchDirection != 0) {
        while (searchDirection == 1 && newIndex < fileList.size() - 1
               && !QFile::exists(fileList.value(newIndex).absoluteFilePath))
            newIndex++;
        while (searchDirection == -1 && newIndex > 0
               && !QFile::exists(fileList.value(newIndex).absoluteFilePath))
            newIndex--;
    }

    const QString nextImageFilePath = fileList.value(newIndex).absoluteFilePath;

    if (!QFile::exists(nextImageFilePath)
        || nextImageFilePath == getCurrentFileDetails().fileInfo.absoluteFilePath())
        return;

    if (shouldRetryFolderInfoUpdate) {
        // If the user just deleted a file through qView, closeImage will have been called which
        // empties currentFileDetails.fileInfo. In this case updateFolderInfo can't infer the
        // directory from fileInfo like it normally does, so we'll explicity pass in the folder
        // here.
        imageCore.updateFolderInfo(QFileInfo(nextImageFilePath).path());
    }

    loadFile(nextImageFilePath);
}

void QVGraphicsView::fitInViewMarginless(const QRectF &rect)
{
#ifdef COCOA_LOADED
    int obscuredHeight = QVCocoaFunctions::getObscuredHeight(window()->windowHandle());
#else
    int obscuredHeight = 0;
#endif

    // Set adjusted image size / bounding rect based on
    QSize adjustedImageSize = getCurrentFileDetails().loadedPixmapSize;
    QRectF adjustedBoundingRect = rect;

    switch (qvGetSettingInt(CropMode)) { // should be enum tbh
    case 1: // only take into account height
    {
        adjustedImageSize.setWidth(1);
        adjustedBoundingRect.setWidth(1);
        break;
    }
    case 2: // only take into account width
    {
        adjustedImageSize.setHeight(1);
        adjustedBoundingRect.setHeight(1);
        break;
    }
    }
    adjustedBoundingRect.moveCenter(rect.center());

    if (!scene() || adjustedBoundingRect.isNull())
        return;

    // Reset the view scale to 1:1.
    QRectF unity = transform().mapRect(QRectF(0, 0, 1, 1));
    if (unity.isEmpty())
        return;
    scale(1 / unity.width(), 1 / unity.height());

    // Determine what we are resizing to
    const int adjWidth = width() - MARGIN;
    const int adjHeight = height() - MARGIN - obscuredHeight;

    QRectF viewRect;
    // Resize to window size unless you are meant to stop at the actual size, basically
    if (qvGetSettingBool(PastActualSizeEnabled)
        || (adjustedImageSize.width() >= adjWidth || adjustedImageSize.height() >= adjHeight)) {
        viewRect = viewport()->rect().adjusted(MARGIN, MARGIN, -MARGIN, -MARGIN);
        viewRect.setHeight(viewRect.height() - obscuredHeight);
    } else {
        // stop at actual size
        viewRect = QRect(QPoint(), getCurrentFileDetails().loadedPixmapSize);
        QPoint center = this->rect().center();
        center.setY(center.y() - obscuredHeight);
        viewRect.moveCenter(center);
    }

    if (viewRect.isEmpty())
        return;

    // Find the ideal x / y scaling ratio to fit \a rect in the view.
    QRectF sceneRect = transform().mapRect(adjustedBoundingRect);
    if (sceneRect.isEmpty())
        return;

    qreal xratio = viewRect.width() / sceneRect.width();
    qreal yratio = viewRect.height() / sceneRect.height();

    xratio = yratio = qMin(xratio, yratio);

    // Find and set the transform required to fit the original image
    // Compact version of above code
    QRectF sceneRect2 = transform().mapRect(QRectF({}, adjustedImageSize));
    qreal absoluteRatio =
            qMin(viewRect.width() / sceneRect2.width(), viewRect.height() / sceneRect2.height());

    absoluteTransform = QTransform::fromScale(absoluteRatio, absoluteRatio);

    // Scale and center on the center of \a rect.
    scale(xratio, yratio);
    centerOn(adjustedBoundingRect.center());

    // variables
    zoomBasis = transform();

    isOriginalSize = false;
    currentScale = 1.0;
    updateFilteringMode();
    zoomBasisScaleFactor = 1.0;
    emit zoomChanged(qFabs(absoluteTransform.m11()));
}

void QVGraphicsView::fitInViewMarginless(const QGraphicsItem *item)
{
    return fitInViewMarginless(item->sceneBoundingRect());
}

void QVGraphicsView::centerOn(const QPointF &pos)
{
#ifdef COCOA_LOADED
    int obscuredHeight = QVCocoaFunctions::getObscuredHeight(window()->windowHandle());
#else
    int obscuredHeight = 0;
#endif

    qreal width = viewport()->width();
    qreal height = viewport()->height() - obscuredHeight;
    QPointF viewPoint = transform().map(pos);

    if (isRightToLeft()) {
        qint64 horizontal = 0;
        horizontal += horizontalScrollBar()->minimum();
        horizontal += horizontalScrollBar()->maximum();
        horizontal -= int(viewPoint.x() - width / 2.0);
        horizontalScrollBar()->setValue(horizontal);
    } else {
        horizontalScrollBar()->setValue(int(viewPoint.x() - width / 2.0));
    }

    verticalScrollBar()->setValue(int(viewPoint.y() - obscuredHeight - (height / 2.0)));
}

void QVGraphicsView::centerOn(qreal x, qreal y)
{
    centerOn(QPointF(x, y));
}

void QVGraphicsView::centerOn(const QGraphicsItem *item)
{
    centerOn(item->sceneBoundingRect().center());
}

void QVGraphicsView::settingsUpdated()
{
    if (getCurrentFileDetails().isPixmapLoaded)
        resetScale();
}

void QVGraphicsView::closeImage()
{
    imageCore.closeImage();
}

void QVGraphicsView::jumpToNextFrame()
{
    imageCore.jumpToNextFrame();
}

void QVGraphicsView::setPaused(const bool &desiredState)
{
    imageCore.setPaused(desiredState);
}

void QVGraphicsView::setSpeed(const int &desiredSpeed)
{
    imageCore.setSpeed(desiredSpeed);
}

void QVGraphicsView::rotateImage(int rotation)
{
    imageCore.rotateImage(rotation);
}

void QVGraphicsView::toggleRetouchMode()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    // Cycle: Off -> Brush -> Lasso -> Off
    if (retouchTool == RetouchTool::Off) retouchTool = RetouchTool::Brush;
    else if (retouchTool == RetouchTool::Brush) retouchTool = RetouchTool::Lasso;
    else retouchTool = RetouchTool::Off;

    if (retouchTool != RetouchTool::Off) {
        // Prevent scaling while editing so mask coordinates map correctly to full res
        if (qvGetSettingBool(ScalingEnabled)) {
            makeUnscaled();
            scale(currentScale, currentScale);
        }

        setDragMode(QGraphicsView::NoDrag);
        viewport()->setCursor(Qt::CrossCursor);
        ensureWorkerStarted();
        
        if (promptBar) {
            promptBar->show();
            int barWidth = qMin(600, width() - 40);
            promptBar->setFixedWidth(barWidth);
            promptBar->move((width() - barWidth) / 2, height() - promptBar->height() - 20);
            promptBar->setFocusToPrompt();
        }

        // Prepare mask image if empty or different size
        // Use the actual oriented pixmap size for the mask
        QSize actualSize = loadedPixmapItem ? loadedPixmapItem->pixmap().size() : QSize();
        if (maskImage.size() != actualSize || maskImage.isNull()) {
            maskImage = QImage(actualSize, QImage::Format_ARGB32);
            maskImage.fill(Qt::transparent);
            updateMaskItem();
        }
        
        // Eagerly start the AI worker so it's warm by the time the user clicks 'Apply'
        ensureWorkerStarted();
    } else {
        setDragMode(QGraphicsView::ScrollHandDrag);
        setMouseTracking(false);
        viewport()->setCursor(Qt::ArrowCursor);
        maskImage = QImage();
        updateMaskItem();

        if (qvGetSettingBool(ScalingEnabled) && !isOriginalSize) {
            expensiveScaleTimerNew->start();
        }

        if (promptBar) {
            promptBar->hide();
            promptBar->clear();
        }
    }
}

void QVGraphicsView::paintOnMask(const QPointF &scenePos)
{
    if (maskImage.isNull() || !loadedPixmapItem)
        return;

    // Map scene coordinates to image relative coordinates (accounting for offset)
    QPointF itemPos = loadedPixmapItem->mapFromScene(scenePos);
    itemPos -= loadedPixmapItem->offset();

    // Use absolute current scene-to-view scale for consistent brush size
    qreal viewScale = transform().m11();

    if (retouchTool == RetouchTool::Brush) {
        QPainter painter(&maskImage);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::red);
        painter.drawEllipse(itemPos, brushSize / viewScale, brushSize / viewScale);
        painter.end();
    } else if (retouchTool == RetouchTool::Lasso) {
        lassoPolygon << itemPos;
    }

    updateMaskItem();
}

void QVGraphicsView::finalizeLasso()
{
    if (lassoPolygon.isEmpty()) return;

    QPainter painter(&maskImage);
    painter.setBrush(Qt::red);
    painter.setPen(Qt::NoPen);
    painter.drawPolygon(lassoPolygon);
    painter.end();
    
    lassoPolygon.clear();
    updateMaskItem();
}

void QVGraphicsView::updateMaskItem()
{
    if (maskItem && loadedPixmapItem) {
        maskItem->setOffset(loadedPixmapItem->offset());
        if (maskImage.isNull()) {
            maskItem->setPixmap(QPixmap());
        } else {
            maskItem->setPixmap(QPixmap::fromImage(maskImage));
        }
    }
}

void QVGraphicsView::applyRetouch()
{
    if (maskImage.isNull() || !getCurrentFileDetails().isPixmapLoaded)
        return;

    // 1. Ensure Python environment is ready
    QString scriptsDir = QCoreApplication::applicationDirPath() + "/scripts";
    // Check project root if running from build
    if (!QDir(scriptsDir).exists()) {
        scriptsDir = "g:/Code/IQView/scripts";
    }
    
    QString venvPath = scriptsDir + "/.venv";
    QString pythonExe = venvPath + "/Scripts/python.exe"; // Windows path
#ifndef Q_OS_WIN
    pythonExe = venvPath + "/bin/python"; // Linux/macOS
#endif

    if (!QFile::exists(pythonExe)) {
        int ret = QMessageBox::information(this, tr("AI Setup"), 
            tr("This is your first time using Retouch. iqView needs to set up a local AI environment (approx. 500MB).\n\nThis may take a minute. Continue?"), 
            QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::No) return;

        QApplication::setOverrideCursor(Qt::WaitCursor);
        
        QProcess setup;
        setup.setWorkingDirectory(scriptsDir);
        // Try creating venv using system python
        setup.start("python", QStringList() << "-m" << "venv" << ".venv");
        if (!setup.waitForStarted() || !setup.waitForFinished(60000)) {
            setup.start("python3", QStringList() << "-m" << "venv" << ".venv");
            if (!setup.waitForStarted() || !setup.waitForFinished(60000)) {
                QApplication::restoreOverrideCursor();
                QMessageBox::critical(this, tr("Error"), tr("Could not create Python Virtual Environment. Please ensure Python is installed."));
                return;
            }
        }

        // Install requirements
        setup.start(pythonExe, QStringList() << "-m" << "pip" << "install" << "-r" << "requirements.txt");
        if (!setup.waitForStarted() || !setup.waitForFinished(300000)) { // 5 min timeout for pip
            QApplication::restoreOverrideCursor();
            QMessageBox::critical(this, tr("Error"), tr("Failed to install AI dependencies."));
            return;
        }
        
        QApplication::restoreOverrideCursor();
        QMessageBox::information(this, tr("Setup Complete"), tr("AI environment is ready!"));
    }

    QString inputPath = QDir::tempPath() + "/iqview_retouch_in.bmp";
    QString maskPath = QDir::tempPath() + "/iqview_retouch_mask.bmp";
    QString outputPath = QDir::tempPath() + "/iqview_retouch_out.bmp";

    undoPixmap = loadedPixmapItem->pixmap();
    loadedPixmapItem->pixmap().save(inputPath, "BMP");
    maskImage.save(maskPath, "BMP");

    pendingOutputPath = outputPath;
    ensureWorkerStarted();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    
    // Silent Wait: If the worker is still loading, wait up to 20s for the READY signal
    // without showing a popup. The mouse cursor will indicate work is happening.
    int retries = 0;
    while (!isWorkerReady && retries < 200) { // 200 * 100ms = 20s
        QCoreApplication::processEvents();
        if (isWorkerReady) break;
        QThread::msleep(100);
        retries++;
    }

    if (isWorkerReady) {
        QString command = QString("%1|%2|%3\n").arg(inputPath, maskPath, outputPath);
        workerProcess->write(command.toUtf8());
    } else {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("AI Error"), tr("The AI service failed to start in time."));
    }
}

void QVGraphicsView::ensureWorkerStarted()
{
    if (workerProcess && workerProcess->state() == QProcess::Running) return;

    QString scriptsDir = QCoreApplication::applicationDirPath() + "/scripts";
    if (!QDir(scriptsDir).exists()) scriptsDir = "g:/Code/IQView/scripts";
    
    QString venvPath = scriptsDir + "/.venv";
    QString pythonExe = venvPath + "/Scripts/python.exe";
#ifndef Q_OS_WIN
    pythonExe = venvPath + "/bin/python";
#endif

    isWorkerReady = false;
    if (workerProcess) workerProcess->deleteLater();
    
    workerProcess = new QProcess(this);
    connect(workerProcess, &QProcess::readyReadStandardOutput, this, &QVGraphicsView::handleWorkerOutput);
    workerProcess->start(pythonExe, QStringList() << scriptsDir + "/worker.py");
}

void QVGraphicsView::handleWorkerOutput()
{
    while (workerProcess->canReadLine()) {
        QString line = QString::fromUtf8(workerProcess->readLine()).trimmed();
        if (line == "READY") {
            isWorkerReady = true;
        } else if (line == "DONE") {
            QApplication::restoreOverrideCursor();
            loadFile(pendingOutputPath);
            // Toggle retouch mode off on success
            retouchTool = RetouchTool::Lasso; // Setup for the toggle to hit Off
            toggleRetouchMode();
        } else if (line.startsWith("ERROR:") || line.startsWith("FATAL:")) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(this, tr("Retouch Error"), line);
        }
    }
}

void QVGraphicsView::changeBrushSize(int delta)
{
    brushSize = qBound(5, brushSize + delta, 500);
    viewport()->update();
    
    if (promptBar && promptBar->isVisible()) {
        int barWidth = qMin(600, width() - 40);
        promptBar->setFixedWidth(barWidth);
        promptBar->move((width() - barWidth) / 2, height() - promptBar->height() - 20);
    }
}

void QVGraphicsView::drawForeground(QPainter *painter, const QRectF &rect)
{
    Q_UNUSED(rect)
    if (retouchTool != RetouchTool::Off) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        
        qreal viewScale = transform().m11();
        
        if (retouchTool == RetouchTool::Brush) {
            painter->setPen(QPen(Qt::white, 2 / viewScale));
            painter->setBrush(QColor(255, 0, 0, 100)); // Semi-transparent red
            painter->drawEllipse(lastMouseScenePos, brushSize / viewScale, brushSize / viewScale);
        } else if (retouchTool == RetouchTool::Lasso) {
            painter->setPen(QPen(Qt::white, 2 / viewScale, Qt::DashLine));
            painter->setBrush(QColor(255, 0, 0, 50));
            
            if (isDrawing && !lassoPolygon.isEmpty()) {
                QPolygonF screenPolygon;
                for (const QPointF &p : lassoPolygon) {
                    // map from image-relative to scene
                    screenPolygon << loadedPixmapItem->mapToScene(p + loadedPixmapItem->offset());
                }
                screenPolygon << lastMouseScenePos;
                painter->drawPolygon(screenPolygon);
            } else {
                painter->setPen(QPen(Qt::white, 1 / viewScale));
                painter->drawLine(lastMouseScenePos - QPointF(10 / viewScale, 0), lastMouseScenePos + QPointF(10 / viewScale, 0));
                painter->drawLine(lastMouseScenePos - QPointF(0, 10 / viewScale), lastMouseScenePos + QPointF(0, 10 / viewScale));
            }
        }
        painter->restore();
    }
}

bool QVGraphicsView::undoRetouch()
{
    if (undoPixmap.isNull()) return false;
    
    QPixmap current = loadedPixmapItem->pixmap();
    loadedPixmapItem->setPixmap(undoPixmap);
    undoPixmap = current;
    
    updateMaskItem();
    viewport()->update();
    return true;
}

bool QVGraphicsView::checkGenerativeAccess()
{
    QString token = qvGetSettingString(HFToken);
    QString pythonPath = QDir(qApp->applicationDirPath()).filePath("scripts/.venv/Scripts/python.exe");
    QString scriptPath = QDir(qApp->applicationDirPath()).filePath("scripts/flux_fill.py");

    while (true) {
        QProcess checkProcess;
        QStringList args;
        args << scriptPath << "--check_only" << "--model" << hfModelId;
        if (!token.isEmpty()) {
            args << "--token" << token;
        }

        checkProcess.start(pythonPath, args);
        if (!checkProcess.waitForFinished(10000)) {
            QMessageBox::critical(this, tr("AI Error"), tr("Timed out checking model access."));
            return false;
        }

        QString output = QString::fromUtf8(checkProcess.readAllStandardOutput()).trimmed();
        if (output == "ACCESS_GRANTED") {
            return true;
        } else if (output == "ACCESS_GATED") {
            HFAuthDialog dialog(hfModelId, this);
            if (dialog.exec() == QDialog::Accepted) {
                token = dialog.getToken();
                qvSetSetting(HFToken, token);
                // Loop again to verify
                continue;
            } else {
                return false;
            }
        } else {
            QMessageBox::critical(this, tr("Access Error"), tr("An error occurred while checking access: %1").arg(output));
            return false;
        }
    }
}

void QVGraphicsView::ensureFluxStarted()
{
    if (fluxProcess && fluxProcess->state() == QProcess::Running)
        return;

    if (!fluxProcess) {
        fluxProcess = new QProcess(this);
        connect(fluxProcess, &QProcess::readyReadStandardOutput, this, &QVGraphicsView::handleFluxOutput);
    }

    QString pythonPath = QDir(qApp->applicationDirPath()).filePath("scripts/.venv/Scripts/python.exe");
    QString scriptPath = QDir(qApp->applicationDirPath()).filePath("scripts/flux_fill.py");
    QString token = qvGetSettingString(HFToken);

    QStringList args;
    args << scriptPath << "--model" << hfModelId;
    if (!token.isEmpty())
        args << "--token" << token;

    fluxProcess->start(pythonPath, args);
}

void QVGraphicsView::handleFluxOutput()
{
    while (fluxProcess->canReadLine()) {
        QString line = QString::fromUtf8(fluxProcess->readLine()).trimmed();
        if (line.startsWith("OUTPUT: ")) {
            QString outPath = line.mid(8);
            QPixmap result(outPath);
            if (!result.isNull()) {
                undoPixmap = loadedPixmapItem->pixmap();
                loadedPixmapItem->setPixmap(result);
                viewport()->setCursor(Qt::CrossCursor);
            }
        }
    }
}

void QVGraphicsView::applyCreativeFill()
{
    if (retouchTool == RetouchTool::Off || !loadedPixmapItem)
        return;

    // First ensure access
    if (qvGetSettingString(HFToken).isEmpty()) {
        if (!checkGenerativeAccess()) return;
    }

    ensureFluxStarted();

    QString prompt = promptBar->prompt();
    if (prompt.isEmpty()) return;

    // Create mask image from the maskImage ARGB32
    QImage mask = maskImage.convertToFormat(QImage::Format_Grayscale8);

    QString tempDir = QDir::tempPath();
    QString inputPath = QDir(tempDir).filePath("iqview_flux_in.bmp");
    QString maskPath = QDir(tempDir).filePath("iqview_flux_mask.bmp");
    QString outputPath = QDir(tempDir).filePath("iqview_flux_out.bmp");

    loadedPixmapItem->pixmap().save(inputPath);
    mask.save(maskPath);

    viewport()->setCursor(Qt::WaitCursor);

    QString cmd = QString("%1|%2|%3|%4\n").arg(inputPath, maskPath, prompt, outputPath);
    fluxProcess->write(cmd.toUtf8());
}

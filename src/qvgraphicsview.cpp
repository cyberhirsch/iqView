#include "qvgraphicsview.h"
#include "isolatedialog.h"
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
#include <QStandardPaths>
#include <QDateTime>
#include <QCoreApplication>
#include <QPainter>
#include <QPen>
#include <QProgressDialog>
#include <QEventLoop>

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
    if (promptBar && promptBar->isVisible())
        repositionPromptBar();
    repositionAiStatus();
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
            exitRetouchMode();
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

    if (retouchTool != RetouchTool::Off) {
        if (isDrawing)
            paintOnMask(lastMouseScenePos);
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

    // Off → Brush; Brush ↔ Lasso (Esc exits, Enter applies)
    if (retouchTool == RetouchTool::Off) retouchTool = RetouchTool::Brush;
    else if (retouchTool == RetouchTool::Brush) retouchTool = RetouchTool::Lasso;
    else retouchTool = RetouchTool::Brush;

    if (retouchTool != RetouchTool::Off) {
        // Prevent scaling while editing so mask coordinates map correctly to full res
        if (qvGetSettingBool(ScalingEnabled)) {
            makeUnscaled();
            scale(currentScale, currentScale);
        }

        setDragMode(QGraphicsView::NoDrag);
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
        viewport()->setCursor(Qt::CrossCursor);

        if (promptBar) {
            promptBar->hide();
            promptBar->clear();
        }

        // Prepare mask image if empty or different size
        // Use the actual oriented pixmap size for the mask
        QSize actualSize = loadedPixmapItem ? loadedPixmapItem->pixmap().size() : QSize();
        if (maskImage.size() != actualSize || maskImage.isNull()) {
            maskImage = QImage(actualSize, QImage::Format_ARGB32);
            maskImage.fill(Qt::transparent);
            maskHasPaint = false;
            updateMaskItem();
        }

        // Eagerly start the AI worker so it's warm by the time the user clicks 'Apply'
        ensureWorkerStarted();
    } else {
        exitRetouchMode();
    }
}

void QVGraphicsView::exitRetouchMode()
{
    retouchTool = RetouchTool::Off;
    setDragMode(QGraphicsView::ScrollHandDrag);
    setMouseTracking(false);
    viewport()->setCursor(Qt::ArrowCursor);
    maskImage = QImage();
    maskHasPaint = false;
    updateMaskItem();

    if (qvGetSettingBool(ScalingEnabled) && !isOriginalSize) {
        expensiveScaleTimerNew->start();
    }

    if (promptBar) {
        promptBar->hide();
        promptBar->clear();
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
        maskHasPaint = true;
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
    maskHasPaint = true;
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

QString QVGraphicsView::resolveLogPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
               .absoluteFilePath("flux.log");
}

QString QVGraphicsView::resolveModelsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/models";
}

QString QVGraphicsView::resolveScriptsDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString &candidate : {
             appDir + "/scripts",
             appDir + "/../../scripts",
             appDir + "/../scripts"
         }) {
        if (QFileInfo(candidate + "/flux_fill.py").exists())
            return QDir(candidate).absolutePath();
    }
    return appDir + "/scripts";
}

QString QVGraphicsView::resolvePythonExe()
{
    const QString venv = resolveScriptsDir() + "/.venv";
#ifdef Q_OS_WIN
    return venv + "/Scripts/python.exe";
#else
    return venv + "/bin/python";
#endif
}

void QVGraphicsView::applyRetouch()
{
    if (maskImage.isNull() || !getCurrentFileDetails().isPixmapLoaded)
        return;

    const QString scriptsDir = resolveScriptsDir();
    const QString pythonExe  = resolvePythonExe();

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
    QApplication::setOverrideCursor(Qt::WaitCursor);
    ensureWorkerStarted();

    if (!isWorkerReady) {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(workerProcess, &QProcess::readyReadStandardOutput, &loop, [&]() {
            handleWorkerOutput();
            if (isWorkerReady) loop.quit();
        });
        // Also quit if the process exits unexpectedly (crash / missing interpreter)
        connect(workerProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                &loop, &QEventLoop::quit);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(600000); // 10 min — first-run download of big-lama.onnx (~200 MB)
        loop.exec();
    }

    if (isWorkerReady) {
        workerProcess->write(QString("%1|%2|%3\n").arg(inputPath, maskPath, outputPath).toUtf8());
    } else {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("AI Error"), tr("The AI service failed to start in time."));
    }
}

void QVGraphicsView::ensureWorkerStarted()
{
    if (workerProcess && workerProcess->state() == QProcess::Running) return;

    isWorkerReady = false;
    if (workerProcess) workerProcess->deleteLater();

    workerProcess = new QProcess(this);
    connect(workerProcess, &QProcess::readyReadStandardOutput, this, &QVGraphicsView::handleWorkerOutput);
    QStringList workerArgs = { resolveScriptsDir() + "/worker.py" };
    QString lamaPath = qvGetSettingString(LamaModelPath);
    if (lamaPath.isEmpty())
        lamaPath = resolveModelsDir() + "/big-lama.onnx";
    workerArgs << "--model" << lamaPath;
    workerProcess->start(resolvePythonExe(), workerArgs);
}

void QVGraphicsView::handleWorkerOutput()
{
    while (workerProcess->canReadLine()) {
        QString line = QString::fromUtf8(workerProcess->readLine()).trimmed();
        if (line == "READY") {
            isWorkerReady = true;
            hideAiStatus();
        } else if (line == "DONE") {
            hideAiStatus();
            QApplication::restoreOverrideCursor();
            loadFile(pendingOutputPath);
            exitRetouchMode();
        } else if (line.startsWith("STATUS: ")) {
            showAiStatus(line.mid(8));
        } else if (line.startsWith("ERROR:") || line.startsWith("FATAL:")) {
            hideAiStatus();
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(this, tr("Retouch Error"), line);
        }
    }
}

void QVGraphicsView::repositionPromptBar()
{
    if (!promptBar) return;
    const int barWidth = qMin(600, width() - 40);
    promptBar->setFixedWidth(barWidth);
    promptBar->move((width() - barWidth) / 2, height() - promptBar->height() - 20);
    repositionAiStatus(); // keep status label above prompt bar
}

void QVGraphicsView::showAiStatus(const QString &text)
{
    if (!aiStatusLabel) {
        aiStatusLabel = new QLabel(this);
        aiStatusLabel->setAlignment(Qt::AlignCenter);
        aiStatusLabel->setStyleSheet(
            "QLabel {"
            "  background: rgba(20, 20, 20, 210);"
            "  color: #e0e0e0;"
            "  border: 1px solid rgba(120, 120, 120, 130);"
            "  border-radius: 8px;"
            "  padding: 7px 18px;"
            "  font-size: 13px;"
            "}"
        );
        aiStatusLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    aiStatusLabel->setText(text);
    aiStatusLabel->adjustSize();
    repositionAiStatus();
    aiStatusLabel->show();
    aiStatusLabel->raise();
}

void QVGraphicsView::hideAiStatus()
{
    if (aiStatusLabel)
        aiStatusLabel->hide();
}

void QVGraphicsView::repositionAiStatus()
{
    if (!aiStatusLabel || !aiStatusLabel->isVisible()) return;
    aiStatusLabel->adjustSize();
    const int x = (width() - aiStatusLabel->width()) / 2;
    // Sit above the prompt bar if visible, otherwise 24px from the bottom
    const int bottomAnchor = (promptBar && promptBar->isVisible())
                             ? promptBar->y() - 8
                             : height() - 24;
    aiStatusLabel->move(x, bottomAnchor - aiStatusLabel->height());
}

void QVGraphicsView::changeBrushSize(int delta)
{
    brushSize = qBound(5, brushSize + delta, 500);
    viewport()->update();
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
    const QString scriptPath = resolveScriptsDir() + "/flux_fill.py";
    const QString pythonPath = resolvePythonExe();

    const QString logPath = resolveLogPath();
    QDir().mkpath(QFileInfo(logPath).absolutePath());

    // No token at all — skip the pointless ACCESS_GATED round-trip and go straight to setup
    if (token.isEmpty()) {
        HFAuthDialog dialog(qvGetSettingString(HFModelId), QString(), QString(), this);
        if (dialog.exec() != QDialog::Accepted) return false;
        token = dialog.getToken();
        qvSetSetting(HFToken, token);
    }

    QString dialogError;
    while (true) {
        QProcess checkProcess;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PYTHONUNBUFFERED", "1");
        checkProcess.setProcessEnvironment(env);

        // Append a session header to the log
        {
            QFile log(logPath);
            if (log.open(QIODevice::Append | QIODevice::Text))
                log.write(QString("\n=== checkGenerativeAccess %1 ===\n")
                              .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                              .toUtf8());
        }

        QStringList args;
        args << "-u" << scriptPath << "--check_only"
             << "--model"    << qvGetSettingString(HFModelId)
             << "--vae"      << qvGetSettingString(HFVaeFile)
             << "--text_enc" << qvGetSettingString(HFTextEncoderFile)
             << "--base_repo"<< qvGetSettingString(HFBaseRepo);
        if (!token.isEmpty())
            args << "--token" << token;

        checkProcess.start(pythonPath, args);

        QProgressDialog progress(tr("Starting..."), tr("Cancel"), 0, 0, this);
        progress.setWindowTitle(tr("Checking Flux Access"));
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        progress.setMinimumWidth(420);
        progress.show();

        QString lastResultLine;

        QEventLoop loop;
        connect(&checkProcess, &QProcess::readyReadStandardOutput, this, [&]() {
            QFile log(logPath);
            const bool logOpen = log.open(QIODevice::Append | QIODevice::Text);
            while (checkProcess.canReadLine()) {
                QString line = QString::fromUtf8(checkProcess.readLine()).trimmed();
                if (logOpen) log.write((line + "\n").toUtf8());
                if (line.isEmpty()) continue;
                if (line.startsWith("STATUS:"))
                    progress.setLabelText(line.mid(7).trimmed());
                else
                    lastResultLine = line;
            }
        });
        connect(&checkProcess, &QProcess::readyReadStandardError, this, [&]() {
            QFile log(logPath);
            if (log.open(QIODevice::Append | QIODevice::Text))
                log.write(checkProcess.readAllStandardError());
        });
        connect(&checkProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                &loop, &QEventLoop::quit);
        connect(&progress, &QProgressDialog::canceled, &checkProcess, &QProcess::kill);
        connect(&progress, &QProgressDialog::canceled, &loop, &QEventLoop::quit);
        loop.exec();
        progress.close();

        if (checkProcess.state() != QProcess::NotRunning || progress.wasCanceled()) {
            checkProcess.kill();
            return false;
        }

        // Drain any remaining output
        while (checkProcess.canReadLine()) {
            QString line = QString::fromUtf8(checkProcess.readLine()).trimmed();
            if (!line.isEmpty() && !line.startsWith("STATUS:"))
                lastResultLine = line;
        }

        QString output = lastResultLine.isEmpty()
            ? QString::fromUtf8(checkProcess.readAllStandardOutput()).trimmed()
            : lastResultLine;
        QString errOutput = QString::fromUtf8(checkProcess.readAllStandardError()).trimmed();

        if (output == "ACCESS_GRANTED") {
            return true;
        } else if (output == "ACCESS_GATED") {
            // Token rejected or not yet agreed to terms — re-show dialog with inline error
            dialogError = token.isEmpty()
                ? tr("Access denied. Please agree to the model's terms on Hugging Face.")
                : tr("Token was rejected. Please check you agreed to the model's terms and that the token has Read access.");
            HFAuthDialog dialog(qvGetSettingString(HFModelId), token, dialogError, this);
            if (dialog.exec() != QDialog::Accepted) return false;
            token = dialog.getToken();
            qvSetSetting(HFToken, token);
            continue;
        } else if (output.isEmpty()) {
            const QString detail = errOutput.isEmpty()
                ? tr("Python process produced no output. Check that the venv is set up correctly.\n\nLog: %1").arg(logPath)
                : errOutput + tr("\n\nLog: %1").arg(logPath);
            QMessageBox::critical(this, tr("Access Error"), tr("Could not check model access:\n\n%1").arg(detail));
            return false;
        } else {
            QString detail = output;
            if (!errOutput.isEmpty()) detail += "\n\n" + errOutput;
            detail += tr("\n\nLog: %1").arg(logPath);
            QMessageBox::critical(this, tr("Access Error"), tr("An error occurred while checking access:\n\n%1").arg(detail));
            return false;
        }
    }
}

void QVGraphicsView::ensureFluxStarted()
{
    // Pick distilled or base model variant based on the options toggle.
    // The base repo ID is the same family but without step-distillation.
    const bool useBase = qvGetSettingInt(HFUseBaseModel) == 1;
    QString modelId = qvGetSettingString(HFModelId);
    if (useBase && modelId == "black-forest-labs/FLUX.2-klein-4B")
        modelId = "black-forest-labs/FLUX.2-klein-base-4B";
    else if (!useBase && modelId == "black-forest-labs/FLUX.2-klein-base-4B")
        modelId = "black-forest-labs/FLUX.2-klein-4B";

    const QString vaeFile  = qvGetSettingString(HFVaeFile);
    const QString teFile   = qvGetSettingString(HFTextEncoderFile);
    const QString baseRepo = qvGetSettingString(HFBaseRepo);

    // Resolve local model file paths (empty setting → computed default)
    QString transformerPath = qvGetSettingString(FluxTransformerPath);
    if (transformerPath.isEmpty()) {
        const QString filename = modelId.split("/").last().toLower().replace(".", "-") + ".safetensors";
        transformerPath = resolveModelsDir() + "/" + filename;
    }
    QString vaePath = qvGetSettingString(FluxVaePath);
    if (vaePath.isEmpty())
        vaePath = resolveModelsDir() + "/flux2-vae.safetensors";
    QString textEncPath = qvGetSettingString(FluxTextEncPath);
    if (textEncPath.isEmpty())
        textEncPath = resolveModelsDir() + "/qwen_3_4b.safetensors";

    // Signature covers all settings that affect the loaded model
    const QString sig = modelId + "|" + vaeFile + "|" + teFile + "|" + baseRepo
                      + "|" + transformerPath + "|" + vaePath + "|" + textEncPath;

    if (fluxProcess && fluxProcess->state() == QProcess::Running && fluxLoadedModelId == sig)
        return;

    // Kill any running process if settings changed
    if (fluxProcess && fluxProcess->state() == QProcess::Running) {
        fluxProcess->kill();
        fluxProcess->waitForFinished(2000);
    }

    if (fluxProcess) fluxProcess->deleteLater();
    fluxProcess = new QProcess(this);
    connect(fluxProcess, &QProcess::readyReadStandardOutput, this, &QVGraphicsView::handleFluxOutput);
    connect(fluxProcess, &QProcess::readyReadStandardError, this, [this]() {
        QFile log(resolveLogPath());
        if (log.open(QIODevice::Append | QIODevice::Text))
            log.write(fluxProcess->readAllStandardError());
    });
    {
        QFile log(resolveLogPath());
        QDir().mkpath(QFileInfo(log.fileName()).absolutePath());
        if (log.open(QIODevice::Append | QIODevice::Text))
            log.write(QString("\n=== ensureFluxStarted %1 ===\n")
                          .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                          .toUtf8());
    }

    const QString token = qvGetSettingString(HFToken);
    QStringList args = { resolveScriptsDir() + "/flux_fill.py",
                         "--model",    modelId,
                         "--vae",      vaeFile,
                         "--text_enc", teFile,
                         "--base_repo",baseRepo };
    args << "--transformer_path" << transformerPath
         << "--vae_path"         << vaePath
         << "--text_enc_path"    << textEncPath;
    if (!token.isEmpty())
        args << "--token" << token;

    fluxLoadedModelId = sig;
    fluxProcess->start(resolvePythonExe(), args);
}

void QVGraphicsView::handleFluxOutput()
{
    QFile log(resolveLogPath());
    const bool logOpen = log.open(QIODevice::Append | QIODevice::Text);
    while (fluxProcess->canReadLine()) {
        QString line = QString::fromUtf8(fluxProcess->readLine()).trimmed();
        if (logOpen) log.write((line + "\n").toUtf8());
        if (line.startsWith("STATUS: ")) {
            showAiStatus(line.mid(8));
        } else if (line.startsWith("OUTPUT: ")) {
            hideAiStatus();
            QPixmap result(line.mid(8));
            if (!result.isNull()) {
                undoPixmap = loadedPixmapItem->pixmap();
                loadedPixmapItem->setPixmap(result);
                exitRetouchMode(); // clear mask overlay so the result is visible
            }
            QApplication::restoreOverrideCursor();
        } else if (line.startsWith("ERROR:") || line.startsWith("FATAL:")) {
            hideAiStatus();
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(this, tr("Generate Error"), line.mid(line.indexOf(':') + 1).trimmed());
        }
    }
}

void QVGraphicsView::applyCreativeFill()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    if (retouchTool == RetouchTool::Off) {
        toggleRetouchMode(); // Enter brush mode
    }

    if (promptBar) {
        if (!promptBar->isVisible()) {
            promptBar->show();
            repositionPromptBar();
            QTimer::singleShot(0, promptBar, [this]() { promptBar->setFocusToPrompt(); });
            return;
        }

        QString prompt = promptBar->prompt();
        if (prompt.isEmpty() || isMaskEmpty()) {
            promptBar->setFocusToPrompt();
            return;
        }
        
        // If we have both prompt and mask, proceed to generation
    }

    // First ensure access
    if (qvGetSettingString(HFToken).isEmpty()) {
        if (!checkGenerativeAccess()) return;
    }

    ensureFluxStarted();

    QString prompt = promptBar ? promptBar->prompt() : QString();
    if (prompt.isEmpty()) return;

    // Build binary mask from the alpha channel (painted=255, unpainted=0)
    QImage mask(maskImage.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < maskImage.height(); ++y) {
        const QRgb *src = reinterpret_cast<const QRgb *>(maskImage.constScanLine(y));
        uchar *dst = mask.scanLine(y);
        for (int x = 0; x < maskImage.width(); ++x)
            dst[x] = qAlpha(src[x]) > 0 ? 255 : 0;
    }

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

bool QVGraphicsView::isMaskEmpty() const
{
    return maskImage.isNull() || !maskHasPaint;
}

// ============================================================================
// Isolate — SAM 3 background removal / subject isolation
// ============================================================================

void QVGraphicsView::ensureIsolateStarted()
{
    if (isolateProcess && isolateProcess->state() == QProcess::Running) return;

    if (isolateProcess) isolateProcess->deleteLater();

    isolateProcess = new QProcess(this);
    connect(isolateProcess, &QProcess::readyReadStandardOutput,
            this, &QVGraphicsView::handleIsolateOutput);
    connect(isolateProcess, &QProcess::readyReadStandardError, this, [this]() {
        QFile log(resolveLogPath());
        if (log.open(QIODevice::Append | QIODevice::Text))
            log.write(isolateProcess->readAllStandardError());
    });

    QStringList args = { resolveScriptsDir() + "/isolate.py" };
    const QString token = qvGetSettingString(HFToken);
    if (!token.isEmpty())
        args << "--token" << token;

    isolateProcess->start(resolvePythonExe(), args);
}

void QVGraphicsView::handleIsolateOutput()
{
    QFile log(resolveLogPath());
    const bool logOpen = log.open(QIODevice::Append | QIODevice::Text);

    while (isolateProcess->canReadLine()) {
        QString line = QString::fromUtf8(isolateProcess->readLine()).trimmed();
        if (logOpen) log.write((line + "\n").toUtf8());

        if (line.startsWith("STATUS: ")) {
            showAiStatus(line.mid(8));

        } else if (line == "ACCESS_GATED") {
            // SAM 3 is gated — reuse the Flux auth dialog to collect a token
            hideAiStatus();
            isolateState = IsolateState::Idle;
            QApplication::restoreOverrideCursor();
            if (isolateProcess) { isolateProcess->kill(); isolateProcess->deleteLater(); isolateProcess = nullptr; }

            QString token = qvGetSettingString(HFToken);
            const QString hint = tr("SAM 3 is a gated model. Accept the terms at "
                                    "huggingface.co/facebook/sam3 and enter a token with Read access.");
            HFAuthDialog dialog("facebook/sam3", token, hint, this);
            if (dialog.exec() != QDialog::Accepted) return;
            token = dialog.getToken();
            qvSetSetting(HFToken, token);
            // Retry with the new token
            applyIsolate();
            return;

        } else if (line.startsWith("SEGMENTS: ")
                   && isolateState == IsolateState::WaitingForSegments) {
            hideAiStatus();
            int n = line.mid(10).toInt();
            if (n == 0) {
                QMessageBox::warning(this, tr("Isolate"), tr("No segments found in this image."));
                isolateState = IsolateState::Idle;
                break;
            }

            QString tempDir  = QDir::tempPath();
            QString vizPath  = QDir(tempDir).filePath("iqview_isolate_viz.png");
            QString idMapPath = QDir(tempDir).filePath("iqview_isolate_ids.png");

            IsolateDialog dlg(vizPath, idMapPath, n, this);
            if (dlg.exec() == QDialog::Accepted) {
                QSet<int> selected = dlg.selectedSegments();
                if (!selected.isEmpty()) {
                    isolateState = IsolateState::WaitingForCompose;

                    QString outputPath = QDir(tempDir).filePath("iqview_isolate_out.png");
                    QStringList ids;
                    for (int id : selected) ids << QString::number(id);

                    showAiStatus(tr("Compositing selection..."));
                    QString cmd = QString("COMPOSE|%1|%2|%3\n")
                                      .arg(isolateInputPath, ids.join(","), outputPath);
                    isolateProcess->write(cmd.toUtf8());
                } else {
                    isolateState = IsolateState::Idle;
                }
            } else {
                isolateState = IsolateState::Idle;
            }

        } else if (line.startsWith("OUTPUT: ")
                   && isolateState == IsolateState::WaitingForCompose) {
            hideAiStatus();
            QPixmap result(line.mid(8));
            if (!result.isNull()) {
                undoPixmap = loadedPixmapItem->pixmap();
                loadedPixmapItem->setPixmap(result);
            }
            isolateState = IsolateState::Idle;
            QApplication::restoreOverrideCursor();

        } else if (line.startsWith("ERROR:") || line.startsWith("FATAL:")) {
            hideAiStatus();
            isolateState = IsolateState::Idle;
            QApplication::restoreOverrideCursor();
            QString msg = line.mid(line.indexOf(':') + 1).trimmed();
            // Treat any gated-access error the same as ACCESS_GATED
            if (msg.contains("gated", Qt::CaseInsensitive)
                    || msg.contains("access", Qt::CaseInsensitive) && msg.contains("repo", Qt::CaseInsensitive)) {
                if (isolateProcess) { isolateProcess->kill(); isolateProcess->deleteLater(); isolateProcess = nullptr; }
                QString token = qvGetSettingString(HFToken);
                const QString hint = tr("SAM 3 is a gated model. Accept the terms at "
                                        "huggingface.co/facebook/sam3 and enter a token with Read access.");
                HFAuthDialog dialog("facebook/sam3", token, hint, this);
                if (dialog.exec() == QDialog::Accepted) {
                    qvSetSetting(HFToken, dialog.getToken());
                    applyIsolate();
                }
            } else {
                QMessageBox::warning(this, tr("Isolate Error"), msg);
            }
        }
    }
}

void QVGraphicsView::applyIsolate()
{
    if (!getCurrentFileDetails().isPixmapLoaded) return;
    if (isolateState != IsolateState::Idle) return;   // already running

    // Prompt for HF token upfront if none stored (SAM 3 is gated)
    if (qvGetSettingString(HFToken).isEmpty()) {
        if (!checkGenerativeAccess()) return;
    }

    QString tempDir   = QDir::tempPath();
    isolateInputPath  = QDir(tempDir).filePath("iqview_isolate_in.png");
    QString vizPath   = QDir(tempDir).filePath("iqview_isolate_viz.png");
    QString idMapPath = QDir(tempDir).filePath("iqview_isolate_ids.png");

    // Save the currently displayed image
    loadedPixmapItem->pixmap().save(isolateInputPath);

    ensureIsolateStarted();

    isolateState = IsolateState::WaitingForSegments;
    showAiStatus(tr("Segmenting image with SAM 3..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);

    QString cmd = QString("SEGMENT|%1|%2|%3\n").arg(isolateInputPath, vizPath, idMapPath);
    isolateProcess->write(cmd.toUtf8());
}

#include "qvgraphicsview.h"
#include "ailogdialog.h"
#include "ratingmanager.h"
#include "variantdialog.h"
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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QStorageInfo>
#include <QFileDialog>
#include <QTextStream>
#include <QProgressBar>
#include <QPushButton>
#include <QInputDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>

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

    ratingManager = new RatingManager(this);

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

    idlePrefetchTimer = new QTimer(this);
    idlePrefetchTimer->setSingleShot(true);
    idlePrefetchTimer->setInterval(4000);
    connect(idlePrefetchTimer, &QTimer::timeout, this, &QVGraphicsView::performIdlePrefetch);

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
    repositionRatingLabel();
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
    // pendingEditedSource is set only by beginAiResultLoad(); every other
    // load is a normal file open, which means there are no unsaved edits.
    editedSource = pendingEditedSource;
    pendingEditedSource.clear();
    imageCore.loadFile(fileName);
}

// Load an AI result while remembering which real file it was derived from, so
// Save can offer to write back to the original rather than the temp output.
void QVGraphicsView::beginAiResultLoad(const QString &outputPath)
{
    // On a chain of edits (Retouch -> Fill -> Isolate) the current file is
    // already a temp output, so keep pointing at the true original.
    pendingEditedSource = editedSource.isEmpty()
            ? getCurrentFileDetails().fileInfo.absoluteFilePath()
            : editedSource;
    loadFile(outputPath);
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
    scheduleIdlePrefetch();

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
    undoStack.clear();
    redoStack.clear();

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
        // If the user just deleted a file through iqView, closeImage will have been called which
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

        // Eagerly start the AI worker so it's warm by the time the user clicks 'Apply'.
        // Only if the environment is already set up — otherwise resolvePythonExe()
        // points at a venv that doesn't exist yet, and starting it here would fail
        // with a raw QProcess error before the user ever reaches applyRetouch()'s
        // friendly "set up the AI environment?" first-run flow.
        idlePrefetchTimer->stop();
        if (QFile::exists(resolvePythonExe())) {
            silentWorkerStart = false;
            ensureWorkerStarted();
        }
    } else {
        exitRetouchMode();
    }
}

void QVGraphicsView::exitRetouchMode()
{
    // Escape is bound here, so it doubles as the cancel key for a running job.
    // If one was interrupted that is the whole action -- leave the mask and the
    // tool as they were so the user can adjust and retry without redrawing it.
    if (cancelAiOperation())
        return;

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

// Returns a unique temp file path for an AI result. The image cache keys on
// path + file size, and same-dimension BMP outputs are byte-identical in size —
// so reusing one output path makes every generation after the first display
// stale cached pixels. A fresh name per run sidesteps that. Outputs from
// previous sessions (older than an hour) are cleaned up opportunistically.
static QString uniqueAiOutputPath(const QString &prefix, const QString &ext,
                                  const QString &keepPath = QString())
{
    QDir temp(QDir::tempPath());
    const auto previous = temp.entryInfoList({ prefix + "_*." + ext }, QDir::Files);
    for (const auto &fileInfo : previous) {
        // Never delete the image the user is currently looking at, however old.
        if (fileInfo.absoluteFilePath() == keepPath)
            continue;
        if (fileInfo.lastModified().secsTo(QDateTime::currentDateTime()) > 3600)
            QFile::remove(fileInfo.absoluteFilePath());
    }
    return temp.filePath(QString("%1_%2.%3")
                             .arg(prefix)
                             .arg(QDateTime::currentMSecsSinceEpoch())
                             .arg(ext));
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

QString QVGraphicsView::resolveVenvDir()
{
    // Prefer a venv sitting next to the scripts -- that's a developer checkout,
    // and reusing it avoids re-downloading several GB of dependencies.
    const QString local = resolveScriptsDir() + "/.venv";
    if (QFileInfo::exists(local))
        return QDir(local).absolutePath();

    // Installed builds live somewhere unwritable (Program Files, /opt, inside an
    // .app bundle), so the environment has to go in app data instead.
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .absoluteFilePath("venv");
}

QString QVGraphicsView::resolvePythonExe()
{
    const QString venv = resolveVenvDir();
#ifdef Q_OS_WIN
    return venv + "/Scripts/python.exe";
#else
    return venv + "/bin/python";
#endif
}

// Pinned uv release. Bump deliberately, not automatically -- keeps the
// download URL and behavior reproducible across sessions.
static const QString UV_VERSION = QStringLiteral("0.12.0");

QString QVGraphicsView::resolveUvDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .absoluteFilePath("uv");
}

QString QVGraphicsView::resolveUvExe()
{
#ifdef Q_OS_WIN
    return resolveUvDir() + "/uv.exe";
#else
    return resolveUvDir() + "/uv";
#endif
}

// uv itself manages the Python interpreter (downloading a portable one if
// needed) and picks the right PyTorch accelerator per machine, so iqView no
// longer depends on a pre-installed system Python at all. uv is a small
// (~20 MB), dependency-free static binary -- fetched once from GitHub
// releases and cached in AppData, the same place the venv itself lives.
bool QVGraphicsView::ensureUvInstalled(QProgressDialog &progress, const QString &logPath)
{
    if (QFile::exists(resolveUvExe()))
        return true;

    QString platformTag;
    QString archiveExt;
#ifdef Q_OS_WIN
    archiveExt = "zip";
    const QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == QLatin1String("arm64"))
        platformTag = "aarch64-pc-windows-msvc";
    else if (arch == QLatin1String("i386") || arch == QLatin1String("x86"))
        platformTag = "i686-pc-windows-msvc";
    else
        platformTag = "x86_64-pc-windows-msvc";
#elif defined(Q_OS_MACOS)
    archiveExt = "tar.gz";
    platformTag = QSysInfo::currentCpuArchitecture() == QLatin1String("arm64")
            ? "aarch64-apple-darwin"
            : "x86_64-apple-darwin";
#else
    archiveExt = "tar.gz";
    platformTag = QSysInfo::currentCpuArchitecture() == QLatin1String("arm64")
            ? "aarch64-unknown-linux-gnu"
            : "x86_64-unknown-linux-gnu";
#endif

    const QString assetName = QString("uv-%1.%2").arg(platformTag, archiveExt);
    const QUrl url(QString("https://github.com/astral-sh/uv/releases/download/%1/%2")
                           .arg(UV_VERSION, assetName));

    progress.setLabelText(tr("Downloading setup tool…"));

    QTemporaryFile archive(QDir::tempPath() + "/iqview_uv_XXXXXX." + archiveExt);
    if (!archive.open()) {
        QMessageBox::critical(this, tr("AI Setup"), tr("Could not create a temporary file."));
        return false;
    }
    archive.setAutoRemove(false);
    const QString archivePath = archive.fileName();
    archive.close();

    QNetworkAccessManager net;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    auto *reply = net.get(request);

    QFile out(archivePath);
    if (!out.open(QIODevice::WriteOnly)) {
        reply->abort();
        reply->deleteLater();
        return false;
    }

    QEventLoop loop;
    connect(reply, &QNetworkReply::readyRead, &loop,
            [&]() { out.write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, &loop,
            [&](qint64 received, qint64 total) {
                if (total > 0)
                    progress.setLabelText(
                            tr("Downloading setup tool… %1 / %2 MB")
                                    .arg(received / 1024 / 1024)
                                    .arg(total / 1024 / 1024));
            });
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    loop.exec();

    out.write(reply->readAll());
    out.close();
    const bool ok = reply->error() == QNetworkReply::NoError;
    const QString errorString = reply->errorString();
    reply->deleteLater();

    if (!ok) {
        QFile::remove(archivePath);
        if (!progress.wasCanceled()) {
            QMessageBox::critical(this, tr("AI Setup"),
                                  tr("Could not download the setup tool: %1").arg(errorString));
        }
        return false;
    }

    const QString uvDir = resolveUvDir();
    QDir().mkpath(uvDir);

    // bsdtar (ships as tar.exe on Windows 10 1803+, and is the default tar on
    // macOS) handles both .zip and .tar.gz with the same -xf invocation, so
    // one code path covers all three platforms without adding a zip library.
    progress.setLabelText(tr("Extracting setup tool…"));
    QProcess extract;
    extract.setWorkingDirectory(uvDir);
    extract.start("tar", { "-xf", archivePath });
    const bool extracted = extract.waitForStarted() && extract.waitForFinished(60000)
            && extract.exitStatus() == QProcess::NormalExit && extract.exitCode() == 0;
    QFile::remove(archivePath);

    if (!extracted || !QFile::exists(resolveUvExe())) {
        QFile log(logPath);
        if (log.open(QIODevice::Append | QIODevice::Text))
            log.write(extract.readAllStandardError());
        QMessageBox::critical(this, tr("AI Setup"),
                              tr("Could not extract the setup tool.\n\nLog: %1")
                                      .arg(QDir::toNativeSeparators(logPath)));
        return false;
    }

#ifndef Q_OS_WIN
    QFile::setPermissions(resolveUvExe(),
                          QFile::permissions(resolveUvExe()) | QFile::ExeOwner | QFile::ExeUser
                                  | QFile::ExeGroup);
#endif
    return true;
}

// Bump when the bootstrap itself changes in a way that makes an already-built
// environment wrong rather than merely out of date. Version 2 marks the move
// from `python -m venv` + plain pip to uv: environments built by the old path
// still run, but never received the platform-correct onnxruntime or the
// driver-matched PyTorch build, so they are worth rebuilding once.
static const int AI_BOOTSTRAP_VERSION = 2;

QString QVGraphicsView::resolveEnvStampPath()
{
    return resolveVenvDir() + "/.iqview-env-stamp";
}

// Identity of the environment the current source tree would produce: the
// bootstrap version plus a hash of requirements.txt. Anything other than an
// exact match against the stored stamp means the installed environment is
// stale -- which is how the missing-torchvision bug went unnoticed, since
// dependencies were only ever installed on the run that created the venv.
QString QVGraphicsView::currentEnvStamp()
{
    QByteArray hash;
    QFile requirements(resolveScriptsDir() + "/requirements.txt");
    if (requirements.open(QIODevice::ReadOnly)) {
        hash = QCryptographicHash::hash(requirements.readAll(), QCryptographicHash::Sha256)
                       .toHex();
    }
    return QString("%1:%2").arg(AI_BOOTSTRAP_VERSION).arg(QString::fromLatin1(hash));
}

// Warn before starting a multi-GB download onto a drive that cannot hold it.
// Returns false only if the user chooses to stop; a low estimate is a warning
// rather than a hard block, since the requirement is approximate.
bool QVGraphicsView::confirmDiskSpace(const QString &targetDir, qint64 requiredBytes)
{
    QDir().mkpath(targetDir);
    const QStorageInfo storage(targetDir);
    if (!storage.isValid() || !storage.isReady())
        return true;   // can't tell -- don't get in the way

    const qint64 available = storage.bytesAvailable();
    if (available >= requiredBytes)
        return true;

    const auto gb = [](qint64 bytes) { return QString::number(bytes / 1073741824.0, 'f', 1); };
    return QMessageBox::warning(
                   this, tr("AI Setup"),
                   tr("Setting up the AI environment needs roughly %1 GB, but only %2 GB is "
                      "free on %3.\n\nThe download will fail part-way through if it runs out "
                      "of space.\n\nContinue anyway?")
                           .arg(gb(requiredBytes), gb(available),
                                QDir::toNativeSeparators(storage.rootPath())),
                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes;
}

// First-run setup, shared by every AI feature. Creates the Python environment
// and installs dependencies, reporting progress as it goes. Returns true when
// the environment is ready to use.
bool QVGraphicsView::ensureAiEnvironment()
{
    const QString requirements = resolveScriptsDir() + "/requirements.txt";
    if (!QFile::exists(requirements)) {
        QMessageBox::critical(this, tr("AI Setup"),
                              tr("Could not find the AI scripts that ship with iqView.\n\n"
                                 "Expected: %1")
                                      .arg(QDir::toNativeSeparators(requirements)));
        return false;
    }

    const bool haveInterpreter = QFile::exists(resolvePythonExe());
    const QString wantStamp = currentEnvStamp();
    QString haveStamp;
    QFile stampFile(resolveEnvStampPath());
    if (stampFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        haveStamp = QString::fromUtf8(stampFile.readAll()).trimmed();
        stampFile.close();
    }

    if (haveInterpreter && haveStamp == wantStamp)
        return true;

    // An interpreter with a stale stamp only needs its dependencies refreshed,
    // not a rebuild from scratch -- much faster, and uv skips anything already
    // at the right version.
    const bool refreshOnly = haveInterpreter;

    if (refreshOnly && declinedEnvStamp == wantStamp)
        return true;   // asked already this session; carry on with what's installed

    if (refreshOnly) {
        if (QMessageBox::question(
                    this, tr("AI Setup"),
                    tr("iqView's AI dependencies have changed since this environment was "
                       "set up.\n\nUpdating keeps the AI features working correctly. It "
                       "downloads only what changed, and you can keep using the current "
                       "setup if you skip it.\n\nUpdate now?"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
            != QMessageBox::Yes) {
            declinedEnvStamp = wantStamp;
            return true;   // their existing environment still works
        }
    } else if (QMessageBox::question(
                       this, tr("AI Setup"),
                       tr("iqView needs to set up a local Python environment before it can use "
                          "its AI features.\n\nThis downloads several GB and can take 10–20 "
                          "minutes, but only happens once.\n\nSet it up now?"),
                       QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
               != QMessageBox::Yes) {
        return false;
    }

    const QString venvDir = resolveVenvDir();
    QDir().mkpath(QFileInfo(venvDir).absolutePath());

    // uv stages downloads in its cache before linking them into the venv, so
    // peak usage is roughly double the installed size. A CUDA PyTorch build
    // alone is ~2.5 GB; 10 GB covers a full install with headroom, and a
    // refresh only re-fetches what changed.
    if (!confirmDiskSpace(QFileInfo(venvDir).absolutePath(),
                          refreshOnly ? 3LL * 1073741824LL : 10LL * 1073741824LL))
        return false;

    const QString logPath = resolveLogPath();
    QDir().mkpath(QFileInfo(logPath).absolutePath());

    QProgressDialog progress(tr("Creating Python environment…"), tr("Cancel"), 0, 0, this);
    progress.setWindowTitle(tr("Setting up AI environment"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setMinimumWidth(480);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.show();

    // uv downloads packages into its own cache before linking them into the
    // venv, so the venv directory barely grows until a burst at the very end
    // -- pointing the cache somewhere known lets the heartbeat below watch
    // the directory that's actually filling up in real time.
    const QString uvCacheDir = resolveUvDir() + "/cache";

    // Runs one setup step, pumping its output into the progress dialog so the
    // UI stays responsive and the user can see something is happening.
    auto runStep = [&](const QString &program, const QStringList &args, int timeoutMs) {
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("UV_CACHE_DIR", uvCacheDir);
        proc.setProcessEnvironment(env);
        QFile log(logPath);
        const bool logOpen = log.open(QIODevice::Append | QIODevice::Text);

        QEventLoop loop;
        connect(&proc, &QProcess::readyReadStandardOutput, &loop, [&]() {
            while (proc.canReadLine()) {
                const QString line = QString::fromUtf8(proc.readLine()).trimmed();
                if (line.isEmpty())
                    continue;
                if (logOpen)
                    log.write((line + "\n").toUtf8());
                progress.setLabelText(line.right(140));
            }
        });
        connect(&proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop,
                &QEventLoop::quit);
        connect(&proc, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
        connect(&progress, &QProgressDialog::canceled, &proc, &QProcess::kill);

        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &loop, [&]() {
            proc.kill();
            loop.quit();
        });
        timeout.start(timeoutMs);

        proc.start(program, args);
        loop.exec();
        return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    };

    // 0. Fetch uv itself if this is the very first AI feature used on this
    //    machine. uv manages its own portable Python (no system Python
    //    dependency at all) and its own accelerator selection below.
    if (!ensureUvInstalled(progress, logPath)) {
        progress.close();
        return false;
    }
    if (progress.wasCanceled()) {
        progress.close();
        return false;
    }

    // 1. Create the virtual environment. uv downloads a portable Python 3.12
    //    automatically if none is already available -- no system Python
    //    install/PATH requirement, on any of the three platforms.
    //    --clear wipes any partial directory left behind by a previous
    //    cancelled/crashed/interrupted attempt -- without it, uv refuses to
    //    reuse a non-empty directory and setup fails permanently every time
    //    afterward until the user manually deletes it.
    if (!refreshOnly) {
        progress.setLabelText(tr("Creating Python environment…"));
        const bool created =
                runStep(resolveUvExe(), { "venv", "--python", "3.12", "--clear", venvDir }, 600000)
                && QFile::exists(resolvePythonExe());
        if (!created) {
            progress.close();
            if (!progress.wasCanceled()) {
                QMessageBox::critical(this, tr("AI Setup"),
                                      tr("Could not create the Python environment.\n\nLog: %1")
                                              .arg(QDir::toNativeSeparators(logPath)));
            }
            return false;
        }
    }

    // 2. Install dependencies. --torch-backend=auto detects the installed GPU
    //    driver (NVIDIA/AMD/Intel) and picks the matching PyTorch build, or
    //    falls back to CPU/MPS automatically -- same command on every
    //    platform. Torch alone is multi-GB, so allow a long window.
    //
    //    --no-progress suppresses uv's own \r-based progress bar, which isn't
    //    line-buffered and would otherwise never reach readyReadStandardOutput
    //    until a real newline arrives -- leaving the dialog looking frozen for
    //    long stretches. In its place, a heartbeat timer reports the venv's
    //    on-disk size every couple of seconds, so there's always visible
    //    movement even during silent multi-minute downloads.
    progress.setLabelText(refreshOnly ? tr("Updating AI dependencies…")
                                      : tr("Installing AI dependencies (several GB)…"));
    QTimer sizeHeartbeat;
    sizeHeartbeat.setInterval(2000);
    connect(&sizeHeartbeat, &QTimer::timeout, &progress, [&]() {
        qint64 bytes = 0;
        // Cache fills up first (real-time download progress); the venv itself
        // only grows in a final burst when uv links the cached files in.
        for (const QString &dir : { uvCacheDir, venvDir }) {
            QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                bytes += it.fileInfo().size();
            }
        }
        progress.setLabelText(
                tr("Installing AI dependencies… %1 MB so far").arg(bytes / 1024 / 1024));
    });
    sizeHeartbeat.start();

    const bool installed = runStep(resolveUvExe(),
                                   { "pip", "install", "--python", resolvePythonExe(),
                                    "--torch-backend", "auto", "--no-progress", "-r",
                                    requirements },
                                   3600000);
    sizeHeartbeat.stop();
    const bool canceled = progress.wasCanceled();
    progress.close();

    if (!installed) {
        if (!canceled) {
            QMessageBox::critical(this, tr("AI Setup"),
                                  tr("Failed to install the AI dependencies.\n\nLog: %1")
                                          .arg(QDir::toNativeSeparators(logPath)));
        }
        // Deliberately leave the stamp untouched on failure so the next attempt
        // still sees the environment as stale and retries.
        return false;
    }

    if (stampFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        stampFile.write(wantStamp.toUtf8());
        stampFile.close();
    }
    return true;
}

void QVGraphicsView::applyRetouch()
{
    if (maskImage.isNull() || !getCurrentFileDetails().isPixmapLoaded)
        return;

    if (!ensureAiEnvironment())
        return;

    // PNG rather than BMP for the image round-trip: BMP cannot carry an alpha
    // channel, so retouching the transparent output of Isolate would silently
    // flatten the cutout back to an opaque rectangle. The mask stays BMP —
    // it is pure 8-bit coverage and never has alpha.
    QString inputPath = QDir::tempPath() + "/iqview_retouch_in.png";
    QString maskPath = QDir::tempPath() + "/iqview_retouch_mask.bmp";
    QString outputPath = uniqueAiOutputPath("iqview_retouch_out", "png",
                                            getCurrentFileDetails().fileInfo.absoluteFilePath());

    pushUndoState(loadedPixmapItem->pixmap());
    loadedPixmapItem->pixmap().save(inputPath, "PNG");
    maskImage.save(maskPath, "BMP");

    pendingOutputPath = outputPath;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    silentWorkerStart = false;
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
        activeAiJob = AiJob::Retouch;
        workerProcess->write(QString("%1|%2|%3\n").arg(inputPath, maskPath, outputPath).toUtf8());
        showAiStatus(tr("Retouching…"));
    } else {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("AI Error"), tr("The AI service failed to start in time."));
    }
}

void QVGraphicsView::scheduleIdlePrefetch()
{
    idlePrefetchTimer->stop();
    if (!qvGetSettingBool(PrefetchLamaOnIdle))
        return;
    if (!getCurrentFileDetails().isPixmapLoaded || retouchTool != RetouchTool::Off)
        return;
    if (workerProcess && workerProcess->state() == QProcess::Running)
        return;   // already warm, nothing to prefetch
    idlePrefetchTimer->start();
}

void QVGraphicsView::performIdlePrefetch()
{
    if (!qvGetSettingBool(PrefetchLamaOnIdle))
        return;
    if (!getCurrentFileDetails().isPixmapLoaded || retouchTool != RetouchTool::Off)
        return;
    if (workerProcess && workerProcess->state() == QProcess::Running)
        return;
    // Never trigger the first-time "set up AI environment" flow on our own —
    // only warm the worker if it's already installed.
    if (!QFile::exists(resolvePythonExe()))
        return;

    silentWorkerStart = true;
    ensureWorkerStarted();
}

void QVGraphicsView::ensureWorkerStarted()
{
    if (workerProcess && workerProcess->state() == QProcess::Running) return;

    isWorkerReady = false;
    if (workerProcess) workerProcess->deleteLater();

    workerProcess = new QProcess(this);
    connect(workerProcess, &QProcess::readyReadStandardOutput, this, &QVGraphicsView::handleWorkerOutput);
    connect(workerProcess, &QProcess::readyReadStandardError, this, [this]() {
        QFile log(QDir(QDir::tempPath()).filePath("iqview_worker_log.txt"));
        if (log.open(QIODevice::Append | QIODevice::Text))
            log.write(workerProcess->readAllStandardError());
    });
    connect(workerProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (isWorkerReady)
            return;
        hideAiStatus();
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("AI Error"),
                              tr("The AI service failed to start: %1\n\n"
                                 "Check that the Python environment is set up (run Retouch once "
                                 "to install it).\n\nLog: %2")
                                  .arg(workerProcess->errorString(),
                                       QDir(QDir::tempPath()).filePath("iqview_worker_log.txt")));
    });
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
            silentWorkerStart = false;
            hideAiStatus();
        } else if (line == "DONE") {
            activeAiJob = AiJob::None;
            hideAiStatus();
            QApplication::restoreOverrideCursor();
            beginAiResultLoad(pendingOutputPath);
            exitRetouchMode();
        } else if (line.startsWith("STATUS: ")) {
            if (!silentWorkerStart)
                showAiStatus(line.mid(8));
        } else if (line.startsWith("ERROR:") || line.startsWith("FATAL:")) {
            // Don't pop an error dialog for a failure the user never asked for —
            // a silent idle prefetch that fails just means R will fall back to
            // the normal cold-start path (with its own error handling) later.
            const bool wasSilentPrefetch = silentWorkerStart;
            silentWorkerStart = false;
            activeAiJob = AiJob::None;
            hideAiStatus();
            QApplication::restoreOverrideCursor();
            if (!wasSilentPrefetch)
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
    if (!aiStatusWidget) {
        aiStatusWidget = new QWidget(this);
        aiStatusWidget->setStyleSheet(
            "QWidget {"
            "  background: rgba(20, 20, 20, 210);"
            "  border-radius: 8px;"
            "}");

        aiStatusLabel = new QLabel(aiStatusWidget);
        aiStatusLabel->setAlignment(Qt::AlignCenter);
        aiStatusLabel->setStyleSheet(
            "QLabel {"
            "  background: transparent;"
            "  color: #e0e0e0;"
            "  font-size: 13px;"
            "}"
        );

        // Indeterminate: none of the workers report a completion fraction, but
        // a moving bar still distinguishes "working" from "hung", which is the
        // question a user actually has during a silent multi-minute step.
        aiProgressBar = new QProgressBar(aiStatusWidget);
        aiProgressBar->setRange(0, 0);
        aiProgressBar->setTextVisible(false);
        aiProgressBar->setFixedHeight(4);
        aiProgressBar->setStyleSheet(
            "QProgressBar {"
            "  background: rgba(255, 255, 255, 30);"
            "  border: none;"
            "  border-radius: 2px;"
            "}"
            "QProgressBar::chunk {"
            "  background: #4a9eff;"
            "  border-radius: 2px;"
            "}"
        );

        aiCancelButton = new QPushButton(tr("Cancel"), aiStatusWidget);
        aiCancelButton->setCursor(Qt::ArrowCursor);
        aiCancelButton->setStyleSheet(
            "QPushButton {"
            "  background: rgba(255, 255, 255, 26);"
            "  color: #e0e0e0;"
            "  border: 1px solid rgba(150, 150, 150, 120);"
            "  border-radius: 4px;"
            "  padding: 3px 12px;"
            "  font-size: 12px;"
            "}"
            "QPushButton:hover { background: rgba(255, 255, 255, 46); }"
        );
        connect(aiCancelButton, &QPushButton::clicked, this,
                [this]() { cancelAiOperation(); });

        auto *bottomRow = new QHBoxLayout;
        bottomRow->setContentsMargins(0, 0, 0, 0);
        bottomRow->addWidget(aiProgressBar, 1);
        bottomRow->addWidget(aiCancelButton, 0);

        auto *layout = new QVBoxLayout(aiStatusWidget);
        layout->setContentsMargins(16, 10, 16, 10);
        layout->setSpacing(8);
        layout->addWidget(aiStatusLabel);
        layout->addLayout(bottomRow);

        aiStatusWidget->setStyleSheet(aiStatusWidget->styleSheet()
                                     + "QWidget#aiStatusPanel { border: 1px solid rgba(120, 120, 120, 130); }");
        aiStatusWidget->setObjectName("aiStatusPanel");
    }

    aiStatusLabel->setText(text);
    // Only offer Cancel when there is a job to cancel: the HUD is also used for
    // the silent idle prefetch and for plain status text.
    aiCancelButton->setVisible(activeAiJob != AiJob::None);
    repositionAiStatus();
    aiStatusWidget->show();
    aiStatusWidget->raise();
}

void QVGraphicsView::hideAiStatus()
{
    if (aiStatusWidget)
        aiStatusWidget->hide();
}

void QVGraphicsView::repositionAiStatus()
{
    if (!aiStatusWidget || !aiStatusWidget->isVisible()) return;

    const int panelWidth = qMin(560, qMax(280, width() - 80));
    aiStatusWidget->setFixedWidth(panelWidth);
    aiStatusWidget->adjustSize();

    const int x = (width() - aiStatusWidget->width()) / 2;
    // Sit above the prompt bar if visible, otherwise 24px from the bottom
    const int bottomAnchor = (promptBar && promptBar->isVisible())
                             ? promptBar->y() - 8
                             : height() - 24;
    aiStatusWidget->move(x, bottomAnchor - aiStatusWidget->height());
}

// Interrupts whatever AI job is in flight. None of the workers implement a
// cancel command -- they block inside a single inference or download call --
// so terminating the process is the only way out. Signals are disconnected
// first so the resulting "process died" handlers don't fire and report a
// deliberate cancellation as an error.
bool QVGraphicsView::cancelAiOperation()
{
    if (activeAiJob == AiJob::None)
        return false;

    const auto stop = [](QProcess *&proc) {
        if (!proc)
            return;
        proc->disconnect();
        proc->kill();
        proc->waitForFinished(2000);
        proc->deleteLater();
        proc = nullptr;
    };

    switch (activeAiJob) {
    case AiJob::Retouch:
        stop(workerProcess);
        isWorkerReady = false;
        pendingOutputPath.clear();
        break;
    case AiJob::Fill:
        stop(fluxProcess);
        fluxLoadedModelId.clear();
        fluxBatchExpected = 1;
        fluxBatchResults.clear();
        break;
    case AiJob::Isolate:
        stop(isolateProcess);
        isolateState = IsolateState::Idle;
        break;
    case AiJob::None:
        break;
    }

    activeAiJob = AiJob::None;
    silentWorkerStart = false;
    hideAiStatus();
    QApplication::restoreOverrideCursor();
    return true;
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

void QVGraphicsView::pushUndoState(const QPixmap &pixmap)
{
    if (pixmap.isNull())
        return;

    undoStack.append(pixmap);
    while (undoStack.size() > MAX_UNDO_STEPS)
        undoStack.removeFirst();

    // Starting a new edit invalidates anything that was undone before it.
    redoStack.clear();
}

bool QVGraphicsView::undoRetouch()
{
    if (undoStack.isEmpty()) return false;

    redoStack.append(loadedPixmapItem->pixmap());
    loadedPixmapItem->setPixmap(undoStack.takeLast());

    updateMaskItem();
    viewport()->update();
    return true;
}

bool QVGraphicsView::redoRetouch()
{
    if (redoStack.isEmpty()) return false;

    undoStack.append(loadedPixmapItem->pixmap());
    loadedPixmapItem->setPixmap(redoStack.takeLast());

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
        // Token goes via environment so it never shows up in process listings
        if (!token.isEmpty())
            env.insert("HF_TOKEN", token);
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
    connect(fluxProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        hideAiStatus();
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("Generate Error"),
                              tr("The AI service failed to start: %1\n\n"
                                 "Check that the Python environment is set up (run Retouch once "
                                 "to install it).\n\nLog: %2")
                                  .arg(fluxProcess->errorString(), resolveLogPath()));
    });
    {
        QFile log(resolveLogPath());
        QDir().mkpath(QFileInfo(log.fileName()).absolutePath());
        if (log.open(QIODevice::Append | QIODevice::Text))
            log.write(QString("\n=== ensureFluxStarted %1 ===\n")
                          .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                          .toUtf8());
    }

    QStringList args = { resolveScriptsDir() + "/flux_fill.py",
                         "--model",    modelId,
                         "--vae",      vaeFile,
                         "--text_enc", teFile,
                         "--base_repo",baseRepo };
    args << "--transformer_path" << transformerPath
         << "--vae_path"         << vaePath
         << "--text_enc_path"    << textEncPath;

    // Token goes via environment so it never shows up in process listings
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONUNBUFFERED", "1");
    const QString token = qvGetSettingString(HFToken);
    if (!token.isEmpty())
        env.insert("HF_TOKEN", token);
    fluxProcess->setProcessEnvironment(env);

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
            const QString outputPath = line.mid(8).trimmed();

            // In a batch, collect results and wait for BATCH_DONE before asking
            // the user to choose.
            if (fluxBatchExpected > 1) {
                fluxBatchResults << outputPath;
                continue;
            }

            activeAiJob = AiJob::None;
            hideAiStatus();
            // Capture the current (original) pixmap for undo before loadFile replaces it.
            pushUndoState(loadedPixmapItem->pixmap());
            // Load result into imageCore — same as the LaMa pipeline does.
            // Direct setPixmap() on the scene item would be overwritten 50 ms later
            // by scaleExpensively(), which scales imageCore's (original) pixmap.
            beginAiResultLoad(outputPath);
            exitRetouchMode();
            QApplication::restoreOverrideCursor();

        } else if (line.startsWith("BATCH_DONE")) {
            finishFluxBatch();
        } else if (line.startsWith("ERROR:") || line.startsWith("FATAL:")) {
            activeAiJob = AiJob::None;
            hideAiStatus();
            fluxBatchExpected = 1;
            fluxBatchResults.clear();
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(this, tr("Generate Error"), line.mid(line.indexOf(':') + 1).trimmed());
        }
    }
}

// All variants of a batch generation have arrived — let the user pick one,
// then discard the rest.
void QVGraphicsView::finishFluxBatch()
{
    activeAiJob = AiJob::None;
    hideAiStatus();
    QApplication::restoreOverrideCursor();

    const QStringList results = fluxBatchResults;
    fluxBatchResults.clear();
    fluxBatchExpected = 1;

    if (results.isEmpty())
        return;

    VariantDialog dialog(results, this);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    const QString chosen = accepted ? dialog.selectedPath() : QString();

    // Remove the variants that weren't chosen so they don't pile up in temp.
    for (const QString &path : results) {
        if (path != chosen)
            QFile::remove(path);
    }

    if (chosen.isEmpty())
        return;   // cancelled — leave the original image untouched

    pushUndoState(loadedPixmapItem->pixmap());
    beginAiResultLoad(chosen);
    exitRetouchMode();
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

    // The environment has to exist before checkGenerativeAccess(), which itself
    // runs flux_fill.py through the venv interpreter.
    if (!ensureAiEnvironment())
        return;

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
    QString outputPath = uniqueAiOutputPath("iqview_flux_out", "bmp",
                                            getCurrentFileDetails().fileInfo.absoluteFilePath());

    loadedPixmapItem->pixmap().save(inputPath);
    mask.save(maskPath);

    QApplication::setOverrideCursor(Qt::WaitCursor);

    const int batchCount = qBound(1, qvGetSettingInt(FluxBatchCount), 8);
    fluxBatchExpected = batchCount;
    fluxBatchResults.clear();

    QString cmd = QString("%1|%2|%3|%4|%5\n")
                          .arg(inputPath, maskPath, prompt, outputPath)
                          .arg(batchCount);
    activeAiJob = AiJob::Fill;
    fluxProcess->write(cmd.toUtf8());
}

bool QVGraphicsView::isMaskEmpty() const
{
    return maskImage.isNull() || !maskHasPaint;
}

void QVGraphicsView::showAiLogWindow()
{
    if (aiLogDialog) {
        aiLogDialog->show();
        aiLogDialog->raise();
        aiLogDialog->activateWindow();
        return;
    }

    const QString tempDir = QDir::tempPath();
    QList<AiLogDialog::LogSource> sources = {
        { tr("Flux / Isolate"), resolveLogPath() },
        { tr("LaMa Worker"), QDir(tempDir).filePath("iqview_worker_log.txt") },
        { tr("Retouch Session"), QDir(tempDir).filePath("iqview_retouch_log.txt") },
    };

    auto *dialog = new AiLogDialog(sources, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    aiLogDialog = dialog;
    dialog->show();
}

// ============================================================================
// Culling — star ratings and rejections, persisted as XMP sidecars
// ============================================================================

int QVGraphicsView::ratingForCurrentFile()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return RatingManager::Unrated;
    return ratingManager->rating(getCurrentFileDetails().fileInfo.absoluteFilePath());
}

void QVGraphicsView::setRatingForCurrentFile(int rating)
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    const QString path = getCurrentFileDetails().fileInfo.absoluteFilePath();

    // Pressing the rating a file already has clears it instead, which is how
    // every culling tool behaves and saves reaching for a separate key.
    if (rating > 0 && ratingManager->rating(path) == rating)
        rating = RatingManager::Unrated;

    if (!ratingManager->setRating(path, rating)) {
        showRatingFeedback(RatingManager::Unrated);
        QMessageBox::warning(this, tr("Rating"),
                             tr("Could not write the rating for this image.\n\n"
                                "iqView stores ratings in an XMP sidecar next to the image, "
                                "so the folder needs to be writable."));
        return;
    }
    showRatingFeedback(rating);
}

void QVGraphicsView::showRatingFeedback(int rating)
{
    if (!ratingLabel) {
        ratingLabel = new QLabel(this);
        ratingLabel->setAlignment(Qt::AlignCenter);
        ratingLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        ratingLabel->setStyleSheet(
            "QLabel {"
            "  background: rgba(20, 20, 20, 210);"
            "  color: #e0e0e0;"
            "  border: 1px solid rgba(120, 120, 120, 130);"
            "  border-radius: 8px;"
            "  padding: 6px 16px;"
            "  font-size: 18px;"
            "}");

        ratingLabelTimer = new QTimer(this);
        ratingLabelTimer->setSingleShot(true);
        ratingLabelTimer->setInterval(1200);
        connect(ratingLabelTimer, &QTimer::timeout, this, [this]() { ratingLabel->hide(); });
    }

    QString text;
    if (rating == RatingManager::Rejected)
        text = tr("✕  Rejected");
    else if (rating == RatingManager::Unrated)
        text = tr("Unrated");
    else
        text = QString("★").repeated(rating) + QString("☆").repeated(RatingManager::MaxStars - rating);

    ratingLabel->setText(text);
    ratingLabel->adjustSize();
    repositionRatingLabel();
    ratingLabel->show();
    ratingLabel->raise();
    ratingLabelTimer->start();
}

void QVGraphicsView::repositionRatingLabel()
{
    if (!ratingLabel || !ratingLabel->isVisible())
        return;
    ratingLabel->adjustSize();
    // Top centre, clear of the AI status HUD which lives along the bottom.
    ratingLabel->move((width() - ratingLabel->width()) / 2, 24);
}

// Copies everything in the current folder rated at or above a threshold into a
// folder of the user's choosing. The point of culling is getting the keepers
// somewhere else; without this the ratings would just sit there.
void QVGraphicsView::exportKeepers()
{
    const auto &files = getCurrentFileDetails().folderFileInfoList;
    if (files.isEmpty()) {
        QMessageBox::information(this, tr("Export Keepers"), tr("No folder is open."));
        return;
    }

    bool ok = false;
    const int threshold = QInputDialog::getInt(
            this, tr("Export Keepers"),
            tr("Copy images rated at least this many stars:"), 1, 1,
            RatingManager::MaxStars, 1, &ok);
    if (!ok)
        return;

    QStringList keepers;
    for (const auto &file : files) {
        if (ratingManager->rating(file.absoluteFilePath) >= threshold)
            keepers << file.absoluteFilePath;
    }

    if (keepers.isEmpty()) {
        QMessageBox::information(
                this, tr("Export Keepers"),
                tr("No images in this folder are rated %n star(s) or higher.", nullptr,
                   threshold));
        return;
    }

    const QString destination = QFileDialog::getExistingDirectory(
            this, tr("Choose a destination folder for %1 image(s)").arg(keepers.size()));
    if (destination.isEmpty())
        return;

    int copied = 0;
    QStringList failed;
    for (const QString &source : keepers) {
        const QFileInfo info(source);
        QString target = QDir(destination).filePath(info.fileName());

        // Never overwrite: a name collision gets a numeric suffix instead.
        int suffix = 1;
        while (QFile::exists(target)) {
            target = QDir(destination).filePath(QString("%1 (%2).%3")
                                                        .arg(info.completeBaseName())
                                                        .arg(suffix++)
                                                        .arg(info.suffix()));
        }

        if (QFile::copy(source, target)) {
            ++copied;
            // Bring the sidecar along so the ratings survive the move.
            const QString sidecar = RatingManager::sidecarPath(source);
            if (QFile::exists(sidecar)) {
                QFile::copy(sidecar,
                            RatingManager::sidecarPath(target));
            }
        } else {
            failed << info.fileName();
        }
    }

    if (failed.isEmpty()) {
        QMessageBox::information(
                this, tr("Export Keepers"),
                tr("Copied %1 image(s) to:\n%2")
                        .arg(copied)
                        .arg(QDir::toNativeSeparators(destination)));
    } else {
        QMessageBox::warning(this, tr("Export Keepers"),
                             tr("Copied %1 image(s), but %2 could not be copied:\n\n%3")
                                     .arg(copied)
                                     .arg(failed.size())
                                     .arg(failed.mid(0, 10).join("\n")));
    }
}

// Collects everything needed to diagnose an AI problem into one text file the
// user can attach to a bug report. Testers on macOS and Linux have no idea
// where these logs live, and collecting them one at a time over several
// round-trips is the slowest part of diagnosing anything remotely. Plain text
// rather than a zip so it can also just be pasted into an issue.
void QVGraphicsView::exportDebugReport()
{
    const QString suggested =
            QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
                    .filePath(QString("iqview-debug-%1.txt")
                                      .arg(QDateTime::currentDateTime().toString(
                                              "yyyyMMdd-HHmmss")));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Debug Report"), suggested,
                                                      tr("Text Files (*.txt)"));
    if (path.isEmpty())
        return;

    QString report;
    QTextStream out(&report);

    out << "iqView debug report\n"
        << "Generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n\n";

    out << "== Environment ==\n"
        << "iqView:      " << QCoreApplication::applicationVersion() << "\n"
        << "Qt:          " << qVersion() << " (built against " << QT_VERSION_STR << ")\n"
        << "OS:          " << QSysInfo::prettyProductName() << "\n"
        << "Kernel:      " << QSysInfo::kernelType() << " " << QSysInfo::kernelVersion() << "\n"
        << "CPU arch:    " << QSysInfo::currentCpuArchitecture() << "\n\n";

    out << "== Paths ==\n";
    const auto describe = [&](const QString &label, const QString &value) {
        out << label.leftJustified(13) << QDir::toNativeSeparators(value)
            << (QFileInfo::exists(value) ? "  [present]" : "  [MISSING]") << "\n";
    };
    describe("scripts:", resolveScriptsDir());
    describe("venv:", resolveVenvDir());
    describe("python:", resolvePythonExe());
    describe("uv:", resolveUvExe());
    describe("models:", resolveModelsDir());

    // Free space matters: a truncated download is a common failure mode and
    // is invisible in the logs themselves.
    const QStorageInfo storage(QFileInfo(resolveVenvDir()).absolutePath());
    if (storage.isValid() && storage.isReady()) {
        out << "free space:  " << storage.bytesAvailable() / 1048576 << " MB on "
            << QDir::toNativeSeparators(storage.rootPath()) << "\n";
    }

    QString stamp;
    QFile stampFile(resolveEnvStampPath());
    if (stampFile.open(QIODevice::ReadOnly | QIODevice::Text))
        stamp = QString::fromUtf8(stampFile.readAll()).trimmed();
    out << "env stamp:   " << (stamp.isEmpty() ? QStringLiteral("(none)") : stamp) << "\n"
        << "expected:    " << currentEnvStamp() << "\n\n";

    // Deliberately not included: the Hugging Face token. It lives in QSettings
    // right next to everything else here and would otherwise be pasted into a
    // public issue by anyone following the instructions.
    out << "HF token:    " << (qvGetSettingString(HFToken).isEmpty() ? "not set" : "set (value withheld)")
        << "\n\n";

    const QString tempDir = QDir::tempPath();
    const QList<QPair<QString, QString>> logs = {
        { "Flux / Isolate", resolveLogPath() },
        { "LaMa Worker", QDir(tempDir).filePath("iqview_worker_log.txt") },
        { "Retouch Session", QDir(tempDir).filePath("iqview_retouch_log.txt") },
    };

    // Tail only -- these files are append-only and can reach many MB, but the
    // interesting part is always the most recent run.
    constexpr qint64 LOG_TAIL_BYTES = 64 * 1024;
    for (const auto &entry : logs) {
        out << "== Log: " << entry.first << " ==\n"
            << QDir::toNativeSeparators(entry.second) << "\n";
        QFile file(entry.second);
        if (!file.exists()) {
            out << "(does not exist)\n\n";
            continue;
        }
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            out << "(could not be opened)\n\n";
            continue;
        }
        if (file.size() > LOG_TAIL_BYTES) {
            file.seek(file.size() - LOG_TAIL_BYTES);
            out << "(truncated to the last " << LOG_TAIL_BYTES / 1024 << " KB of "
                << file.size() / 1024 << " KB)\n";
        }
        out << QString::fromUtf8(file.readAll()) << "\n\n";
    }

    QFile outFile(path);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export Debug Report"),
                             tr("Could not write to %1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    outFile.write(report.toUtf8());
    outFile.close();

    QMessageBox::information(
            this, tr("Export Debug Report"),
            tr("Debug report saved to:\n%1\n\nAttach this file to your bug report. It contains "
               "no Hugging Face token or image data.")
                    .arg(QDir::toNativeSeparators(path)));
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
    connect(isolateProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (isolateState == IsolateState::Idle)
            return;
        hideAiStatus();
        isolateState = IsolateState::Idle;
        activeAiJob = AiJob::None;
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("Isolate Error"),
                              tr("The AI service failed to start: %1\n\n"
                                 "Check that the Python environment is set up (run Retouch once "
                                 "to install it).\n\nLog: %2")
                                  .arg(isolateProcess->errorString(), resolveLogPath()));
    });

    QStringList args = { resolveScriptsDir() + "/isolate.py" };

    // Pass the HF token via environment variable rather than the command line,
    // where it would be visible to any local process inspecting arguments.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONUNBUFFERED", "1");
    const QString token = qvGetSettingString(HFToken);
    if (!token.isEmpty())
        env.insert("HF_TOKEN", token);
    isolateProcess->setProcessEnvironment(env);

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
            activeAiJob = AiJob::None;
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

        } else if (line.startsWith("OUTPUT: ")
                   && isolateState == IsolateState::WaitingForResult) {
            hideAiStatus();
            // Load via imageCore (not setPixmap) so a later scaleExpensively()
            // doesn't restore the original image over the result.
            pushUndoState(loadedPixmapItem->pixmap());
            beginAiResultLoad(line.mid(8).trimmed());
            isolateState = IsolateState::Idle;
            activeAiJob = AiJob::None;
            QApplication::restoreOverrideCursor();

        } else if (line.startsWith("ERROR:") || line.startsWith("FATAL:")) {
            hideAiStatus();
            isolateState = IsolateState::Idle;
            activeAiJob = AiJob::None;
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

    if (!ensureAiEnvironment())
        return;

    // Prompt for HF token upfront if none stored (SAM 3 is gated)
    if (qvGetSettingString(HFToken).isEmpty()) {
        if (!checkGenerativeAccess()) return;
    }

    QString tempDir  = QDir::tempPath();
    isolateInputPath = QDir(tempDir).filePath("iqview_isolate_in.png");
    QString outputPath = uniqueAiOutputPath(
            "iqview_isolate_out", "png", getCurrentFileDetails().fileInfo.absoluteFilePath());

    // Save the currently displayed image
    loadedPixmapItem->pixmap().save(isolateInputPath);

    ensureIsolateStarted();

    isolateState = IsolateState::WaitingForResult;
    activeAiJob = AiJob::Isolate;
    showAiStatus(tr("Removing background with SAM 3..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);

    QString cmd = QString("REMOVE_BG|%1|%2\n").arg(isolateInputPath, outputPath);
    isolateProcess->write(cmd.toUtf8());
}

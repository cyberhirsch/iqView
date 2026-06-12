#include "isolatedialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QMouseEvent>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QDialogButtonBox>

// ============================================================================
// SegmentView
// ============================================================================

SegmentView::SegmentView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(320, 240);
}

void SegmentView::setImages(const QImage &viz, const QImage &idMap, int segmentCount)
{
    m_viz          = viz.convertToFormat(QImage::Format_RGB32);
    m_idMap        = idMap.convertToFormat(QImage::Format_Grayscale8);
    m_segmentCount = segmentCount;

    // Start with all segments selected
    m_selected.clear();
    for (int i = 1; i <= m_segmentCount; ++i)
        m_selected.insert(i);

    rebuildDisplay();
    emit selectionChanged(m_selected.size(), m_segmentCount);
}

void SegmentView::selectAll()
{
    m_selected.clear();
    for (int i = 1; i <= m_segmentCount; ++i)
        m_selected.insert(i);
    rebuildDisplay();
    emit selectionChanged(m_selected.size(), m_segmentCount);
}

void SegmentView::deselectAll()
{
    m_selected.clear();
    rebuildDisplay();
    emit selectionChanged(0, m_segmentCount);
}

void SegmentView::invertSelection()
{
    QSet<int> inv;
    for (int i = 1; i <= m_segmentCount; ++i) {
        if (!m_selected.contains(i))
            inv.insert(i);
    }
    m_selected = inv;
    rebuildDisplay();
    emit selectionChanged(m_selected.size(), m_segmentCount);
}

// Rebuild m_display from m_viz by darkening unselected segments in-place.
// All operations run directly on scanline data for speed.
void SegmentView::rebuildDisplay()
{
    if (m_viz.isNull() || m_idMap.isNull()) return;

    m_display = m_viz.copy();  // Format_RGB32

    const int h = m_display.height();
    const int w = m_display.width();

    for (int y = 0; y < h; ++y) {
        QRgb       *dst = reinterpret_cast<QRgb *>(m_display.scanLine(y));
        const uchar *id = m_idMap.constScanLine(y);

        for (int x = 0; x < w; ++x) {
            int seg = id[x];
            if (seg > 0 && !m_selected.contains(seg)) {
                // Heavily darken unselected segments (keep ~15% brightness)
                QRgb px = dst[x];
                dst[x] = qRgb(qRed(px) * 15 / 100,
                               qGreen(px) * 15 / 100,
                               qBlue(px) * 15 / 100);
            }
        }
    }

    update();
}

QPoint SegmentView::imagePos(const QPoint &widgetPos) const
{
    if (m_idMap.isNull()) return QPoint(-1, -1);

    QSize scaled = m_idMap.size().scaled(size(), Qt::KeepAspectRatio);
    int ox = (width()  - scaled.width())  / 2;
    int oy = (height() - scaled.height()) / 2;

    float sx = float(m_idMap.width())  / scaled.width();
    float sy = float(m_idMap.height()) / scaled.height();

    int ix = int((widgetPos.x() - ox) * sx);
    int iy = int((widgetPos.y() - oy) * sy);

    if (ix < 0 || iy < 0 || ix >= m_idMap.width() || iy >= m_idMap.height())
        return QPoint(-1, -1);

    return QPoint(ix, iy);
}

void SegmentView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_idMap.isNull()) return;

    QPoint ip = imagePos(event->pos());
    if (ip.x() < 0) return;

    int seg = m_idMap.constScanLine(ip.y())[ip.x()];
    if (seg == 0) return;  // clicked background

    if (m_selected.contains(seg))
        m_selected.remove(seg);
    else
        m_selected.insert(seg);

    rebuildDisplay();
    emit selectionChanged(m_selected.size(), m_segmentCount);
}

void SegmentView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 30));

    if (m_display.isNull()) return;

    QSize scaled = m_display.size().scaled(size(), Qt::KeepAspectRatio);
    QRect dr((width()  - scaled.width())  / 2,
             (height() - scaled.height()) / 2,
             scaled.width(), scaled.height());
    p.drawImage(dr, m_display);
}

QSize SegmentView::sizeHint() const
{
    if (!m_viz.isNull()) {
        QSize s = m_viz.size();
        s.scale(900, 650, Qt::KeepAspectRatio);
        return s;
    }
    return QSize(640, 480);
}


// ============================================================================
// IsolateDialog
// ============================================================================

IsolateDialog::IsolateDialog(const QString &vizPath,
                             const QString &idMapPath,
                             int            segmentCount,
                             QWidget       *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Isolate — select segments to keep"));
    setModal(true);

    auto *root = new QVBoxLayout(this);
    root->setSpacing(8);
    root->setContentsMargins(10, 10, 10, 10);

    // Instruction banner
    auto *hint = new QLabel(
        tr("Click segments to toggle selection. Selected segments will be kept; "
           "everything else becomes transparent."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #aaa; font-size: 12px;");
    root->addWidget(hint);

    // Segment view (main content)
    m_view = new SegmentView(this);
    root->addWidget(m_view, 1);

    // Status line
    m_status = new QLabel(this);
    m_status->setAlignment(Qt::AlignCenter);
    root->addWidget(m_status);

    // Button row
    auto *btnRow = new QHBoxLayout;

    auto *allBtn  = new QPushButton(tr("Select All"),  this);
    auto *noneBtn = new QPushButton(tr("Deselect All"), this);
    auto *invBtn  = new QPushButton(tr("Invert"),       this);
    connect(allBtn,  &QPushButton::clicked, m_view, &SegmentView::selectAll);
    connect(noneBtn, &QPushButton::clicked, m_view, &SegmentView::deselectAll);
    connect(invBtn,  &QPushButton::clicked, m_view, &SegmentView::invertSelection);

    btnRow->addWidget(allBtn);
    btnRow->addWidget(noneBtn);
    btnRow->addWidget(invBtn);
    btnRow->addStretch();

    m_okBtn = new QPushButton(tr("Apply"), this);
    m_okBtn->setDefault(true);
    auto *cancelBtn = new QPushButton(tr("Cancel"), this);
    connect(m_okBtn,    &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(m_okBtn);
    root->addLayout(btnRow);

    // Wire selection changes
    connect(m_view, &SegmentView::selectionChanged,
            this,   &IsolateDialog::onSelectionChanged);

    // Load images
    QImage viz(vizPath);
    QImage idMap(idMapPath);
    m_view->setImages(viz, idMap, segmentCount);

    // Sensible initial size
    adjustSize();
}

QSet<int> IsolateDialog::selectedSegments() const
{
    // Convert from 1-based (internal) to 0-based (Python worker expects 0-based index)
    QSet<int> result;
    for (int id : m_view->selectedSegments())
        result.insert(id - 1);
    return result;
}

void IsolateDialog::onSelectionChanged(int selected, int total)
{
    m_status->setText(tr("%1 of %2 segments selected").arg(selected).arg(total));
    m_okBtn->setEnabled(selected > 0);
}

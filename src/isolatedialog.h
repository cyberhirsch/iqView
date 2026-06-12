#ifndef ISOLATEDIALOG_H
#define ISOLATEDIALOG_H

#include <QDialog>
#include <QImage>
#include <QSet>
#include <QWidget>
#include <QLabel>
#include <QPushButton>

// ---------------------------------------------------------------------------
// SegmentView — custom widget that displays the colorized segment map and
// lets the user toggle segment selection by clicking on them.
// ---------------------------------------------------------------------------
class SegmentView : public QWidget
{
    Q_OBJECT

public:
    explicit SegmentView(QWidget *parent = nullptr);

    void setImages(const QImage &viz, const QImage &idMap, int segmentCount);

    QSet<int> selectedSegments() const { return m_selected; }
    int       segmentCount()     const { return m_segmentCount; }

    void selectAll();
    void deselectAll();
    void invertSelection();

signals:
    void selectionChanged(int selectedCount, int totalCount);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    QSize sizeHint() const override;

private:
    void rebuildDisplay();

    // Map a widget-space click position to image-space pixel coordinates.
    QPoint imagePos(const QPoint &widgetPos) const;

    QImage   m_viz;           // colorized segment visualization (RGB)
    QImage   m_idMap;         // 8-bit grayscale: pixel value = segment index (1-N)
    QImage   m_display;       // current blended display image, rebuilt on each toggle
    QSet<int> m_selected;     // segment indices that are currently selected (1-N)
    int      m_segmentCount = 0;
};


// ---------------------------------------------------------------------------
// IsolateDialog — modal dialog wrapping SegmentView with action buttons.
// ---------------------------------------------------------------------------
class IsolateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit IsolateDialog(const QString &vizPath,
                           const QString &idMapPath,
                           int            segmentCount,
                           QWidget       *parent = nullptr);

    // Returns selected segment indices (0-based, as expected by IsolateWorker).
    QSet<int> selectedSegments() const;

private:
    SegmentView *m_view    = nullptr;
    QLabel      *m_status  = nullptr;
    QPushButton *m_okBtn   = nullptr;

    void onSelectionChanged(int selected, int total);
};

#endif // ISOLATEDIALOG_H

#include "ailogdialog.h"

#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

// Only the tail of large logs is shown; enough for any single AI session.
static constexpr qint64 TAIL_BYTES = 256 * 1024;

AiLogDialog::AiLogDialog(const QList<LogSource> &sources, QWidget *parent)
    : QDialog(parent), sources(sources)
{
    setWindowTitle(tr("AI Debug Log"));
    setWindowFlag(Qt::WindowMaximizeButtonHint);
    resize(820, 520);

    selector = new QComboBox(this);
    for (const auto &source : sources)
        selector->addItem(source.name);

    auto *clearButton = new QPushButton(tr("Clear Log File"), this);

    auto *topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel(tr("Log:"), this));
    topRow->addWidget(selector, 1);
    topRow->addWidget(clearButton);

    view = new QPlainTextEdit(this);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    view->setFont(mono);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(topRow);
    layout->addWidget(view);

    timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &AiLogDialog::refresh);
    timer->start();

    connect(selector, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        lastSize = -1;
        refresh();
    });
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        const int index = selector->currentIndex();
        if (index < 0 || index >= this->sources.size())
            return;
        QFile file(this->sources[index].path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.close();
        lastSize = -1;
        refresh();
    });

    refresh();
}

void AiLogDialog::refresh()
{
    if (!isVisible())
        return;

    const int index = selector->currentIndex();
    if (index < 0 || index >= sources.size())
        return;

    const QString path = sources[index].path;
    QFileInfo info(path);
    if (!info.exists()) {
        if (lastSize != 0) {
            lastSize = 0;
            view->setPlainText(tr("(log file does not exist yet: %1)").arg(path));
        }
        return;
    }

    // Skip re-reading if the file hasn't grown or shrunk
    if (info.size() == lastSize)
        return;
    lastSize = info.size();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    if (file.size() > TAIL_BYTES)
        file.seek(file.size() - TAIL_BYTES);
    const QString text = QString::fromUtf8(file.readAll());

    // Keep the view pinned to the bottom unless the user has scrolled up
    QScrollBar *scrollBar = view->verticalScrollBar();
    const bool wasAtBottom = scrollBar->value() >= scrollBar->maximum() - 4;
    view->setPlainText(text);
    if (wasAtBottom)
        scrollBar->setValue(scrollBar->maximum());
}

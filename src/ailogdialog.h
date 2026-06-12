#ifndef AILOGDIALOG_H
#define AILOGDIALOG_H

#include <QDialog>
#include <QList>

class QComboBox;
class QPlainTextEdit;
class QTimer;

// AiLogDialog — non-modal live viewer for the AI worker log files.
// Tails the selected log every 500 ms so output appears while a
// generation is running.
class AiLogDialog : public QDialog
{
    Q_OBJECT

public:
    struct LogSource
    {
        QString name;
        QString path;
    };

    explicit AiLogDialog(const QList<LogSource> &sources, QWidget *parent = nullptr);

private slots:
    void refresh();

private:
    QList<LogSource> sources;
    QComboBox *selector;
    QPlainTextEdit *view;
    QTimer *timer;
    qint64 lastSize = -1;
};

#endif // AILOGDIALOG_H

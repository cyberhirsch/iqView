#ifndef HFAUTHDIALOG_H
#define HFAUTHDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QDesktopServices>
#include <QUrl>

class HFAuthDialog : public QDialog {
    Q_OBJECT
public:
    explicit HFAuthDialog(const QString &modelId, QWidget *parent = nullptr) : QDialog(parent) {
        setWindowTitle(tr("Hugging Face Access Required"));
        setMinimumWidth(400);

        auto *layout = new QVBoxLayout(this);

        auto *title = new QLabel(tr("<h3>Flux Access Required</h3>"), this);
        layout->addWidget(title);

        auto *desc = new QLabel(tr("The model <b>%1</b> requires you to agree to its terms on Hugging Face.").arg(modelId), this);
        desc->setWordWrap(true);
        layout->addWidget(desc);

        auto *agreeBtn = new QPushButton(tr("1. Open Hugging Face to Agree to Terms"), this);
        connect(agreeBtn, &QPushButton::clicked, [modelId]() {
            QDesktopServices::openUrl(QUrl(QString("https://huggingface.co/%1").arg(modelId)));
        });
        layout->addWidget(agreeBtn);

        layout->addSpacing(10);

        auto *tokenLabel = new QLabel(tr("2. Paste your Hugging Face Access Token:"), this);
        layout->addWidget(tokenLabel);

        tokenEdit = new QLineEdit(this);
        tokenEdit->setEchoMode(QLineEdit::Password);
        tokenEdit->setPlaceholderText(tr("hf_..."));
        layout->addWidget(tokenEdit);

        auto *tokenHelp = new QLabel(tr("<set small><a href='https://huggingface.co/settings/tokens'>Where do I find my token?</a></set>"), this);
        tokenHelp->setOpenExternalLinks(true);
        layout->addWidget(tokenHelp);

        layout->addSpacing(20);

        auto *confirmBtn = new QPushButton(tr("Verify & Continue"), this);
        confirmBtn->setDefault(true);
        connect(confirmBtn, &QPushButton::clicked, this, &QDialog::accept);
        layout->addWidget(confirmBtn);

        auto *cancelBtn = new QPushButton(tr("Cancel"), this);
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        layout->addWidget(cancelBtn);
    }

    QString getToken() const { return tokenEdit->text(); }

private:
    QLineEdit *tokenEdit;
};

#endif // HFAUTHDIALOG_H

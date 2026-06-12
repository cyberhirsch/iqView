#ifndef HFAUTHDIALOG_H
#define HFAUTHDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDesktopServices>
#include <QUrl>
#include <QFrame>

class HFAuthDialog : public QDialog {
    Q_OBJECT
public:
    explicit HFAuthDialog(const QString &modelId,
                          const QString &existingToken = QString(),
                          const QString &errorMessage = QString(),
                          QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(tr("AI Setup — Generate"));
        setMinimumWidth(460);
        setWindowFlag(Qt::WindowStaysOnTopHint);

        auto *layout = new QVBoxLayout(this);
        layout->setSpacing(12);
        layout->setContentsMargins(20, 20, 20, 20);

        // Header
        auto *title = new QLabel(tr("<b>One-time setup required</b>"), this);
        title->setStyleSheet("font-size: 14px;");
        layout->addWidget(title);

        auto *modelInfo = new QLabel(
            tr("The <b>Generate</b> feature uses <b>FLUX.2 Klein</b>, a 9B AI model "
               "hosted on Hugging Face. It requires a free account and a one-time "
               "agreement to the model's terms of use."), this);
        modelInfo->setWordWrap(true);
        modelInfo->setStyleSheet("color: palette(mid); font-size: 12px;");
        layout->addWidget(modelInfo);

        // Show inline error if token was rejected
        if (!errorMessage.isEmpty()) {
            auto *errorFrame = new QFrame(this);
            errorFrame->setStyleSheet(
                "QFrame { background: #3c1f1f; border: 1px solid #7a3030; border-radius: 4px; }");
            auto *errorLayout = new QHBoxLayout(errorFrame);
            auto *errorLabel = new QLabel("⚠  " + errorMessage, errorFrame);
            errorLabel->setWordWrap(true);
            errorLabel->setStyleSheet("color: #ff8080; font-size: 12px;");
            errorLayout->addWidget(errorLabel);
            layout->addWidget(errorFrame);
        }

        auto *sep1 = new QFrame(this);
        sep1->setFrameShape(QFrame::HLine);
        sep1->setFrameShadow(QFrame::Sunken);
        layout->addWidget(sep1);

        // Step 1
        auto *step1Label = new QLabel(tr("<b>Step 1</b> — Create a free Hugging Face account"), this);
        layout->addWidget(step1Label);
        auto *step1Btn = new QPushButton(tr("Open huggingface.co →"), this);
        step1Btn->setAutoDefault(false);
        step1Btn->setStyleSheet("text-align: left; padding: 4px 8px;");
        connect(step1Btn, &QPushButton::clicked, this, [this]() {
            QDesktopServices::openUrl(QUrl("https://huggingface.co/join"));
            raise(); activateWindow();
        });
        layout->addWidget(step1Btn);

        // Step 2
        auto *step2Label = new QLabel(tr("<b>Step 2</b> — Agree to the model's terms of use"), this);
        layout->addWidget(step2Label);
        auto *step2Btn = new QPushButton(tr("Open model page and click \"Agree\" →"), this);
        step2Btn->setAutoDefault(false);
        step2Btn->setStyleSheet("text-align: left; padding: 4px 8px;");
        connect(step2Btn, &QPushButton::clicked, this, [this, modelId]() {
            QDesktopServices::openUrl(QUrl(QString("https://huggingface.co/%1").arg(modelId)));
            raise(); activateWindow();
        });
        layout->addWidget(step2Btn);

        // Step 3
        auto *step3Label = new QLabel(tr("<b>Step 3</b> — Create a Read access token"), this);
        layout->addWidget(step3Label);
        auto *step3Btn = new QPushButton(tr("Open huggingface.co/settings/tokens →"), this);
        step3Btn->setAutoDefault(false);
        step3Btn->setStyleSheet("text-align: left; padding: 4px 8px;");
        connect(step3Btn, &QPushButton::clicked, this, [this]() {
            QDesktopServices::openUrl(QUrl("https://huggingface.co/settings/tokens"));
            raise(); activateWindow();
        });
        layout->addWidget(step3Btn);

        auto *sep2 = new QFrame(this);
        sep2->setFrameShape(QFrame::HLine);
        sep2->setFrameShadow(QFrame::Sunken);
        layout->addWidget(sep2);

        // Step 4 — token input
        auto *step4Label = new QLabel(tr("<b>Step 4</b> — Paste your token here:"), this);
        layout->addWidget(step4Label);

        auto *tokenRow = new QHBoxLayout();
        tokenEdit = new QLineEdit(this);
        tokenEdit->setEchoMode(QLineEdit::Password);
        tokenEdit->setPlaceholderText(tr("hf_..."));
        if (!existingToken.isEmpty())
            tokenEdit->setText(existingToken);
        tokenRow->addWidget(tokenEdit, 1);

        showBtn = new QPushButton(tr("Show"), this);
        showBtn->setAutoDefault(false);
        showBtn->setCheckable(true);
        showBtn->setFixedWidth(55);
        connect(showBtn, &QPushButton::toggled, this, [this](bool checked) {
            tokenEdit->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
            showBtn->setText(checked ? tr("Hide") : tr("Show"));
        });
        tokenRow->addWidget(showBtn);
        layout->addLayout(tokenRow);

        auto *tokenHint = new QLabel(
            tr("<small>Tokens start with <tt>hf_</tt> and are about 40 characters long. "
               "A <b>Read</b> role is sufficient.</small>"), this);
        tokenHint->setWordWrap(true);
        layout->addWidget(tokenHint);

        layout->addSpacing(8);

        // Buttons
        auto *btnRow = new QHBoxLayout();
        auto *cancelBtn = new QPushButton(tr("Cancel"), this);
        cancelBtn->setAutoDefault(false);
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        btnRow->addWidget(cancelBtn);
        btnRow->addStretch();

        confirmBtn = new QPushButton(tr("Verify & Enable Generate"), this);
        confirmBtn->setDefault(true);
        confirmBtn->setEnabled(!existingToken.isEmpty());
        connect(confirmBtn, &QPushButton::clicked, this, [this]() {
            const QString t = tokenEdit->text().trimmed();
            if (t.isEmpty() || !t.startsWith("hf_")) {
                tokenEdit->setStyleSheet("border: 1px solid red;");
                tokenEdit->setPlaceholderText(tr("Must start with hf_"));
                tokenEdit->setFocus();
                return;
            }
            tokenEdit->setStyleSheet({});
            accept();
        });
        btnRow->addWidget(confirmBtn);
        layout->addLayout(btnRow);

        // Enable confirm once the user types anything
        connect(tokenEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
            tokenEdit->setStyleSheet({});
            confirmBtn->setEnabled(!text.trimmed().isEmpty());
        });
    }

    QString getToken() const { return tokenEdit->text().trimmed(); }

private:
    QLineEdit *tokenEdit;
    QPushButton *showBtn;
    QPushButton *confirmBtn;
};

#endif // HFAUTHDIALOG_H

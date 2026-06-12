#ifndef RETOUCHPROMPTBAR_H
#define RETOUCHPROMPTBAR_H

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QGraphicsDropShadowEffect>
#include <QEvent>

class RetouchPromptBar : public QWidget {
    Q_OBJECT
public:
    explicit RetouchPromptBar(QWidget *parent = nullptr) : QWidget(parent) {
        setContentsMargins(10, 10, 10, 10);
        
        auto *layout = new QHBoxLayout(this);
        
        promptEdit = new QLineEdit(this);
        promptEdit->setPlaceholderText(tr("Describe what to generate in the masked area..."));
        promptEdit->setStyleSheet(
            "QLineEdit { "
            "  background: rgba(30, 30, 30, 200); "
            "  color: white; "
            "  border: 1px solid rgba(100, 100, 100, 150); "
            "  border-radius: 8px; "
            "  padding: 8px 12px; "
            "  font-size: 14px; "
            "} "
            "QLineEdit:focus { "
            "  border: 1px solid #0078d4; "
            "  background: rgba(40, 40, 40, 230); "
            "}"
        );
        layout->addWidget(promptEdit, 1);
        
        generateBtn = new QPushButton(tr("Creative Fill"), this);
        generateBtn->setStyleSheet(
            "QPushButton { "
            "  background: #0078d4; "
            "  color: white; "
            "  border: none; "
            "  border-radius: 8px; "
            "  padding: 8px 20px; "
            "  font-weight: bold; "
            "} "
            "QPushButton:hover { "
            "  background: #0086f0; "
            "} "
            "QPushButton:pressed { "
            "  background: #005a9e; "
            "}"
        );
        layout->addWidget(generateBtn);
        
        // Premium glassmorphism shadow
        auto *shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(20);
        shadow->setColor(QColor(0, 0, 0, 150));
        shadow->setOffset(0, 4);
        setGraphicsEffect(shadow);
        
        connect(generateBtn, &QPushButton::clicked, this, &RetouchPromptBar::generateRequested);
        connect(promptEdit, &QLineEdit::returnPressed, this, &RetouchPromptBar::generateRequested);
    }
    
    QString prompt() const { return promptEdit->text(); }
    void clear() { promptEdit->clear(); }
    void setFocusToPrompt() { promptEdit->setFocus(Qt::OtherFocusReason); }

protected:
    bool event(QEvent *e) override {
        // Absorb ShortcutOverride so window-level shortcuts don't fire while typing
        if (e->type() == QEvent::ShortcutOverride && promptEdit->hasFocus()) {
            e->accept();
            return true;
        }
        return QWidget::event(e);
    }

signals:
    void generateRequested();

private:
    QLineEdit *promptEdit;
    QPushButton *generateBtn;
};

#endif // RETOUCHPROMPTBAR_H

#include "TriggerEditDialog.h"

#include "wt/session/Theme.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

using wt::session::TriggerAction;
using wt::session::TriggerRule;

namespace {

QHBoxLayout* colorRow(QLineEdit* edit, QWidget* parent) {
    auto* row = new QHBoxLayout;
    auto* pick = new QPushButton(QObject::tr("Choose..."), parent);
    QObject::connect(pick, &QPushButton::clicked, parent, [edit, parent] {
        const QColor initial(edit->text().isEmpty() ? QStringLiteral("#ffffff") : edit->text());
        const QColor color = QColorDialog::getColor(initial, parent);
        if (color.isValid()) {
            edit->setText(color.name());
        }
    });
    row->addWidget(edit, 1);
    row->addWidget(pick);
    return row;
}

} // namespace

TriggerEditDialog::TriggerEditDialog(const TriggerRule& rule, QWidget* parent)
    : QDialog(parent),
      m_original(rule),
      m_name(new QLineEdit(QString::fromStdString(rule.name), this)),
      m_pattern(new QLineEdit(QString::fromStdString(rule.pattern), this)),
      m_caseSensitive(new QCheckBox(tr("Case sensitive"), this)),
      m_enabled(new QCheckBox(tr("Enabled"), this)),
      m_action(new QComboBox(this)),
      m_foreground(new QLineEdit(QString::fromStdString(rule.foreground), this)),
      m_background(new QLineEdit(QString::fromStdString(rule.background), this)),
      m_text(new QLineEdit(QString::fromStdString(rule.text), this)) {
    setWindowTitle(tr("Trigger"));
    m_name->setObjectName(QStringLiteral("triggerName"));
    m_pattern->setObjectName(QStringLiteral("triggerPattern"));
    m_pattern->setPlaceholderText(tr("Regular expression, e.g. error|failed"));
    m_action->setObjectName(QStringLiteral("triggerAction"));
    for (const TriggerAction action :
         {TriggerAction::Highlight, TriggerAction::Notify, TriggerAction::Beep, TriggerAction::Send}) {
        m_action->addItem(actionName(action), static_cast<int>(action));
    }
    m_action->setCurrentIndex(std::max(0, m_action->findData(static_cast<int>(rule.action))));
    m_caseSensitive->setChecked(rule.caseSensitive);
    m_enabled->setChecked(rule.enabled);
    m_foreground->setObjectName(QStringLiteral("triggerForeground"));
    m_foreground->setPlaceholderText(QStringLiteral("#rrggbb"));
    m_background->setObjectName(QStringLiteral("triggerBackground"));
    m_background->setPlaceholderText(QStringLiteral("#rrggbb"));
    m_text->setObjectName(QStringLiteral("triggerText"));

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Pattern:"), m_pattern);
    form->addRow(QString(), m_caseSensitive);
    form->addRow(tr("Action:"), m_action);
    form->addRow(tr("Text colour:"), colorRow(m_foreground, this));
    form->addRow(tr("Background:"), colorRow(m_background, this));
    form->addRow(tr("Text:"), m_text);
    form->addRow(QString(), m_enabled);
    layout->addLayout(form);
    auto* hint = new QLabel(this);
    hint->setObjectName(QStringLiteral("triggerHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &TriggerEditDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &TriggerEditDialog::reject);
    layout->addWidget(buttons);
    connect(m_action, &QComboBox::currentIndexChanged, this, &TriggerEditDialog::updateVisibility);
    updateVisibility();
    resize(520, 0);
}

QString TriggerEditDialog::actionName(TriggerAction action) {
    switch (action) {
    case TriggerAction::Highlight: return tr("Highlight");
    case TriggerAction::Notify: return tr("Notify");
    case TriggerAction::Beep: return tr("Beep");
    case TriggerAction::Send: return tr("Send text");
    }
    return {};
}

void TriggerEditDialog::updateVisibility() {
    const auto action = static_cast<TriggerAction>(m_action->currentData().toInt());
    m_foreground->setEnabled(action == TriggerAction::Highlight);
    m_background->setEnabled(action == TriggerAction::Highlight);
    m_text->setEnabled(action == TriggerAction::Send || action == TriggerAction::Notify);
    QString hint;
    switch (action) {
    case TriggerAction::Highlight: hint = tr("Matches are drawn in the colours above."); break;
    case TriggerAction::Notify: hint = tr("Shows the text (or the matching line) and marks the tab."); break;
    case TriggerAction::Beep: hint = tr("Plays the system beep."); break;
    case TriggerAction::Send:
        hint = tr("Sends the text to the session, at most once per line (\\r = Enter, \\n, \\t, \\e = Esc). "
                  "Prompts match before Enter is pressed.");
        break;
    }
    if (auto* label = findChild<QLabel*>(QStringLiteral("triggerHint"))) {
        label->setText(hint);
    }
}

TriggerRule TriggerEditDialog::rule() const {
    TriggerRule rule = m_original;
    rule.name = m_name->text().trimmed().toStdString();
    rule.pattern = m_pattern->text().toStdString();
    rule.caseSensitive = m_caseSensitive->isChecked();
    rule.enabled = m_enabled->isChecked();
    rule.action = static_cast<TriggerAction>(m_action->currentData().toInt());
    rule.foreground = m_foreground->text().trimmed().toStdString();
    rule.background = m_background->text().trimmed().toStdString();
    rule.text = m_text->text().toStdString();
    if (rule.name.empty()) {
        rule.name = rule.pattern;
    }
    return rule;
}

QString TriggerEditDialog::problem() const {
    const TriggerRule r = rule();
    if (r.pattern.empty()) {
        return tr("Enter a pattern.");
    }
    const QRegularExpression regex(QString::fromStdString(r.pattern));
    if (!regex.isValid()) {
        return tr("The pattern is not a valid regular expression: %1").arg(regex.errorString());
    }
    for (const std::string& color : {r.foreground, r.background}) {
        if (!color.empty() && !wt::session::parseHexColor(color)) {
            return tr("Colours are written as #rrggbb.");
        }
    }
    if (r.action == TriggerAction::Send && r.text.empty()) {
        return tr("Enter the text to send.");
    }
    return {};
}

void TriggerEditDialog::accept() {
    if (const QString message = problem(); !message.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), message);
        return;
    }
    QDialog::accept();
}

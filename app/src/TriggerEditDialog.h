#pragma once

#include "wt/session/Settings.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;

// Edits one trigger rule (pattern, action and its parameters).
class TriggerEditDialog : public QDialog {
    Q_OBJECT

public:
    explicit TriggerEditDialog(const wt::session::TriggerRule& rule, QWidget* parent = nullptr);

    wt::session::TriggerRule rule() const;
    // Empty when the rule is usable (the pattern compiles, colours parse).
    QString problem() const;
    void accept() override;

    static QString actionName(wt::session::TriggerAction action);

private:
    void updateVisibility();

    wt::session::TriggerRule m_original;
    QLineEdit* m_name;
    QLineEdit* m_pattern;
    QCheckBox* m_caseSensitive;
    QCheckBox* m_enabled;
    QComboBox* m_action;
    QLineEdit* m_foreground;
    QLineEdit* m_background;
    QLineEdit* m_text;
};

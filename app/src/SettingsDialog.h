#pragma once

#include "wt/session/Settings.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;

// Edits the application settings: appearance, terminal behaviour, key bindings and SSH options.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const wt::session::Settings& settings, QWidget* parent = nullptr);

    // The edited settings (valid after the dialog was accepted, or any time for tests).
    wt::session::Settings settings() const;
    // Key binding problems (unparsable sequences, conflicts); empty when all is well.
    QStringList keymapProblems() const;

    void accept() override;

private:
    QWidget* createAppearancePage();
    QWidget* createTerminalPage();
    QWidget* createKeyboardPage();
    QWidget* createSshPage();
    void loadKeymap(const wt::session::Keymap& keymap);
    wt::session::Keymap editedKeymap(QStringList* problems) const;
    void updateKeymapStatus();

    wt::session::Settings m_original;
    QCheckBox* m_systemFont = nullptr;
    QFontComboBox* m_font = nullptr;
    QDoubleSpinBox* m_fontSize = nullptr;
    QComboBox* m_theme = nullptr;
    QComboBox* m_cursorStyle = nullptr;
    QCheckBox* m_cursorBlink = nullptr;
    QSpinBox* m_scrollback = nullptr;
    QComboBox* m_bell = nullptr;
    QCheckBox* m_copyOnSelect = nullptr;
    QCheckBox* m_clipboardWrite = nullptr;
    QCheckBox* m_optionAsMeta = nullptr;
    QCheckBox* m_confirmClose = nullptr;
    QTableWidget* m_keys = nullptr;
    QLabel* m_keyStatus = nullptr;
    QLineEdit* m_knownHosts = nullptr;
};

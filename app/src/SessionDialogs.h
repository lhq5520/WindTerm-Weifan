#pragma once

#include "wt/session/SessionProfile.h"

#include <QDialog>
#include <QPointer>

class AppContext;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

// Edits one session profile (local or SSH, including proxy/jump host and reconnect options).
class SessionEditDialog : public QDialog {
    Q_OBJECT

public:
    SessionEditDialog(AppContext* context, const wt::session::SessionProfile& profile, QWidget* parent = nullptr);

    wt::session::SessionProfile profile() const;
    // For new sessions: whether the user asked to keep it in the session list.
    bool saveRequested() const;
    // Secret typed into the password/passphrase field (empty if none) and whether to keep it in
    // the credential vault.
    QString secret() const;
    bool storeSecret() const;
    QStringList problems() const;

    void accept() override;

    // Repopulates the serial device list (keeps the current text).
    void refreshSerialPorts();
    // "New SSH Session", "New Serial Session", ...
    static QString titleForNew(wt::session::SessionKind kind);

private:
    QWidget* createGeneralPage();
    QWidget* createSshPage();
    QWidget* createProxyPage();
    QWidget* createLocalPage();
    QWidget* createTelnetPage();
    QWidget* createSerialPage();
    void updateVisibility();

    AppContext* m_context; // outlives the (modal) dialog
    wt::session::SessionProfile m_original;
    QLineEdit* m_name = nullptr;
    QComboBox* m_group = nullptr;
    QComboBox* m_kind = nullptr;
    QComboBox* m_theme = nullptr;
    QPlainTextEdit* m_notes = nullptr;
    QStackedWidget* m_kindPages = nullptr;
    // local
    QLineEdit* m_program = nullptr;
    QLineEdit* m_arguments = nullptr;
    QLineEdit* m_workingDirectory = nullptr;
    QCheckBox* m_loginShell = nullptr;
    // ssh
    QLineEdit* m_host = nullptr;
    QSpinBox* m_port = nullptr;
    QLineEdit* m_user = nullptr;
    QComboBox* m_auth = nullptr;
    QLineEdit* m_identity = nullptr;
    QLineEdit* m_secret = nullptr;
    QCheckBox* m_storeSecret = nullptr;
    QSpinBox* m_keepAlive = nullptr;
    QSpinBox* m_timeout = nullptr;
    QLineEdit* m_command = nullptr;
    // telnet
    QLineEdit* m_telnetHost = nullptr;
    QSpinBox* m_telnetPort = nullptr;
    QComboBox* m_terminalType = nullptr;
    QComboBox* m_telnetEcho = nullptr;
    QCheckBox* m_telnetCrlf = nullptr;
    QSpinBox* m_telnetTimeout = nullptr;
    // serial
    QComboBox* m_serialDevice = nullptr;
    QComboBox* m_baudRate = nullptr;
    QComboBox* m_dataBits = nullptr;
    QComboBox* m_parity = nullptr;
    QComboBox* m_stopBits = nullptr;
    QComboBox* m_flowControl = nullptr;
    QComboBox* m_serialEcho = nullptr;
    QComboBox* m_serialNewline = nullptr;
    // proxy / reconnect
    QComboBox* m_proxyKind = nullptr;
    QLineEdit* m_proxyHost = nullptr;
    QSpinBox* m_proxyPort = nullptr;
    QLineEdit* m_proxyUser = nullptr;
    QComboBox* m_jump = nullptr;
    QCheckBox* m_reconnect = nullptr;
    QSpinBox* m_reconnectAttempts = nullptr;
    QCheckBox* m_saveToList = nullptr;
    std::string m_credentialId; // set when accept() stored a new secret
};

// Lists saved sessions grouped by folder; create, edit, duplicate, delete and open them.
class SessionManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit SessionManagerDialog(AppContext* context, QWidget* parent = nullptr);

    QTreeWidget* tree() const { return m_tree; }
    // Id of the selected profile, empty if a folder or nothing is selected.
    QString selectedId() const;
    void selectId(const QString& id);

Q_SIGNALS:
    void openRequested(const wt::session::SessionProfile& profile);

private:
    void reload();
    void createSession(wt::session::SessionKind kind);
    void editSelected();
    void duplicateSelected();
    void deleteSelected();
    void openSelected();
    bool editProfile(wt::session::SessionProfile profile, const QString& title);

    AppContext* m_context; // outlives the (modal) dialog
    QTreeWidget* m_tree;
};

#pragma once

#include "Triggers.h"

#include "wt/session/CommandHistory.h"
#include "wt/session/SessionProfile.h"
#include "wt/session/Settings.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <string>

class QWidget;
namespace wt::security {
class CredentialVault;
}

// Application-wide state shared by all windows: settings and saved sessions, loaded from and
// saved to the configuration directory (unless the context is ephemeral).
class AppContext : public QObject {
    Q_OBJECT

public:
    // `configDir` empty: the platform's per-user application config directory.
    explicit AppContext(const QString& configDir = {}, QObject* parent = nullptr);
    // A context that never touches the disk (tests, --no-config).
    static AppContext* createEphemeral(QObject* parent = nullptr);

    bool isPersistent() const { return m_persistent; }
    QString configDir() const { return m_configDir; }
    QString settingsPath() const;
    QString sessionsPath() const;
    QString vaultPath() const;
    // known_hosts used for SSH host key checks (settings override or ~/.ssh/known_hosts).
    QString knownHostsPath() const;

    const wt::session::Settings& settings() const { return m_settings; }
    // Replaces the settings, saves them and notifies all views.
    bool setSettings(const wt::session::Settings& settings);

    wt::session::SessionStore& sessions() { return m_sessions; }
    const wt::session::SessionStore& sessions() const { return m_sessions; }
    bool saveSessions();

    // ---- Command history for completion (history.json; saved shortly after changes)
    QString historyPath() const;
    void recordCommand(const QString& context, const QString& command);
    QStringList completeCommand(const QString& context, const QString& prefix, int limit = 8) const;
    wt::session::CommandHistory& history() { return m_history; }
    bool saveHistory();

    // ---- Triggers from the settings, compiled (invalid patterns are listed in triggerErrors()).
    const std::vector<CompiledTrigger>& triggers() const { return m_triggers; }
    QStringList triggerErrors() const { return m_triggerErrors; }

    // ---- Session logs: the settings' directory, or <Documents>/WindTerm-Weifan/Logs.
    QString logDirectory() const;
    // A new log file path for a session, following the settings' file name pattern.
    QString logPathFor(const QString& sessionName, const QString& host) const;

    // ---- Credential vault (encrypted with the master password; see wt::security::CredentialVault)
    // Asks for the master password: `creating` is true when a new vault will be created (the
    // provider should ask twice). nullopt = cancelled. The default shows a dialog.
    using MasterPasswordProvider = std::function<std::optional<QString>(bool creating, QWidget* parent)>;
    void setMasterPasswordProvider(MasterPasswordProvider provider) { m_masterPassword = std::move(provider); }
    static bool vaultSupported();
    bool vaultExists() const;
    bool isVaultUnlocked() const;
    // Opens (or creates) the vault, prompting for the master password. Returns false if the user
    // cancelled or the password was wrong (see lastError()).
    bool unlockVault(QWidget* parent = nullptr);
    void lockVault();
    // A stored secret; unlocks the vault first if needed. nullopt if missing or locked.
    std::optional<std::string> credential(const std::string& id, QWidget* parent = nullptr);
    // Stores a secret (creating/unlocking the vault as needed) and saves the vault. Returns the
    // id used (`id` or a new one), empty on failure.
    std::string storeCredential(std::string id, const std::string& secret, const std::string& label,
                                QWidget* parent = nullptr);
    bool removeCredential(const std::string& id, QWidget* parent = nullptr);
    // Argon2id cost for new vaults (tests lower it).
    void setVaultCost(unsigned memoryKiB, unsigned iterations);

    // Problems found while loading (malformed files, unknown values), for display.
    QStringList warnings() const { return m_warnings; }
    QString lastError() const { return m_lastError; }

Q_SIGNALS:
    void settingsChanged();
    void sessionsChanged();

public:
    ~AppContext() override;

private:
    struct EphemeralTag {};
    AppContext(EphemeralTag, QObject* parent);
    void load();
    void compileTriggers();

    wt::session::CommandHistory m_history;
    QTimer m_historySave;
    std::vector<CompiledTrigger> m_triggers;
    QStringList m_triggerErrors;

    std::shared_ptr<wt::security::CredentialVault> m_vault;
    MasterPasswordProvider m_masterPassword;
    unsigned m_vaultMemoryKiB = 64 * 1024;
    unsigned m_vaultIterations = 3;
    QString m_ephemeralVaultPath;
    bool m_persistent = true;
    QString m_configDir;
    wt::session::Settings m_settings;
    wt::session::SessionStore m_sessions;
    QStringList m_warnings;
    QString m_lastError;
};

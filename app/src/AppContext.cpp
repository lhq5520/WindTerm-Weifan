#include "AppContext.h"

#include "credentials/SystemCredentials.h"

#include "wt/platform/Files.h"

#ifdef WT_HAVE_VAULT
#include "wt/security/Vault.h"
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstring>
#include <ctime>

AppContext::AppContext(const QString& configDir, QObject* parent)
    : QObject(parent),
      m_configDir(configDir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                                      : configDir) {
    m_historySave.setSingleShot(true);
    m_historySave.setInterval(2000);
    connect(&m_historySave, &QTimer::timeout, this, &AppContext::saveHistory);
    m_vaultLock.setSingleShot(true);
    connect(&m_vaultLock, &QTimer::timeout, this, &AppContext::lockVault);
    load();
}

AppContext::AppContext(EphemeralTag, QObject* parent) : QObject(parent), m_persistent(false) {
    m_vaultLock.setSingleShot(true);
    connect(&m_vaultLock, &QTimer::timeout, this, &AppContext::lockVault);
}

AppContext::~AppContext() {
    if (m_historySave.isActive()) {
        saveHistory();
    }
}

QString AppContext::workspacePath() const {
    return QDir(m_configDir).filePath(QStringLiteral("workspace.json"));
}

std::optional<wt::session::WorkspaceState> AppContext::loadWorkspace() {
    if (!m_persistent) {
        return std::nullopt;
    }
    std::string error;
    auto state = wt::session::WorkspaceState::load(workspacePath().toStdString(), &error);
    if (!state && !error.empty()) {
        m_warnings << QString::fromStdString(error);
    }
    return state;
}

bool AppContext::saveWorkspace(const wt::session::WorkspaceState& state) {
    if (!m_persistent) {
        return true;
    }
    if (const auto status = state.save(workspacePath().toStdString()); !status) {
        m_lastError = QString::fromStdString(status.message());
        return false;
    }
    return true;
}

void AppContext::clearWorkspace() {
    if (m_persistent) {
        QFile::remove(workspacePath());
    }
}

QString AppContext::historyPath() const {
    return QDir(m_configDir).filePath(QStringLiteral("history.json"));
}

void AppContext::recordCommand(const QString& context, const QString& command) {
    if (m_history.add(context.toStdString(), command.toStdString()) && m_persistent) {
        m_historySave.start();
    }
}

QStringList AppContext::completeCommand(const QString& context, const QString& prefix, int limit) const {
    QStringList list;
    for (const auto& command :
         m_history.complete(context.toStdString(), prefix.toStdString(), static_cast<std::size_t>(std::max(1, limit)))) {
        list << QString::fromStdString(command);
    }
    return list;
}

bool AppContext::saveHistory() {
    m_historySave.stop();
    if (!m_persistent) {
        return true;
    }
    if (const auto status = m_history.save(historyPath().toStdString()); !status) {
        m_lastError = QString::fromStdString(status.message());
        return false;
    }
    return true;
}

void AppContext::compileTriggers() {
    m_triggerErrors.clear();
    m_triggers = ::compileTriggers(m_settings.triggers, &m_triggerErrors);
}

QString AppContext::logDirectory() const {
    if (!m_settings.logging.directory.empty()) {
        return QString::fromStdString(m_settings.logging.directory);
    }
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir(documents.isEmpty() ? QDir::homePath() : documents).filePath(QStringLiteral("WindTerm-Weifan/Logs"));
}

QString AppContext::logPathFor(const QString& sessionName, const QString& host) const {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef Q_OS_WIN
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const std::string name =
        wt::session::expandLogFileName(m_settings.logging.fileName, sessionName.toStdString(), host.toStdString(), local);
    QString path = QDir(logDirectory()).filePath(QString::fromStdString(name));
    // Two sessions started in the same second with the same name: do not interleave their logs.
    for (int n = 2; QFile::exists(path) && n < 1000; ++n) {
        const QFileInfo info(QDir(logDirectory()).filePath(QString::fromStdString(name)));
        path = info.dir().filePath(QStringLiteral("%1-%2.%3").arg(info.completeBaseName()).arg(n).arg(info.suffix()));
    }
    return path;
}

AppContext* AppContext::createEphemeral(QObject* parent) {
    return new AppContext(EphemeralTag{}, parent);
}

QString AppContext::settingsPath() const {
    return QDir(m_configDir).filePath(QStringLiteral("settings.json"));
}

QString AppContext::sessionsPath() const {
    return QDir(m_configDir).filePath(QStringLiteral("sessions.json"));
}

QString AppContext::vaultPath() const {
    if (!m_persistent) {
        return m_ephemeralVaultPath; // set on first use: a private temporary directory
    }
    return QDir(m_configDir).filePath(QStringLiteral("vault.json"));
}

QString AppContext::knownHostsPath() const {
    if (!m_settings.knownHostsFile.empty()) {
        return QString::fromStdString(m_settings.knownHostsFile);
    }
    return QDir::home().filePath(QStringLiteral(".ssh/known_hosts"));
}

void AppContext::load() {
    std::string error;
    if (QFile::exists(settingsPath())) {
        if (const auto text = wt::platform::readFile(settingsPath().toStdString(), &error)) {
            if (const auto json = wt::core::Json::parse(*text, &error)) {
                std::vector<std::string> warnings;
                m_settings = wt::session::Settings::fromJson(*json, &warnings);
                for (const auto& warning : warnings) {
                    m_warnings << QStringLiteral("settings.json: %1").arg(QString::fromStdString(warning));
                }
            } else {
                m_warnings << QStringLiteral("settings.json: %1").arg(QString::fromStdString(error));
            }
        } else {
            m_warnings << QString::fromStdString(error);
        }
    }
    if (const auto status = m_sessions.load(sessionsPath().toStdString()); !status) {
        m_warnings << QString::fromStdString(status.message());
    }
    if (const auto status = m_history.load(historyPath().toStdString()); !status) {
        m_warnings << QString::fromStdString(status.message());
    }
    compileTriggers();
    m_warnings << m_triggerErrors;
}

bool AppContext::setSettings(const wt::session::Settings& settings) {
    m_settings = settings;
    compileTriggers();
    touchVault(); // a changed lock time applies at once
    bool ok = true;
    if (m_persistent) {
        const auto status =
            wt::platform::writeFileAtomic(settingsPath().toStdString(), m_settings.toJson().dump(2) + "\n");
        if (!status) {
            m_lastError = QString::fromStdString(status.message());
            ok = false;
        }
    }
    Q_EMIT settingsChanged();
    return ok;
}

bool AppContext::saveSessions() {
    bool ok = true;
    if (m_persistent) {
        if (const auto status = m_sessions.save(sessionsPath().toStdString()); !status) {
            m_lastError = QString::fromStdString(status.message());
            ok = false;
        }
    }
    Q_EMIT sessionsChanged();
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Credential vault

namespace {

std::optional<QString> askMasterPassword(bool creating, QWidget* parent) {
    bool ok = false;
    const QString title = QObject::tr("Credential Vault");
    if (!creating) {
        const QString password = QInputDialog::getText(parent, title, QObject::tr("Master password:"),
                                                       QLineEdit::Password, {}, &ok);
        return ok ? std::optional<QString>(password) : std::nullopt;
    }
    for (;;) {
        const QString first = QInputDialog::getText(
            parent, title,
            QObject::tr("Choose a master password for the encrypted credential vault.\n"
                        "It cannot be recovered if you forget it."),
            QLineEdit::Password, {}, &ok);
        if (!ok) {
            return std::nullopt;
        }
        const QString second =
            QInputDialog::getText(parent, title, QObject::tr("Repeat the master password:"), QLineEdit::Password, {}, &ok);
        if (!ok) {
            return std::nullopt;
        }
        if (first == second && !first.isEmpty()) {
            return first;
        }
        QMessageBox::warning(parent, title, QObject::tr("The passwords do not match or are empty."));
    }
}

} // namespace

bool AppContext::vaultSupported() {
#ifdef WT_HAVE_VAULT
    return true;
#else
    return false;
#endif
}

void AppContext::setVaultCost(unsigned memoryKiB, unsigned iterations) {
    m_vaultMemoryKiB = memoryKiB;
    m_vaultIterations = iterations;
}

#ifdef WT_HAVE_VAULT

bool AppContext::vaultExists() const {
    return !vaultPath().isEmpty() && wt::security::CredentialVault::exists(vaultPath().toStdString());
}

bool AppContext::isVaultUnlocked() const {
    return m_vault != nullptr;
}

bool AppContext::unlockVault(QWidget* parent) {
    if (m_vault) {
        return true;
    }
    if (vaultPath().isEmpty()) {
        // Ephemeral contexts keep their vault in a private temporary directory.
        static QTemporaryDir directory;
        m_ephemeralVaultPath = directory.filePath(QStringLiteral("vault-%1.json").arg(reinterpret_cast<quintptr>(this)));
    }
    const bool creating = !vaultExists();
    const auto provider = m_masterPassword ? m_masterPassword : MasterPasswordProvider(askMasterPassword);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto password = provider(creating, parent);
        if (!password) {
            m_lastError = tr("The credential vault stays locked");
            return false;
        }
        std::string error;
        const std::string utf8 = password->toStdString();
        if (creating) {
            wt::security::Argon2Params params;
            params.memoryKiB = m_vaultMemoryKiB;
            params.iterations = m_vaultIterations;
            m_vault = wt::security::CredentialVault::create(vaultPath().toStdString(), utf8, params, &error);
        } else {
            m_vault = wt::security::CredentialVault::open(vaultPath().toStdString(), utf8, &error);
        }
        if (m_vault) {
            touchVault();
            return true;
        }
        m_lastError = QString::fromStdString(error);
        if (creating) {
            return false; // not a password problem
        }
    }
    return false;
}

void AppContext::lockVault() {
    m_vaultLock.stop();
    const bool wasUnlocked = m_vault != nullptr;
    m_vault.reset(); // wipes the key
    if (wasUnlocked) {
        Q_EMIT vaultLocked();
    }
}

std::optional<std::string> AppContext::vaultCredential(const std::string& id, QWidget* parent) {
    if (id.empty() || (!m_vault && (!vaultExists() || !unlockVault(parent)))) {
        return std::nullopt;
    }
    touchVault();
    return m_vault->secret(id);
}

std::string AppContext::storeVaultCredential(std::string id, const std::string& secret, const std::string& label,
                                             QWidget* parent) {
    if (!unlockVault(parent)) {
        return {};
    }
    touchVault();
    if (id.empty()) {
        id = wt::security::CredentialVault::newId();
    }
    m_vault->set(id, secret, label);
    if (const auto status = m_vault->save(); !status) {
        m_lastError = QString::fromStdString(status.message());
        return {};
    }
    return id;
}

bool AppContext::removeVaultCredential(const std::string& id, QWidget* parent) {
    if (!unlockVault(parent) || !m_vault->remove(id)) {
        return false;
    }
    touchVault();
    return m_vault->save().isOk();
}

#else

bool AppContext::vaultExists() const {
    return false;
}
bool AppContext::isVaultUnlocked() const {
    return false;
}
bool AppContext::unlockVault(QWidget*) {
    m_lastError = tr("This build has no credential vault (OpenSSL 3.2+ was not found)");
    return false;
}
void AppContext::lockVault() {}
std::optional<std::string> AppContext::vaultCredential(const std::string&, QWidget*) {
    return std::nullopt;
}
std::string AppContext::storeVaultCredential(std::string, const std::string&, const std::string&, QWidget*) {
    m_lastError = tr("This build has no credential vault (OpenSSL 3.2+ was not found)");
    return {};
}
bool AppContext::removeVaultCredential(const std::string&, QWidget*) {
    return false;
}

#endif

// ---------------------------------------------------------------------------------------------
// Credentials: vault entries have plain ids, system store entries "system:<account>".

bool AppContext::isSystemCredential(const std::string& id) {
    return id.rfind(kSystemCredentialPrefix, 0) == 0;
}

bool AppContext::systemCredentialsAvailable() const {
    return SystemCredentials::available();
}

QString AppContext::credentialStoreName() const {
    if (m_settings.security.credentialStore == wt::session::CredentialStoreKind::System && systemCredentialsAvailable()) {
        return SystemCredentials::storeName();
    }
    return tr("encrypted credential vault");
}

std::optional<std::string> AppContext::credential(const std::string& id, QWidget* parent) {
    if (!isSystemCredential(id)) {
        return vaultCredential(id, parent);
    }
    QString error;
    auto secret = SystemCredentials::load(QString::fromStdString(id.substr(std::strlen(kSystemCredentialPrefix))), &error);
    if (!secret && !error.isEmpty()) {
        m_lastError = error;
    }
    return secret;
}

std::string AppContext::storeCredential(std::string id, const std::string& secret, const std::string& label,
                                        QWidget* parent) {
    const bool useSystem =
        isSystemCredential(id) ||
        (id.empty() && m_settings.security.credentialStore == wt::session::CredentialStoreKind::System &&
         systemCredentialsAvailable());
    if (!useSystem) {
        return storeVaultCredential(std::move(id), secret, label, parent);
    }
    if (id.empty()) {
        id = kSystemCredentialPrefix + wt::session::SessionStore::generateId();
    }
    QString error;
    if (!SystemCredentials::store(QString::fromStdString(id.substr(std::strlen(kSystemCredentialPrefix))),
                                  QStringLiteral("WindTerm-Weifan: %1").arg(QString::fromStdString(label)), secret,
                                  &error)) {
        m_lastError = error;
        return {};
    }
    return id;
}

bool AppContext::removeCredential(const std::string& id, QWidget* parent) {
    if (!isSystemCredential(id)) {
        return removeVaultCredential(id, parent);
    }
    QString error;
    if (!SystemCredentials::remove(QString::fromStdString(id.substr(std::strlen(kSystemCredentialPrefix))), &error)) {
        m_lastError = error;
        return false;
    }
    return true;
}

void AppContext::setVaultLockTimeout(std::chrono::milliseconds timeout) {
    m_vaultLockOverride = timeout;
    touchVault();
}

void AppContext::touchVault() {
    if (!isVaultUnlocked()) {
        return;
    }
    const auto timeout = m_vaultLockOverride.count() > 0
                             ? m_vaultLockOverride
                             : std::chrono::milliseconds(std::chrono::minutes(m_settings.security.vaultLockMinutes));
    if (timeout.count() <= 0) {
        m_vaultLock.stop();
        return;
    }
    m_vaultLock.start(static_cast<int>(std::min<std::chrono::milliseconds::rep>(timeout.count(), 0x7FFFFFFF)));
}

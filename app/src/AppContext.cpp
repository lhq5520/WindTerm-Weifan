#include "AppContext.h"

#include "wt/platform/Files.h"

#ifdef WT_HAVE_VAULT
#include "wt/security/Vault.h"
#endif

#include <QDir>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryDir>

AppContext::AppContext(const QString& configDir, QObject* parent)
    : QObject(parent),
      m_configDir(configDir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                                      : configDir) {
    load();
}

AppContext::AppContext(EphemeralTag, QObject* parent) : QObject(parent), m_persistent(false) {}

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
}

bool AppContext::setSettings(const wt::session::Settings& settings) {
    m_settings = settings;
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
    m_vault.reset();
}

std::optional<std::string> AppContext::credential(const std::string& id, QWidget* parent) {
    if (id.empty() || (!m_vault && (!vaultExists() || !unlockVault(parent)))) {
        return std::nullopt;
    }
    return m_vault->secret(id);
}

std::string AppContext::storeCredential(std::string id, const std::string& secret, const std::string& label,
                                        QWidget* parent) {
    if (!unlockVault(parent)) {
        return {};
    }
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

bool AppContext::removeCredential(const std::string& id, QWidget* parent) {
    if (!unlockVault(parent) || !m_vault->remove(id)) {
        return false;
    }
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
std::optional<std::string> AppContext::credential(const std::string&, QWidget*) {
    return std::nullopt;
}
std::string AppContext::storeCredential(std::string, const std::string&, const std::string&, QWidget*) {
    m_lastError = tr("This build has no credential vault (OpenSSL 3.2+ was not found)");
    return {};
}
bool AppContext::removeCredential(const std::string&, QWidget*) {
    return false;
}

#endif

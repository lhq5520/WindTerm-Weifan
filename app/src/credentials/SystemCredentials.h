#pragma once

#include <QString>

#include <optional>
#include <string>

class QDBusConnection;

// The operating system's secret store: the macOS Keychain, the Windows Credential Manager or, on
// Linux/BSD desktops, the freedesktop Secret Service (GNOME Keyring, KWallet, KeePassXC). Items
// belong to the service "WindTerm-Weifan"; `account` names them.
namespace SystemCredentials {

bool available();
QString storeName();
bool store(const QString& account, const QString& label, const std::string& secret, QString* error);
// nullopt with an empty `error`: no such item.
std::optional<std::string> load(const QString& account, QString* error);
bool remove(const QString& account, QString* error);

// Tests: talk to a Secret Service on this connection instead of the session bus.
void setSecretServiceBus(const QDBusConnection& bus);
void resetSecretServiceBus();

} // namespace SystemCredentials

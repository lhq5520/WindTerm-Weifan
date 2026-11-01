#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QMap>
#include <QMetaType>
#include <QString>

#include <optional>

// D-Bus types of the freedesktop.org Secret Service API.
struct SecretStruct {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};
Q_DECLARE_METATYPE(SecretStruct)
using SecretStringMap = QMap<QString, QString>;
Q_DECLARE_METATYPE(SecretStringMap)

QDBusArgument& operator<<(QDBusArgument& argument, const SecretStruct& secret);
const QDBusArgument& operator>>(const QDBusArgument& argument, SecretStruct& secret);
void registerSecretServiceTypes();

// Client of the Secret Service (GNOME Keyring, KWallet, KeePassXC, ...) over D-Bus. Items carry
// the attributes application=WindTerm-Weifan, service=<service>, account=<account> and are stored
// in the default collection. Secrets travel with the "plain" algorithm over the private session
// bus. Locked collections are unlocked through the service's own prompt.
class SecretServiceClient {
public:
    explicit SecretServiceClient(QDBusConnection bus = QDBusConnection::sessionBus());

    // A Secret Service is running (or can be activated) on the bus.
    bool isAvailable() const;
    bool store(const QString& service, const QString& account, const QString& label, const QByteArray& secret,
               QString* error);
    // nullopt with an empty `error`: no such item.
    std::optional<QByteArray> load(const QString& service, const QString& account, QString* error);
    bool remove(const QString& service, const QString& account, QString* error);

private:
    bool ensureSession(QString* error);
    bool waitForPrompt(const QDBusObjectPath& prompt, QString* error);
    QList<QDBusObjectPath> findItems(const QString& service, const QString& account, QString* error);

    QDBusConnection m_bus;
    QDBusObjectPath m_session;
};

#include "SecretService.h"

#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusVariant>
#include <QEventLoop>
#include <QTimer>

namespace {

const QString kService = QStringLiteral("org.freedesktop.secrets");
const QString kServicePath = QStringLiteral("/org/freedesktop/secrets");
const QString kServiceInterface = QStringLiteral("org.freedesktop.Secret.Service");
const QString kCollectionInterface = QStringLiteral("org.freedesktop.Secret.Collection");
const QString kItemInterface = QStringLiteral("org.freedesktop.Secret.Item");
const QString kPromptInterface = QStringLiteral("org.freedesktop.Secret.Prompt");
constexpr int kCallTimeoutMs = 10000;
constexpr int kPromptTimeoutMs = 120000;

SecretStringMap attributes(const QString& service, const QString& account) {
    return {{QStringLiteral("application"), QStringLiteral("WindTerm-Weifan")},
            {QStringLiteral("service"), service},
            {QStringLiteral("account"), account}};
}

QString errorOf(const QDBusMessage& reply) {
    return reply.errorMessage().isEmpty() ? reply.errorName() : reply.errorMessage();
}

bool isNoPrompt(const QDBusObjectPath& prompt) {
    return prompt.path().isEmpty() || prompt.path() == QLatin1String("/");
}

// Waits for org.freedesktop.Secret.Prompt.Completed.
class PromptWaiter : public QObject {
    Q_OBJECT

public:
    bool done = false;
    bool dismissed = false;
    QEventLoop loop;

public Q_SLOTS:
    void completed(bool wasDismissed, const QDBusVariant& result) {
        Q_UNUSED(result);
        done = true;
        dismissed = wasDismissed;
        loop.quit();
    }
};

} // namespace

QDBusArgument& operator<<(QDBusArgument& argument, const SecretStruct& secret) {
    argument.beginStructure();
    argument << secret.session << secret.parameters << secret.value << secret.contentType;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, SecretStruct& secret) {
    argument.beginStructure();
    argument >> secret.session >> secret.parameters >> secret.value >> secret.contentType;
    argument.endStructure();
    return argument;
}

void registerSecretServiceTypes() {
    static const bool registered = [] {
        qDBusRegisterMetaType<SecretStruct>();
        qDBusRegisterMetaType<SecretStringMap>();
        return true;
    }();
    Q_UNUSED(registered);
}

SecretServiceClient::SecretServiceClient(QDBusConnection bus) : m_bus(std::move(bus)) {
    registerSecretServiceTypes();
}

bool SecretServiceClient::isAvailable() const {
    if (!m_bus.isConnected()) {
        return false;
    }
    QDBusConnectionInterface* interface = m_bus.interface();
    if (!interface) {
        return false;
    }
    if (interface->isServiceRegistered(kService).value()) {
        return true;
    }
    const QDBusReply<QStringList> activatable = interface->activatableServiceNames();
    return activatable.isValid() && activatable.value().contains(kService);
}

bool SecretServiceClient::ensureSession(QString* error) {
    if (!m_session.path().isEmpty()) {
        return true;
    }
    QDBusInterface service(kService, kServicePath, kServiceInterface, m_bus);
    service.setTimeout(kCallTimeoutMs);
    const QDBusMessage reply = service.call(QStringLiteral("OpenSession"), QStringLiteral("plain"),
                                            QVariant::fromValue(QDBusVariant(QString())));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        if (error) {
            *error = QObject::tr("Secret Service: %1").arg(errorOf(reply));
        }
        return false;
    }
    m_session = qvariant_cast<QDBusObjectPath>(reply.arguments().at(1));
    return true;
}

bool SecretServiceClient::waitForPrompt(const QDBusObjectPath& prompt, QString* error) {
    if (isNoPrompt(prompt)) {
        return true;
    }
    PromptWaiter waiter;
    if (!m_bus.connect(kService, prompt.path(), kPromptInterface, QStringLiteral("Completed"), &waiter,
                       SLOT(completed(bool, QDBusVariant)))) {
        if (error) {
            *error = QObject::tr("Secret Service: cannot wait for the unlock prompt");
        }
        return false;
    }
    QDBusInterface promptInterface(kService, prompt.path(), kPromptInterface, m_bus);
    const QDBusMessage reply = promptInterface.call(QStringLiteral("Prompt"), QString());
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (error) {
            *error = QObject::tr("Secret Service: %1").arg(errorOf(reply));
        }
        return false;
    }
    if (!waiter.done) {
        QTimer::singleShot(kPromptTimeoutMs, &waiter.loop, &QEventLoop::quit);
        waiter.loop.exec();
    }
    m_bus.disconnect(kService, prompt.path(), kPromptInterface, QStringLiteral("Completed"), &waiter,
                     SLOT(completed(bool, QDBusVariant)));
    if (!waiter.done || waiter.dismissed) {
        if (error) {
            *error = QObject::tr("The keyring stays locked");
        }
        return false;
    }
    return true;
}

QList<QDBusObjectPath> SecretServiceClient::findItems(const QString& service, const QString& account, QString* error) {
    QDBusInterface secrets(kService, kServicePath, kServiceInterface, m_bus);
    secrets.setTimeout(kCallTimeoutMs);
    const QDBusMessage reply =
        secrets.call(QStringLiteral("SearchItems"), QVariant::fromValue(attributes(service, account)));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        if (error) {
            *error = QObject::tr("Secret Service: %1").arg(errorOf(reply));
        }
        return {};
    }
    QList<QDBusObjectPath> unlocked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));
    const QList<QDBusObjectPath> locked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(1));
    if (!locked.isEmpty()) {
        const QDBusMessage unlock = secrets.call(QStringLiteral("Unlock"), QVariant::fromValue(locked));
        if (unlock.type() == QDBusMessage::ReplyMessage && unlock.arguments().size() >= 2) {
            unlocked += qdbus_cast<QList<QDBusObjectPath>>(unlock.arguments().at(0));
            const auto prompt = qvariant_cast<QDBusObjectPath>(unlock.arguments().at(1));
            if (!isNoPrompt(prompt) && waitForPrompt(prompt, error)) {
                unlocked += locked; // unlocked by the user
            }
        }
    }
    return unlocked;
}

bool SecretServiceClient::store(const QString& service, const QString& account, const QString& label,
                                const QByteArray& secret, QString* error) {
    if (!ensureSession(error)) {
        return false;
    }
    QVariantMap properties;
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"), label);
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"),
                      QVariant::fromValue(attributes(service, account)));
    const SecretStruct value{m_session, QByteArray(), secret, QStringLiteral("text/plain")};
    QDBusMessage reply;
    for (const QString& collection : {QStringLiteral("/org/freedesktop/secrets/aliases/default"),
                                      QStringLiteral("/org/freedesktop/secrets/collection/login")}) {
        QDBusInterface target(kService, collection, kCollectionInterface, m_bus);
        target.setTimeout(kCallTimeoutMs);
        reply = target.call(QStringLiteral("CreateItem"), properties, QVariant::fromValue(value), true);
        if (reply.type() == QDBusMessage::ReplyMessage) {
            break;
        }
    }
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        if (error) {
            *error = QObject::tr("Secret Service: %1").arg(errorOf(reply));
        }
        return false;
    }
    return waitForPrompt(qvariant_cast<QDBusObjectPath>(reply.arguments().at(1)), error);
}

std::optional<QByteArray> SecretServiceClient::load(const QString& service, const QString& account, QString* error) {
    if (error) {
        error->clear();
    }
    if (!ensureSession(error)) {
        return std::nullopt;
    }
    const QList<QDBusObjectPath> items = findItems(service, account, error);
    if (items.isEmpty()) {
        return std::nullopt;
    }
    QDBusInterface item(kService, items.first().path(), kItemInterface, m_bus);
    item.setTimeout(kCallTimeoutMs);
    const QDBusMessage reply = item.call(QStringLiteral("GetSecret"), QVariant::fromValue(m_session));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        if (error) {
            *error = QObject::tr("Secret Service: %1").arg(errorOf(reply));
        }
        return std::nullopt;
    }
    return qdbus_cast<SecretStruct>(reply.arguments().at(0)).value;
}

bool SecretServiceClient::remove(const QString& service, const QString& account, QString* error) {
    if (!ensureSession(error)) {
        return false;
    }
    for (const QDBusObjectPath& path : findItems(service, account, error)) {
        QDBusInterface item(kService, path.path(), kItemInterface, m_bus);
        item.setTimeout(kCallTimeoutMs);
        const QDBusMessage reply = item.call(QStringLiteral("Delete"));
        if (reply.type() != QDBusMessage::ReplyMessage) {
            if (error) {
                *error = QObject::tr("Secret Service: %1").arg(errorOf(reply));
            }
            return false;
        }
        if (!reply.arguments().isEmpty() && !waitForPrompt(qvariant_cast<QDBusObjectPath>(reply.arguments().at(0)), error)) {
            return false;
        }
    }
    return true;
}

#include "SecretService.moc"

#include "SystemCredentials.h"

#include "wt/platform/Keychain.h"

#ifdef WT_HAVE_SECRET_SERVICE
#include "SecretService.h"

#include <QDBusConnection>

#include <memory>
#endif

#include <QObject>

namespace {

const std::string kService = "WindTerm-Weifan";

#ifdef WT_HAVE_SECRET_SERVICE
std::unique_ptr<QDBusConnection>& testBus() {
    static std::unique_ptr<QDBusConnection> bus;
    return bus;
}

SecretServiceClient client() {
    return testBus() ? SecretServiceClient(*testBus()) : SecretServiceClient();
}
#endif

} // namespace

namespace SystemCredentials {

void setSecretServiceBus(const QDBusConnection& bus) {
#ifdef WT_HAVE_SECRET_SERVICE
    testBus() = std::make_unique<QDBusConnection>(bus);
#else
    Q_UNUSED(bus);
#endif
}

void resetSecretServiceBus() {
#ifdef WT_HAVE_SECRET_SERVICE
    testBus().reset();
#endif
}

bool available() {
#ifdef WT_HAVE_SECRET_SERVICE
    return client().isAvailable();
#else
    return wt::platform::keychain::available();
#endif
}

QString storeName() {
#ifdef WT_HAVE_SECRET_SERVICE
    return QObject::tr("system keyring (Secret Service)");
#else
    return QString::fromStdString(wt::platform::keychain::storeName());
#endif
}

bool store(const QString& account, const QString& label, const std::string& secret, QString* error) {
#ifdef WT_HAVE_SECRET_SERVICE
    return client().store(QString::fromStdString(kService), account, label,
                          QByteArray(secret.data(), static_cast<qsizetype>(secret.size())), error);
#else
    const auto status = wt::platform::keychain::store(kService, account.toStdString(), label.toStdString(), secret);
    if (!status && error) {
        *error = QString::fromStdString(status.message());
    }
    return status.isOk();
#endif
}

std::optional<std::string> load(const QString& account, QString* error) {
#ifdef WT_HAVE_SECRET_SERVICE
    const auto secret = client().load(QString::fromStdString(kService), account, error);
    if (!secret) {
        return std::nullopt;
    }
    return secret->toStdString();
#else
    wt::core::Status status;
    auto secret = wt::platform::keychain::load(kService, account.toStdString(), &status);
    if (!status && error) {
        *error = QString::fromStdString(status.message());
    }
    return secret;
#endif
}

bool remove(const QString& account, QString* error) {
#ifdef WT_HAVE_SECRET_SERVICE
    return client().remove(QString::fromStdString(kService), account, error);
#else
    const auto status = wt::platform::keychain::remove(kService, account.toStdString());
    if (!status && error) {
        *error = QString::fromStdString(status.message());
    }
    return status.isOk();
#endif
}

} // namespace SystemCredentials

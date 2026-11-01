// GUI tests for the credential stores (vault idle lock, system store through a fake Secret
// Service on a private D-Bus daemon) and for restoring the window layout and sessions.

#include "AppContext.h"
#include "MainWindow.h"
#include "TerminalPane.h"
#include "TerminalSession.h"
#include "TerminalTab.h"

#ifdef WT_HAVE_SECRET_SERVICE
#include "credentials/SecretService.h"
#include "credentials/SystemCredentials.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusVariant>
#endif

#include <QDir>
#include <QMutex>
#include <QProcess>
#include <QSignalSpy>
#include <QSplitter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <map>
#include <memory>

using namespace wt::session;

#ifdef WT_HAVE_SECRET_SERVICE

namespace {

struct StoredItem {
    QString label;
    SecretStringMap attributes;
    QByteArray secret;
};

// Shared between the fake service's thread and the test.
struct FakeKeyring {
    QMutex mutex;
    std::map<QString, StoredItem> items; // object path -> item
    int sessions = 0;
};

class FakeItem : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Item")

public:
    FakeItem(FakeKeyring* keyring, QString path, QDBusConnection bus)
        : m_keyring(keyring), m_path(std::move(path)), m_bus(std::move(bus)) {}

public Q_SLOTS:
    SecretStruct GetSecret(const QDBusObjectPath& session) {
        QMutexLocker lock(&m_keyring->mutex);
        return {session, {}, m_keyring->items[m_path].secret, QStringLiteral("text/plain")};
    }
    QDBusObjectPath Delete() {
        {
            QMutexLocker lock(&m_keyring->mutex);
            m_keyring->items.erase(m_path);
        }
        m_bus.unregisterObject(m_path);
        deleteLater();
        return QDBusObjectPath(QStringLiteral("/"));
    }

private:
    FakeKeyring* m_keyring;
    QString m_path;
    QDBusConnection m_bus;
};

class FakeCollection : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Collection")

public:
    FakeCollection(FakeKeyring* keyring, QDBusConnection bus) : m_keyring(keyring), m_bus(std::move(bus)) {}

public Q_SLOTS:
    QDBusObjectPath CreateItem(const QVariantMap& properties, const SecretStruct& secret, bool replace,
                               QDBusObjectPath& prompt) {
        prompt = QDBusObjectPath(QStringLiteral("/"));
        StoredItem item;
        item.label = properties.value(QStringLiteral("org.freedesktop.Secret.Item.Label")).toString();
        item.attributes = qdbus_cast<SecretStringMap>(properties.value(QStringLiteral("org.freedesktop.Secret.Item.Attributes")));
        item.secret = secret.value;
        QMutexLocker lock(&m_keyring->mutex);
        if (replace) {
            for (auto& [path, existing] : m_keyring->items) {
                if (existing.attributes == item.attributes) {
                    existing = item;
                    return QDBusObjectPath(path);
                }
            }
        }
        const QString path = QStringLiteral("/org/freedesktop/secrets/collection/login/%1").arg(++m_next);
        m_keyring->items[path] = item;
        auto* object = new FakeItem(m_keyring, path, m_bus);
        object->setParent(this);
        m_bus.registerObject(path, object, QDBusConnection::ExportAllSlots);
        return QDBusObjectPath(path);
    }

private:
    FakeKeyring* m_keyring;
    QDBusConnection m_bus;
    int m_next = 0;
};

class FakeService : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Service")

public:
    explicit FakeService(FakeKeyring* keyring) : m_keyring(keyring) {}

public Q_SLOTS:
    QDBusVariant OpenSession(const QString& algorithm, const QDBusVariant& input, QDBusObjectPath& result) {
        Q_UNUSED(input);
        QMutexLocker lock(&m_keyring->mutex);
        result = QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/session/%1").arg(++m_keyring->sessions));
        if (algorithm != QLatin1String("plain")) {
            result = QDBusObjectPath(QStringLiteral("/"));
        }
        return QDBusVariant(QString());
    }
    QList<QDBusObjectPath> SearchItems(const SecretStringMap& attributes, QList<QDBusObjectPath>& locked) {
        locked.clear();
        QList<QDBusObjectPath> found;
        QMutexLocker lock(&m_keyring->mutex);
        for (const auto& [path, item] : m_keyring->items) {
            bool matches = true;
            for (auto it = attributes.begin(); it != attributes.end(); ++it) {
                matches = matches && item.attributes.value(it.key()) == it.value();
            }
            if (matches) {
                found << QDBusObjectPath(path);
            }
        }
        return found;
    }
    QList<QDBusObjectPath> Unlock(const QList<QDBusObjectPath>& objects, QDBusObjectPath& prompt) {
        prompt = QDBusObjectPath(QStringLiteral("/"));
        return objects;
    }

private:
    FakeKeyring* m_keyring;
};

// The fake service runs on its own thread: the client's D-Bus calls block the test thread.
class FakeSecretServer : public QThread {
public:
    FakeSecretServer(QString address, FakeKeyring* keyring) : m_address(std::move(address)), m_keyring(keyring) {}
    std::atomic<bool> ready{false};

protected:
    void run() override {
        {
            QDBusConnection bus = QDBusConnection::connectToBus(m_address, QStringLiteral("fake-secret-server"));
            FakeService service(m_keyring);
            FakeCollection collection(m_keyring, bus);
            bus.registerObject(QStringLiteral("/org/freedesktop/secrets"), &service, QDBusConnection::ExportAllSlots);
            bus.registerObject(QStringLiteral("/org/freedesktop/secrets/aliases/default"), &collection,
                               QDBusConnection::ExportAllSlots);
            bus.registerService(QStringLiteral("org.freedesktop.secrets"));
            ready = true;
            exec();
            bus.unregisterService(QStringLiteral("org.freedesktop.secrets"));
        }
        QDBusConnection::disconnectFromBus(QStringLiteral("fake-secret-server"));
    }

private:
    QString m_address;
    FakeKeyring* m_keyring;
};

} // namespace

#endif

class CredentialTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

private Q_SLOTS:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        qputenv("SHELL", "/bin/sh");
        qputenv("ENV", "");
        qputenv("PS1", "$ ");
    }

    void vaultLocksWhenIdle() {
        if (!AppContext::vaultSupported()) {
            QSKIP("no vault in this build");
        }
        AppContext* context = AppContext::createEphemeral(this);
        context->setVaultCost(1024, 1);
        int prompts = 0;
        context->setMasterPasswordProvider([&prompts](bool, QWidget*) {
            ++prompts;
            return std::optional<QString>(QStringLiteral("master"));
        });
        const std::string id = context->storeCredential({}, "hunter2", "test");
        QVERIFY(!id.empty());
        QVERIFY(!AppContext::isSystemCredential(id));
        QVERIFY(context->isVaultUnlocked());
        QSignalSpy locked(context, &AppContext::vaultLocked);
        context->setVaultLockTimeout(std::chrono::milliseconds(150));
        QTRY_VERIFY_WITH_TIMEOUT(!context->isVaultUnlocked(), 3000);
        QCOMPARE(locked.count(), 1);
        // The next use asks for the master password again.
        QCOMPARE(context->credential(id).value_or(""), std::string("hunter2"));
        QCOMPARE(prompts, 2);
        // Using the vault postpones the lock.
        for (int i = 0; i < 4; ++i) {
            QTest::qWait(80);
            QVERIFY(context->credential(id).has_value());
        }
        QVERIFY(context->isVaultUnlocked());
        QCOMPARE(prompts, 2);
        // 0 in the settings: never lock.
        context->setVaultLockTimeout(std::chrono::milliseconds(0));
        Settings settings = context->settings();
        settings.security.vaultLockMinutes = 0;
        context->setSettings(settings);
        QTest::qWait(300);
        QVERIFY(context->isVaultUnlocked());
    }

    void systemStoreThroughTheSecretService() {
#ifndef WT_HAVE_SECRET_SERVICE
        QSKIP("the system store here is the macOS Keychain / Windows Credential Manager");
#else
        const QString daemon = QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"));
        if (daemon.isEmpty()) {
            QSKIP("dbus-daemon not found");
        }
        QProcess bus;
        bus.start(daemon, {QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--nopidfile"),
                           QStringLiteral("--print-address=1")});
        QVERIFY(bus.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(bus.canReadLine(), 5000);
        const QString address = QString::fromLocal8Bit(bus.readLine()).trimmed();
        QVERIFY(!address.isEmpty());
        registerSecretServiceTypes();
        FakeKeyring keyring;
        FakeSecretServer server(address, &keyring);
        server.start();
        QTRY_VERIFY_WITH_TIMEOUT(server.ready.load(), 5000);
        {
            QDBusConnection client = QDBusConnection::connectToBus(address, QStringLiteral("secret-client"));
            QVERIFY(client.isConnected());
            QTRY_VERIFY_WITH_TIMEOUT(client.interface()->isServiceRegistered(QStringLiteral("org.freedesktop.secrets")).value(),
                                     5000);
            SystemCredentials::setSecretServiceBus(client);
            QVERIFY(SystemCredentials::available());

            AppContext* context = AppContext::createEphemeral(this);
            Settings settings = context->settings();
            settings.security.credentialStore = CredentialStoreKind::System;
            context->setSettings(settings);
            QVERIFY(context->systemCredentialsAvailable());
            QVERIFY(context->credentialStoreName().contains(QStringLiteral("Secret Service")));
            const std::string id = context->storeCredential({}, "s3cr\xc3\xa9t", "alice@db");
            QVERIFY2(!id.empty(), qPrintable(context->lastError()));
            QVERIFY(AppContext::isSystemCredential(id));
            QVERIFY(!context->isVaultUnlocked()); // the vault was never involved
            {
                QMutexLocker lock(&keyring.mutex);
                QCOMPARE(keyring.items.size(), std::size_t(1));
                const StoredItem& item = keyring.items.begin()->second;
                QCOMPARE(item.label, QStringLiteral("WindTerm-Weifan: alice@db"));
                QCOMPARE(item.attributes.value(QStringLiteral("application")), QStringLiteral("WindTerm-Weifan"));
                QCOMPARE(item.attributes.value(QStringLiteral("service")), QStringLiteral("WindTerm-Weifan"));
                QCOMPARE(item.attributes.value(QStringLiteral("account")),
                         QString::fromStdString(id.substr(std::strlen(AppContext::kSystemCredentialPrefix))));
            }
            QCOMPARE(context->credential(id).value_or(""), std::string("s3cr\xc3\xa9t"));
            // Updating keeps one item.
            QCOMPARE(context->storeCredential(id, "changed", "alice@db"), id);
            QCOMPARE(context->credential(id).value_or(""), std::string("changed"));
            {
                QMutexLocker lock(&keyring.mutex);
                QCOMPARE(keyring.items.size(), std::size_t(1));
            }
            QVERIFY(context->removeCredential(id));
            QVERIFY(!context->credential(id).has_value());
            {
                QMutexLocker lock(&keyring.mutex);
                QVERIFY(keyring.items.empty());
            }
            // Vault ids keep working next to system ones.
            if (AppContext::vaultSupported()) {
                context->setVaultCost(1024, 1);
                context->setMasterPasswordProvider([](bool, QWidget*) { return std::optional<QString>(QStringLiteral("m")); });
                settings.security.credentialStore = CredentialStoreKind::Vault;
                context->setSettings(settings);
                const std::string vaultId = context->storeCredential({}, "in-vault", "x");
                QVERIFY(!AppContext::isSystemCredential(vaultId));
                QCOMPARE(context->credential(vaultId).value_or(""), std::string("in-vault"));
            }
            SystemCredentials::resetSecretServiceBus();
        }
        QDBusConnection::disconnectFromBus(QStringLiteral("secret-client"));
        server.quit();
        server.wait(5000);
        bus.terminate();
        bus.waitForFinished(5000);
#endif
    }

    void windowLayoutAndSessionsAreRestored() {
        const QString config = m_dir.filePath(QStringLiteral("restore"));
        const QString work = m_dir.filePath(QStringLiteral("work"));
        QVERIFY(QDir().mkpath(work));
        auto* context = new AppContext(config, this);
        Settings settings = context->settings();
        settings.confirmCloseRunning = false;
        context->setSettings(settings);
        SessionProfile saved;
        saved.name = "saved local";
        saved.kind = SessionKind::Local;
        saved.local.program = "/bin/sh";
        saved.id = context->sessions().upsert(saved);
        context->saveSessions();

        wt::session::WindowState captured;
        {
            MainWindow::Options options;
            options.session.program = QStringLiteral("/bin/sh");
            options.session.workingDirectory = work;
            MainWindow window(options, context);
            QVERIFY(window.start());
            window.show();
            TerminalPane* first = window.activePane();
            window.splitActive(Qt::Horizontal);         // [first | second]
            window.splitActive(Qt::Vertical);           // [first | [second / third]]
            window.openProfile(saved);                  // second tab: a saved session
            window.tabs()->setCurrentIndex(0);
            first->setFocus();
            captured = window.captureState();
            QCOMPARE(captured.tabs.size(), std::size_t(2));
            const LayoutNode& root = captured.tabs[0].root;
            QVERIFY(root.split);
            QVERIFY(!root.vertical);
            QCOMPARE(root.children.size(), std::size_t(2));
            QCOMPARE(root.children[0].pane.program, std::string("/bin/sh"));
            QCOMPARE(QString::fromStdString(root.children[0].pane.workingDirectory), work);
            QVERIFY(root.children[1].split);
            QVERIFY(root.children[1].vertical);
            QCOMPARE(root.paneCount(), 3);
            QCOMPARE(captured.tabs[1].root.pane.profileId, saved.id);
            window.close(); // writes workspace.json
        }
        const auto stored = context->loadWorkspace();
        QVERIFY(stored.has_value());
        QCOMPARE(stored->windows.size(), std::size_t(1));
        QCOMPARE(stored->windows[0].tabs.size(), std::size_t(2));

        MainWindow restored(MainWindow::Options{}, context);
        QVERIFY(restored.restoreState(stored->windows[0]));
        restored.show();
        QCOMPARE(restored.tabCount(), 2);
        TerminalTab* tab = restored.tabAt(0);
        QCOMPARE(tab->paneCount(), 3);
        auto* outer = qobject_cast<QSplitter*>(tab->panes().first()->parentWidget());
        QVERIFY(outer);
        QCOMPARE(outer->orientation(), Qt::Horizontal);
        auto* inner = qobject_cast<QSplitter*>(tab->panes().at(1)->parentWidget());
        QVERIFY(inner && inner != outer);
        QCOMPARE(inner->orientation(), Qt::Vertical);
        for (TerminalPane* pane : tab->panes()) {
            QTRY_VERIFY_WITH_TIMEOUT(pane->session()->isRunning(), 5000);
        }
        // The restored shell runs where the first one did.
        TerminalPane* firstPane = tab->panes().first();
        QCOMPARE(QString::fromStdString(firstPane->paneState().workingDirectory), work);
        firstPane->session()->sendBytes("pwd\r");
        QTRY_VERIFY_WITH_TIMEOUT(QString::fromStdString(firstPane->session()->terminal().screenText()).contains(work), 5000);
        QCOMPARE(restored.tabAt(1)->panes().first()->profile()->id, saved.id);
        QCOMPARE(restored.captureState().tabs[0].root.paneCount(), 3);

        // Turning the option off forgets the workspace when the window closes.
        settings.restoreWorkspace = false;
        context->setSettings(settings);
        restored.close();
        QVERIFY(!QFile::exists(context->workspacePath()));
    }
};

QTEST_MAIN(CredentialTests)
#include "CredentialTests.moc"

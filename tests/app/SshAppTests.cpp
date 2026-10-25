// GUI integration tests for SSH sessions, the credential vault, reconnects and the SFTP browser,
// against an unprivileged OpenSSH server (see tests/support/SshdFixture.h).

#include "AppContext.h"
#include "MainWindow.h"
#include "SessionDialogs.h"
#include "TerminalPane.h"
#include "TerminalSession.h"
#include "TerminalTab.h"
#include "TerminalWidget.h"
#include "ssh/SftpPanel.h"
#include "ssh/SshBackend.h"
#include "ssh/SshPrompter.h"

#include "SshdFixture.h"
#include "TestServers.h"

#include "wt/platform/Files.h"

#include <QCheckBox>
#include <QLineEdit>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

using namespace wt::session;

namespace {

// Answers SSH questions without dialogs and records them.
struct ScriptedPrompter : SshPrompter {
    explicit ScriptedPrompter(QWidget* parent) : SshPrompter(parent) {}
    static inline wt::ssh::HostKeyDecision decision = wt::ssh::HostKeyDecision::AcceptAndSave;
    static inline int hostKeyQuestions = 0;
    static inline int secretQuestions = 0;
    static inline QStringList secrets;

    wt::ssh::HostKeyDecision confirmHostKey(const QString&, int, const wt::ssh::HostKey&, wt::ssh::HostKeyCheck) override {
        ++hostKeyQuestions;
        return decision;
    }
    std::optional<QString> askSecret(const QString&, const QString&) override {
        ++secretQuestions;
        if (secrets.isEmpty()) {
            return std::nullopt;
        }
        return secrets.takeFirst();
    }
    static void reset() {
        decision = wt::ssh::HostKeyDecision::AcceptAndSave;
        hostKeyQuestions = 0;
        secretQuestions = 0;
        secrets.clear();
    }
};

bool screenContains(const TerminalSession& session, const QString& text) {
    const std::string screen = session.terminal().screenText();
    return QString::fromStdString(screen).contains(text);
}

// Scrollback + screen, for notices that may have scrolled away.
QString allText(const TerminalSession& session) {
    const auto& t = session.terminal();
    QString text;
    for (std::size_t i = 0; i < t.scrollbackSize(); ++i) {
        text += QString::fromStdString(t.lineToText(t.scrollbackLine(i))) + QLatin1Char('\n');
    }
    return text + QString::fromStdString(t.screenText());
}

SshBackend* sshBackend(TerminalPane* pane) {
    return qobject_cast<SshBackend*>(pane->session()->backend());
}

bool isConnected(TerminalPane* pane) {
    const SshBackend* backend = sshBackend(pane);
    return backend && backend->state() == SshBackend::State::Connected;
}

} // namespace

class SshAppTests : public QObject {
    Q_OBJECT

    wt::test::SshdFixture m_sshd;
    bool m_available = false;
    QTemporaryDir m_config;

    AppContext* makeContext() {
        auto* context = new AppContext(m_config.path(), this);
        Settings settings = context->settings();
        settings.confirmCloseRunning = false;
        settings.knownHostsFile = m_sshd.knownHosts();
        context->setSettings(settings);
        context->setVaultCost(1024, 1);
        context->setMasterPasswordProvider([](bool, QWidget*) { return std::optional<QString>(QStringLiteral("master")); });
        return context;
    }

    SessionProfile profile(const QString& name) const {
        SessionProfile p;
        p.name = name.toStdString();
        p.kind = SessionKind::Ssh;
        p.ssh.host = "127.0.0.1";
        p.ssh.port = m_sshd.port();
        p.ssh.username = wt::test::SshdFixture::user();
        p.ssh.auth = AuthMethod::PublicKey;
        p.ssh.identityFile = m_sshd.userKey();
        p.ssh.remoteCommand = "/bin/sh";
        p.ssh.reconnect.initialDelayMs = 200;
        p.ssh.reconnect.maxDelayMs = 400;
        return p;
    }

    std::unique_ptr<MainWindow> window(AppContext* context) {
        MainWindow::Options options;
        options.session.program = QStringLiteral("/bin/sh");
        options.session.arguments = {QStringLiteral("-c"), QStringLiteral("exec sleep 60")};
        auto w = std::make_unique<MainWindow>(options, context);
        w->start();
        w->show();
        return w;
    }

private Q_SLOTS:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        ::unsetenv("SSH_AUTH_SOCK");
        std::string reason;
        m_available = m_sshd.start(&reason);
        if (!m_available) {
            QSKIP(reason.c_str());
        }
        SshPrompter::setFactory([](QWidget* parent) { return std::make_shared<ScriptedPrompter>(parent); });
    }

    void init() { ScriptedPrompter::reset(); }

    void sshProfileRunsAShell() {
        std::filesystem::remove(m_sshd.knownHosts());
        auto w = window(makeContext());
        TerminalPane* pane = w->openProfile(profile(QStringLiteral("box")));
        QVERIFY(sshBackend(pane));
        QTRY_VERIFY_WITH_TIMEOUT(isConnected(pane), 15000);
        QCOMPARE(ScriptedPrompter::hostKeyQuestions, 1);
        QVERIFY(wt::platform::readFile(m_sshd.knownHosts()).has_value());
        pane->session()->sendBytes("echo hi-$((40+2)); stty size\r");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("hi-42")), 10000);
        const auto& t = pane->session()->terminal();
        const QString size = QStringLiteral("%1 %2").arg(t.rows()).arg(t.columns());
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), size), 10000);
        QCOMPARE(w->tabs()->tabText(w->tabs()->currentIndex()), QStringLiteral("box"));

        pane->session()->sendBytes("exit 5\r");
        QTRY_VERIFY_WITH_TIMEOUT(pane->session()->hasFinished(), 10000);
        QCOMPARE(pane->session()->exitCode().value_or(0), 5);
        QTRY_VERIFY(allText(*pane->session()).contains(QStringLiteral("exit code 5")));
        QVERIFY(w->isVisible()); // remote panes stay open

        // Manual reconnect re-opens the shell without asking about the (now known) host key.
        w->reconnectActive();
        QTRY_VERIFY_WITH_TIMEOUT(isConnected(pane), 15000);
        QCOMPARE(ScriptedPrompter::hostKeyQuestions, 1);
        pane->session()->sendBytes("echo again-$((1+1))\r");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("again-2")), 10000);
    }

    void immediatelyExitingCommandFinishes() {
        // The remote command ends before the GUI adopts the shell: it must not look connected.
        auto w = window(makeContext());
        SessionProfile p = profile(QStringLiteral("quick"));
        p.ssh.remoteCommand = "exit 7";
        TerminalPane* pane = w->openProfile(p);
        QTRY_VERIFY_WITH_TIMEOUT(pane->session()->hasFinished(), 15000);
        QCOMPARE(pane->session()->exitCode().value_or(0), 7);
        QVERIFY(!isConnected(pane));
    }

    void rejectedHostKeyIsReported() {
        std::filesystem::remove(m_sshd.knownHosts());
        ScriptedPrompter::decision = wt::ssh::HostKeyDecision::Reject;
        auto w = window(makeContext());
        TerminalPane* pane = w->openProfile(profile(QStringLiteral("rejected")));
        QTRY_VERIFY_WITH_TIMEOUT(pane->session()->hasFinished(), 15000);
        QVERIFY(allText(*pane->session()).contains(QStringLiteral("not accepted")));
        const SshBackend* backend = sshBackend(pane);
        QVERIFY(backend);
        QCOMPARE(backend->reconnectAttempts(), 0); // no retry loop
    }

    void vaultSuppliesThePassphrase() {
        AppContext* context = makeContext();
        // Store the passphrase through the session dialog, as a user would.
        SessionProfile p = profile(QStringLiteral("encrypted"));
        p.ssh.identityFile = m_sshd.encryptedUserKey();
        SessionEditDialog dialog(context, p);
        dialog.findChild<QLineEdit*>(QStringLiteral("secret"))->setText(QString::fromLatin1(wt::test::SshdFixture::kPassphrase));
        dialog.findChild<QCheckBox*>(QStringLiteral("storeSecret"))->setChecked(true);
        dialog.accept();
        const SessionProfile stored = dialog.profile();
        QVERIFY(!stored.ssh.credentialId.empty());
        QVERIFY(context->vaultExists());
        const std::string file = *wt::platform::readFile(context->vaultPath().toStdString());
        QVERIFY(file.find(wt::test::SshdFixture::kPassphrase) == std::string::npos);

        // A fresh context must unlock the vault (master password) but never asks for the passphrase.
        auto* reopened = new AppContext(m_config.path(), this);
        reopened->setVaultCost(1024, 1);
        int masterQuestions = 0;
        reopened->setMasterPasswordProvider([&masterQuestions](bool creating, QWidget*) {
            ++masterQuestions;
            return creating ? std::nullopt : std::optional<QString>(QStringLiteral("master"));
        });
        auto w = window(reopened);
        TerminalPane* pane = w->openProfile(stored);
        QTRY_VERIFY_WITH_TIMEOUT(isConnected(pane), 15000);
        QCOMPARE(masterQuestions, 1);
        QCOMPARE(ScriptedPrompter::secretQuestions, 0);

        // Without the vault the passphrase is asked for (wrong first, then right).
        SessionProfile unsaved = stored;
        unsaved.ssh.credentialId.clear();
        ScriptedPrompter::secrets = {QStringLiteral("wrong"), QString::fromLatin1(wt::test::SshdFixture::kPassphrase)};
        TerminalPane* asked = w->openProfile(unsaved);
        QTRY_VERIFY_WITH_TIMEOUT(isConnected(asked), 15000);
        QCOMPARE(ScriptedPrompter::secretQuestions, 2);
    }

    void droppedConnectionReconnects() {
        wt::test::ProxyServer proxy(wt::test::ProxyServer::Kind::Socks5);
        QVERIFY(proxy.start());
        auto w = window(makeContext());
        SessionProfile p = profile(QStringLiteral("flaky"));
        p.ssh.keepAliveSeconds = 1;
        p.ssh.proxy.kind = ProxyKind::Socks5;
        p.ssh.proxy.host = "127.0.0.1";
        p.ssh.proxy.port = proxy.port();
        TerminalPane* pane = w->openProfile(p);
        QTRY_VERIFY_WITH_TIMEOUT(isConnected(pane), 15000);
        pane->session()->sendBytes("echo before-drop\r");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("before-drop")), 10000);
        proxy.dropAll();
        QTRY_VERIFY_WITH_TIMEOUT(allText(*pane->session()).contains(QStringLiteral("Reconnecting in")), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(allText(*pane->session()).contains(QStringLiteral("[Reconnected")), 15000);
        QVERIFY(isConnected(pane));
        QVERIFY(!pane->session()->hasFinished());
        pane->session()->sendBytes("echo after-$((2*21))\r");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("after-42")), 10000);
        QVERIFY(proxy.connections() >= 2);
    }

    void jumpHostProfileConnects() {
        AppContext* context = makeContext();
        SessionProfile bastion = profile(QStringLiteral("bastion"));
        bastion.id = "bastion";
        context->sessions().upsert(bastion);
        SessionProfile inner = profile(QStringLiteral("inner"));
        inner.ssh.host = "localhost";
        inner.ssh.proxy.kind = ProxyKind::JumpHost;
        inner.ssh.proxy.jumpProfileId = "bastion";
        ScriptedPrompter::decision = wt::ssh::HostKeyDecision::AcceptOnce;
        auto w = window(context);
        TerminalPane* pane = w->openProfile(inner);
        QTRY_VERIFY_WITH_TIMEOUT(isConnected(pane), 15000);
        pane->session()->sendBytes("echo through-$((6*7))\r");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("through-42")), 10000);
    }

    void sftpBrowserTransfersFiles() {
        auto w = window(makeContext());
        TerminalPane* pane = w->openProfile(profile(QStringLiteral("files")));
        QTRY_VERIFY_WITH_TIMEOUT(isConnected(pane), 15000);
        w->action(Action::ToggleFileBrowser)->trigger();
        SftpPanel* panel = w->fileBrowser();
        QVERIFY(panel);
        QTRY_VERIFY_WITH_TIMEOUT(panel->isConnected() && !panel->isBusy(), 15000);
        const SshBackend* backend = sshBackend(pane);
        QVERIFY(backend);
        QCOMPARE(panel->target(), backend->description());

        const QString remoteDir = QString::fromStdString(m_sshd.dir());
        panel->navigate(remoteDir);
        QTRY_COMPARE_WITH_TIMEOUT(panel->currentPath(), remoteDir, 10000);
        QVERIFY(panel->entryNames().contains(QStringLiteral("authorized_keys")));

        QTemporaryDir local;
        const QString upload = local.filePath(QStringLiteral("upload.txt"));
        QVERIFY(wt::platform::writeFileAtomic(upload.toStdString(), std::string(200000, 'u')).isOk());
        QSignalSpy finished(panel, &SftpPanel::operationFinished);
        panel->uploadFiles({upload});
        QTRY_VERIFY_WITH_TIMEOUT(panel->entryNames().contains(QStringLiteral("upload.txt")), 10000);
        QVERIFY(!finished.isEmpty() && finished.first().at(0).toBool());

        panel->makeDirectory(QStringLiteral("folder"));
        QTRY_VERIFY_WITH_TIMEOUT(panel->entryNames().contains(QStringLiteral("folder")), 10000);
        panel->renameEntry(QStringLiteral("upload.txt"), QStringLiteral("folder/moved.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(!panel->entryNames().contains(QStringLiteral("upload.txt")), 10000);
        panel->navigate(remoteDir + QStringLiteral("/folder"));
        QTRY_VERIFY_WITH_TIMEOUT(panel->entryNames().contains(QStringLiteral("moved.txt")), 10000);

        const QString downloaded = local.filePath(QStringLiteral("back.txt"));
        panel->download(QStringLiteral("moved.txt"), downloaded);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(downloaded) && !panel->isBusy(), 10000);
        QCOMPARE(QFile(downloaded).size(), qint64(200000));

        panel->goUp();
        QTRY_COMPARE_WITH_TIMEOUT(panel->currentPath(), remoteDir, 10000);
        panel->removeEntries({QStringLiteral("folder")}); // recursive
        QTRY_VERIFY_WITH_TIMEOUT(!panel->entryNames().contains(QStringLiteral("folder")), 10000);
        QVERIFY(!QFile::exists(remoteDir + QStringLiteral("/folder")));

        // Errors are reported, not fatal.
        finished.clear();
        panel->renameEntry(QStringLiteral("does-not-exist"), QStringLiteral("x"));
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
        QVERIFY(!finished.last().at(0).toBool());

        w->action(Action::ToggleFileBrowser)->trigger();
        QVERIFY(!w->action(Action::ToggleFileBrowser)->isChecked());
    }
};

QTEST_MAIN(SshAppTests)
#include "SshAppTests.moc"

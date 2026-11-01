// GUI integration tests for Telnet and serial sessions: a scripted in-process Telnet server and,
// on POSIX, pseudo-terminals standing in for serial devices.

#include "AppContext.h"
#include "MainWindow.h"
#include "SerialBackend.h"
#include "SessionDialogs.h"
#include "TelnetBackend.h"
#include "TerminalPane.h"
#include "TerminalSession.h"

#include "wt/net/Socket.h"
#include "wt/net/Telnet.h"

#include <QComboBox>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace wt::session;
using namespace wt::net::telnet;
using namespace std::chrono_literals;

namespace {

std::string cmd(std::uint8_t verb, std::uint8_t option) {
    return std::string{static_cast<char>(kIac), static_cast<char>(verb), static_cast<char>(option)};
}

QString allText(const TerminalSession& session) {
    const auto& t = session.terminal();
    QString text;
    for (std::size_t i = 0; i < t.scrollbackSize(); ++i) {
        text += QString::fromStdString(t.lineToText(t.scrollbackLine(i))) + QLatin1Char('\n');
    }
    return text + QString::fromStdString(t.screenText());
}

bool screenContains(const TerminalSession& session, const QString& text) {
    return QString::fromStdString(session.terminal().screenText()).contains(text);
}

// A Telnet server that negotiates like a typical telnetd (DO NAWS, DO TTYPE, WILL ECHO, WILL SGA),
// asks for the terminal type, echoes what it receives and records the raw bytes.
class FakeTelnetServer {
public:
    bool negotiate = true; // send the options above
    bool echo = true;      // echo received data (as a server with WILL ECHO does)

    ~FakeTelnetServer() { stop(); }

    bool start() {
        if (!wt::net::listenLoopback(0, m_listener, m_port)) {
            return false;
        }
        m_thread = std::thread([this] { run(); });
        return true;
    }
    void stop() {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
        for (auto& worker : m_workers) {
            worker.join();
        }
        m_workers.clear();
    }
    std::uint16_t port() const { return m_port; }
    int connections() const { return m_connections.load(); }
    std::string received() {
        std::lock_guard lock(m_mutex);
        return m_received;
    }
    void send(const std::string& data) {
        std::lock_guard lock(m_mutex);
        m_outbox += data;
    }
    // Ends the current connection: orderly (FIN) or abortive (RST, looks like a network failure).
    void closeClient(bool reset) {
        std::lock_guard lock(m_mutex);
        m_closeRequest = reset ? 2 : 1;
    }

private:
    void run() {
        while (!m_stop) {
            if (!wt::net::waitReadable(m_listener.native(), 50ms)) {
                continue;
            }
            auto client = std::make_shared<wt::net::Socket>();
            if (!wt::net::acceptConnection(m_listener, *client)) {
                continue;
            }
            ++m_connections;
            m_workers.emplace_back([this, client] { serve(*client); }); // one thread per connection
        }
    }

    void serve(wt::net::Socket& client) {
        if (negotiate) {
            client.sendAll(cmd(kDo, kOptNaws) + cmd(kDo, kOptTerminalType) + cmd(kWill, kOptEcho) +
                           cmd(kWill, kOptSga) +
                           std::string{static_cast<char>(kIac), static_cast<char>(kSb), static_cast<char>(kOptTerminalType),
                                       static_cast<char>(kSend), static_cast<char>(kIac), static_cast<char>(kSe)});
        }
        client.sendAll("Welcome\r\nlogin: ");
        int state = 0; // 0 data, 1 IAC, 2 option byte, 3 SB, 4 IAC inside SB
        char buffer[4096];
        while (!m_stop) {
            std::string outbox;
            int close = 0;
            {
                std::lock_guard lock(m_mutex);
                outbox.swap(m_outbox);
                close = std::exchange(m_closeRequest, 0);
            }
            if (!outbox.empty()) {
                client.sendAll(outbox);
            }
            if (close == 2) {
                linger value{1, 0}; // RST on close
                ::setsockopt(client.native(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&value),
                             sizeof(value));
                client.close();
                return;
            }
            if (close == 1) {
                client.shutdown();
                client.close();
                return;
            }
            if (!wt::net::waitReadable(client.native(), 20ms)) {
                continue;
            }
            std::size_t n = 0;
            if (!client.receiveSome(buffer, sizeof(buffer), n) || n == 0) {
                return;
            }
            std::string data;
            for (std::size_t i = 0; i < n; ++i) {
                const auto byte = static_cast<std::uint8_t>(buffer[i]);
                switch (state) {
                case 0:
                    if (byte == kIac) {
                        state = 1;
                    } else if (byte != 0) {
                        data += static_cast<char>(byte);
                    }
                    break;
                case 1:
                    state = byte == kSb ? 3 : (byte >= kWill && byte <= kDont) ? 2 : 0;
                    break;
                case 2: state = 0; break;
                case 3: state = byte == kIac ? 4 : 3; break;
                case 4: state = byte == kSe ? 0 : 3; break;
                }
            }
            {
                std::lock_guard lock(m_mutex);
                m_received.append(buffer, n);
            }
            if (echo && !data.empty()) {
                client.sendAll(data);
            }
        }
    }

    wt::net::Socket m_listener;
    std::uint16_t m_port = 0;
    std::thread m_thread;
    std::vector<std::thread> m_workers; // touched by the accept thread and stop() only
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_connections{0};
    std::mutex m_mutex;
    std::string m_received;
    std::string m_outbox;
    int m_closeRequest = 0;
};

TelnetBackend::Options telnetOptions(std::uint16_t port) {
    TelnetBackend::Options options;
    options.host = "127.0.0.1";
    options.port = port;
    options.timeout = 5000ms;
    options.telnet.terminalTypes = {"XTERM-256COLOR", "VT100"};
    return options;
}

TelnetBackend* telnetBackend(TerminalSession& session) {
    return qobject_cast<TelnetBackend*>(session.backend());
}

#ifndef _WIN32
struct PtyPair {
    int master = -1;
    std::string slave;
    PtyPair() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0) {
            if (const char* name = ::ptsname(master)) {
                slave = name;
            }
        }
    }
    ~PtyPair() { close(); }
    void close() {
        if (master >= 0) {
            ::close(master);
            master = -1;
        }
    }
    void write(const std::string& data) const {
        [[maybe_unused]] const auto n = ::write(master, data.data(), data.size());
    }
    // Everything the "device" received so far (non-blocking).
    std::string drain() const {
        std::string out;
        char buffer[256];
        for (;;) {
            pollfd pfd{master, POLLIN, 0};
            if (::poll(&pfd, 1, 0) <= 0) {
                break;
            }
            const ssize_t n = ::read(master, buffer, sizeof(buffer));
            if (n <= 0) {
                break;
            }
            out.append(buffer, static_cast<std::size_t>(n));
        }
        return out;
    }
};
#endif

} // namespace

class ProtocolAppTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_config;

private Q_SLOTS:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void telnetNegotiatesEchoesAndReportsSize() {
        FakeTelnetServer server;
        QVERIFY(server.start());
        TerminalSession session;
        session.resize(30, 100);
        QVERIFY(session.start(std::make_unique<TelnetBackend>(telnetOptions(server.port()))));
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, QStringLiteral("login:")), 10000);
        TelnetBackend* backend = telnetBackend(session);
        QVERIFY(backend);
        QCOMPARE(backend->state(), TelnetBackend::State::Connected);
        // The client agreed to NAWS (reporting 100x30) and TTYPE, and answered the TTYPE request.
        const std::string naws = std::string{static_cast<char>(kIac), static_cast<char>(kSb),
                                             static_cast<char>(kOptNaws), 0, 100, 0, 30, static_cast<char>(kIac),
                                             static_cast<char>(kSe)};
        QTRY_VERIFY_WITH_TIMEOUT(server.received().find(naws) != std::string::npos, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.received().find("XTERM-256COLOR") != std::string::npos, 5000);
        QVERIFY(server.received().find(cmd(kDo, kOptEcho)) != std::string::npos);
        QVERIFY(!backend->localEchoActive()); // the server echoes

        session.sendText(QStringLiteral("alice\r"));
        QTRY_VERIFY_WITH_TIMEOUT(server.received().find("alice\r\n") != std::string::npos, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, QStringLiteral("login: alice")), 5000);
        // 0xFF in user data is escaped on the wire.
        session.sendBytes("\xff");
        QTRY_VERIFY_WITH_TIMEOUT(server.received().find("\xff\xff") != std::string::npos, 5000);

        // Window size changes are reported.
        session.resize(40, 132);
        const std::string resized = std::string{static_cast<char>(kIac), static_cast<char>(kSb),
                                                static_cast<char>(kOptNaws), 0, static_cast<char>(132), 0, 40,
                                                static_cast<char>(kIac), static_cast<char>(kSe)};
        QTRY_VERIFY_WITH_TIMEOUT(server.received().find(resized) != std::string::npos, 5000);

        // Break goes out as IAC BRK.
        backend->sendCommand(wt::net::TelnetCommand::Break);
        QTRY_VERIFY_WITH_TIMEOUT(server.received().find(std::string{static_cast<char>(kIac), static_cast<char>(kBreak)}) !=
                                     std::string::npos,
                                 5000);

        // The server logs us out: the session ends normally.
        QSignalSpy finished(&session, &TerminalSession::finished);
        server.send("bye\r\n");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, QStringLiteral("bye")), 5000);
        server.closeClient(false);
        QTRY_VERIFY_WITH_TIMEOUT(session.hasFinished(), 10000);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().first().toInt(), 0);
        QVERIFY(allText(session).contains(QStringLiteral("Connection closed")));
        QCOMPARE(server.connections(), 1);
    }

    void telnetLocalEchoWhenTheServerDoesNotEcho() {
        FakeTelnetServer server;
        server.negotiate = false;
        server.echo = false;
        QVERIFY(server.start());
        TerminalSession session;
        QVERIFY(session.start(std::make_unique<TelnetBackend>(telnetOptions(server.port()))));
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, QStringLiteral("login:")), 10000);
        QVERIFY(telnetBackend(session)->localEchoActive());
        session.sendText(QStringLiteral("bob\r"));
        QTRY_VERIFY_WITH_TIMEOUT(server.received().find("bob\r\n") != std::string::npos, 5000);
        QVERIFY(screenContains(session, QStringLiteral("login: bob"))); // echoed locally
    }

    void telnetConnectFailureFinishesTheSession() {
        std::uint16_t port = 0;
        {
            wt::net::Socket listener;
            QVERIFY(wt::net::listenLoopback(0, listener, port));
        } // nothing listens there any more
        TerminalSession session;
        QSignalSpy finished(&session, &TerminalSession::finished);
        QVERIFY(session.start(std::make_unique<TelnetBackend>(telnetOptions(port))));
        QTRY_VERIFY_WITH_TIMEOUT(session.hasFinished(), 10000);
        QCOMPARE(finished.first().first().toInt(), -1);
        QVERIFY(allText(session).contains(QStringLiteral("cannot connect")));
    }

    void telnetReconnectsAfterAConnectionFailure() {
        FakeTelnetServer server;
        QVERIFY(server.start());
        auto options = telnetOptions(server.port());
        options.reconnect = {true, 5, 100, 200};
        TerminalSession session;
        QVERIFY(session.start(std::make_unique<TelnetBackend>(options)));
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, QStringLiteral("login:")), 10000);
        server.closeClient(true); // connection reset
        QTRY_VERIFY_WITH_TIMEOUT(allText(session).contains(QStringLiteral("Reconnecting in")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(allText(session).contains(QStringLiteral("[Reconnected")), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(server.connections(), 2, 5000);
        QVERIFY(!session.hasFinished());
        session.sendText(QStringLiteral("again\r"));
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, QStringLiteral("again")), 5000);
    }

    void telnetProfileOpensFromTheWindow() {
        FakeTelnetServer server;
        QVERIFY(server.start());
        AppContext* context = AppContext::createEphemeral(this);
        Settings settings = context->settings();
        settings.confirmCloseRunning = false;
        context->setSettings(settings);
        MainWindow::Options options;
        options.session.program = QStringLiteral("/bin/sh");
        options.session.arguments = {QStringLiteral("-c"), QStringLiteral("exec sleep 60")};
        MainWindow window(options, context);
        QVERIFY(window.start());
        window.show();
        SessionProfile profile;
        profile.id = "telnet-1";
        profile.name = "switch";
        profile.kind = SessionKind::Telnet;
        profile.telnet.host = "127.0.0.1";
        profile.telnet.port = server.port();
        TerminalPane* pane = window.openProfile(profile);
        QVERIFY(pane);
        QVERIFY(pane->isRemote());
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(*pane->session(), QStringLiteral("login:")), 10000);
        QCOMPARE(pane->title(), QStringLiteral("switch"));
        window.action(Action::SendBreak)->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(server.received().find(std::string{static_cast<char>(kIac), static_cast<char>(kBreak)}) !=
                                     std::string::npos,
                                 5000);
        // Splitting a Telnet pane opens a second connection.
        window.splitActive(Qt::Horizontal);
        QTRY_COMPARE_WITH_TIMEOUT(server.connections(), 2, 10000);
    }

    void sessionDialogEditsTelnetAndSerialProfiles() {
        AppContext* context = AppContext::createEphemeral(this);
        SessionProfile profile;
        profile.kind = SessionKind::Serial;
        SessionEditDialog dialog(context, profile);
        auto* device = dialog.findChild<QComboBox*>(QStringLiteral("serialDevice"));
        auto* baud = dialog.findChild<QComboBox*>(QStringLiteral("baudRate"));
        auto* parity = dialog.findChild<QComboBox*>(QStringLiteral("parity"));
        auto* newline = dialog.findChild<QComboBox*>(QStringLiteral("serialNewline"));
        QVERIFY(device && baud && parity && newline);
        device->setCurrentText(QStringLiteral("/dev/ttyUSB7"));
        baud->setCurrentText(QStringLiteral("57600"));
        parity->setCurrentIndex(parity->findData(static_cast<int>(wt::platform::Parity::Even)));
        newline->setCurrentIndex(newline->findData(static_cast<int>(NewlineMode::CrLf)));
        SessionProfile edited = dialog.profile();
        QCOMPARE(edited.kind, SessionKind::Serial);
        QCOMPARE(edited.serial.line.device, std::string("/dev/ttyUSB7"));
        QCOMPARE(edited.serial.line.baudRate, 57600);
        QCOMPARE(edited.serial.line.parity, wt::platform::Parity::Even);
        QCOMPARE(edited.serial.newline, NewlineMode::CrLf);
        QCOMPARE(edited.name, std::string("/dev/ttyUSB7"));
        QVERIFY(dialog.problems().isEmpty());

        auto* kind = dialog.findChild<QComboBox*>(QStringLiteral("kind"));
        kind->setCurrentIndex(kind->findData(static_cast<int>(SessionKind::Telnet)));
        dialog.findChild<QLineEdit*>(QStringLiteral("name"))->setText(QStringLiteral("router"));
        QVERIFY(!dialog.problems().isEmpty()); // no host yet
        dialog.findChild<QLineEdit*>(QStringLiteral("telnetHost"))->setText(QStringLiteral("192.0.2.7"));
        dialog.findChild<QSpinBox*>(QStringLiteral("telnetPort"))->setValue(2323);
        auto* echo = dialog.findChild<QComboBox*>(QStringLiteral("telnetEcho"));
        echo->setCurrentIndex(echo->findData(static_cast<int>(EchoMode::On)));
        edited = dialog.profile();
        QCOMPARE(edited.kind, SessionKind::Telnet);
        QCOMPARE(edited.telnet.host, std::string("192.0.2.7"));
        QCOMPARE(edited.telnet.port, std::uint16_t(2323));
        QCOMPARE(edited.telnet.localEcho, EchoMode::On);
        QCOMPARE(edited.target(), std::string("telnet://192.0.2.7:2323"));
        QVERIFY(dialog.problems().isEmpty());
    }

#ifndef _WIN32
    void serialSessionExchangesData() {
        PtyPair device;
        QVERIFY(!device.slave.empty());
        SerialBackend::Options options;
        options.line.device = device.slave;
        options.newline = NewlineMode::CrLf;
        options.localEcho = EchoMode::On;
        TerminalSession session;
        QVERIFY(session.start(std::make_unique<SerialBackend>(options)));
        QTRY_VERIFY_WITH_TIMEOUT(allText(session).contains(QStringLiteral("Connected to")), 5000);
        device.write("U-Boot 2024.01\r\n=> ");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, QStringLiteral("U-Boot 2024.01")), 5000);
        session.sendText(QStringLiteral("help\r"));
        std::string received;
        QTRY_VERIFY_WITH_TIMEOUT((received += device.drain(), received == "help\r\n"), 5000);
        QVERIFY(screenContains(session, QStringLiteral("=> help"))); // local echo
        auto* backend = qobject_cast<SerialBackend*>(session.backend());
        QVERIFY(backend && backend->isOpen());
        wt::platform::SerialConfig line = options.line;
        line.baudRate = 9600;
        QString error;
        QVERIFY2(backend->reconfigure(line, &error), qPrintable(error));
        QCOMPARE(backend->options().line.baudRate, 9600);
    }

    void serialDeviceDisappearsAndComesBack() {
        // A stable name (like /dev/serial/by-id/...) that is re-pointed when the "device" returns.
        QTemporaryDir dir;
        const std::string link = dir.filePath(QStringLiteral("ttyBoard")).toStdString();
        auto first = std::make_unique<PtyPair>();
        QVERIFY(::symlink(first->slave.c_str(), link.c_str()) == 0);
        SerialBackend::Options options;
        options.line.device = link;
        options.reconnect = {true, 50, 100, 200};
        TerminalSession session;
        session.resize(24, 300); // keep the long notices on one line
        QVERIFY(session.start(std::make_unique<SerialBackend>(options)));
        first->write("first boot\r\n");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, QStringLiteral("first boot")), 5000);

        first.reset(); // unplugged
        QTRY_VERIFY_WITH_TIMEOUT(allText(session).contains(QStringLiteral("Waiting for the device")), 5000);
        QVERIFY(!session.hasFinished());
        PtyPair second;
        ::unlink(link.c_str());
        QVERIFY(::symlink(second.slave.c_str(), link.c_str()) == 0);
        QTRY_VERIFY_WITH_TIMEOUT(allText(session).contains(QStringLiteral("[Reconnected")), 10000);
        second.write("second boot\r\n");
        QTRY_VERIFY_WITH_TIMEOUT(screenContains(session, QStringLiteral("second boot")), 5000);
        session.sendText(QStringLiteral("x"));
        std::string received;
        QTRY_VERIFY_WITH_TIMEOUT((received += second.drain(), received == "x"), 5000);
    }

    void serialDeviceGoneWithoutReconnectFinishes() {
        auto device = std::make_unique<PtyPair>();
        SerialBackend::Options options;
        options.line.device = device->slave;
        options.reconnect.enabled = false;
        TerminalSession session;
        QSignalSpy finished(&session, &TerminalSession::finished);
        QVERIFY(session.start(std::make_unique<SerialBackend>(options)));
        device.reset();
        QTRY_VERIFY_WITH_TIMEOUT(session.hasFinished(), 5000);
        QCOMPARE(finished.count(), 1);
        QVERIFY(allText(session).contains(QStringLiteral("disconnected")));
    }

    void serialOpenFailureIsReported() {
        TerminalSession session;
        SerialBackend::Options options;
        options.line.device = "/dev/does-not-exist-wt";
        QVERIFY(!session.start(std::make_unique<SerialBackend>(options)));
        QVERIFY(session.errorString().contains(QStringLiteral("does-not-exist")));
    }
#endif
};

QTEST_MAIN(ProtocolAppTests)
#include "ProtocolAppTests.moc"

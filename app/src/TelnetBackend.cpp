#include "TelnetBackend.h"

#include <QCoreApplication>
#include <QPointer>
#include <QSocketNotifier>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>

namespace {

// How long teardown waits for a cancelled connect (DNS lookups cannot be interrupted).
constexpr int kWorkerJoinMs = 300;
// A connection only resets the reconnect backoff after it has worked this long.
constexpr qint64 kStableConnectionMs = 10000;
// Bytes read per event-loop pass before yielding.
constexpr std::size_t kReadBudget = 256 * 1024;

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

std::string localUserName() {
    for (const char* name : {"USER", "LOGNAME", "USERNAME"}) {
        if (const char* value = std::getenv(name); value && *value) {
            return value;
        }
    }
    return {};
}

} // namespace

struct TelnetBackend::Attempt {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    std::mutex mutex;
    wt::net::NativeSocket socket = wt::net::kInvalidSocket; // to abort a slow proxy exchange

    void abort() {
        cancelled = true;
        std::lock_guard lock(mutex);
        wt::net::shutdownSocket(socket);
    }
};

TelnetBackend::TelnetBackend(Options options, QObject* parent)
    : SessionBackend(parent), m_options(std::move(options)), m_backoff(m_options.reconnect) {
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &TelnetBackend::startAttempt);
}

TelnetBackend::~TelnetBackend() {
    terminate();
}

TelnetBackend::Options TelnetBackend::fromProfile(const wt::session::SessionProfile& profile,
                                                  const std::string& proxyPassword) {
    const auto& t = profile.telnet;
    Options options;
    options.host = t.host;
    options.port = t.port;
    options.timeout = std::chrono::seconds(t.connectTimeoutSeconds);
    if (t.proxy.kind == wt::session::ProxyKind::Socks5 || t.proxy.kind == wt::session::ProxyKind::HttpConnect) {
        options.proxy.type = t.proxy.kind == wt::session::ProxyKind::Socks5 ? wt::net::ProxyType::Socks5
                                                                           : wt::net::ProxyType::HttpConnect;
        options.proxy.host = t.proxy.host;
        options.proxy.port = t.proxy.port;
        options.proxy.username = t.proxy.username;
        options.proxy.password = proxyPassword;
    }
    const std::string type = t.terminalType.empty() ? "xterm-256color" : t.terminalType;
    options.telnet.terminalTypes = {upper(type)};
    if (upper(type) != "XTERM") {
        options.telnet.terminalTypes.emplace_back("XTERM");
    }
    options.telnet.terminalTypes.emplace_back("VT100");
    options.telnet.crlf = t.crlf;
    options.telnet.environment = t.environment;
    const bool hasUser = std::any_of(t.environment.begin(), t.environment.end(),
                                     [](const auto& entry) { return entry.first == "USER"; });
    if (!hasUser) {
        if (const std::string user = localUserName(); !user.empty()) {
            options.telnet.environment.emplace_back("USER", user);
        }
    }
    options.localEcho = t.localEcho;
    options.reconnect = t.reconnect;
    return options;
}

QString TelnetBackend::description() const {
    QString text = QStringLiteral("telnet://%1").arg(QString::fromStdString(m_options.host));
    if (m_options.port != 23) {
        text += QStringLiteral(":%1").arg(m_options.port);
    }
    return text;
}

void TelnetBackend::setState(State state) {
    if (m_state != state) {
        m_state = state;
        Q_EMIT stateChanged(state);
    }
}

bool TelnetBackend::start(int rows, int columns, QString* error) {
    if (m_options.host.empty()) {
        if (error) {
            *error = tr("No host given");
        }
        return false;
    }
    m_rows = rows;
    m_columns = columns;
    startAttempt();
    return true;
}

void TelnetBackend::joinWorker() {
    if (m_attempt) {
        m_attempt->abort();
    }
    if (!m_worker.joinable()) {
        return;
    }
    if (m_attempt) {
        for (int waited = 0; waited < kWorkerJoinMs && !m_attempt->done.load(); waited += 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!m_attempt->done.load()) {
            m_worker.detach(); // it only holds shared state and reports through a guarded pointer
            return;
        }
    }
    m_worker.join();
}

void TelnetBackend::startAttempt() {
    if (m_state == State::Connecting || m_state == State::Connected) {
        return;
    }
    joinWorker();
    setState(State::Connecting);
    Q_EMIT notice(tr("Connecting to %1...").arg(description()));
    auto attempt = std::make_shared<Attempt>();
    m_attempt = attempt;
    QPointer<TelnetBackend> self(this);
    const Options options = m_options;
    m_worker = std::thread([attempt, options, self] {
        wt::net::Socket socket;
        const auto status = wt::net::connectThrough(options.proxy, options.host, options.port, options.timeout, socket,
                                                    &attempt->cancelled, [attempt](wt::net::NativeSocket fd) {
                                                        std::lock_guard lock(attempt->mutex);
                                                        attempt->socket = fd;
                                                        if (attempt->cancelled) {
                                                            wt::net::shutdownSocket(fd);
                                                        }
                                                    });
        {
            std::lock_guard lock(attempt->mutex);
            attempt->socket = wt::net::kInvalidSocket; // from now on the socket may be closed and reused
        }
        if (attempt->cancelled) {
            attempt->done = true;
            return;
        }
        const wt::net::NativeSocket fd = status ? socket.release() : wt::net::kInvalidSocket;
        const QString error = status ? QString() : QString::fromStdString(status.message());
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, attempt, fd, error] {
            TelnetBackend* backend = self.data();
            if (!backend || backend->m_attempt != attempt) {
                wt::net::Socket orphan(fd); // closes it
                return;
            }
            if (fd != wt::net::kInvalidSocket) {
                backend->onConnected(fd);
            } else {
                backend->onConnectFailed(error);
            }
        });
        attempt->done = true;
    });
}

void TelnetBackend::onConnected(wt::net::NativeSocket fd) {
    m_attempt.reset();
    if (m_state != State::Connecting) {
        wt::net::Socket orphan(fd);
        return;
    }
    m_socket = wt::net::Socket(fd);
    m_socket.setNonBlocking(true);
    m_socket.setKeepAlive(true, 60, 10, 6);
    wt::net::TelnetConfig config = m_options.telnet;
    config.rows = m_rows;
    config.columns = m_columns;
    m_protocol = std::make_unique<wt::net::TelnetProtocol>(config);
    m_pendingWrite.clear();
    m_readNotifier = new QSocketNotifier(static_cast<qintptr>(fd), QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &TelnetBackend::onReadable);
    m_writeNotifier = new QSocketNotifier(static_cast<qintptr>(fd), QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &TelnetBackend::flushWrites);
    if (m_everConnected) {
        Q_EMIT notice(tr("[Reconnected to %1]").arg(description()));
    }
    m_everConnected = true;
    m_connectedSince.start();
    setState(State::Connected);
    queue(m_protocol->initialNegotiation());
}

void TelnetBackend::onConnectFailed(const QString& error) {
    m_attempt.reset();
    if (m_state != State::Connecting) {
        return;
    }
    setState(State::Idle);
    const QString message = error.isEmpty() ? tr("Connection failed") : error;
    if (error.contains(QLatin1String("cancelled")) || !scheduleReconnect(message)) {
        finishWith(-1, message);
    }
}

void TelnetBackend::onReadable() {
    if (!m_protocol) {
        return;
    }
    char buffer[16384];
    std::string reply;
    QByteArray output;
    std::size_t total = 0;
    QString lost;
    bool closed = false;
    while (total < kReadBudget) {
        const auto result = m_socket.readSome(buffer, sizeof(buffer));
        if (result.state == wt::net::Socket::IoState::Ok) {
            total += result.bytes;
            const std::string data = m_protocol->receive(std::string_view(buffer, result.bytes), reply);
            output.append(data.data(), static_cast<qsizetype>(data.size()));
            continue;
        }
        if (result.state == wt::net::Socket::IoState::Closed) {
            closed = true;
        } else if (result.state == wt::net::Socket::IoState::Error) {
            lost = tr("Connection lost: %1").arg(QString::fromStdString(wt::net::socketErrorText(result.error)));
        }
        break;
    }
    if (!output.isEmpty()) {
        Q_EMIT dataReceived(output);
    }
    if (!reply.empty() && !closed && lost.isEmpty()) {
        queue(std::move(reply));
    }
    if (closed) {
        dropConnection();
        setState(State::Idle);
        finishWith(0, tr("Connection closed by %1").arg(description()));
    } else if (!lost.isEmpty()) {
        connectionLost(lost);
    }
}

void TelnetBackend::queue(std::string bytes) {
    if (bytes.empty()) {
        return;
    }
    m_pendingWrite += bytes;
    flushWrites();
}

void TelnetBackend::flushWrites() {
    while (!m_pendingWrite.empty() && m_socket.isValid()) {
        const auto result = m_socket.writeSome(m_pendingWrite);
        if (result.state == wt::net::Socket::IoState::Ok) {
            m_pendingWrite.erase(0, result.bytes);
            continue;
        }
        if (result.state == wt::net::Socket::IoState::WouldBlock) {
            break;
        }
        // The read side reports the details; just stop writing.
        m_pendingWrite.clear();
        QMetaObject::invokeMethod(
            this,
            [this, code = result.error] {
                if (m_state == State::Connected) {
                    connectionLost(tr("Connection lost: %1").arg(QString::fromStdString(wt::net::socketErrorText(code))));
                }
            },
            Qt::QueuedConnection);
        break;
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(!m_pendingWrite.empty());
    }
}

bool TelnetBackend::localEchoActive() const {
    switch (m_options.localEcho) {
    case wt::session::EchoMode::On: return true;
    case wt::session::EchoMode::Off: return false;
    case wt::session::EchoMode::Auto: break;
    }
    return m_protocol && m_protocol->wantsLocalEcho();
}

std::string TelnetBackend::echoFor(std::string_view input) const {
    if (!input.empty() && input.front() == '\x1b') {
        return {}; // cursor/function keys: nothing sensible to echo
    }
    std::string echo;
    for (const char c : input) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\r') {
            echo += "\r\n";
        } else if (byte == 0x7F || byte == 0x08) {
            echo += "\b \b";
        } else if (byte >= 0x20 || c == '\t' || c == '\n') {
            echo += c;
        }
    }
    return echo;
}

void TelnetBackend::write(std::string_view bytes) {
    if (m_state != State::Connected || !m_protocol) {
        return;
    }
    if (localEchoActive()) {
        if (const std::string echo = echoFor(bytes); !echo.empty()) {
            Q_EMIT dataReceived(QByteArray(echo.data(), static_cast<qsizetype>(echo.size())));
        }
    }
    queue(m_protocol->encode(bytes));
}

void TelnetBackend::sendCommand(wt::net::TelnetCommand command) {
    if (m_state == State::Connected) {
        queue(wt::net::TelnetProtocol::command(command));
    }
}

void TelnetBackend::resize(int rows, int columns, int pixelWidth, int pixelHeight) {
    Q_UNUSED(pixelWidth);
    Q_UNUSED(pixelHeight);
    m_rows = rows;
    m_columns = columns;
    if (m_protocol && m_state == State::Connected) {
        queue(m_protocol->resize(rows, columns));
    }
}

void TelnetBackend::dropConnection() {
    delete m_readNotifier;
    m_readNotifier = nullptr;
    delete m_writeNotifier;
    m_writeNotifier = nullptr;
    m_socket.close();
    m_pendingWrite.clear();
}

void TelnetBackend::connectionLost(const QString& reason) {
    dropConnection();
    setState(State::Idle);
    if (m_connectedSince.isValid() && m_connectedSince.elapsed() >= kStableConnectionMs) {
        m_backoff.reset();
    }
    if (!scheduleReconnect(reason)) {
        finishWith(-1, reason);
    }
}

bool TelnetBackend::scheduleReconnect(const QString& reason) {
    const auto delay = m_backoff.next();
    if (!delay) {
        return false;
    }
    setState(State::WaitingToReconnect);
    Q_EMIT notice(tr("[%1. Reconnecting in %2 s (attempt %3 of %4)...]")
                      .arg(reason)
                      .arg(static_cast<double>(delay->count()) / 1000.0, 0, 'f', 1)
                      .arg(m_backoff.attempts())
                      .arg(m_backoff.maxAttempts()));
    m_reconnectTimer.start(static_cast<int>(delay->count()));
    return true;
}

void TelnetBackend::finishWith(int exitCode, const QString& message) {
    m_reconnectTimer.stop();
    setState(State::Finished);
    if (!message.isEmpty()) {
        Q_EMIT notice(QStringLiteral("[%1]").arg(message));
    }
    Q_EMIT finished(exitCode);
}

void TelnetBackend::reconnect() {
    if (m_state == State::Connected || m_state == State::Connecting) {
        return;
    }
    m_reconnectTimer.stop();
    m_backoff.reset();
    setState(State::Idle);
    startAttempt();
}

void TelnetBackend::terminate() {
    m_reconnectTimer.stop();
    joinWorker();
    m_attempt.reset();
    dropConnection();
    m_protocol.reset();
    m_state = State::Finished;
}

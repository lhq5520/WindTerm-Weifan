#pragma once

#include "SessionBackend.h"

#include "wt/net/Proxy.h"
#include "wt/net/Socket.h"
#include "wt/net/Telnet.h"
#include "wt/session/Backoff.h"
#include "wt/session/SessionProfile.h"

#include <QElapsedTimer>
#include <QTimer>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

class QSocketNotifier;

// Telnet session: the TCP connection (optionally through a SOCKS5/HTTP proxy) is made on a worker
// thread; afterwards the socket is serviced on the GUI thread through socket notifiers and the
// Qt-free wt::net::TelnetProtocol state machine. Dropped connections can be re-established with
// backoff; a remote logout ends the session.
class TelnetBackend final : public SessionBackend {
    Q_OBJECT

public:
    enum class State { Idle, Connecting, Connected, WaitingToReconnect, Finished };

    struct Options {
        std::string host;
        std::uint16_t port = 23;
        wt::net::ProxyConfig proxy;
        std::chrono::milliseconds timeout{15000};
        wt::net::TelnetConfig telnet;
        wt::session::EchoMode localEcho = wt::session::EchoMode::Auto;
        wt::session::ReconnectSettings reconnect{false, 5, 1000, 30000};
    };

    explicit TelnetBackend(Options options, QObject* parent = nullptr);
    ~TelnetBackend() override;

    // Options for a profile (the proxy password comes from `proxyPassword`).
    static Options fromProfile(const wt::session::SessionProfile& profile, const std::string& proxyPassword = {});

    bool start(int rows, int columns, QString* error) override;
    void write(std::string_view bytes) override;
    void resize(int rows, int columns, int pixelWidth, int pixelHeight) override;
    void terminate() override;
    bool isRunning() const override { return m_state != State::Finished && m_state != State::Idle; }
    bool canReconnect() const override { return true; }
    void reconnect() override;
    QString description() const override;

    // Sends a Telnet command (BREAK, IP, AYT, ...) to the server.
    void sendCommand(wt::net::TelnetCommand command);
    State state() const { return m_state; }
    const wt::net::TelnetProtocol* protocol() const { return m_protocol.get(); }
    bool localEchoActive() const;

Q_SIGNALS:
    void stateChanged(TelnetBackend::State state);

private:
    struct Attempt;
    void setState(State state);
    void startAttempt();
    void joinWorker();
    void onConnected(wt::net::NativeSocket socket);
    void onConnectFailed(const QString& error);
    void onReadable();
    void queue(std::string bytes);
    void flushWrites();
    void dropConnection();
    void connectionLost(const QString& reason);
    bool scheduleReconnect(const QString& reason);
    void finishWith(int exitCode, const QString& message);
    std::string echoFor(std::string_view input) const;

    Options m_options;
    wt::session::Backoff m_backoff;
    std::shared_ptr<Attempt> m_attempt;
    std::thread m_worker;
    wt::net::Socket m_socket;
    std::unique_ptr<wt::net::TelnetProtocol> m_protocol;
    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    std::string m_pendingWrite;
    QTimer m_reconnectTimer;
    QElapsedTimer m_connectedSince;
    State m_state = State::Idle;
    int m_rows = 24;
    int m_columns = 80;
    bool m_everConnected = false;
};

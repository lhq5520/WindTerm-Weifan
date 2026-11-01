#include "Forwarding.h"

#include "Agent.h"
#include "Libssh2Init.h"

#include "wt/platform/Files.h"

#include <algorithm>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace wt::ssh {

std::string PortForward::describe() const {
    auto endpoint = [](const std::string& host, std::uint16_t port, const char* fallback) {
        return (host.empty() ? std::string(fallback) : host) + ":" + std::to_string(port);
    };
    switch (kind) {
    case ForwardKind::Local:
        return "L " + endpoint(bindAddress, bindPort, "127.0.0.1") + " -> " + endpoint(targetHost, targetPort, "?");
    case ForwardKind::Remote:
        return "R " + endpoint(bindAddress, bindPort, "localhost") + " -> " + endpoint(targetHost, targetPort, "?");
    case ForwardKind::Dynamic: return "D " + endpoint(bindAddress, bindPort, "127.0.0.1") + " (SOCKS)";
    }
    return {};
}

namespace detail {
namespace {

constexpr std::size_t kMaxBuffered = 256 * 1024; // per direction and connection
constexpr int kRemoteQueue = 16;

// Shuts down the sending direction only (half close).
void shutdownWrite(net::NativeSocket socket) {
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(socket), SD_SEND);
#else
    ::shutdown(socket, SHUT_WR);
#endif
}

std::string localHostName() {
    char name[256] = {};
#ifdef _WIN32
    net::initializeNetworking();
    if (::gethostname(name, sizeof(name) - 1) != 0) {
        return {};
    }
#else
    if (::gethostname(name, sizeof(name) - 1) != 0) {
        return {};
    }
#endif
    return name;
}

} // namespace

struct ForwardingEngine::Forward {
    ForwardStatus status;
    net::Socket listener;               // Local / Dynamic
    LIBSSH2_LISTENER* remote = nullptr; // Remote
    bool removing = false;
};

// A blocking connect done on a helper thread, so slow name lookups do not stall the session.
struct ForwardingEngine::ConnectJob {
    std::mutex mutex;
    bool done = false;
    net::Socket socket;
    std::string error;
};

struct ForwardingEngine::Relay {
    enum class Kind { Local, Dynamic, Remote, X11, Agent };
    enum class Phase { Socks, WaitOpen, Opening, Connecting, Open, Closing };
    Kind kind = Kind::Local;
    Phase phase = Phase::Open;
    int forwardId = -1;
    LIBSSH2_CHANNEL* channel = nullptr;
    net::Socket socket;
    std::string toChannel;
    std::string toSocket;
    bool socketReadDone = false;  // the local side sent EOF
    bool eofSent = false;         // ... and we passed it on
    bool channelDone = false;     // the channel sent EOF
    bool socketWriteShut = false; // ... and we passed it on
    bool closeAfterFlush = false; // e.g. after a SOCKS failure reply
    bool counted = false;
    std::string host; // direct-tcpip target
    std::uint16_t port = 0;
    std::string originHost = "127.0.0.1";
    std::uint16_t originPort = 0;
    std::unique_ptr<net::SocksServerHandshake> socks;
    std::unique_ptr<X11AuthRewriter> x11;
    std::shared_ptr<ConnectJob> job;
};

ForwardingEngine::ForwardingEngine(SshConnection& connection, const ForwardingOptions& options,
                                   const SshConnection::ShellExtras& extras, Notice notice, Wake wake)
    : m_connection(connection),
      m_options(options),
      m_extras(extras),
      m_notice(std::move(notice)),
      m_wake(std::move(wake)) {
    if (m_extras.x11Forwarding) {
        const char* environmentDisplay = std::getenv("DISPLAY");
        m_display = parseX11Display(!m_options.x11Display.empty() ? m_options.x11Display
                                    : environmentDisplay          ? environmentDisplay
                                                                  : "");
        const std::string path =
            m_options.xauthorityPath.empty() ? defaultXauthorityPath() : m_options.xauthorityPath;
        if (m_display) {
            if (auto contents = platform::readFile(path)) {
                m_cookie = findX11Cookie(parseXauthority(*contents), *m_display, localHostName());
            }
        }
    }
    int id = 1;
    for (const PortForward& forward : m_options.ports) {
        add(id++, forward);
    }
}

ForwardingEngine::~ForwardingEngine() {
    shutdown();
}

ForwardingEngine::Forward* ForwardingEngine::findForward(int id) {
    for (auto& forward : m_forwards) {
        if (forward->status.id == id) {
            return forward.get();
        }
    }
    return nullptr;
}

void ForwardingEngine::publish() {
    std::vector<ForwardStatus> snapshot;
    for (const auto& forward : m_forwards) {
        if (!forward->removing) {
            snapshot.push_back(forward->status);
        }
    }
    std::lock_guard lock(m_statusMutex);
    m_published = std::move(snapshot);
}

std::vector<ForwardStatus> ForwardingEngine::status() const {
    std::lock_guard lock(m_statusMutex);
    return m_published;
}

void ForwardingEngine::add(int id, const PortForward& spec) {
    auto forward = std::make_unique<Forward>();
    forward->status.id = id;
    forward->status.forward = spec;
    if (spec.kind == ForwardKind::Local || spec.kind == ForwardKind::Dynamic) {
        if (spec.kind == ForwardKind::Local && (spec.targetHost.empty() || spec.targetPort == 0)) {
            forward->status.state = ForwardStatus::State::Failed;
            forward->status.message = "no destination";
        } else if (auto status = net::listenTcp(spec.bindAddress, spec.bindPort, forward->listener, forward->status.boundPort);
                   !status) {
            forward->status.state = ForwardStatus::State::Failed;
            forward->status.message = status.message();
        } else {
            forward->status.state = ForwardStatus::State::Active;
        }
    } else if (spec.targetHost.empty() || spec.targetPort == 0) {
        forward->status.state = ForwardStatus::State::Failed;
        forward->status.message = "no destination";
    } // Remote: requested from the server by serviceRemoteListeners()
    if (forward->status.state == ForwardStatus::State::Failed && m_notice) {
        m_notice("Port forwarding " + spec.describe() + " failed: " + forward->status.message);
    }
    m_forwards.push_back(std::move(forward));
    publish();
}

void ForwardingEngine::remove(int id) {
    Forward* forward = findForward(id);
    if (!forward || forward->removing) {
        return;
    }
    forward->removing = true;
    forward->listener.close();
    // Connections already made through the forward stay open until they end (like OpenSSH).
    publish();
}

void ForwardingEngine::relayCounted(Relay& relay, int delta) {
    if (Forward* forward = findForward(relay.forwardId)) {
        forward->status.openConnections += delta;
        if (delta > 0) {
            ++forward->status.totalConnections;
        }
        publish();
    }
}

void ForwardingEngine::acceptIncoming() {
    for (const auto& incoming : m_connection.takeIncomingChannels()) {
        auto relay = std::make_unique<Relay>();
        relay->kind = incoming.type == SshConnection::IncomingChannel::Type::X11 ? Relay::Kind::X11 : Relay::Kind::Agent;
        relay->channel = incoming.channel;
        relay->phase = Relay::Phase::Open;
        core::Status status = core::Status::error("not enabled");
        if (relay->kind == Relay::Kind::X11 && m_extras.x11Forwarding && m_display) {
            status = connectX11(*m_display, relay->socket);
            relay->x11 = std::make_unique<X11AuthRewriter>(m_extras.x11FakeCookie, m_cookie ? m_cookie->name : "",
                                                           m_cookie ? m_cookie->data : "");
        } else if (relay->kind == Relay::Kind::Agent && m_extras.agentForwarding) {
            status = connectAgent(relay->socket);
        }
        if (status) {
            relay->socket.setNonBlocking(true);
        } else {
            if (m_notice) {
                m_notice(std::string(relay->kind == Relay::Kind::X11 ? "X11" : "Agent") +
                         " forwarding: " + status.message());
            }
            relay->phase = Relay::Phase::Closing;
        }
        m_relays.push_back(std::move(relay));
    }
}

bool ForwardingEngine::serviceListeners() {
    bool progress = false;
    for (auto& forward : m_forwards) {
        if (forward->removing || !forward->listener.isValid()) {
            continue;
        }
        for (int accepted = 0; accepted < 16; ++accepted) {
            net::Socket client;
            if (!net::waitReadable(forward->listener.native(), std::chrono::milliseconds(0)) ||
                !net::acceptConnection(forward->listener, client)) {
                break;
            }
            progress = true;
            client.setNonBlocking(true);
            client.setNoDelay(true);
            auto relay = std::make_unique<Relay>();
            relay->forwardId = forward->status.id;
            relay->socket = std::move(client);
            if (forward->status.forward.kind == ForwardKind::Dynamic) {
                relay->kind = Relay::Kind::Dynamic;
                relay->phase = Relay::Phase::Socks;
                relay->socks = std::make_unique<net::SocksServerHandshake>();
            } else {
                relay->kind = Relay::Kind::Local;
                relay->phase = Relay::Phase::WaitOpen;
                relay->host = forward->status.forward.targetHost;
                relay->port = forward->status.forward.targetPort;
            }
            relay->originPort = forward->status.boundPort;
            relay->counted = true;
            relayCounted(*relay, +1);
            m_relays.push_back(std::move(relay));
        }
    }
    return progress;
}

bool ForwardingEngine::serviceRemoteListeners(std::string& fatal) {
    LIBSSH2_SESSION* session = m_connection.session();
    bool progress = false;
    // Requests go out one at a time (libssh2 keeps their state in the session).
    if (!m_listening) {
        for (auto& forward : m_forwards) {
            if (!forward->removing && forward->status.forward.kind == ForwardKind::Remote && !forward->remote &&
                forward->status.state == ForwardStatus::State::Starting) {
                m_listening = forward.get();
                break;
            }
        }
    }
    if (m_listening) {
        Forward& forward = *m_listening;
        const PortForward& spec = forward.status.forward;
        const std::string host = spec.bindAddress.empty() ? "localhost" : spec.bindAddress;
        int bound = 0;
        LIBSSH2_LISTENER* listener =
            libssh2_channel_forward_listen_ex(session, host.c_str(), spec.bindPort, &bound, kRemoteQueue);
        if (listener) {
            forward.remote = listener;
            forward.status.boundPort = static_cast<std::uint16_t>(bound > 0 ? bound : spec.bindPort);
            forward.status.state = ForwardStatus::State::Active;
            m_listening = nullptr;
            progress = true;
            publish();
        } else {
            const int code = libssh2_session_last_errno(session);
            if (code != LIBSSH2_ERROR_EAGAIN) {
                forward.status.state = ForwardStatus::State::Failed;
                forward.status.message = code == LIBSSH2_ERROR_REQUEST_DENIED ? "the server refused the port"
                                                                              : libssh2Error(session);
                if (m_notice) {
                    m_notice("Port forwarding " + spec.describe() + " failed: " + forward.status.message);
                }
                m_listening = nullptr;
                progress = true;
                publish();
                if (code == LIBSSH2_ERROR_SOCKET_SEND || code == LIBSSH2_ERROR_SOCKET_RECV ||
                    code == LIBSSH2_ERROR_SOCKET_DISCONNECT) {
                    fatal = "Connection lost: " + libssh2Error(session);
                    return progress;
                }
            }
        }
    }
    for (auto& forward : m_forwards) {
        if (!forward->remote) {
            continue;
        }
        if (forward->removing) {
            const int rc = libssh2_channel_forward_cancel(forward->remote);
            if (rc != LIBSSH2_ERROR_EAGAIN) {
                forward->remote = nullptr;
                progress = true;
            }
            continue;
        }
        for (int accepted = 0; accepted < 16; ++accepted) {
            LIBSSH2_CHANNEL* channel = libssh2_channel_forward_accept(forward->remote);
            if (!channel) {
                break;
            }
            progress = true;
            auto relay = std::make_unique<Relay>();
            relay->kind = Relay::Kind::Remote;
            relay->forwardId = forward->status.id;
            relay->channel = channel;
            relay->counted = true;
            relayCounted(*relay, +1);
            startRelayConnect(*relay, forward->status.forward.targetHost, forward->status.forward.targetPort);
            m_relays.push_back(std::move(relay));
        }
    }
    return progress;
}

void ForwardingEngine::startRelayConnect(Relay& relay, const std::string& host, std::uint16_t port) {
    relay.phase = Relay::Phase::Connecting;
    relay.job = std::make_shared<ConnectJob>();
    std::thread([job = relay.job, host, port, wake = m_wake] {
        net::Socket socket;
        const auto status = net::connectTcp(host, port, std::chrono::seconds(10), socket);
        {
            std::lock_guard lock(job->mutex);
            job->done = true;
            if (status) {
                job->socket = std::move(socket);
            } else {
                job->error = status.message();
            }
        }
        if (wake) {
            wake();
        }
    }).detach();
}

void ForwardingEngine::closeRelay(Relay& relay, const std::string& reason) {
    if (!reason.empty() && m_notice) {
        m_notice(reason);
    }
    relay.phase = Relay::Phase::Closing;
}

bool ForwardingEngine::openPending(std::string& fatal) {
    LIBSSH2_SESSION* session = m_connection.session();
    if (!m_opening) {
        for (auto& relay : m_relays) {
            if (relay->phase == Relay::Phase::WaitOpen) {
                m_opening = relay.get();
                m_opening->phase = Relay::Phase::Opening;
                break;
            }
        }
    }
    if (!m_opening) {
        return false;
    }
    Relay& relay = *m_opening;
    LIBSSH2_CHANNEL* channel = libssh2_channel_direct_tcpip_ex(session, relay.host.c_str(), relay.port,
                                                               relay.originHost.c_str(), relay.originPort);
    if (channel) {
        relay.channel = channel;
        relay.phase = Relay::Phase::Open;
        if (relay.socks) {
            relay.toSocket += relay.socks->finish(true);
        }
        m_opening = nullptr;
        return true;
    }
    const int code = libssh2_session_last_errno(session);
    if (code == LIBSSH2_ERROR_EAGAIN) {
        return false;
    }
    m_opening = nullptr;
    const std::string reason = "Port forwarding: cannot open a channel to " + relay.host + ":" +
                               std::to_string(relay.port) + ": " + libssh2Error(session);
    if (code == LIBSSH2_ERROR_SOCKET_SEND || code == LIBSSH2_ERROR_SOCKET_RECV ||
        code == LIBSSH2_ERROR_SOCKET_DISCONNECT) {
        fatal = "Connection lost: " + libssh2Error(session);
    }
    if (relay.socks) {
        relay.toSocket += relay.socks->finish(false);
        relay.closeAfterFlush = true;
        relay.phase = Relay::Phase::Open; // flush the failure reply first
        relay.socketReadDone = true;
        relay.eofSent = true;
        relay.channelDone = true;
        if (m_notice) {
            m_notice(reason);
        }
    } else {
        closeRelay(relay, reason);
    }
    return true;
}

bool ForwardingEngine::serviceRelay(Relay& relay, std::string& fatal) {
    LIBSSH2_SESSION* session = m_connection.session();
    bool progress = false;
    char buffer[32768];

    if (relay.phase == Relay::Phase::Connecting) {
        std::lock_guard lock(relay.job->mutex);
        if (!relay.job->done) {
            return false;
        }
        if (!relay.job->socket.isValid()) {
            closeRelay(relay, "Port forwarding: " + relay.job->error);
            return true;
        }
        relay.socket = std::move(relay.job->socket);
        relay.socket.setNonBlocking(true);
        relay.socket.setNoDelay(true);
        relay.job.reset();
        relay.phase = Relay::Phase::Open;
        progress = true;
    }

    if (relay.phase == Relay::Phase::Socks) {
        const auto result = relay.socket.readSome(buffer, sizeof(buffer));
        if (result.state == net::Socket::IoState::Ok) {
            progress = true;
            std::string reply;
            const auto state = relay.socks->feed(std::string_view(buffer, result.bytes), reply);
            relay.toSocket += reply;
            if (state == net::SocksServerHandshake::State::Ready) {
                relay.host = relay.socks->host();
                relay.port = relay.socks->port();
                relay.toChannel = relay.socks->takeLeftover();
                relay.phase = Relay::Phase::WaitOpen;
            } else if (state == net::SocksServerHandshake::State::Failed) {
                relay.closeAfterFlush = true;
                relay.socketReadDone = relay.eofSent = relay.channelDone = true;
                relay.phase = Relay::Phase::Open;
            }
        } else if (result.state != net::Socket::IoState::WouldBlock) {
            closeRelay(relay);
            return true;
        }
    }

    if (relay.phase == Relay::Phase::Socks || relay.phase == Relay::Phase::WaitOpen ||
        relay.phase == Relay::Phase::Opening) {
        // Flush handshake replies while waiting.
        while (!relay.toSocket.empty()) {
            const auto result = relay.socket.writeSome(relay.toSocket);
            if (result.state != net::Socket::IoState::Ok) {
                break;
            }
            relay.toSocket.erase(0, result.bytes);
            progress = true;
        }
        return progress;
    }
    if (relay.phase != Relay::Phase::Open) {
        return progress;
    }

    // Local side -> channel.
    if (!relay.socketReadDone && relay.toChannel.size() < kMaxBuffered && relay.channel) {
        const auto result = relay.socket.readSome(buffer, sizeof(buffer));
        if (result.state == net::Socket::IoState::Ok) {
            relay.toChannel.append(buffer, result.bytes);
            progress = true;
        } else if (result.state == net::Socket::IoState::Closed) {
            relay.socketReadDone = true;
            progress = true;
        } else if (result.state == net::Socket::IoState::Error) {
            closeRelay(relay);
            return true;
        }
    }
    while (!relay.toChannel.empty() && relay.channel) {
        const auto n = libssh2_channel_write(relay.channel, relay.toChannel.data(), relay.toChannel.size());
        if (n > 0) {
            relay.toChannel.erase(0, static_cast<std::size_t>(n));
            progress = true;
            continue;
        }
        if (n == 0 || n == LIBSSH2_ERROR_EAGAIN) {
            break; // window full or socket busy
        }
        const int code = libssh2_session_last_errno(session);
        if (code == LIBSSH2_ERROR_SOCKET_SEND || code == LIBSSH2_ERROR_SOCKET_RECV ||
            code == LIBSSH2_ERROR_SOCKET_DISCONNECT) {
            fatal = "Connection lost: " + libssh2Error(session);
        }
        closeRelay(relay);
        return true;
    }
    if (relay.socketReadDone && relay.toChannel.empty() && !relay.eofSent && relay.channel) {
        const int rc = libssh2_channel_send_eof(relay.channel);
        if (rc == 0) {
            relay.eofSent = true;
            progress = true;
        } else if (rc != LIBSSH2_ERROR_EAGAIN) {
            closeRelay(relay);
            return true;
        }
    }

    // Channel -> local side.
    if (!relay.channelDone && relay.toSocket.size() < kMaxBuffered && relay.channel) {
        for (int round = 0; round < 8 && relay.toSocket.size() < kMaxBuffered; ++round) {
            const auto n = libssh2_channel_read(relay.channel, buffer, sizeof(buffer));
            if (n > 0) {
                progress = true;
                if (relay.x11) {
                    std::string rewritten;
                    if (relay.x11->feed(std::string_view(buffer, static_cast<std::size_t>(n)), rewritten) ==
                        X11AuthRewriter::State::Rejected) {
                        closeRelay(relay, "X11 forwarding: a connection with the wrong authentication was refused");
                        return true;
                    }
                    relay.toSocket += rewritten;
                } else {
                    relay.toSocket.append(buffer, static_cast<std::size_t>(n));
                }
                continue;
            }
            if (n == 0 || n == LIBSSH2_ERROR_EAGAIN) {
                if (libssh2_channel_eof(relay.channel)) {
                    relay.channelDone = true;
                    progress = true;
                }
                break;
            }
            const int code = libssh2_session_last_errno(session);
            if (code == LIBSSH2_ERROR_SOCKET_SEND || code == LIBSSH2_ERROR_SOCKET_RECV ||
                code == LIBSSH2_ERROR_SOCKET_DISCONNECT) {
                fatal = "Connection lost: " + libssh2Error(session);
            }
            closeRelay(relay);
            return true;
        }
    }
    while (!relay.toSocket.empty()) {
        const auto result = relay.socket.writeSome(relay.toSocket);
        if (result.state == net::Socket::IoState::Ok) {
            relay.toSocket.erase(0, result.bytes);
            progress = true;
            continue;
        }
        if (result.state != net::Socket::IoState::WouldBlock) {
            closeRelay(relay);
            return true;
        }
        break;
    }
    if (relay.toSocket.empty() && relay.closeAfterFlush) {
        closeRelay(relay);
        return true;
    }
    if (relay.channelDone && relay.toSocket.empty() && !relay.socketWriteShut) {
        shutdownWrite(relay.socket.native());
        relay.socketWriteShut = true;
        progress = true;
    }
    if (relay.channelDone && relay.socketReadDone && relay.eofSent && relay.toSocket.empty()) {
        closeRelay(relay);
        progress = true;
    }
    return progress;
}

bool ForwardingEngine::reapClosed() {
    bool progress = false;
    for (auto it = m_relays.begin(); it != m_relays.end();) {
        Relay& relay = **it;
        if (relay.phase != Relay::Phase::Closing) {
            ++it;
            continue;
        }
        if (relay.channel) {
            const int rc = libssh2_channel_free(relay.channel);
            if (rc == LIBSSH2_ERROR_EAGAIN) {
                ++it;
                continue; // waiting for the server's CLOSE
            }
            relay.channel = nullptr;
        }
        if (m_opening == &relay) {
            m_opening = nullptr;
        }
        relay.socket.close();
        if (relay.counted) {
            relayCounted(relay, -1);
        }
        it = m_relays.erase(it);
        progress = true;
    }
    // Forwards that were removed and have no listener left can go.
    for (auto it = m_forwards.begin(); it != m_forwards.end();) {
        Forward& forward = **it;
        const bool busy = std::any_of(m_relays.begin(), m_relays.end(),
                                      [&](const auto& relay) { return relay->forwardId == forward.status.id; });
        if (forward.removing && !forward.remote && !busy && m_listening != &forward) {
            it = m_forwards.erase(it);
            progress = true;
        } else {
            ++it;
        }
    }
    return progress;
}

bool ForwardingEngine::process(std::string& fatal) {
    acceptIncoming();
    bool progress = serviceListeners();
    progress |= serviceRemoteListeners(fatal);
    if (!fatal.empty()) {
        return progress;
    }
    progress |= openPending(fatal);
    for (auto& relay : m_relays) {
        if (relay->phase != Relay::Phase::Closing) {
            progress |= serviceRelay(*relay, fatal);
            if (!fatal.empty()) {
                return progress;
            }
        }
    }
    progress |= reapClosed();
    return progress;
}

void ForwardingEngine::pollRequests(std::vector<PollRequest>& out) const {
    for (const auto& forward : m_forwards) {
        if (!forward->removing && forward->listener.isValid()) {
            out.push_back({forward->listener.native(), true, false});
        }
    }
    for (const auto& relay : m_relays) {
        if (!relay->socket.isValid()) {
            continue;
        }
        PollRequest request{relay->socket.native(), false, !relay->toSocket.empty()};
        if (relay->phase == Relay::Phase::Socks ||
            (relay->phase == Relay::Phase::Open && !relay->socketReadDone && relay->toChannel.size() < kMaxBuffered)) {
            request.read = true;
        }
        if (request.read || request.write) {
            out.push_back(request);
        }
    }
}

void ForwardingEngine::shutdown() {
    for (auto& relay : m_relays) {
        if (relay->channel) {
            libssh2_channel_free(relay->channel);
            relay->channel = nullptr;
        }
        relay->socket.close();
    }
    m_relays.clear();
    m_opening = nullptr;
    for (auto& forward : m_forwards) {
        if (forward->remote) {
            libssh2_channel_forward_cancel(forward->remote);
            forward->remote = nullptr;
        }
        forward->listener.close();
    }
    m_listening = nullptr;
    for (auto& forward : m_forwards) {
        forward->status.state = ForwardStatus::State::Closed;
    }
    publish();
    m_forwards.clear();
}

} // namespace detail
} // namespace wt::ssh

#pragma once

#include "wt/core/Status.h"
#include "wt/net/Socket.h"
#include "wt/ssh/SshTypes.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

namespace wt::ssh {

class SftpSession;

// Lets another thread abort a connection attempt (by shutting the socket down, which makes
// blocking libssh2 calls fail promptly). connect() detaches it before returning, so a later
// abort() can never touch a socket number that was closed and reused.
class Aborter {
public:
    void abort();
    bool aborted() const { return m_aborted.load(); }
    const std::atomic<bool>* flag() const { return &m_aborted; }
    void attach(net::NativeSocket socket);
    void detach();

private:
    std::mutex m_mutex;
    net::NativeSocket m_socket = net::kInvalidSocket;
    std::atomic<bool> m_aborted{false};
};

// An authenticated SSH connection (libssh2 session). Not thread-safe: use it from one thread at
// a time. Channels and SFTP sessions opened from it must be destroyed before it.
class SshConnection : public std::enable_shared_from_this<SshConnection> {
public:
    // Connects, verifies the host key against options.knownHostsPath (asking `ui` about unknown
    // or changed keys) and authenticates. With `via` the TCP connection is tunnelled through
    // that (jump host) connection, which this connection then owns and services on a bridge
    // thread: `via` must not be used for anything else afterwards.
    static std::shared_ptr<SshConnection> connect(const ConnectOptions& options, SshInteraction& ui,
                                                  std::string* error, std::shared_ptr<SshConnection> via = {},
                                                  Aborter* aborter = nullptr);
    ~SshConnection();
    SshConnection(const SshConnection&) = delete;
    SshConnection& operator=(const SshConnection&) = delete;

    const ConnectOptions& options() const { return m_options; }
    const HostKey& hostKey() const { return m_hostKey; }
    std::string serverBanner() const;
    // Secrets that authenticated successfully (to remember them or reuse them for reconnects).
    const std::optional<std::string>& usedPassword() const { return m_usedPassword; }
    const std::optional<std::string>& usedPassphrase() const { return m_usedPassphrase; }
    std::string authMethodUsed() const { return m_authMethod; }

    LIBSSH2_SESSION* session() const { return m_session; }
    net::NativeSocket socket() const { return m_socket.native(); }
    void setBlocking(bool blocking);
    bool isAlive() const;

    // What openShellChannel() set up besides the shell.
    struct ShellExtras {
        bool agentForwarding = false;
        bool x11Forwarding = false;
        std::string x11FakeCookie; // raw bytes given to the server
        std::vector<std::string> warnings; // forwarding requests the server refused
    };
    // Opens a session channel with a PTY running a shell (or options.command), requesting agent
    // and X11 forwarding first when options.forwarding asks for them. Blocking.
    LIBSSH2_CHANNEL* openShellChannel(const ShellOptions& options, std::string* error, ShellExtras* extras = nullptr);

    // Channels the server opens towards us (X11 connections, agent requests) once forwarding was
    // requested. Filled by libssh2 callbacks while the session is being serviced; collected by the
    // thread that services it.
    struct IncomingChannel {
        enum class Type { X11, Agent };
        Type type = Type::X11;
        LIBSSH2_CHANNEL* channel = nullptr;
        std::string originHost;
        int originPort = 0;
    };
    std::vector<IncomingChannel> takeIncomingChannels();
    // Runs a command without a PTY and collects its output (for tests and small helpers).
    core::Status execute(const std::string& command, std::string& output, int& exitStatus);
    // direct-tcpip channel to host:port as seen from the server (used for jump hosts).
    LIBSSH2_CHANNEL* openDirectTcpip(const std::string& host, std::uint16_t port, std::string* error);
    std::unique_ptr<SftpSession> openSftp(std::string* error);

    std::string lastError() const;

private:
    SshConnection() = default;
    core::Status verifyHostKey(SshInteraction& ui);
    core::Status authenticate(SshInteraction& ui);
    bool tryAgent();
    int tryKeyFile(const std::string& path, SshInteraction& ui, bool& cancelled);
    int tryPassword(SshInteraction& ui, bool& cancelled);
    int tryKeyboardInteractive(SshInteraction& ui, bool& cancelled);

    struct Bridge;
    ConnectOptions m_options;
    net::Socket m_socket;
    LIBSSH2_SESSION* m_session = nullptr;
    std::shared_ptr<SshConnection> m_via;
    std::unique_ptr<Bridge> m_bridge;
    HostKey m_hostKey;
    std::optional<std::string> m_usedPassword;
    std::optional<std::string> m_usedPassphrase;
    std::string m_authMethod;
    std::string m_lastAuthError;

    // Keyboard-interactive callback state.
    friend struct KbdIntContext;
    SshInteraction* m_ui = nullptr;
    bool m_kbdCancelled = false;
    int m_kbdRound = 0;

    friend struct IncomingChannelCallbacks;
    std::vector<IncomingChannel> m_incoming;
};

// Formats libssh2's last error for a session.
std::string libssh2Error(LIBSSH2_SESSION* session);

} // namespace wt::ssh

#pragma once

#include "wt/net/Proxy.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wt::ssh {

// Server host key as received during the key exchange.
struct HostKey {
    std::string type;        // "ssh-ed25519", "ecdsa-sha2-nistp256", "ssh-rsa", ...
    std::string fingerprint; // "SHA256:<base64 without padding>", as shown by OpenSSH
    std::string blob;        // raw public key blob
    int libssh2Type = 0;
};

enum class HostKeyCheck { Match, Mismatch, NotFound, Failure };
enum class HostKeyDecision { Reject, AcceptOnce, AcceptAndSave };

enum class AuthMethod { Auto, Password, PublicKey, Agent, KeyboardInteractive };

struct KeyboardPrompt {
    std::string text;
    bool echo = false;
};

// Questions the connection asks the user. Called on the connecting thread; implementations
// usually forward them to the UI thread and block until answered. Returning nullopt cancels.
class SshInteraction {
public:
    virtual ~SshInteraction() = default;
    virtual HostKeyDecision confirmHostKey(const std::string& host, std::uint16_t port, const HostKey& key,
                                           HostKeyCheck check) = 0;
    virtual std::optional<std::string> askPassword(const std::string& prompt) = 0;
    virtual std::optional<std::string> askPassphrase(const std::string& keyFile) = 0;
    virtual std::optional<std::vector<std::string>> keyboardInteractive(const std::string& name,
                                                                        const std::string& instruction,
                                                                        const std::vector<KeyboardPrompt>& prompts) = 0;
    // Progress / banner text; default ignores it.
    virtual void info(const std::string& message) { (void)message; }
};

struct ConnectOptions {
    std::string host;
    std::uint16_t port = 22;
    std::string username;
    AuthMethod auth = AuthMethod::Auto;
    std::string identityFile;                // "~" is expanded
    // OpenSSH certificate for the identity; empty: "<identityFile>-cert.pub" when that exists.
    std::string certificateFile;
    std::optional<std::string> password;     // tried before asking (e.g. from the vault)
    std::optional<std::string> passphrase;   // for an encrypted identity file
    std::string knownHostsPath;              // empty: no persistent host key store (always ask)
    std::chrono::milliseconds timeout{15000};
    int keepAliveSeconds = 30;
    net::ProxyConfig proxy;                  // SOCKS5 / HTTP CONNECT; jump hosts use `via`
    bool tryDefaultKeys = true;              // Auto: also try ~/.ssh/id_ed25519, id_ecdsa, id_rsa
};

enum class ForwardKind { Local, Remote, Dynamic };

// One port forward, like OpenSSH's -L, -R and -D.
struct PortForward {
    ForwardKind kind = ForwardKind::Local;
    // Local/Dynamic: local address to listen on (empty: 127.0.0.1). Remote: address on the server
    // (empty: its loopback interface; the server's GatewayPorts setting decides what is allowed).
    std::string bindAddress;
    std::uint16_t bindPort = 0; // 0: any free port (reported in ForwardStatus::boundPort)
    // Local: destination as seen from the server. Remote: destination as seen from this machine.
    // Unused for Dynamic (SOCKS clients choose the destination).
    std::string targetHost;
    std::uint16_t targetPort = 0;

    // "L 127.0.0.1:8080 -> db:5432", "R localhost:9000 -> 127.0.0.1:3000", "D 127.0.0.1:1080 (SOCKS)"
    std::string describe() const;
    friend bool operator==(const PortForward&, const PortForward&) = default;
};

struct ForwardingOptions {
    std::vector<PortForward> ports;
    // Forward the local SSH agent ($SSH_AUTH_SOCK, or OpenSSH's agent pipe on Windows).
    bool agent = false;
    bool x11 = false;
    std::string x11Display;     // empty: $DISPLAY
    std::string xauthorityPath; // empty: $XAUTHORITY or ~/.Xauthority
};

struct ForwardStatus {
    enum class State { Starting, Active, Failed, Closed };
    int id = 0;
    PortForward forward;
    State state = State::Starting;
    std::string message;        // why it failed
    std::uint16_t boundPort = 0; // the port actually listened on
    int openConnections = 0;
    int totalConnections = 0;
};

struct ShellOptions {
    std::string term = "xterm-256color";
    int rows = 24;
    int columns = 80;
    int pixelWidth = 0;
    int pixelHeight = 0;
    std::vector<std::pair<std::string, std::string>> environment; // sent with "env" requests
    std::string command;                                          // empty: interactive shell
    ForwardingOptions forwarding;
};

} // namespace wt::ssh

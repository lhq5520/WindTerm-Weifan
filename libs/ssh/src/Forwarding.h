#pragma once

// Port forwarding, agent forwarding and X11 forwarding on the thread that services an SSH
// session (see SshShell). Everything here is non-blocking: process() makes whatever progress is
// possible and pollRequests() tells the caller which descriptors to wait for.

#include "wt/net/Socket.h"
#include "wt/net/SocksServer.h"
#include "wt/ssh/SshConnection.h"
#include "wt/ssh/SshTypes.h"
#include "wt/ssh/X11.h"

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

typedef struct _LIBSSH2_LISTENER LIBSSH2_LISTENER;

namespace wt::ssh::detail {

struct PollRequest {
    net::NativeSocket fd = net::kInvalidSocket;
    bool read = false;
    bool write = false;
};

class ForwardingEngine {
public:
    using Notice = std::function<void(const std::string&)>;
    using Wake = std::function<void()>;

    ForwardingEngine(SshConnection& connection, const ForwardingOptions& options,
                     const SshConnection::ShellExtras& extras, Notice notice, Wake wake);
    ~ForwardingEngine();
    ForwardingEngine(const ForwardingEngine&) = delete;
    ForwardingEngine& operator=(const ForwardingEngine&) = delete;

    // Service thread only.
    void add(int id, const PortForward& forward);
    void remove(int id);
    // Returns true when anything moved (call again before sleeping). A fatal session error ends
    // everything and is stored in `fatal`.
    bool process(std::string& fatal);
    void pollRequests(std::vector<PollRequest>& out) const;
    // Closes every channel and listener; the session must be in blocking mode (with a timeout).
    void shutdown();

    // Any thread.
    std::vector<ForwardStatus> status() const;

private:
    struct Forward;
    struct Relay;
    struct ConnectJob;

    Forward* findForward(int id);
    void publish();
    void acceptIncoming();
    bool serviceListeners();
    bool serviceRemoteListeners(std::string& fatal);
    bool serviceRelay(Relay& relay, std::string& fatal);
    bool openPending(std::string& fatal);
    bool reapClosed();
    void startRelayConnect(Relay& relay, const std::string& host, std::uint16_t port);
    void closeRelay(Relay& relay, const std::string& reason = {});
    void relayCounted(Relay& relay, int delta);

    SshConnection& m_connection;
    ForwardingOptions m_options;
    SshConnection::ShellExtras m_extras;
    Notice m_notice;
    Wake m_wake;
    std::optional<X11Display> m_display;
    std::optional<XauthEntry> m_cookie;
    std::list<std::unique_ptr<Forward>> m_forwards;
    std::list<std::unique_ptr<Relay>> m_relays;
    Relay* m_opening = nullptr;   // libssh2 runs one direct-tcpip open at a time per session
    Forward* m_listening = nullptr; // ... and one tcpip-forward request
    mutable std::mutex m_statusMutex;
    std::vector<ForwardStatus> m_published;
};

} // namespace wt::ssh::detail

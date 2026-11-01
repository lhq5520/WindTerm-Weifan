#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace wt::net {

// Server side of a SOCKS 4, 4a or 5 CONNECT handshake without authentication, as a pure state
// machine (used by SSH dynamic port forwarding). Feed it what the client sends; once Ready, connect
// to host():port() and send finish(success).
class SocksServerHandshake {
public:
    enum class State { NeedMore, Ready, Failed };

    // Appends bytes for the client to `reply` (SOCKS5 method selection, error replies).
    State feed(std::string_view data, std::string& reply);
    // Final reply once the outcome of the connection to the target is known.
    std::string finish(bool success) const;

    State state() const { return m_state; }
    int version() const { return m_version; }
    const std::string& host() const { return m_host; }
    std::uint16_t port() const { return m_port; }
    const std::string& error() const { return m_error; }
    // Data the client sent after its request (sent on once the tunnel is open).
    std::string takeLeftover();

private:
    State fail(std::string message, std::string& reply, std::string_view failureReply);
    State parse(std::string& reply);

    enum class Phase { Version, Socks5Methods, Socks5Request, Socks4Request, Done };
    Phase m_phase = Phase::Version;
    State m_state = State::NeedMore;
    std::string m_buffer;
    int m_version = 0;
    std::string m_host;
    std::uint16_t m_port = 0;
    std::string m_error;
};

} // namespace wt::net

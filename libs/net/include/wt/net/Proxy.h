#pragma once

#include "wt/core/Status.h"
#include "wt/net/Socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace wt::net {

enum class ProxyType { None, Socks5, HttpConnect };

struct ProxyConfig {
    ProxyType type = ProxyType::None;
    std::string host;
    std::uint16_t port = 0;
    std::string username; // empty: no authentication
    std::string password;
};

// Asks a SOCKS5 proxy (RFC 1928, username/password auth per RFC 1929) on `socket` to connect to
// target:port. The proxy resolves the target name.
core::Status socks5Connect(Socket& socket, const std::string& targetHost, std::uint16_t targetPort,
                           const std::string& username, const std::string& password,
                           std::chrono::milliseconds timeout);

// Opens a tunnel through an HTTP proxy with "CONNECT host:port" (Basic auth when a user is set).
core::Status httpConnect(Socket& socket, const std::string& targetHost, std::uint16_t targetPort,
                         const std::string& username, const std::string& password,
                         std::chrono::milliseconds timeout);

// Connects to target:port, directly or through `proxy`. `onConnected` (optional) receives the
// TCP socket as soon as it is connected, before any proxy negotiation, so another thread can shut
// it down to abort a slow exchange.
core::Status connectThrough(const ProxyConfig& proxy, const std::string& targetHost, std::uint16_t targetPort,
                            std::chrono::milliseconds timeout, Socket& out,
                            const std::atomic<bool>* cancel = nullptr,
                            const std::function<void(NativeSocket)>& onConnected = {});

} // namespace wt::net

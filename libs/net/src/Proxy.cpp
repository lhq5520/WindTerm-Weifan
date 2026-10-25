#include "wt/net/Proxy.h"

#include "wt/core/Base64.h"

#include <cstdint>

namespace wt::net {
namespace {

std::string bracketed(const std::string& host) {
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

std::string socksReplyText(std::uint8_t code) {
    switch (code) {
    case 1: return "general SOCKS server failure";
    case 2: return "connection not allowed by ruleset";
    case 3: return "network unreachable";
    case 4: return "host unreachable";
    case 5: return "connection refused";
    case 6: return "TTL expired";
    case 7: return "command not supported";
    case 8: return "address type not supported";
    default: return "error " + std::to_string(code);
    }
}

} // namespace

core::Status socks5Connect(Socket& socket, const std::string& targetHost, std::uint16_t targetPort,
                           const std::string& username, const std::string& password,
                           std::chrono::milliseconds timeout) {
    if (targetHost.empty() || targetHost.size() > 255) {
        return core::Status::error("SOCKS5: invalid target host name");
    }
    const bool withAuth = !username.empty();
    std::string greeting = {5, static_cast<char>(withAuth ? 2 : 1), 0};
    if (withAuth) {
        greeting.push_back(2); // username/password
    }
    if (auto status = socket.sendAll(greeting, timeout); !status) {
        return status;
    }
    char choice[2];
    if (auto status = socket.receiveExact(choice, 2, timeout); !status) {
        return core::Status::error("SOCKS5: " + status.message());
    }
    if (choice[0] != 5) {
        return core::Status::error("SOCKS5: the proxy did not answer with SOCKS version 5");
    }
    const auto method = static_cast<std::uint8_t>(choice[1]);
    if (method == 0xFF) {
        return core::Status::error(withAuth ? "SOCKS5: the proxy accepts none of our authentication methods"
                                            : "SOCKS5: the proxy requires authentication");
    }
    if (method == 2) {
        if (!withAuth || username.size() > 255 || password.size() > 255) {
            return core::Status::error("SOCKS5: the proxy requires a user name and password");
        }
        std::string auth = {1, static_cast<char>(username.size())};
        auth += username;
        auth.push_back(static_cast<char>(password.size()));
        auth += password;
        if (auto status = socket.sendAll(auth, timeout); !status) {
            return status;
        }
        char result[2];
        if (auto status = socket.receiveExact(result, 2, timeout); !status) {
            return core::Status::error("SOCKS5: " + status.message());
        }
        if (result[0] != 1 || result[1] != 0) {
            return core::Status::error("SOCKS5: authentication failed");
        }
    } else if (method != 0) {
        return core::Status::error("SOCKS5: unsupported authentication method " + std::to_string(method));
    }

    std::string request = {5, 1, 0, 3, static_cast<char>(targetHost.size())};
    request += targetHost;
    request.push_back(static_cast<char>(targetPort >> 8));
    request.push_back(static_cast<char>(targetPort & 0xFF));
    if (auto status = socket.sendAll(request, timeout); !status) {
        return status;
    }
    char head[4];
    if (auto status = socket.receiveExact(head, 4, timeout); !status) {
        return core::Status::error("SOCKS5: " + status.message());
    }
    if (head[0] != 5) {
        return core::Status::error("SOCKS5: malformed reply");
    }
    if (head[1] != 0) {
        return core::Status::error("SOCKS5: " + socksReplyText(static_cast<std::uint8_t>(head[1])));
    }
    std::size_t addressLength = 0;
    switch (head[3]) {
    case 1: addressLength = 4; break;
    case 4: addressLength = 16; break;
    case 3: {
        char length;
        if (auto status = socket.receiveExact(&length, 1, timeout); !status) {
            return status;
        }
        addressLength = static_cast<std::uint8_t>(length);
        break;
    }
    default: return core::Status::error("SOCKS5: malformed reply");
    }
    std::string bound(addressLength + 2, '\0'); // bound address + port, unused
    return socket.receiveExact(bound.data(), bound.size(), timeout);
}

core::Status httpConnect(Socket& socket, const std::string& targetHost, std::uint16_t targetPort,
                         const std::string& username, const std::string& password,
                         std::chrono::milliseconds timeout) {
    const std::string authority = bracketed(targetHost) + ":" + std::to_string(targetPort);
    std::string request = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n";
    if (!username.empty()) {
        request += "Proxy-Authorization: Basic " + core::base64Encode(username + ":" + password) + "\r\n";
    }
    request += "\r\n";
    if (auto status = socket.sendAll(request, timeout); !status) {
        return status;
    }
    // Read the response header byte by byte so no tunnelled data is consumed.
    std::string header;
    for (;;) {
        if (header.size() >= 16 * 1024) {
            return core::Status::error("HTTP proxy: response header too long");
        }
        char c;
        if (auto status = socket.receiveExact(&c, 1, timeout); !status) {
            return core::Status::error("HTTP proxy: " + status.message());
        }
        header.push_back(c);
        if (header.size() >= 4 && header.compare(header.size() - 4, 4, "\r\n\r\n") == 0) {
            break;
        }
    }
    const auto lineEnd = header.find("\r\n");
    const std::string statusLine = header.substr(0, lineEnd);
    const auto space = statusLine.find(' ');
    if (statusLine.rfind("HTTP/1.", 0) != 0 || space == std::string::npos) {
        return core::Status::error("HTTP proxy: malformed response");
    }
    const std::string code = statusLine.substr(space + 1, 3);
    if (code != "200") {
        return core::Status::error("HTTP proxy refused the tunnel: " + statusLine.substr(space + 1));
    }
    return core::Status::ok();
}

core::Status connectThrough(const ProxyConfig& proxy, const std::string& targetHost, std::uint16_t targetPort,
                            std::chrono::milliseconds timeout, Socket& out, const std::atomic<bool>* cancel,
                            const std::function<void(NativeSocket)>& onConnected) {
    if (proxy.type == ProxyType::None) {
        auto status = connectTcp(targetHost, targetPort, timeout, out, cancel);
        if (status && onConnected) {
            onConnected(out.native());
        }
        return status;
    }
    Socket socket;
    if (auto status = connectTcp(proxy.host, proxy.port, timeout, socket, cancel); !status) {
        return core::Status::error("proxy " + proxy.host + ":" + std::to_string(proxy.port) + ": " + status.message(),
                                   status.systemError());
    }
    if (onConnected) {
        onConnected(socket.native());
    }
    const core::Status status =
        proxy.type == ProxyType::Socks5
            ? socks5Connect(socket, targetHost, targetPort, proxy.username, proxy.password, timeout)
            : httpConnect(socket, targetHost, targetPort, proxy.username, proxy.password, timeout);
    if (!status) {
        return status;
    }
    out = std::move(socket);
    return core::Status::ok();
}

} // namespace wt::net

#pragma once

// In-process TCP servers for tests: an echo server and a relaying SOCKS5 / HTTP CONNECT proxy
// (optionally with authentication). Each runs on its own threads on 127.0.0.1.

#include "wt/core/Base64.h"
#include "wt/net/Socket.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace wt::test {

class TestServer {
public:
    virtual ~TestServer() { stop(); }

    bool start() {
        if (!net::listenLoopback(0, m_listener, m_port)) {
            return false;
        }
        m_thread = std::thread([this] { acceptLoop(); });
        return true;
    }
    void stop() {
        if (m_stopped.exchange(true)) {
            return;
        }
        m_listener.shutdown();
        m_listener.close();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        dropAll();
        std::lock_guard lock(m_mutex);
        for (auto& worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
    std::uint16_t port() const { return m_port; }
    int connections() const { return m_connections.load(); }

    // Abruptly closes every open connection (simulates a network failure).
    void dropAll() {
        std::lock_guard lock(m_mutex);
        for (auto& socket : m_open) {
            if (auto s = socket.lock()) {
                s->shutdown();
            }
        }
    }

protected:
    virtual void serve(std::shared_ptr<net::Socket> client) = 0;

    void track(const std::shared_ptr<net::Socket>& socket) {
        std::lock_guard lock(m_mutex);
        m_open.push_back(socket);
    }

    // Copies data both ways until either side closes.
    static void relay(net::Socket& a, net::Socket& b) {
        char buffer[16384];
        for (;;) {
#ifdef _WIN32
            WSAPOLLFD fds[2] = {{static_cast<SOCKET>(a.native()), POLLRDNORM, 0}, {static_cast<SOCKET>(b.native()), POLLRDNORM, 0}};
            if (WSAPoll(fds, 2, 200) < 0) {
                return;
            }
#else
            pollfd fds[2] = {{a.native(), POLLIN, 0}, {b.native(), POLLIN, 0}};
            if (::poll(fds, 2, 200) < 0) {
                return;
            }
#endif
            for (int i = 0; i < 2; ++i) {
                if (fds[i].revents == 0) {
                    continue;
                }
                net::Socket& from = i == 0 ? a : b;
                net::Socket& to = i == 0 ? b : a;
                std::size_t n = 0;
                if (!from.receiveSome(buffer, sizeof(buffer), n) || n == 0 || !to.sendAll({buffer, n})) {
                    a.shutdown();
                    b.shutdown();
                    return;
                }
            }
        }
    }

private:
    void acceptLoop() {
        while (!m_stopped) {
            if (!net::waitReadable(m_listener.native(), std::chrono::milliseconds(100))) {
                continue;
            }
            auto client = std::make_shared<net::Socket>();
            if (!net::acceptConnection(m_listener, *client)) {
                continue;
            }
            ++m_connections;
            track(client);
            std::lock_guard lock(m_mutex);
            m_workers.emplace_back([this, client] { serve(client); });
        }
    }

    net::Socket m_listener;
    std::uint16_t m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_stopped{false};
    std::atomic<int> m_connections{0};
    std::mutex m_mutex;
    std::vector<std::thread> m_workers;
    std::vector<std::weak_ptr<net::Socket>> m_open;
};

class EchoServer : public TestServer {
protected:
    void serve(std::shared_ptr<net::Socket> client) override {
        char buffer[4096];
        for (;;) {
            std::size_t n = 0;
            if (!client->receiveSome(buffer, sizeof(buffer), n) || n == 0 || !client->sendAll({buffer, n})) {
                return;
            }
        }
    }
};

// SOCKS5 (no auth, or username/password) and HTTP CONNECT (optionally Basic auth) relays.
class ProxyServer : public TestServer {
public:
    enum class Kind { Socks5, Http };
    explicit ProxyServer(Kind kind, std::string user = {}, std::string password = {})
        : m_kind(kind), m_user(std::move(user)), m_password(std::move(password)) {}
    std::string lastTarget() {
        std::lock_guard lock(m_targetMutex);
        return m_lastTarget;
    }

protected:
    void serve(std::shared_ptr<net::Socket> client) override {
        std::string host;
        std::uint16_t port = 0;
        if (!(m_kind == Kind::Socks5 ? socksHandshake(*client, host, port) : httpHandshake(*client, host, port))) {
            return;
        }
        {
            std::lock_guard lock(m_targetMutex);
            m_lastTarget = host + ":" + std::to_string(port);
        }
        auto upstream = std::make_shared<net::Socket>();
        const bool ok = net::connectTcp(host, port, std::chrono::milliseconds(5000), *upstream).isOk();
        if (m_kind == Kind::Socks5) {
            const char reply[] = {5, static_cast<char>(ok ? 0 : 5), 0, 1, 0, 0, 0, 0, 0, 0};
            client->sendAll({reply, sizeof(reply)});
        } else {
            client->sendAll(ok ? "HTTP/1.1 200 Connection established\r\n\r\n" : "HTTP/1.1 502 Bad Gateway\r\n\r\n");
        }
        if (!ok) {
            return;
        }
        track(upstream);
        relay(*client, *upstream);
    }

private:
    bool socksHandshake(net::Socket& s, std::string& host, std::uint16_t& port) {
        char head[2];
        if (!s.receiveExact(head, 2) || head[0] != 5) {
            return false;
        }
        std::string methods(static_cast<std::size_t>(head[1]), '\0');
        if (!s.receiveExact(methods.data(), methods.size())) {
            return false;
        }
        const char wanted = m_user.empty() ? 0 : 2;
        if (methods.find(wanted) == std::string::npos) {
            s.sendAll(std::string("\x05\xff", 2));
            return false;
        }
        s.sendAll(std::string{5, wanted});
        if (wanted == 2) {
            char version[2];
            if (!s.receiveExact(version, 2)) {
                return false;
            }
            std::string user(static_cast<std::uint8_t>(version[1]), '\0');
            char plen;
            if (!s.receiveExact(user.data(), user.size()) || !s.receiveExact(&plen, 1)) {
                return false;
            }
            std::string password(static_cast<std::uint8_t>(plen), '\0');
            if (!s.receiveExact(password.data(), password.size())) {
                return false;
            }
            const bool ok = user == m_user && password == m_password;
            s.sendAll(std::string{1, static_cast<char>(ok ? 0 : 1)});
            if (!ok) {
                return false;
            }
        }
        char request[5];
        if (!s.receiveExact(request, 5) || request[1] != 1 || request[3] != 3) {
            return false;
        }
        host.assign(static_cast<std::uint8_t>(request[4]), '\0');
        char portBytes[2];
        if (!s.receiveExact(host.data(), host.size()) || !s.receiveExact(portBytes, 2)) {
            return false;
        }
        port = static_cast<std::uint16_t>((static_cast<std::uint8_t>(portBytes[0]) << 8) |
                                          static_cast<std::uint8_t>(portBytes[1]));
        return true;
    }

    bool httpHandshake(net::Socket& s, std::string& host, std::uint16_t& port) {
        std::string header;
        while (header.find("\r\n\r\n") == std::string::npos && header.size() < 8192) {
            char c;
            if (!s.receiveExact(&c, 1)) {
                return false;
            }
            header.push_back(c);
        }
        if (header.rfind("CONNECT ", 0) != 0) {
            return false;
        }
        if (!m_user.empty()) {
            const std::string expected =
                "Proxy-Authorization: Basic " + core::base64Encode(m_user + ":" + m_password) + "\r\n";
            if (header.find(expected) == std::string::npos) {
                s.sendAll("HTTP/1.1 407 Proxy Authentication Required\r\n\r\n");
                return false;
            }
        }
        const std::string authority = header.substr(8, header.find(' ', 8) - 8);
        const auto colon = authority.rfind(':');
        host = authority.substr(0, colon);
        if (!host.empty() && host.front() == '[') {
            host = host.substr(1, host.size() - 2);
        }
        port = static_cast<std::uint16_t>(std::stoi(authority.substr(colon + 1)));
        return true;
    }

    Kind m_kind;
    std::string m_user;
    std::string m_password;
    std::mutex m_targetMutex;
    std::string m_lastTarget;
};

} // namespace wt::test

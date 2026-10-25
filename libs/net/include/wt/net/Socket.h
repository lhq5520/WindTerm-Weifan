#pragma once

#include "wt/core/Status.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace wt::net {

#ifdef _WIN32
using NativeSocket = std::uintptr_t; // SOCKET
inline constexpr NativeSocket kInvalidSocket = ~NativeSocket{0};
#else
using NativeSocket = int;
inline constexpr NativeSocket kInvalidSocket = -1;
#endif

// Initialises the socket library once (WSAStartup on Windows; no-op elsewhere). Thread-safe.
void initializeNetworking();

// Owning wrapper around a blocking stream socket.
class Socket {
public:
    Socket() = default;
    explicit Socket(NativeSocket fd) : m_fd(fd) {}
    ~Socket() { close(); }
    Socket(Socket&& other) noexcept : m_fd(std::exchange(other.m_fd, kInvalidSocket)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            m_fd = std::exchange(other.m_fd, kInvalidSocket);
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool isValid() const { return m_fd != kInvalidSocket; }
    NativeSocket native() const { return m_fd; }
    NativeSocket release() { return std::exchange(m_fd, kInvalidSocket); }
    void close();
    // Shuts down both directions without closing the descriptor (wakes blocked peers/readers).
    void shutdown();

    core::Status setNonBlocking(bool enabled);
    void setNoDelay(bool enabled);
    // TCP keep-alive; with idleSeconds > 0 probes start after that much silence and repeat every
    // intervalSeconds, so a dead peer is noticed after about idle + count * interval seconds.
    void setKeepAlive(bool enabled, int idleSeconds = 0, int intervalSeconds = 0, int count = 0);
    // Drops the connection when sent data stays unacknowledged for `seconds` (TCP_USER_TIMEOUT on
    // Linux, TCP_RXT_CONNDROPTIME on macOS, TCP_MAXRT on Windows): detects dead peers while
    // keep-alive packets are outstanding, like OpenSSH's ServerAliveCountMax.
    void setUserTimeout(int seconds);
    // One non-blocking send attempt (for wake-up pipes); returns false if nothing was sent.
    bool trySend(std::string_view data);

    // Blocking helpers with an overall timeout (0 = no timeout).
    core::Status sendAll(std::string_view data, std::chrono::milliseconds timeout = {});
    // Reads exactly `size` bytes.
    core::Status receiveExact(char* buffer, std::size_t size, std::chrono::milliseconds timeout = {});
    // Reads up to `size` bytes; 0 means the peer closed the connection.
    core::Status receiveSome(char* buffer, std::size_t size, std::size_t& received,
                             std::chrono::milliseconds timeout = {});

private:
    NativeSocket m_fd = kInvalidSocket;
};

// Resolves `host` and connects to the first address that answers within `timeout`. `cancel`
// (optional) is polled while waiting so a user can abort a slow connect.
core::Status connectTcp(std::string_view host, std::uint16_t port, std::chrono::milliseconds timeout, Socket& out,
                        const std::atomic<bool>* cancel = nullptr);

// Shuts down both directions of a descriptor owned elsewhere (thread-safe at the OS level).
void shutdownSocket(NativeSocket socket);

// Waits until the socket is readable (or writable); false on timeout. Negative timeout = forever.
bool waitReadable(NativeSocket socket, std::chrono::milliseconds timeout);
bool waitWritable(NativeSocket socket, std::chrono::milliseconds timeout);

// A connected pair of stream sockets (socketpair on POSIX, a loopback connection on Windows).
core::Status socketPair(Socket& first, Socket& second);

// Listening TCP socket on 127.0.0.1 (port 0 picks a free port); used by tests and port forwards.
core::Status listenLoopback(std::uint16_t port, Socket& out, std::uint16_t& boundPort);
core::Status acceptConnection(Socket& listener, Socket& out, std::chrono::milliseconds timeout = {});

} // namespace wt::net

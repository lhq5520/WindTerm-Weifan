#include "wt/net/Socket.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>
using socklen_type = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstddef>
using socklen_type = socklen_t;
#endif

namespace wt::net {
namespace {

using Clock = std::chrono::steady_clock;

int lastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string errorText(int code) {
#ifdef _WIN32
    char buffer[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(code), 0,
                   buffer, sizeof(buffer), nullptr);
    std::string text(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
#else
    return std::strerror(code);
#endif
}

bool wouldBlock(int code) {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
    return code == EWOULDBLOCK || code == EAGAIN || code == EINPROGRESS;
#endif
}

void closeNative(NativeSocket fd) {
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

bool waitFor(NativeSocket fd, bool write, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = static_cast<SOCKET>(fd);
    pfd.events = write ? POLLWRNORM : POLLRDNORM;
    const int rc = WSAPoll(&pfd, 1, timeout.count() < 0 ? -1 : static_cast<int>(timeout.count()));
#else
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = write ? POLLOUT : POLLIN;
    int rc;
    do {
        rc = ::poll(&pfd, 1, timeout.count() < 0 ? -1 : static_cast<int>(timeout.count()));
    } while (rc < 0 && errno == EINTR);
#endif
    return rc > 0;
}

// Remaining time until `deadline`, or -1 (infinite) when there is no deadline.
std::chrono::milliseconds remaining(Clock::time_point deadline, bool hasDeadline) {
    if (!hasDeadline) {
        return std::chrono::milliseconds(-1);
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return std::max(left, std::chrono::milliseconds(0));
}

} // namespace

void initializeNetworking() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

void Socket::close() {
    if (m_fd != kInvalidSocket) {
        closeNative(m_fd);
        m_fd = kInvalidSocket;
    }
}

void Socket::shutdown() {
    shutdownSocket(m_fd);
}

void shutdownSocket(NativeSocket socket) {
    if (socket != kInvalidSocket) {
#ifdef _WIN32
        ::shutdown(static_cast<SOCKET>(socket), SD_BOTH);
#else
        ::shutdown(socket, SHUT_RDWR);
#endif
    }
}

core::Status Socket::setNonBlocking(bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    if (ioctlsocket(static_cast<SOCKET>(m_fd), FIONBIO, &mode) != 0) {
        return core::Status::error("ioctlsocket failed", lastError());
    }
#else
    const int flags = fcntl(m_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(m_fd, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) < 0) {
        return core::Status::error(std::string("fcntl failed: ") + std::strerror(errno), errno);
    }
#endif
    return core::Status::ok();
}

void Socket::setNoDelay(bool enabled) {
    const int value = enabled ? 1 : 0;
    ::setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value), sizeof(value));
}

void Socket::setKeepAlive(bool enabled, int idleSeconds, int intervalSeconds, int count) {
    const int value = enabled ? 1 : 0;
    ::setsockopt(m_fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&value), sizeof(value));
    if (!enabled || idleSeconds <= 0) {
        return;
    }
    const int interval = intervalSeconds > 0 ? intervalSeconds : idleSeconds;
#ifdef _WIN32
    tcp_keepalive settings{};
    settings.onoff = 1;
    settings.keepalivetime = static_cast<ULONG>(idleSeconds) * 1000;
    settings.keepaliveinterval = static_cast<ULONG>(interval) * 1000;
    DWORD returned = 0;
    WSAIoctl(static_cast<SOCKET>(m_fd), SIO_KEEPALIVE_VALS, &settings, sizeof(settings), nullptr, 0, &returned, nullptr,
             nullptr);
    (void)count; // fixed at 10 probes on Windows
#else
#if defined(TCP_KEEPIDLE)
    ::setsockopt(m_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idleSeconds, sizeof(idleSeconds));
#elif defined(TCP_KEEPALIVE) // macOS
    ::setsockopt(m_fd, IPPROTO_TCP, TCP_KEEPALIVE, &idleSeconds, sizeof(idleSeconds));
#endif
#if defined(TCP_KEEPINTVL)
    ::setsockopt(m_fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif
#if defined(TCP_KEEPCNT)
    if (count > 0) {
        ::setsockopt(m_fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    }
#endif
#endif
}

void Socket::setUserTimeout(int seconds) {
    if (m_fd == kInvalidSocket || seconds <= 0) {
        return;
    }
#if defined(TCP_USER_TIMEOUT)
    const unsigned milliseconds = static_cast<unsigned>(seconds) * 1000u;
    ::setsockopt(m_fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &milliseconds, sizeof(milliseconds));
#elif defined(TCP_RXT_CONNDROPTIME)
    ::setsockopt(m_fd, IPPROTO_TCP, TCP_RXT_CONNDROPTIME, &seconds, sizeof(seconds));
#elif defined(TCP_MAXRT)
    const DWORD value = static_cast<DWORD>(seconds);
    ::setsockopt(static_cast<SOCKET>(m_fd), IPPROTO_TCP, TCP_MAXRT, reinterpret_cast<const char*>(&value), sizeof(value));
#endif
}

bool Socket::trySend(std::string_view data) {
    if (m_fd == kInvalidSocket) {
        return false;
    }
#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(static_cast<SOCKET>(m_fd), FIONBIO, &nonBlocking);
    const int n = ::send(static_cast<SOCKET>(m_fd), data.data(), static_cast<int>(data.size()), 0);
#else
    const ssize_t n = ::send(m_fd, data.data(), data.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
#endif
    return n > 0;
}

core::Status Socket::sendAll(std::string_view data, std::chrono::milliseconds timeout) {
    const bool hasDeadline = timeout.count() > 0;
    const auto deadline = Clock::now() + timeout;
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (hasDeadline && !waitFor(m_fd, true, remaining(deadline, true))) {
            return core::Status::error("timed out while sending");
        }
#ifdef _WIN32
        const int n = ::send(static_cast<SOCKET>(m_fd), data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
        const ssize_t n = ::send(m_fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
        if (n < 0) {
            const int code = lastError();
#ifndef _WIN32
            if (code == EINTR) {
                continue;
            }
#endif
            if (wouldBlock(code)) {
                waitFor(m_fd, true, std::chrono::milliseconds(100));
                continue;
            }
            return core::Status::error("send failed: " + errorText(code), code);
        }
        sent += static_cast<std::size_t>(n);
    }
    return core::Status::ok();
}

core::Status Socket::receiveSome(char* buffer, std::size_t size, std::size_t& received,
                                 std::chrono::milliseconds timeout) {
    received = 0;
    for (;;) {
        if (timeout.count() > 0 && !waitFor(m_fd, false, timeout)) {
            return core::Status::error("timed out while receiving");
        }
#ifdef _WIN32
        const int n = ::recv(static_cast<SOCKET>(m_fd), buffer, static_cast<int>(size), 0);
#else
        const ssize_t n = ::recv(m_fd, buffer, size, 0);
#endif
        if (n < 0) {
            const int code = lastError();
#ifndef _WIN32
            if (code == EINTR) {
                continue;
            }
#endif
            if (wouldBlock(code)) {
                waitFor(m_fd, false, std::chrono::milliseconds(100));
                continue;
            }
            return core::Status::error("receive failed: " + errorText(code), code);
        }
        received = static_cast<std::size_t>(n);
        return core::Status::ok();
    }
}

Socket::IoResult Socket::readSome(char* buffer, std::size_t size) {
    if (m_fd == kInvalidSocket) {
        return {IoState::Closed, 0, 0};
    }
    for (;;) {
#ifdef _WIN32
        const int n = ::recv(static_cast<SOCKET>(m_fd), buffer, static_cast<int>(std::min<std::size_t>(size, 1 << 30)), 0);
#else
        const ssize_t n = ::recv(m_fd, buffer, size, 0);
#endif
        if (n > 0) {
            return {IoState::Ok, static_cast<std::size_t>(n), 0};
        }
        if (n == 0) {
            return {IoState::Closed, 0, 0};
        }
        const int code = lastError();
#ifndef _WIN32
        if (code == EINTR) {
            continue;
        }
#endif
        if (wouldBlock(code)) {
            return {IoState::WouldBlock, 0, 0};
        }
        return {IoState::Error, 0, code};
    }
}

Socket::IoResult Socket::writeSome(std::string_view data) {
    if (m_fd == kInvalidSocket) {
        return {IoState::Closed, 0, 0};
    }
    if (data.empty()) {
        return {IoState::Ok, 0, 0};
    }
    for (;;) {
#ifdef _WIN32
        const int n = ::send(static_cast<SOCKET>(m_fd), data.data(),
                             static_cast<int>(std::min<std::size_t>(data.size(), 1 << 30)), 0);
#else
        const ssize_t n = ::send(m_fd, data.data(), data.size(), MSG_NOSIGNAL);
#endif
        if (n >= 0) {
            return {IoState::Ok, static_cast<std::size_t>(n), 0};
        }
        const int code = lastError();
#ifndef _WIN32
        if (code == EINTR) {
            continue;
        }
#endif
        if (wouldBlock(code)) {
            return {IoState::WouldBlock, 0, 0};
        }
        return {IoState::Error, 0, code};
    }
}

std::string socketErrorText(int code) {
    return errorText(code);
}

core::Status Socket::receiveExact(char* buffer, std::size_t size, std::chrono::milliseconds timeout) {
    const bool hasDeadline = timeout.count() > 0;
    const auto deadline = Clock::now() + timeout;
    std::size_t done = 0;
    while (done < size) {
        std::size_t n = 0;
        const auto left = hasDeadline ? remaining(deadline, true) : std::chrono::milliseconds(0);
        if (hasDeadline && left.count() == 0) {
            return core::Status::error("timed out while receiving");
        }
        if (auto status = receiveSome(buffer + done, size - done, n, left); !status) {
            return status;
        }
        if (n == 0) {
            return core::Status::error("connection closed by peer");
        }
        done += n;
    }
    return core::Status::ok();
}

bool waitReadable(NativeSocket socket, std::chrono::milliseconds timeout) {
    return waitFor(socket, false, timeout);
}

bool waitWritable(NativeSocket socket, std::chrono::milliseconds timeout) {
    return waitFor(socket, true, timeout);
}

core::Status connectTcp(std::string_view host, std::uint16_t port, std::chrono::milliseconds timeout, Socket& out,
                        const std::atomic<bool>* cancel) {
    initializeNetworking();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string hostName(host);
    const std::string service = std::to_string(port);
    if (const int rc = ::getaddrinfo(hostName.c_str(), service.c_str(), &hints, &results); rc != 0) {
#ifdef _WIN32
        return core::Status::error("cannot resolve " + hostName + ": " + errorText(rc), rc);
#else
        return core::Status::error("cannot resolve " + hostName + ": " + gai_strerror(rc), rc);
#endif
    }
    const bool hasDeadline = timeout.count() > 0;
    const auto deadline = Clock::now() + timeout;
    core::Status lastStatus = core::Status::error("no address for " + hostName);
    for (addrinfo* ai = results; ai; ai = ai->ai_next) {
        Socket socket(static_cast<NativeSocket>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)));
        if (!socket.isValid()) {
            lastStatus = core::Status::error("socket failed: " + errorText(lastError()), lastError());
            continue;
        }
#ifndef _WIN32
        fcntl(socket.native(), F_SETFD, FD_CLOEXEC);
#endif
        socket.setNonBlocking(true);
        int rc = ::connect(socket.native(), ai->ai_addr, static_cast<socklen_type>(ai->ai_addrlen));
        if (rc != 0 && !wouldBlock(lastError())) {
            const int code = lastError();
            lastStatus = core::Status::error("cannot connect to " + hostName + ":" + service + ": " + errorText(code), code);
            continue;
        }
        bool connected = rc == 0;
        bool timedOut = false;
        while (!connected) {
            if (cancel && cancel->load()) {
                freeaddrinfo(results);
                return core::Status::error("cancelled");
            }
            auto slice = std::chrono::milliseconds(100);
            if (hasDeadline) {
                const auto left = remaining(deadline, true);
                if (left.count() == 0) {
                    timedOut = true;
                    break;
                }
                slice = std::min(slice, left);
            }
            if (waitFor(socket.native(), true, slice)) {
                int error = 0;
                socklen_type length = sizeof(error);
                ::getsockopt(socket.native(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
                if (error != 0) {
                    lastStatus = core::Status::error(
                        "cannot connect to " + hostName + ":" + service + ": " + errorText(error), error);
                    break;
                }
                connected = true;
            }
        }
        if (timedOut) {
            lastStatus = core::Status::error("timed out connecting to " + hostName + ":" + service);
            break; // the overall deadline has passed
        }
        if (!connected) {
            continue;
        }
        socket.setNonBlocking(false);
        socket.setNoDelay(true);
        out = std::move(socket);
        freeaddrinfo(results);
        return core::Status::ok();
    }
    freeaddrinfo(results);
    return lastStatus;
}

core::Status listenLoopback(std::uint16_t port, Socket& out, std::uint16_t& boundPort) {
    initializeNetworking();
    Socket socket(static_cast<NativeSocket>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
    if (!socket.isValid()) {
        return core::Status::error("socket failed: " + errorText(lastError()), lastError());
    }
#ifndef _WIN32
    const int reuse = 1;
    ::setsockopt(socket.native(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    fcntl(socket.native(), F_SETFD, FD_CLOEXEC);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket.native(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(socket.native(), 16) != 0) {
        return core::Status::error("cannot listen: " + errorText(lastError()), lastError());
    }
    socklen_type length = sizeof(address);
    ::getsockname(socket.native(), reinterpret_cast<sockaddr*>(&address), &length);
    boundPort = ntohs(address.sin_port);
    out = std::move(socket);
    return core::Status::ok();
}

core::Status listenTcp(const std::string& address, std::uint16_t port, Socket& out, std::uint16_t& boundPort) {
    initializeNetworking();
    std::string host = address;
    if (host.empty() || host == "localhost") {
        host = "127.0.0.1";
    } else if (host == "*") {
        host = "0.0.0.0";
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    if (const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results); rc != 0) {
#ifdef _WIN32
        return core::Status::error("cannot resolve " + host + ": " + errorText(rc), rc);
#else
        return core::Status::error("cannot resolve " + host + ": " + gai_strerror(rc), rc);
#endif
    }
    core::Status lastStatus = core::Status::error("no address for " + host);
    for (addrinfo* ai = results; ai; ai = ai->ai_next) {
        Socket socket(static_cast<NativeSocket>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)));
        if (!socket.isValid()) {
            lastStatus = core::Status::error("socket failed: " + errorText(lastError()), lastError());
            continue;
        }
#ifndef _WIN32
        const int reuse = 1;
        ::setsockopt(socket.native(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        fcntl(socket.native(), F_SETFD, FD_CLOEXEC);
#else
        const BOOL exclusive = TRUE; // no port stealing between processes
        ::setsockopt(static_cast<SOCKET>(socket.native()), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
        if (ai->ai_family == AF_INET6) {
            const int v6only = 1;
            ::setsockopt(socket.native(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only),
                         sizeof(v6only));
        }
        if (::bind(socket.native(), ai->ai_addr, static_cast<socklen_type>(ai->ai_addrlen)) != 0 ||
            ::listen(socket.native(), 64) != 0) {
            const int code = lastError();
            lastStatus = core::Status::error("cannot listen on " + host + ":" + service + ": " + errorText(code), code);
            continue;
        }
        sockaddr_storage bound{};
        socklen_type length = sizeof(bound);
        ::getsockname(socket.native(), reinterpret_cast<sockaddr*>(&bound), &length);
        boundPort = bound.ss_family == AF_INET6 ? ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port)
                                                : ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
        socket.setNonBlocking(true);
        out = std::move(socket);
        freeaddrinfo(results);
        return core::Status::ok();
    }
    freeaddrinfo(results);
    return lastStatus;
}

core::Status connectUnix(const std::string& path, Socket& out) {
#ifdef _WIN32
    (void)out;
    return core::Status::error("Unix domain sockets are not supported here: " + path);
#else
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        return core::Status::error("invalid socket path: " + path);
    }
    socklen_type length = 0;
    if (path.front() == '@') {
#ifdef __linux__
        address.sun_path[0] = '\0'; // abstract namespace
        std::memcpy(address.sun_path + 1, path.data() + 1, path.size() - 1);
        length = static_cast<socklen_type>(offsetof(sockaddr_un, sun_path) + path.size());
#else
        return core::Status::error("abstract sockets are Linux-only: " + path);
#endif
    } else {
        std::memcpy(address.sun_path, path.data(), path.size());
        length = static_cast<socklen_type>(sizeof(address));
    }
    Socket socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!socket.isValid()) {
        return core::Status::error(std::string("socket failed: ") + std::strerror(errno), errno);
    }
    fcntl(socket.native(), F_SETFD, FD_CLOEXEC);
    int rc;
    do {
        rc = ::connect(socket.native(), reinterpret_cast<sockaddr*>(&address), length);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        return core::Status::error("cannot connect to " + path + ": " + std::strerror(errno), errno);
    }
    out = std::move(socket);
    return core::Status::ok();
#endif
}

core::Status acceptConnection(Socket& listener, Socket& out, std::chrono::milliseconds timeout) {
    if (timeout.count() > 0 && !waitFor(listener.native(), false, timeout)) {
        return core::Status::error("timed out waiting for a connection");
    }
    const auto fd = static_cast<NativeSocket>(::accept(listener.native(), nullptr, nullptr));
    if (fd == kInvalidSocket) {
        return core::Status::error("accept failed: " + errorText(lastError()), lastError());
    }
#ifndef _WIN32
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    out = Socket(fd);
    return core::Status::ok();
}

core::Status socketPair(Socket& first, Socket& second) {
#ifdef _WIN32
    Socket listener;
    std::uint16_t port = 0;
    if (auto status = listenLoopback(0, listener, port); !status) {
        return status;
    }
    if (auto status = connectTcp("127.0.0.1", port, std::chrono::milliseconds(5000), first); !status) {
        return status;
    }
    // Another local process could race us to the listener: only accept our own connection.
    sockaddr_in local{};
    socklen_type length = sizeof(local);
    ::getsockname(first.native(), reinterpret_cast<sockaddr*>(&local), &length);
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (auto status = acceptConnection(listener, second, std::chrono::milliseconds(5000)); !status) {
            return status;
        }
        sockaddr_in peer{};
        length = sizeof(peer);
        ::getpeername(second.native(), reinterpret_cast<sockaddr*>(&peer), &length);
        if (peer.sin_port == local.sin_port) {
            return core::Status::ok();
        }
        second.close();
    }
    return core::Status::error("socket pair: unexpected connections on the loopback listener");
#else
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return core::Status::error(std::string("socketpair failed: ") + std::strerror(errno), errno);
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    first = Socket(fds[0]);
    second = Socket(fds[1]);
    return core::Status::ok();
#endif
}

} // namespace wt::net

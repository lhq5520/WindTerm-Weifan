#include "wt/ssh/SshShell.h"

#include "Forwarding.h"
#include "Libssh2Init.h"

#include <atomic>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <poll.h>
#endif

namespace wt::ssh {

struct SshShell::Impl {
    struct Command {
        bool add = true;
        int id = 0;
        PortForward forward;
    };

    std::shared_ptr<SshConnection> connection;
    LIBSSH2_CHANNEL* channel = nullptr;
    Callbacks callbacks;
    SshConnection::ShellExtras extras;
    std::unique_ptr<detail::ForwardingEngine> engine;
    net::Socket wakeRead;                    // the I/O thread polls this
    std::shared_ptr<net::Socket> wakeWrite;  // other threads (and connect helpers) poke this
    std::thread thread;
    mutable std::mutex mutex;
    std::string pendingWrite;
    std::vector<Command> commands;
    std::atomic<int> nextForwardId{1};
    bool resizePending = false;
    int rows = 0, columns = 0, pixelWidth = 0, pixelHeight = 0;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    int closedStatus = -1;
    std::string closedError;

    void wake() {
        // Never block: if the pipe is full a wake-up is already pending.
        wakeWrite->trySend("x");
    }

    void finish(int exitStatus, const std::string& error) {
        if (stop.exchange(true)) {
            return;
        }
        {
            std::lock_guard lock(mutex);
            closedStatus = exitStatus;
            closedError = error;
        }
        finished = true;
        if (callbacks.onClosed) {
            callbacks.onClosed(exitStatus, error);
        }
    }

    // Reads all available shell output. False (with `error`) when the connection failed.
    bool readShell(char* buffer, std::size_t size, bool& progress, std::string& error) {
        LIBSSH2_SESSION* session = connection->session();
        // Stderr is merged into the PTY by the server, but drain it anyway. Reading one stream
        // can pull packets of the other (and of forwarded channels) into libssh2's queues, so
        // repeat until a full pass makes no progress.
        for (bool more = true; more;) {
            more = false;
            for (int stream : {0, SSH_EXTENDED_DATA_STDERR}) {
                for (;;) {
                    const auto n = libssh2_channel_read_ex(channel, stream, buffer, size);
                    if (n > 0) {
                        more = true;
                        progress = true;
                        if (callbacks.onData) {
                            callbacks.onData(std::string_view(buffer, static_cast<std::size_t>(n)));
                        }
                        continue;
                    }
                    if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
                        error = "Connection lost: " + libssh2Error(session);
                        return false;
                    }
                    break;
                }
            }
        }
        return true;
    }

    void run() {
        LIBSSH2_SESSION* session = connection->session();
        std::string writing;
        char buffer[32768];
        std::vector<detail::PollRequest> requests;
        while (!stop) {
            std::vector<Command> todo;
            {
                std::lock_guard lock(mutex);
                writing += pendingWrite;
                pendingWrite.clear();
                todo.swap(commands);
            }
            for (const Command& command : todo) {
                if (command.add) {
                    engine->add(command.id, command.forward);
                } else {
                    engine->remove(command.id);
                }
            }
            bool progress = !todo.empty();
            // Outgoing keystrokes.
            while (!writing.empty()) {
                const auto n = libssh2_channel_write(channel, writing.data(), writing.size());
                if (n == LIBSSH2_ERROR_EAGAIN || n == 0) {
                    break;
                }
                if (n < 0) {
                    finish(-1, "Connection lost: " + libssh2Error(session));
                    return;
                }
                writing.erase(0, static_cast<std::size_t>(n));
                progress = true;
            }
            // Window size changes.
            bool resize = false;
            int r = 0, c = 0, pw = 0, ph = 0;
            {
                std::lock_guard lock(mutex);
                resize = resizePending;
                r = rows, c = columns, pw = pixelWidth, ph = pixelHeight;
            }
            if (resize) {
                const int rc = libssh2_channel_request_pty_size_ex(channel, c, r, pw, ph);
                if (rc != LIBSSH2_ERROR_EAGAIN) {
                    std::lock_guard lock(mutex);
                    if (rows == r && columns == c && pixelWidth == pw && pixelHeight == ph) {
                        resizePending = false;
                    }
                }
            }
            std::string error;
            if (!readShell(buffer, sizeof(buffer), progress, error)) {
                finish(-1, error);
                return;
            }
            // Port forwards, agent and X11 channels.
            std::string fatal;
            progress |= engine->process(fatal);
            if (!fatal.empty()) {
                finish(-1, fatal);
                return;
            }
            // Forwarded channels may have queued shell output while reading their own data.
            if (!readShell(buffer, sizeof(buffer), progress, error)) {
                finish(-1, error);
                return;
            }
            if (libssh2_channel_eof(channel)) {
                // The remote shell exited: collect its status.
                libssh2_session_set_blocking(session, 1);
                libssh2_session_set_timeout(session, 2000);
                libssh2_channel_close(channel);
                libssh2_channel_wait_closed(channel);
                const int status = libssh2_channel_get_exit_status(channel);
                char* signal = nullptr;
                std::size_t signalLength = 0;
                libssh2_channel_get_exit_signal(channel, &signal, &signalLength, nullptr, nullptr, nullptr, nullptr);
                const std::string reason = signal ? "terminated by signal " + std::string(signal, signalLength) : "";
                if (signal) {
                    libssh2_free(session, signal);
                }
                libssh2_session_set_blocking(session, 0);
                finish(signal ? 128 : status, reason);
                return;
            }
            int nextKeepAlive = 0;
            if (libssh2_keepalive_send(session, &nextKeepAlive) != 0) {
                finish(-1, "Connection lost: " + libssh2Error(session));
                return;
            }
            if (progress) {
                continue; // more may be possible right away
            }
            const int timeoutMs = nextKeepAlive > 0 ? std::min(nextKeepAlive * 1000, 1000) : 1000;
            const int directions = libssh2_session_block_directions(session);
            const bool sessionWrite = !writing.empty() || (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND);
            requests.clear();
            engine->pollRequests(requests);
#ifdef _WIN32
            std::vector<WSAPOLLFD> fds;
            fds.push_back({static_cast<SOCKET>(connection->socket()),
                           static_cast<SHORT>(POLLRDNORM | (sessionWrite ? POLLWRNORM : 0)), 0});
            fds.push_back({static_cast<SOCKET>(wakeRead.native()), POLLRDNORM, 0});
            for (const auto& request : requests) {
                fds.push_back({static_cast<SOCKET>(request.fd),
                               static_cast<SHORT>((request.read ? POLLRDNORM : 0) | (request.write ? POLLWRNORM : 0)), 0});
            }
            WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMs);
            const bool woken = fds[1].revents != 0;
#else
            std::vector<pollfd> fds;
            fds.push_back({connection->socket(), static_cast<short>(POLLIN | (sessionWrite ? POLLOUT : 0)), 0});
            fds.push_back({wakeRead.native(), POLLIN, 0});
            for (const auto& request : requests) {
                fds.push_back({request.fd, static_cast<short>((request.read ? POLLIN : 0) | (request.write ? POLLOUT : 0)), 0});
            }
            ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
            const bool woken = fds[1].revents != 0;
#endif
            if (woken) {
                char drain[256];
                std::size_t n = 0;
                wakeRead.receiveSome(drain, sizeof(drain), n);
            }
        }
    }
};

SshShell::SshShell(std::unique_ptr<Impl> impl) : m(std::move(impl)) {}

std::unique_ptr<SshShell> SshShell::start(std::shared_ptr<SshConnection> connection, const ShellOptions& options,
                                          Callbacks callbacks, std::string* error) {
    auto impl = std::make_unique<Impl>();
    impl->channel = connection->openShellChannel(options, error, &impl->extras);
    if (!impl->channel) {
        return nullptr;
    }
    impl->wakeWrite = std::make_shared<net::Socket>();
    if (auto status = net::socketPair(impl->wakeRead, *impl->wakeWrite); !status) {
        if (error) {
            *error = status.message();
        }
        libssh2_channel_free(impl->channel);
        return nullptr;
    }
    impl->wakeRead.setNonBlocking(true);
    impl->connection = std::move(connection);
    impl->callbacks = std::move(callbacks);
    impl->rows = options.rows;
    impl->columns = options.columns;
    impl->pixelWidth = options.pixelWidth;
    impl->pixelHeight = options.pixelHeight;
    for (const std::string& warning : impl->extras.warnings) {
        if (impl->callbacks.onNotice) {
            impl->callbacks.onNotice(warning);
        }
    }
    // Listening sockets are opened here (on the caller's thread) so the first status is known.
    std::shared_ptr<net::Socket> wake = impl->wakeWrite;
    impl->engine = std::make_unique<detail::ForwardingEngine>(
        *impl->connection, options.forwarding, impl->extras, impl->callbacks.onNotice, [wake] { wake->trySend("x"); });
    impl->nextForwardId = static_cast<int>(options.forwarding.ports.size()) + 1;
    impl->connection->setBlocking(false);
    Impl* raw = impl.get();
    impl->thread = std::thread([raw] { raw->run(); });
    return std::unique_ptr<SshShell>(new SshShell(std::move(impl)));
}

SshShell::~SshShell() {
    close();
}

void SshShell::write(std::string_view data) {
    {
        std::lock_guard lock(m->mutex);
        m->pendingWrite.append(data);
    }
    m->wake();
}

void SshShell::resize(int rows, int columns, int pixelWidth, int pixelHeight) {
    {
        std::lock_guard lock(m->mutex);
        m->rows = rows;
        m->columns = columns;
        m->pixelWidth = pixelWidth;
        m->pixelHeight = pixelHeight;
        m->resizePending = true;
    }
    m->wake();
}

void SshShell::close() {
    if (!m || !m->thread.joinable()) {
        return;
    }
    m->stop = true;
    m->wake();
    m->thread.join();
    LIBSSH2_SESSION* session = m->connection->session();
    libssh2_session_set_blocking(session, 1);
    libssh2_session_set_timeout(session, 1000);
    if (m->engine) {
        m->engine->shutdown(); // keeps the final status for forwards()
    }
    if (m->channel) {
        libssh2_channel_free(m->channel);
        m->channel = nullptr;
    }
}

std::shared_ptr<SshConnection> SshShell::connection() const {
    return m->connection;
}

bool SshShell::hasFinished() const {
    return m->finished.load();
}

int SshShell::exitStatus() const {
    std::lock_guard lock(m->mutex);
    return m->closedStatus;
}

std::string SshShell::closeError() const {
    std::lock_guard lock(m->mutex);
    return m->closedError;
}

int SshShell::addForward(const PortForward& forward) {
    const int id = m->nextForwardId++;
    {
        std::lock_guard lock(m->mutex);
        m->commands.push_back({true, id, forward});
    }
    m->wake();
    return id;
}

void SshShell::removeForward(int id) {
    {
        std::lock_guard lock(m->mutex);
        m->commands.push_back({false, id, {}});
    }
    m->wake();
}

std::vector<ForwardStatus> SshShell::forwards() const {
    return m->engine ? m->engine->status() : std::vector<ForwardStatus>{};
}

bool SshShell::agentForwarding() const {
    return m->extras.agentForwarding;
}

bool SshShell::x11Forwarding() const {
    return m->extras.x11Forwarding;
}

} // namespace wt::ssh

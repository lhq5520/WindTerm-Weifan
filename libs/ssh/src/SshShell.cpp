#include "wt/ssh/SshShell.h"

#include "Libssh2Init.h"

#include <mutex>
#include <thread>

#ifndef _WIN32
#include <poll.h>
#endif

namespace wt::ssh {

struct SshShell::Impl {
    std::shared_ptr<SshConnection> connection;
    LIBSSH2_CHANNEL* channel = nullptr;
    Callbacks callbacks;
    net::Socket wakeRead;  // the I/O thread polls this
    net::Socket wakeWrite; // other threads poke this
    std::thread thread;
    std::mutex mutex;
    std::string pendingWrite;
    bool resizePending = false;
    int rows = 0, columns = 0, pixelWidth = 0, pixelHeight = 0;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    int closedStatus = -1;
    std::string closedError;

    void wake() {
        // Never block: if the pipe is full a wake-up is already pending.
        wakeWrite.trySend("x");
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

    void run() {
        LIBSSH2_SESSION* session = connection->session();
        std::string writing;
        char buffer[32768];
        while (!stop) {
            {
                std::lock_guard lock(mutex);
                writing += pendingWrite;
                pendingWrite.clear();
            }
            // Outgoing keystrokes.
            while (!writing.empty()) {
                const auto n = libssh2_channel_write(channel, writing.data(), writing.size());
                if (n == LIBSSH2_ERROR_EAGAIN) {
                    break;
                }
                if (n < 0) {
                    finish(-1, "Connection lost: " + libssh2Error(session));
                    return;
                }
                writing.erase(0, static_cast<std::size_t>(n));
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
            // Incoming output (stderr is merged into the PTY by the server, but drain it anyway).
            // Reading one stream can pull packets of the other into libssh2's queue, so repeat
            // until a full pass makes no progress; otherwise poll() could sleep on queued data.
            for (bool progress = true; progress;) {
                progress = false;
                for (int stream : {0, SSH_EXTENDED_DATA_STDERR}) {
                    for (;;) {
                        const auto n = libssh2_channel_read_ex(channel, stream, buffer, sizeof(buffer));
                        if (n > 0) {
                            progress = true;
                            if (callbacks.onData) {
                                callbacks.onData(std::string_view(buffer, static_cast<std::size_t>(n)));
                            }
                            continue;
                        }
                        if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
                            finish(-1, "Connection lost: " + libssh2Error(session));
                            return;
                        }
                        break;
                    }
                }
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
                const std::string error = signal ? "terminated by signal " + std::string(signal, signalLength) : "";
                if (signal) {
                    libssh2_free(session, signal);
                }
                libssh2_session_set_blocking(session, 0);
                finish(signal ? 128 : status, error);
                return;
            }
            int nextKeepAlive = 0;
            if (libssh2_keepalive_send(session, &nextKeepAlive) != 0) {
                finish(-1, "Connection lost: " + libssh2Error(session));
                return;
            }
            const int timeoutMs = nextKeepAlive > 0 ? std::min(nextKeepAlive * 1000, 1000) : 1000;
            const int directions = libssh2_session_block_directions(session);
#ifdef _WIN32
            WSAPOLLFD fds[2] = {{static_cast<SOCKET>(connection->socket()), POLLRDNORM, 0},
                                {static_cast<SOCKET>(wakeRead.native()), POLLRDNORM, 0}};
            if (!writing.empty() || (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND)) {
                fds[0].events |= POLLWRNORM;
            }
            WSAPoll(fds, 2, timeoutMs);
            const bool woken = fds[1].revents != 0;
#else
            pollfd fds[2] = {{connection->socket(), POLLIN, 0}, {wakeRead.native(), POLLIN, 0}};
            if (!writing.empty() || (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND)) {
                fds[0].events |= POLLOUT;
            }
            ::poll(fds, 2, timeoutMs);
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
    impl->channel = connection->openShellChannel(options, error);
    if (!impl->channel) {
        return nullptr;
    }
    if (auto status = net::socketPair(impl->wakeRead, impl->wakeWrite); !status) {
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
    if (m->channel) {
        LIBSSH2_SESSION* session = m->connection->session();
        libssh2_session_set_blocking(session, 1);
        libssh2_session_set_timeout(session, 1000);
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

} // namespace wt::ssh

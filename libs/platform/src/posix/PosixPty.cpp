// POSIX pseudo-terminal backend (Linux, macOS incl. Apple Silicon, *BSD).
//
// Replaces the legacy src/Pty/UnixPty.cpp, which relied on QProcess::setupChildProcess()
// (removed in Qt 6), wrote utmp records from the child and called non async-signal-safe
// functions after fork(). Here everything the child needs is prepared before fork(), and the
// child only performs async-signal-safe calls before execve().

#include "wt/platform/Pty.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

extern char** environ;

namespace wt::platform {
namespace {

using core::Status;
using Clock = std::chrono::steady_clock;

Status errnoStatus(const std::string& what, int err) {
    return Status::error(what + ": " + std::strerror(err), err);
}

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : m_fd(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    int release() {
        const int fd = m_fd;
        m_fd = -1;
        return fd;
    }
    void reset(int fd = -1) {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_fd = fd;
    }

private:
    int m_fd = -1;
};

bool setFdFlag(int fd, int flag) {
    const int flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && ::fcntl(fd, F_SETFD, flags | flag) == 0;
}

bool setStatusFlag(int fd, int flag) {
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | flag) == 0;
}

struct winsize toWinsize(const PtySize& size) {
    struct winsize ws {};
    ws.ws_row = size.rows;
    ws.ws_col = size.columns;
    ws.ws_xpixel = size.pixelWidth;
    ws.ws_ypixel = size.pixelHeight;
    return ws;
}

Status configureTermios(int fd) {
    struct termios t {};
    if (::tcgetattr(fd, &t) != 0) {
        return errnoStatus("tcgetattr", errno);
    }

    t.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT;
#ifdef IUTF8
    t.c_iflag |= IUTF8;
#endif
    t.c_oflag = OPOST | ONLCR;
    t.c_cflag = CREAD | CS8 | HUPCL;
    t.c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL;

    t.c_cc[VEOF] = 0x04;    // ^D
    t.c_cc[VEOL] = _POSIX_VDISABLE;
    t.c_cc[VERASE] = 0x7F;  // DEL, matches what the key encoder sends for Backspace
    t.c_cc[VINTR] = 0x03;   // ^C
    t.c_cc[VKILL] = 0x15;   // ^U
    t.c_cc[VQUIT] = 0x1C;   // ^backslash
    t.c_cc[VSTART] = 0x11;  // ^Q
    t.c_cc[VSTOP] = 0x13;   // ^S
    t.c_cc[VSUSP] = 0x1A;   // ^Z
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
#ifdef VEOL2
    t.c_cc[VEOL2] = _POSIX_VDISABLE;
#endif
#ifdef VWERASE
    t.c_cc[VWERASE] = 0x17; // ^W
#endif
#ifdef VREPRINT
    t.c_cc[VREPRINT] = 0x12; // ^R
#endif
#ifdef VLNEXT
    t.c_cc[VLNEXT] = 0x16; // ^V
#endif
#ifdef VDISCARD
    t.c_cc[VDISCARD] = 0x0F; // ^O
#endif
#ifdef VDSUSP
    t.c_cc[VDSUSP] = 0x19; // ^Y (BSD/macOS)
#endif
#ifdef VSTATUS
    t.c_cc[VSTATUS] = 0x14; // ^T (BSD/macOS)
#endif
    ::cfsetispeed(&t, B38400);
    ::cfsetospeed(&t, B38400);

    if (::tcsetattr(fd, TCSANOW, &t) != 0) {
        return errnoStatus("tcsetattr", errno);
    }
    return Status::ok();
}

std::string_view envName(std::string_view entry) {
    return entry.substr(0, entry.find('='));
}

std::vector<std::string> buildEnvironment(const PtyOptions& options) {
    std::vector<std::string> env;
    auto overridden = [&options](std::string_view name) {
        for (const auto& [key, value] : options.environment) {
            if (key == name) {
                return true;
            }
        }
        return false;
    };

    if (options.inheritEnvironment) {
        for (char** entry = environ; entry && *entry; ++entry) {
            const std::string_view kv(*entry);
            const std::string_view name = envName(kv);
            // Stale geometry variables confuse full-screen programs; the PTY reports the size.
            if (name == "LINES" || name == "COLUMNS" || name == "TERMCAP" || overridden(name)) {
                continue;
            }
            env.emplace_back(kv);
        }
    }
    for (const auto& [key, value] : options.environment) {
        env.push_back(key + "=" + value);
    }
    return env;
}

std::string lookupEnv(const std::vector<std::string>& env, std::string_view name) {
    for (const auto& entry : env) {
        if (envName(entry) == name && entry.size() > name.size()) {
            return entry.substr(name.size() + 1);
        }
    }
    return {};
}

bool isExecutableFile(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

// Error report sent from the child to the parent through a close-on-exec pipe.
struct ChildError {
    enum Stage : int { Setsid = 1, ControllingTty, Dup, Chdir, Exec };
    int stage;
    int error;
};

const char* stageName(int stage) {
    switch (stage) {
    case ChildError::Setsid: return "setsid";
    case ChildError::ControllingTty: return "acquire controlling terminal";
    case ChildError::Dup: return "redirect stdio";
    case ChildError::Chdir: return "change working directory";
    case ChildError::Exec: return "execute program";
    default: return "start child";
    }
}

struct ChildSetup {
    int master;
    int slave;
    int errorPipe;
    long maxFd;
    const char* executable;
    const char* workingDirectory; // nullptr: inherit
    char* const* argv;
    char* const* envp;
};

// Runs in the forked child: async-signal-safe calls only.
[[noreturn]] void runChild(const ChildSetup& setup) {
    auto fail = [&setup](int stage) {
        const ChildError report{stage, errno};
        [[maybe_unused]] const ssize_t written = ::write(setup.errorPipe, &report, sizeof(report));
        ::_exit(127);
    };

#ifdef NSIG
    constexpr int kSignalCount = NSIG;
#else
    constexpr int kSignalCount = 32;
#endif
    for (int sig = 1; sig < kSignalCount; ++sig) {
        ::signal(sig, SIG_DFL);
    }
    sigset_t empty;
    sigemptyset(&empty);
    ::sigprocmask(SIG_SETMASK, &empty, nullptr);

    if (::setsid() < 0) {
        fail(ChildError::Setsid);
    }
#ifdef TIOCSCTTY
    if (::ioctl(setup.slave, TIOCSCTTY, 0) < 0) {
        fail(ChildError::ControllingTty);
    }
#endif
    for (int target = STDIN_FILENO; target <= STDERR_FILENO; ++target) {
        if (::dup2(setup.slave, target) < 0) {
            fail(ChildError::Dup);
        }
    }
    if (setup.slave > STDERR_FILENO) {
        ::close(setup.slave);
    }
    ::close(setup.master);

    // Do not leak the parent's descriptors (GUI sockets, files) into the shell.
    bool marked = false;
#if defined(__linux__) && defined(SYS_close_range)
    constexpr unsigned kCloseRangeCloexec = 1u << 2; // CLOSE_RANGE_CLOEXEC, Linux >= 5.11
    marked = ::syscall(SYS_close_range, 3u, ~0u, kCloseRangeCloexec) == 0;
#endif
    if (!marked) {
        for (int fd = 3; fd < setup.maxFd; ++fd) {
            if (fd != setup.errorPipe) {
                const int flags = ::fcntl(fd, F_GETFD);
                if (flags >= 0) {
                    ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
                }
            }
        }
    }

    if (setup.workingDirectory && ::chdir(setup.workingDirectory) != 0) {
        fail(ChildError::Chdir);
    }
    ::execve(setup.executable, setup.argv, setup.envp);
    fail(ChildError::Exec);
    ::_exit(127); // unreachable, keeps [[noreturn]] honest for the compiler
}

std::vector<char*> toCStringArray(std::vector<std::string>& strings) {
    std::vector<char*> result;
    result.reserve(strings.size() + 1);
    for (auto& s : strings) {
        result.push_back(s.data());
    }
    result.push_back(nullptr);
    return result;
}

class PosixPty final : public Pty {
public:
    PosixPty() = default;
    ~PosixPty() override {
        terminate(std::chrono::milliseconds(200));
        closeMaster();
    }

    Status start(const PtyOptions& options) override;
    IoResult read(char* buffer, std::size_t size) override;
    IoResult write(std::string_view data) override;
    Status resize(const PtySize& size) override;
    bool waitForReadable(std::chrono::milliseconds timeout) override;
    bool isRunning() override { return m_pid > 0 && !pollExit().has_value(); }
    std::optional<int> pollExit() override;
    std::optional<int> waitForExit(std::chrono::milliseconds timeout) override;
    void terminate(std::chrono::milliseconds grace) override;
    std::intptr_t nativeHandle() const override { return m_master; }
    std::int64_t processId() const override { return m_pid; }
    PtySize size() const override { return m_size; }

private:
    void closeMaster() {
        if (m_master >= 0) {
            ::close(m_master);
            m_master = -1;
        }
    }

    int m_master = -1;
    pid_t m_pid = -1;
    std::optional<int> m_exitCode;
    PtySize m_size;
};

Status PosixPty::start(const PtyOptions& options) {
    if (m_master >= 0 || m_pid > 0) {
        return Status::error("PTY already started");
    }

    std::vector<std::string> env = buildEnvironment(options);
    const std::string program = options.program.empty() ? defaultShell() : options.program;
    std::string pathVariable = lookupEnv(env, "PATH");
    if (pathVariable.empty()) {
        pathVariable = "/usr/local/bin:/usr/bin:/bin";
    }
    std::string executable = findExecutable(program, pathVariable);
    if (executable.empty()) {
        return Status::error("Program not found or not executable: " + program, ENOENT);
    }

    const auto slash = program.find_last_of('/');
    std::string argv0 = slash == std::string::npos ? program : program.substr(slash + 1);
    if (options.loginShell) {
        argv0.insert(argv0.begin(), '-');
    }
    std::vector<std::string> args;
    args.reserve(options.arguments.size() + 1);
    args.push_back(std::move(argv0));
    args.insert(args.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv = toCStringArray(args);
    std::vector<char*> envp = toCStringArray(env);

    UniqueFd master(::posix_openpt(O_RDWR | O_NOCTTY));
    if (!master.valid()) {
        return errnoStatus("posix_openpt", errno);
    }
    if (::grantpt(master.get()) != 0) {
        return errnoStatus("grantpt", errno);
    }
    if (::unlockpt(master.get()) != 0) {
        return errnoStatus("unlockpt", errno);
    }
    char slaveName[128] = {};
#if defined(__linux__) || defined(__APPLE__)
    if (::ptsname_r(master.get(), slaveName, sizeof(slaveName)) != 0) {
        return errnoStatus("ptsname_r", errno);
    }
#else
    const char* name = ::ptsname(master.get());
    if (!name) {
        return errnoStatus("ptsname", errno);
    }
    std::strncpy(slaveName, name, sizeof(slaveName) - 1);
#endif
    if (!setFdFlag(master.get(), FD_CLOEXEC) || !setStatusFlag(master.get(), O_NONBLOCK)) {
        return errnoStatus("configure master descriptor", errno);
    }

    UniqueFd slave(::open(slaveName, O_RDWR | O_NOCTTY));
    if (!slave.valid()) {
        return errnoStatus(std::string("open ") + slaveName, errno);
    }
    if (!setFdFlag(slave.get(), FD_CLOEXEC)) {
        return errnoStatus("configure slave descriptor", errno);
    }
    if (Status s = configureTermios(slave.get()); !s) {
        return s;
    }
    const struct winsize ws = toWinsize(options.size);
    if (::ioctl(slave.get(), TIOCSWINSZ, &ws) != 0) {
        return errnoStatus("TIOCSWINSZ", errno);
    }

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        return errnoStatus("pipe", errno);
    }
    UniqueFd errorRead(pipeFds[0]);
    UniqueFd errorWrite(pipeFds[1]);
    if (!setFdFlag(errorRead.get(), FD_CLOEXEC) || !setFdFlag(errorWrite.get(), FD_CLOEXEC)) {
        return errnoStatus("configure error pipe", errno);
    }

    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd <= 0 || maxFd > 65536) {
        maxFd = 65536;
    }
    const ChildSetup setup{master.get(),
                           slave.get(),
                           errorWrite.get(),
                           maxFd,
                           executable.c_str(),
                           options.workingDirectory.empty() ? nullptr : options.workingDirectory.c_str(),
                           argv.data(),
                           envp.data()};

    // Block signals across fork() so no parent handler runs in the child before it resets them.
    sigset_t all;
    sigset_t previous;
    sigfillset(&all);
    ::pthread_sigmask(SIG_SETMASK, &all, &previous);
    const pid_t pid = ::fork();
    if (pid == 0) {
        runChild(setup);
    }
    const int forkError = errno;
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);

    slave.reset();
    errorWrite.reset();
    if (pid < 0) {
        return errnoStatus("fork", forkError);
    }

    ChildError report{};
    std::size_t received = 0;
    while (received < sizeof(report)) {
        const ssize_t n = ::read(errorRead.get(), reinterpret_cast<char*>(&report) + received,
                                 sizeof(report) - received);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        received += static_cast<std::size_t>(n);
    }
    if (received == sizeof(report)) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return errnoStatus(std::string("Failed to ") + stageName(report.stage) + " (" + executable + ")",
                           report.error);
    }

    m_master = master.release();
    m_pid = pid;
    m_exitCode.reset();
    m_size = options.size;
    return Status::ok();
}

IoResult PosixPty::read(char* buffer, std::size_t size) {
    if (m_master < 0) {
        return {IoState::Closed, 0, 0};
    }
    for (;;) {
        const ssize_t n = ::read(m_master, buffer, size);
        if (n > 0) {
            return {IoState::Ok, static_cast<std::size_t>(n), 0};
        }
        if (n == 0) {
            return {IoState::Closed, 0, 0}; // BSD/macOS report hangup as EOF
        }
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return {IoState::WouldBlock, 0, 0};
        }
        if (err == EIO) {
            return {IoState::Closed, 0, err}; // Linux reports hangup as EIO
        }
        return {IoState::Error, 0, err};
    }
}

IoResult PosixPty::write(std::string_view data) {
    if (m_master < 0) {
        return {IoState::Closed, 0, 0};
    }
    if (data.empty()) {
        return {IoState::Ok, 0, 0};
    }
    for (;;) {
        const ssize_t n = ::write(m_master, data.data(), data.size());
        if (n >= 0) {
            return {IoState::Ok, static_cast<std::size_t>(n), 0};
        }
        const int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return {IoState::WouldBlock, 0, 0};
        }
        if (err == EIO) {
            return {IoState::Closed, 0, err};
        }
        return {IoState::Error, 0, err};
    }
}

Status PosixPty::resize(const PtySize& size) {
    if (m_master < 0) {
        return Status::error("PTY not started");
    }
    if (size.rows == 0 || size.columns == 0) {
        return Status::error("Invalid PTY size");
    }
    const struct winsize ws = toWinsize(size);
    // The kernel delivers SIGWINCH to the foreground process group.
    if (::ioctl(m_master, TIOCSWINSZ, &ws) != 0) {
        return errnoStatus("TIOCSWINSZ", errno);
    }
    m_size = size;
    return Status::ok();
}

bool PosixPty::waitForReadable(std::chrono::milliseconds timeout) {
    if (m_master < 0) {
        return false;
    }
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        struct pollfd pfd {m_master, POLLIN, 0};
        const int r = ::poll(&pfd, 1, static_cast<int>(remaining > 0 ? remaining : 0));
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return r > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    }
}

std::optional<int> PosixPty::pollExit() {
    if (m_exitCode || m_pid <= 0) {
        return m_exitCode;
    }
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(m_pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);

    if (r == m_pid) {
        if (WIFEXITED(status)) {
            m_exitCode = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            m_exitCode = 128 + WTERMSIG(status);
        } else {
            return std::nullopt; // stopped/continued
        }
    } else if (r < 0 && errno == ECHILD) {
        m_exitCode = -1; // reaped elsewhere (e.g. a SIGCHLD handler set to SIG_IGN)
    }
    return m_exitCode;
}

std::optional<int> PosixPty::waitForExit(std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        if (auto code = pollExit()) {
            return code;
        }
        if (m_pid <= 0 || Clock::now() >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void PosixPty::terminate(std::chrono::milliseconds grace) {
    if (m_pid <= 0 || pollExit()) {
        return;
    }
    // The child is a session leader, so -pid addresses its whole process group.
    if (::kill(-m_pid, SIGHUP) != 0) {
        ::kill(m_pid, SIGHUP);
    }
    closeMaster();
    if (waitForExit(grace)) {
        return;
    }
    if (::kill(-m_pid, SIGKILL) != 0) {
        ::kill(m_pid, SIGKILL);
    }
    waitForExit(std::chrono::milliseconds(1000));
}

} // namespace

std::unique_ptr<Pty> createPty() {
    return std::make_unique<PosixPty>();
}

std::string defaultShell() {
    if (const char* shell = std::getenv("SHELL"); shell && *shell && isExecutableFile(shell)) {
        return shell;
    }
    if (const struct passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_shell && *pw->pw_shell) {
        if (isExecutableFile(pw->pw_shell)) {
            return pw->pw_shell;
        }
    }
    return "/bin/sh";
}

std::string findExecutable(std::string_view program, std::string_view pathVariable) {
    if (program.empty()) {
        return {};
    }
    if (program.find('/') != std::string_view::npos) {
        std::string path(program);
        return isExecutableFile(path) ? path : std::string();
    }
    std::size_t begin = 0;
    while (begin <= pathVariable.size()) {
        std::size_t end = pathVariable.find(':', begin);
        if (end == std::string_view::npos) {
            end = pathVariable.size();
        }
        std::string dir(pathVariable.substr(begin, end - begin));
        if (dir.empty()) {
            dir = ".";
        }
        std::string candidate = dir + "/" + std::string(program);
        if (isExecutableFile(candidate)) {
            return candidate;
        }
        begin = end + 1;
    }
    return {};
}

} // namespace wt::platform

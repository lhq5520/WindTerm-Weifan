// Integration tests for the POSIX PTY backend: they spawn real processes under /bin/sh.

#include "WtTest.h"

#include "wt/platform/Pty.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

using namespace wt::platform;
using namespace std::chrono_literals;

namespace {

PtyOptions shell(const std::string& script, PtySize size = {}) {
    PtyOptions options;
    options.program = "/bin/sh";
    options.arguments = {"-c", script};
    options.size = size;
    options.environment = {{"TERM", "xterm-256color"}};
    return options;
}

// Reads until `needle` shows up, the PTY closes, or the timeout expires.
std::string readUntil(Pty& pty, const std::string& needle, std::chrono::milliseconds timeout = 5000ms) {
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        if (!needle.empty() && output.find(needle) != std::string::npos) {
            break;
        }
        if (!pty.waitForReadable(50ms)) {
            continue;
        }
        const IoResult r = pty.read(buffer, sizeof(buffer));
        if (r.state == IoState::Ok) {
            output.append(buffer, r.bytes);
        } else if (r.state == IoState::Closed || r.state == IoState::Error) {
            break;
        }
    }
    return output;
}

bool writeAll(Pty& pty, std::string_view data) {
    while (!data.empty()) {
        const IoResult r = pty.write(data);
        if (r.state == IoState::Ok) {
            data.remove_prefix(r.bytes);
        } else if (r.state != IoState::WouldBlock) {
            return false;
        }
    }
    return true;
}

} // namespace

WT_TEST(pty_runs_command_and_reports_exit_code) {
    auto pty = createPty();
    const auto status = pty->start(shell("echo hello-pty; exit 3"));
    WT_REQUIRE(status.isOk());
    WT_CHECK(pty->processId() > 0);
    WT_CHECK(pty->nativeHandle() >= 0);
    const std::string out = readUntil(*pty, "");
    WT_CHECK(out.find("hello-pty\r\n") != std::string::npos); // ONLCR translates \n to \r\n
    WT_CHECK_EQ(pty->waitForExit(5000ms).value_or(-100), 3);
    WT_CHECK(!pty->isRunning());
}

WT_TEST(pty_child_has_controlling_terminal) {
    auto pty = createPty();
    WT_REQUIRE(pty->start(shell("if [ -t 0 ] && [ -t 1 ]; then echo IS_TTY; fi; tty")).isOk());
    const std::string out = readUntil(*pty, "");
    WT_CHECK(out.find("IS_TTY") != std::string::npos);
    WT_CHECK(out.find("/dev/") != std::string::npos);
    WT_CHECK_EQ(pty->waitForExit(5000ms).value_or(-100), 0);
}

WT_TEST(pty_initial_window_size) {
    auto pty = createPty();
    WT_REQUIRE(pty->start(shell("stty size", PtySize{33, 101})).isOk());
    const std::string out = readUntil(*pty, "");
    WT_CHECK(out.find("33 101") != std::string::npos);
}

WT_TEST(pty_resize_delivers_sigwinch) {
    auto pty = createPty();
    // The shell reports the size each time SIGWINCH arrives, then exits on input.
    WT_REQUIRE(pty->start(shell("trap 'stty size' WINCH; echo READY; read line; stty size", PtySize{24, 80}))
                   .isOk());
    WT_REQUIRE(readUntil(*pty, "READY").find("READY") != std::string::npos);
    WT_REQUIRE(pty->resize(PtySize{40, 120}).isOk());
    WT_CHECK_EQ(pty->size().rows, 40);
    WT_CHECK(writeAll(*pty, "\r"));
    const std::string out = readUntil(*pty, "");
    WT_CHECK(out.find("40 120") != std::string::npos);
}

WT_TEST(pty_echoes_input_and_line_discipline_works) {
    auto pty = createPty();
    WT_REQUIRE(pty->start(shell("read value; echo \"got:$value\"")).isOk());
    WT_CHECK(writeAll(*pty, "abc\x7f" "d\r")); // DEL (VERASE) removes the 'c'
    const std::string out = readUntil(*pty, "got:");
    const std::string rest = readUntil(*pty, "");
    WT_CHECK((out + rest).find("got:abd") != std::string::npos);
}

WT_TEST(pty_environment_and_working_directory) {
    auto pty = createPty();
    PtyOptions options = shell("echo \"term=$TERM custom=$WT_CUSTOM lines=${LINES:-unset}\"; pwd");
    options.environment.emplace_back("WT_CUSTOM", "42");
    options.workingDirectory = "/tmp";
    ::setenv("LINES", "99", 1); // stale geometry must not leak into the child
    WT_REQUIRE(pty->start(options).isOk());
    ::unsetenv("LINES");
    const std::string out = readUntil(*pty, "");
    WT_CHECK(out.find("term=xterm-256color custom=42 lines=unset") != std::string::npos);
    WT_CHECK(out.find("/tmp") != std::string::npos);
}

WT_TEST(pty_resolves_program_from_path) {
    auto pty = createPty();
    PtyOptions options;
    options.program = "sh"; // bare name, resolved via PATH
    options.arguments = {"-c", "echo via-path"};
    WT_REQUIRE(pty->start(options).isOk());
    WT_CHECK(readUntil(*pty, "").find("via-path") != std::string::npos);
}

WT_TEST(pty_reports_missing_program) {
    auto pty = createPty();
    PtyOptions options;
    options.program = "/nonexistent/definitely-not-a-shell";
    const auto status = pty->start(options);
    WT_CHECK(!status.isOk());
    WT_CHECK(status.message().find("not found") != std::string::npos);
    WT_CHECK(pty->processId() <= 0);
}

WT_TEST(pty_reports_bad_working_directory_from_child) {
    auto pty = createPty();
    PtyOptions options = shell("echo should-not-run");
    options.workingDirectory = "/nonexistent-dir-for-wt-tests";
    const auto status = pty->start(options);
    WT_CHECK(!status.isOk());
    WT_CHECK(status.message().find("working directory") != std::string::npos);
    WT_CHECK(status.systemError() != 0);
}

WT_TEST(pty_terminate_stops_long_running_child) {
    auto pty = createPty();
    WT_REQUIRE(pty->start(shell("echo STARTED; exec sleep 30")).isOk());
    WT_REQUIRE(readUntil(*pty, "STARTED").find("STARTED") != std::string::npos);
    WT_CHECK(pty->isRunning());
    const auto begin = std::chrono::steady_clock::now();
    pty->terminate(1000ms);
    WT_CHECK(!pty->isRunning());
    WT_CHECK(std::chrono::steady_clock::now() - begin < 3s);
    WT_CHECK_EQ(pty->pollExit().value_or(-100), 128 + 1); // SIGHUP
}

WT_TEST(pty_does_not_leak_parent_descriptors) {
    // A descriptor opened without O_CLOEXEC in the parent must not be visible in the child.
    int fds[2];
    WT_REQUIRE(::pipe(fds) == 0);
    auto pty = createPty();
    const std::string script = "if [ -e /dev/fd/" + std::to_string(fds[0]) + " ]; then echo LEAK; else echo CLEAN; fi";
    WT_REQUIRE(pty->start(shell(script)).isOk());
    const std::string out = readUntil(*pty, "");
    ::close(fds[0]);
    ::close(fds[1]);
    WT_CHECK(out.find("CLEAN") != std::string::npos);
}

WT_TEST(pty_find_executable_and_default_shell) {
    WT_CHECK_EQ(findExecutable("sh", "/nonexistent:/bin:/usr/bin").empty(), false);
    WT_CHECK_EQ(findExecutable("definitely-not-a-program-wt", "/bin:/usr/bin"), std::string());
    WT_CHECK_EQ(findExecutable("/bin/sh", ""), std::string("/bin/sh"));
    const std::string shellPath = defaultShell();
    WT_CHECK(!shellPath.empty() && shellPath.front() == '/');
}

WT_TEST(pty_resize_sets_pixel_size_and_validates_input) {
    auto pty = createPty();
    WT_CHECK(!pty->resize(PtySize{10, 10}).isOk()); // not started yet
    WT_REQUIRE(pty->start(shell("read x")).isOk());
    WT_REQUIRE(pty->resize(PtySize{30, 90, 720, 480}).isOk());
    struct winsize ws {};
    WT_REQUIRE(::ioctl(static_cast<int>(pty->nativeHandle()), TIOCGWINSZ, &ws) == 0);
    WT_CHECK_EQ(ws.ws_row, 30);
    WT_CHECK_EQ(ws.ws_col, 90);
    WT_CHECK_EQ(ws.ws_xpixel, 720);
    WT_CHECK_EQ(ws.ws_ypixel, 480);
    WT_CHECK(!pty->resize(PtySize{0, 80}).isOk());
    WT_CHECK_EQ(pty->size().rows, 30); // a rejected resize keeps the previous size
    WT_CHECK(!pty->start(shell("true")).isOk()); // one child per PTY
}

WT_TEST(pty_terminate_kills_child_that_ignores_sighup) {
    auto pty = createPty();
    WT_REQUIRE(pty->start(shell("trap '' HUP; echo ARMED; while :; do sleep 0.05; done")).isOk());
    WT_REQUIRE(readUntil(*pty, "ARMED").find("ARMED") != std::string::npos);
    const auto begin = std::chrono::steady_clock::now();
    pty->terminate(200ms);
    WT_CHECK(std::chrono::steady_clock::now() - begin < 3s);
    WT_CHECK(!pty->isRunning());
    WT_CHECK_EQ(pty->pollExit().value_or(-100), 128 + SIGKILL);
    pty->terminate(200ms); // idempotent once the child is gone
    WT_CHECK_EQ(pty->read(nullptr, 0).state, IoState::Closed);
    WT_CHECK_EQ(pty->write("x").state, IoState::Closed);
}

WT_TEST(pty_child_gets_default_signal_dispositions) {
    // Ignored or blocked signals in the GUI process must not be inherited by the shell.
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    struct sigaction previous {};
    ::sigaction(SIGINT, &ignore, &previous);
    sigset_t blocked;
    sigset_t oldMask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    ::pthread_sigmask(SIG_BLOCK, &blocked, &oldMask);

    auto pty = createPty();
    const auto status = pty->start(shell("kill -INT $$; echo survived"));
    ::pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
    ::sigaction(SIGINT, &previous, nullptr);
    WT_REQUIRE(status.isOk());
    const std::string out = readUntil(*pty, "");
    WT_CHECK(out.find("survived") == std::string::npos);
    WT_CHECK_EQ(pty->waitForExit(5000ms).value_or(-100), 128 + SIGINT);
}

WT_TEST(pty_output_is_fully_drained_after_exit) {
    auto pty = createPty();
    // ~200 KB written right before exiting; nothing may be lost between EOF/EIO and reaping.
    WT_REQUIRE(pty->start(shell("i=0; while [ $i -lt 4000 ]; do echo 0123456789012345678901234567890123456789"
                                "0123456; i=$((i+1)); done; echo TAIL-MARKER; exit 7"))
                   .isOk());
    const std::string out = readUntil(*pty, "", 20000ms);
    WT_CHECK(out.find("TAIL-MARKER") != std::string::npos);
    WT_CHECK(out.size() >= 4000u * 49u); // 47 characters + CR LF per line
    WT_CHECK_EQ(pty->waitForExit(5000ms).value_or(-100), 7);
}

WT_TEST(pty_termios_defaults_match_key_encoder) {
    auto pty = createPty();
    WT_REQUIRE(pty->start(shell("stty -a")).isOk());
    const std::string out = readUntil(*pty, "");
    // Backspace sends DEL (^?) and the line discipline must treat it as erase; UTF-8 input mode
    // keeps multi-byte characters intact when erasing (Linux only).
    WT_CHECK(out.find("erase = ^?") != std::string::npos);
    WT_CHECK(out.find("intr = ^C") != std::string::npos);
#if defined(__linux__)
    WT_CHECK(out.find(" iutf8") != std::string::npos && out.find("-iutf8") == std::string::npos);
#endif
}

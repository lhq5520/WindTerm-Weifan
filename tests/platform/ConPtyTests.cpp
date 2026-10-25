// Windows ConPTY integration tests (run on Windows 10 1809+ only).

#include "WtTest.h"

#include "wt/platform/Pty.h"

#include <chrono>
#include <string>

using namespace wt::platform;
using namespace std::chrono_literals;

namespace {

// Reads until `needle` shows up or the timeout expires.
bool readUntil(Pty& pty, const std::string& needle, std::string& output, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        if (!pty.waitForReadable(100ms)) {
            continue;
        }
        for (;;) {
            const IoResult result = pty.read(buffer, sizeof(buffer));
            if (result.state != IoState::Ok) {
                break;
            }
            output.append(buffer, result.bytes);
        }
        if (output.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

WT_TEST(conpty_runs_a_command_and_reports_the_exit_code) {
    auto pty = createPty();
    PtyOptions options;
    options.program = "cmd.exe";
    options.arguments = {"/d", "/c", "echo conpty-ok && exit /b 3"};
    const auto status = pty->start(options);
    WT_REQUIRE(status.isOk());
    WT_CHECK(pty->processId() > 0);
    std::string output;
    WT_CHECK(readUntil(*pty, "conpty-ok", output));
    WT_CHECK(pty->waitForExit(10s) == std::optional<int>(3));
    WT_CHECK(!pty->isRunning());
}

WT_TEST(conpty_input_resize_and_terminate) {
    auto pty = createPty();
    PtyOptions options;
    options.program = "cmd.exe";
    options.arguments = {"/d", "/q"};
    options.environment = {{"WT_CONPTY_TEST", "env-ok"}};
    options.size = {30, 100, 0, 0};
    WT_REQUIRE(pty->start(options).isOk());
    std::string output;
    WT_CHECK(pty->write("echo %WT_CONPTY_TEST%\r\n").state == IoState::Ok);
    WT_CHECK(readUntil(*pty, "env-ok", output));
    WT_CHECK(pty->resize({40, 120, 0, 0}).isOk());
    WT_CHECK_EQ(pty->size().columns, std::uint16_t(120));
    output.clear();
    WT_CHECK(pty->write("mode con\r\n").state == IoState::Ok);
    WT_CHECK(readUntil(*pty, "120", output));
    pty->terminate(500ms);
    WT_CHECK(!pty->isRunning());
}

WT_TEST(conpty_reports_missing_programs) {
    auto pty = createPty();
    PtyOptions options;
    options.program = "definitely-not-a-program-wt.exe";
    const auto status = pty->start(options);
    WT_CHECK(!status.isOk());
    WT_CHECK(status.message().find("not found") != std::string::npos);
    WT_CHECK(!findExecutable("cmd", "").empty());
}

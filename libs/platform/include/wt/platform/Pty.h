#pragma once

#include "wt/core/Status.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wt::platform {

struct PtySize {
    std::uint16_t rows = 24;
    std::uint16_t columns = 80;
    std::uint16_t pixelWidth = 0;
    std::uint16_t pixelHeight = 0;
};

struct PtyOptions {
    // Program to run. Empty means the user's default shell (see defaultShell()).
    // A bare name is resolved against PATH before the child is spawned.
    std::string program;
    // argv[1..]; argv[0] is derived from `program`.
    std::vector<std::string> arguments;
    // Prefix argv[0] with '-' so shells behave as login shells (macOS Terminal convention).
    bool loginShell = false;
    // Empty means inherit the parent's working directory.
    std::string workingDirectory;
    // Start from the parent environment (minus stale LINES/COLUMNS/TERMCAP)?
    bool inheritEnvironment = true;
    // Variables to set or override in the child environment.
    std::vector<std::pair<std::string, std::string>> environment;
    PtySize size;
};

enum class IoState {
    Ok,         // `bytes` were transferred
    WouldBlock, // nothing available right now (non-blocking descriptor)
    Closed,     // the slave side is gone (child exited / hung up)
    Error,      // unexpected failure, see `systemError`
};

struct IoResult {
    IoState state = IoState::Ok;
    std::size_t bytes = 0;
    int systemError = 0;
};

// Pseudo-terminal running a child process. The master side is non-blocking; integrate it
// into an event loop through nativeHandle(): a file descriptor on POSIX (poll for readability),
// a manual-reset event HANDLE on Windows (signalled while output is buffered or after the
// console closed). Writes on Windows block briefly instead of returning WouldBlock.
// Not thread-safe: use one instance from one thread at a time.
class Pty {
public:
    virtual ~Pty() = default;

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    virtual core::Status start(const PtyOptions& options) = 0;

    virtual IoResult read(char* buffer, std::size_t size) = 0;
    // May transfer fewer bytes than requested; the caller keeps the remainder queued.
    virtual IoResult write(std::string_view data) = 0;
    virtual core::Status resize(const PtySize& size) = 0;

    // Blocks until the master is readable, the slave hangs up, or the timeout expires.
    virtual bool waitForReadable(std::chrono::milliseconds timeout) = 0;

    virtual bool isRunning() = 0;
    // Non-blocking: reaps the child if it has exited. Exit code is 128 + signal for signalled children.
    virtual std::optional<int> pollExit() = 0;
    virtual std::optional<int> waitForExit(std::chrono::milliseconds timeout) = 0;
    // Sends SIGHUP, waits up to `grace`, then SIGKILLs. Safe to call repeatedly.
    virtual void terminate(std::chrono::milliseconds grace = std::chrono::milliseconds(500)) = 0;

    virtual std::intptr_t nativeHandle() const = 0;
    virtual std::int64_t processId() const = 0;
    virtual PtySize size() const = 0;

protected:
    Pty() = default;
};

// Creates the native PTY backend for this platform (POSIX openpt on Linux/macOS, ConPTY on
// Windows 10 1809+).
std::unique_ptr<Pty> createPty();

// The user's login shell: $SHELL, then the passwd database, then /bin/sh.
std::string defaultShell();

// Inside a Flatpak sandbox (Linux) the user's shell and programs live on the host, so local
// sessions are started through `flatpak-spawn --host`, which hands the PTY over as their
// controlling terminal. WT_FLATPAK_SANDBOX_SHELL=1 keeps them in the sandbox.
bool runningInFlatpak();
// The flatpak-spawn command line (program first) that runs `options` on the host.
std::vector<std::string> flatpakSpawnCommand(const PtyOptions& options);

// Resolves `program` against `pathVariable` (PATH syntax). Returns empty if not found/executable.
std::string findExecutable(std::string_view program, std::string_view pathVariable);

} // namespace wt::platform

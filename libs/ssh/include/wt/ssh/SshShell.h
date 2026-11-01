#pragma once

#include "wt/ssh/SshConnection.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wt::ssh {

// Interactive shell on a connection, serviced by a dedicated I/O thread together with the
// connection's port forwards and agent/X11 forwarding channels. Callbacks run on that thread.
// write()/resize()/close() and the forwarding methods may be called from any thread.
class SshShell {
public:
    struct Callbacks {
        std::function<void(std::string_view data)> onData;
        // The shell ended: `exitStatus` is the remote exit status (-1 if unknown) and `error` is
        // empty for a normal end, otherwise why the connection was lost.
        std::function<void(int exitStatus, const std::string& error)> onClosed;
        // Forwarding events: refused requests, ports that could not be opened, rejected X11 clients.
        std::function<void(const std::string& message)> onNotice;
    };

    // Takes over `connection` (switches it to non-blocking mode) and starts the I/O thread.
    static std::unique_ptr<SshShell> start(std::shared_ptr<SshConnection> connection, const ShellOptions& options,
                                           Callbacks callbacks, std::string* error);
    ~SshShell();

    void write(std::string_view data);
    void resize(int rows, int columns, int pixelWidth, int pixelHeight);
    // Closes the channel and stops the thread (no onClosed callback). Idempotent.
    void close();
    std::shared_ptr<SshConnection> connection() const;

    // True once the shell ended on its own (onClosed was called); exitStatus()/closeError() then
    // hold the values passed to it. Lets owners handle a close that raced with start().
    bool hasFinished() const;
    int exitStatus() const;
    std::string closeError() const;

    // Port forwards. Those from ShellOptions::forwarding have ids 1..n; added ones get new ids.
    int addForward(const PortForward& forward);
    void removeForward(int id);
    std::vector<ForwardStatus> forwards() const;
    // Whether the server accepted the agent / X11 forwarding requests.
    bool agentForwarding() const;
    bool x11Forwarding() const;

private:
    struct Impl;
    explicit SshShell(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m;
};

} // namespace wt::ssh

// Integration tests for port, agent and X11 forwarding against an unprivileged OpenSSH server.

#include "SshdFixture.h"
#include "TestServers.h"
#include "WtTest.h"

#include "wt/net/Proxy.h"
#include "wt/platform/Files.h"
#include "wt/ssh/SshConnection.h"
#include "wt/ssh/SshShell.h"
#include "wt/ssh/X11.h"

#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>

using namespace wt;
using namespace std::chrono_literals;

namespace {

test::SshdFixture* forwardingServer() {
    static test::SshdFixture fixture;
    static bool started = false;
    static bool tried = false;
    if (!tried) {
        tried = true;
        ::unsetenv("SSH_AUTH_SOCK");
        std::string reason;
        started = fixture.start(&reason);
        if (!started) {
            std::fprintf(stderr, "    SKIP: %s\n", reason.c_str());
        }
    }
    return started ? &fixture : nullptr;
}

struct AcceptingUi : ssh::SshInteraction {
    ssh::HostKeyDecision confirmHostKey(const std::string&, std::uint16_t, const ssh::HostKey&, ssh::HostKeyCheck) override {
        return ssh::HostKeyDecision::AcceptAndSave;
    }
    std::optional<std::string> askPassword(const std::string&) override { return std::nullopt; }
    std::optional<std::string> askPassphrase(const std::string&) override { return std::nullopt; }
    std::optional<std::vector<std::string>> keyboardInteractive(const std::string&, const std::string&,
                                                                const std::vector<ssh::KeyboardPrompt>&) override {
        return std::nullopt;
    }
};

std::shared_ptr<ssh::SshConnection> connect(const test::SshdFixture& sshd) {
    ssh::ConnectOptions options;
    options.host = "127.0.0.1";
    options.port = sshd.port();
    options.username = test::SshdFixture::user();
    options.auth = ssh::AuthMethod::PublicKey;
    options.identityFile = sshd.userKey();
    options.knownHostsPath = sshd.knownHosts();
    options.timeout = 10s;
    options.tryDefaultKeys = false;
    AcceptingUi ui;
    std::string error;
    auto connection = ssh::SshConnection::connect(options, ui, &error);
    if (!connection) {
        std::fprintf(stderr, "    connect failed: %s\n", error.c_str());
    }
    return connection;
}

struct Sink {
    std::mutex mutex;
    std::condition_variable changed;
    std::string output;
    std::vector<std::string> notices;
    bool closed = false;

    ssh::SshShell::Callbacks callbacks() {
        return {[this](std::string_view data) {
                    std::lock_guard lock(mutex);
                    output.append(data);
                    changed.notify_all();
                },
                [this](int, const std::string&) {
                    std::lock_guard lock(mutex);
                    closed = true;
                    changed.notify_all();
                },
                [this](const std::string& message) {
                    std::lock_guard lock(mutex);
                    notices.push_back(message);
                    changed.notify_all();
                }};
    }
    bool waitFor(const std::string& text, std::chrono::milliseconds timeout = 15s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return output.find(text) != std::string::npos; });
    }
    bool hasNotice(const std::string& text) {
        std::lock_guard lock(mutex);
        for (const auto& notice : notices) {
            if (notice.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

// Polls forwards() until `predicate` holds for the forward with `id`.
std::optional<ssh::ForwardStatus> waitForward(const ssh::SshShell& shell, int id,
                                              const std::function<bool(const ssh::ForwardStatus&)>& predicate,
                                              std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& status : shell.forwards()) {
            if (status.id == id && predicate(status)) {
                return status;
            }
        }
        std::this_thread::sleep_for(20ms);
    }
    for (const auto& status : shell.forwards()) {
        std::fprintf(stderr, "    forward %d (%s): state %d, port %u, %s\n", status.id, status.forward.describe().c_str(),
                     static_cast<int>(status.state), status.boundPort, status.message.c_str());
    }
    return std::nullopt;
}

bool echoRoundTrip(net::Socket& socket, const std::string& text) {
    // Send and receive concurrently: the payload may exceed every buffer on the way.
    bool sent = false;
    std::thread writer([&] { sent = socket.sendAll(text, 20000ms).isOk(); });
    std::string reply(text.size(), '\0');
    const bool received = socket.receiveExact(reply.data(), reply.size(), 20000ms).isOk();
    writer.join();
    return sent && received && reply == text;
}

std::string payload(std::size_t size) {
    std::mt19937 engine(7);
    std::string data(size, '\0');
    for (char& c : data) {
        c = static_cast<char>(engine() & 0xFF);
    }
    return data;
}

std::uint16_t freePort() {
    net::Socket probe;
    std::uint16_t port = 0;
    net::listenLoopback(0, probe, port);
    return port;
}

ssh::ShellOptions quietShell() {
    ssh::ShellOptions options;
    options.command = "/bin/sh";
    return options;
}

} // namespace

WT_TEST(forwarding_local_dynamic_and_remote) {
    auto* sshd = forwardingServer();
    if (!sshd) {
        return;
    }
    test::EchoServer echo;
    WT_REQUIRE(echo.start());
    auto connection = connect(*sshd);
    WT_REQUIRE(connection != nullptr);
    const std::uint16_t remotePort = freePort();
    ssh::ShellOptions options = quietShell();
    options.forwarding.ports = {
        {ssh::ForwardKind::Local, "127.0.0.1", 0, "127.0.0.1", echo.port()},
        {ssh::ForwardKind::Dynamic, "", 0, "", 0},
        {ssh::ForwardKind::Remote, "127.0.0.1", remotePort, "127.0.0.1", echo.port()},
    };
    Sink sink;
    std::string error;
    auto shell = ssh::SshShell::start(connection, options, sink.callbacks(), &error);
    WT_REQUIRE(shell != nullptr);
    auto active = [](const ssh::ForwardStatus& s) { return s.state == ssh::ForwardStatus::State::Active; };

    // -L: a local port that reaches the echo server through sshd.
    const auto local = waitForward(*shell, 1, active);
    WT_REQUIRE(local.has_value());
    WT_CHECK(local->boundPort != 0);
    {
        net::Socket socket;
        WT_REQUIRE(net::connectTcp("127.0.0.1", local->boundPort, 5s, socket).isOk());
        WT_CHECK(echoRoundTrip(socket, "hello through -L"));
        WT_CHECK(echoRoundTrip(socket, payload(3 * 1024 * 1024))); // larger than every window
    }
    WT_CHECK(waitForward(*shell, 1, [](const auto& s) { return s.openConnections == 0 && s.totalConnections == 1; })
                 .has_value());

    // -D: a SOCKS5 proxy whose connections leave from the server.
    const auto dynamic = waitForward(*shell, 2, active);
    WT_REQUIRE(dynamic.has_value());
    {
        net::Socket socket;
        WT_REQUIRE(net::connectTcp("127.0.0.1", dynamic->boundPort, 5s, socket).isOk());
        WT_REQUIRE(net::socks5Connect(socket, "127.0.0.1", echo.port(), "", "", 5s).isOk());
        WT_CHECK(echoRoundTrip(socket, "hello through -D"));
    }
    {
        // A destination the server cannot reach gets a SOCKS failure reply.
        net::Socket socket;
        WT_REQUIRE(net::connectTcp("127.0.0.1", dynamic->boundPort, 5s, socket).isOk());
        WT_CHECK(!net::socks5Connect(socket, "127.0.0.1", freePort(), "", "", 5s).isOk());
    }

    // -R: sshd listens on remotePort and the connections come back to the echo server here.
    const auto remote = waitForward(*shell, 3, active);
    WT_REQUIRE(remote.has_value());
    WT_CHECK_EQ(remote->boundPort, remotePort);
    {
        net::Socket socket;
        WT_REQUIRE(net::connectTcp("127.0.0.1", remotePort, 5s, socket).isOk());
        WT_CHECK(echoRoundTrip(socket, "hello through -R"));
        WT_CHECK(echoRoundTrip(socket, payload(1024 * 1024)));
    }

    // The shell keeps working alongside.
    shell->write("echo shell-still-$((40+2))\n");
    WT_CHECK(sink.waitFor("shell-still-42"));
    shell->close();
}

WT_TEST(forwarding_added_and_removed_at_runtime) {
    auto* sshd = forwardingServer();
    if (!sshd) {
        return;
    }
    test::EchoServer echo;
    WT_REQUIRE(echo.start());
    auto connection = connect(*sshd);
    WT_REQUIRE(connection != nullptr);
    Sink sink;
    std::string error;
    auto shell = ssh::SshShell::start(connection, quietShell(), sink.callbacks(), &error);
    WT_REQUIRE(shell != nullptr);
    WT_CHECK(shell->forwards().empty());
    const int id = shell->addForward({ssh::ForwardKind::Local, "", 0, "127.0.0.1", echo.port()});
    const auto status =
        waitForward(*shell, id, [](const auto& s) { return s.state == ssh::ForwardStatus::State::Active; });
    WT_REQUIRE(status.has_value());
    {
        net::Socket socket;
        WT_REQUIRE(net::connectTcp("127.0.0.1", status->boundPort, 5s, socket).isOk());
        WT_CHECK(echoRoundTrip(socket, "runtime forward"));
    }
    const int remoteId = shell->addForward({ssh::ForwardKind::Remote, "", freePort(), "127.0.0.1", echo.port()});
    WT_CHECK(waitForward(*shell, remoteId, [](const auto& s) { return s.state == ssh::ForwardStatus::State::Active; })
                 .has_value());
    shell->removeForward(id);
    shell->removeForward(remoteId);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!shell->forwards().empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    WT_CHECK(shell->forwards().empty());
    net::Socket refused;
    WT_CHECK(!net::connectTcp("127.0.0.1", status->boundPort, 2s, refused).isOk()); // nobody listens any more
}

WT_TEST(forwarding_failures_are_reported) {
    auto* sshd = forwardingServer();
    if (!sshd) {
        return;
    }
    net::Socket occupied;
    std::uint16_t busyPort = 0;
    WT_REQUIRE(net::listenLoopback(0, occupied, busyPort).isOk());
    auto connection = connect(*sshd);
    WT_REQUIRE(connection != nullptr);
    ssh::ShellOptions options = quietShell();
    options.forwarding.ports = {
        {ssh::ForwardKind::Local, "127.0.0.1", busyPort, "127.0.0.1", 9},  // port in use
        {ssh::ForwardKind::Remote, "127.0.0.1", 1, "127.0.0.1", 9},        // privileged port on the server
        {ssh::ForwardKind::Local, "127.0.0.1", 0, "127.0.0.1", freePort()}, // nobody listens at the target
    };
    Sink sink;
    std::string error;
    auto shell = ssh::SshShell::start(connection, options, sink.callbacks(), &error);
    WT_REQUIRE(shell != nullptr);
    auto failed = [](const ssh::ForwardStatus& s) { return s.state == ssh::ForwardStatus::State::Failed; };
    const auto local = waitForward(*shell, 1, failed);
    WT_REQUIRE(local.has_value());
    WT_CHECK(local->message.find("cannot listen") != std::string::npos);
    WT_CHECK(waitForward(*shell, 2, failed).has_value());
    WT_CHECK(sink.hasNotice("failed"));
    // The third forward listens, but its connections fail on the server side and are closed.
    const auto unreachable =
        waitForward(*shell, 3, [](const auto& s) { return s.state == ssh::ForwardStatus::State::Active; });
    WT_REQUIRE(unreachable.has_value());
    net::Socket socket;
    WT_REQUIRE(net::connectTcp("127.0.0.1", unreachable->boundPort, 5s, socket).isOk());
    char byte = 0;
    std::size_t n = 1;
    socket.receiveSome(&byte, 1, n, 10s);
    WT_CHECK_EQ(n, std::size_t(0)); // closed by us after the channel open failed
    // The session itself is unaffected.
    shell->write("echo after-failures\n");
    WT_CHECK(sink.waitFor("after-failures"));
}

WT_TEST(forwarding_agent) {
    auto* sshd = forwardingServer();
    if (!sshd) {
        return;
    }
    if (!sshd->startAgent()) {
        std::fprintf(stderr, "    SKIP: ssh-agent unavailable\n");
        return;
    }
    auto connection = connect(*sshd);
    WT_REQUIRE(connection != nullptr);
    ssh::ShellOptions options = quietShell();
    options.forwarding.agent = true;
    Sink sink;
    std::string error;
    auto shell = ssh::SshShell::start(connection, options, sink.callbacks(), &error);
    WT_REQUIRE(shell != nullptr);
    WT_CHECK(shell->agentForwarding());
    // ssh-add on the server talks to our agent through the forwarded channel.
    shell->write("test -n \"$SSH_AUTH_SOCK\" && echo sock-set; ssh-add -l; echo agent-done\n");
    WT_CHECK(sink.waitFor("agent-done"));
    WT_CHECK(sink.waitFor("sock-set", 1s));
    WT_CHECK(sink.waitFor("plain", 1s)); // the comment of the key in the local agent
    shell->close();
    sshd->stopAgent();

    // Without the option the server gets no agent.
    auto plain = connect(*sshd);
    WT_REQUIRE(plain != nullptr);
    Sink quiet;
    auto noAgent = ssh::SshShell::start(plain, quietShell(), quiet.callbacks(), &error);
    WT_REQUIRE(noAgent != nullptr);
    WT_CHECK(!noAgent->agentForwarding());
    noAgent->write("test -z \"$SSH_AUTH_SOCK\" && echo no-agent\n");
    WT_CHECK(quiet.waitFor("no-agent"));
}

WT_TEST(forwarding_x11_substitutes_the_cookie) {
    auto* sshd = forwardingServer();
    if (!sshd) {
        return;
    }
    if (!sshd->x11Available() || ::access("/usr/bin/python3", X_OK) != 0) {
        std::fprintf(stderr, "    SKIP: xauth or python3 unavailable\n");
        return;
    }
    // A fake local X server on display 100..199 (TCP 6100..6199).
    net::Socket xserver;
    std::uint16_t xport = 0;
    for (std::uint16_t candidate = 6100; candidate < 6200 && !xserver.isValid(); ++candidate) {
        if (net::listenTcp("127.0.0.1", candidate, xserver, xport)) {
            break;
        }
    }
    WT_REQUIRE(xserver.isValid());
    xserver.setNonBlocking(false);
    const std::string display = "127.0.0.1:" + std::to_string(xport - 6000);
    // Our Xauthority holds the real cookie for that display.
    const std::string realCookie(16, '\x42');
    const std::string number = std::to_string(xport - 6000);
    auto counted = [](const std::string& text) {
        return std::string{static_cast<char>(text.size() >> 8), static_cast<char>(text.size() & 0xFF)} + text;
    };
    const std::string xauthority = sshd->dir() + "/local.xauth";
    WT_REQUIRE(platform::writeFileAtomic(xauthority, std::string("\xff\xff", 2) + counted("") + counted(number) +
                                                         counted("MIT-MAGIC-COOKIE-1") + counted(realCookie)));
    // The remote X client: reads the fake cookie sshd stored and opens the forwarded display.
    const std::string client = sshd->dir() + "/x11client.py";
    WT_REQUIRE(platform::writeFileAtomic(
        client,
        "import os, socket, struct, subprocess\n"
        "disp = os.environ['DISPLAY']\n"
        "host, rest = disp.rsplit(':', 1)\n"
        "num = int(rest.split('.')[0])\n"
        "cookie = bytes.fromhex(subprocess.check_output(['xauth', 'list']).decode().split()[2])\n"
        "s = socket.create_connection((host or 'localhost', 6000 + num))\n"
        "name = b'MIT-MAGIC-COOKIE-1'\n"
        "pad = lambda b: b + b'\\0' * ((4 - len(b) % 4) % 4)\n"
        "s.sendall(b'l\\0' + struct.pack('<HHHH', 11, 0, len(name), len(cookie)) + b'\\0\\0' + pad(name) + pad(cookie))\n"
        "print('reply:' + s.recv(64).decode())\n"));

    std::string seen;
    std::thread server([&] {
        net::Socket peer;
        if (!net::acceptConnection(xserver, peer, 15000ms)) {
            return;
        }
        char buffer[256];
        std::size_t total = 0;
        while (total < 48) { // 12-byte header + 20 (name) + 16 (cookie)
            std::size_t n = 0;
            if (!peer.receiveSome(buffer + total, sizeof(buffer) - total, n, 5000ms) || n == 0) {
                break;
            }
            total += n;
        }
        seen.assign(buffer, total);
        peer.sendAll(seen.find(realCookie) != std::string::npos ? "real-cookie" : "wrong-cookie");
    });

    auto connection = connect(*sshd);
    WT_REQUIRE(connection != nullptr);
    ssh::ShellOptions options = quietShell();
    options.forwarding.x11 = true;
    options.forwarding.x11Display = display;
    options.forwarding.xauthorityPath = xauthority;
    Sink sink;
    std::string error;
    auto shell = ssh::SshShell::start(connection, options, sink.callbacks(), &error);
    WT_REQUIRE(shell != nullptr);
    WT_CHECK(shell->x11Forwarding());
    shell->write("XAUTHORITY=" + sshd->remoteXauthority() + " python3 " + client + "; echo x11-done\n");
    WT_CHECK(sink.waitFor("x11-done", 20s));
    server.join();
    WT_CHECK(sink.waitFor("reply:real-cookie", 1s));
    WT_CHECK(seen.find(realCookie) != std::string::npos);
    shell->close();
}

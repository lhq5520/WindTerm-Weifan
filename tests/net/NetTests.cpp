#include "TestServers.h"
#include "WtTest.h"

#include "wt/net/Proxy.h"
#include "wt/net/Socket.h"

#include <thread>

using namespace wt;
using namespace std::chrono_literals;

namespace {

bool echoRoundTrip(net::Socket& socket, const std::string& text) {
    if (!socket.sendAll(text, 2000ms)) {
        return false;
    }
    std::string reply(text.size(), '\0');
    return socket.receiveExact(reply.data(), reply.size(), 2000ms).isOk() && reply == text;
}

} // namespace

WT_TEST(net_direct_connect_and_echo) {
    test::EchoServer echo;
    WT_REQUIRE(echo.start());
    net::Socket socket;
    const auto status = net::connectTcp("127.0.0.1", echo.port(), 2000ms, socket);
    WT_REQUIRE(status.isOk());
    WT_CHECK(echoRoundTrip(socket, "hello"));
    WT_CHECK(echoRoundTrip(socket, std::string(100000, 'x')));
}

WT_TEST(net_connect_errors) {
    // A port that was just freed refuses connections.
    std::uint16_t port = 0;
    {
        net::Socket listener;
        WT_REQUIRE(net::listenLoopback(0, listener, port).isOk());
    }
    net::Socket socket;
    auto status = net::connectTcp("127.0.0.1", port, 2000ms, socket);
    WT_CHECK(!status.isOk());
    WT_CHECK(status.message().find("connect") != std::string::npos);
    status = net::connectTcp("no-such-host.invalid", 22, 2000ms, socket);
    WT_CHECK(!status.isOk());
    WT_CHECK(status.message().find("resolve") != std::string::npos);

    // A cancelled connect returns promptly. 10.255.255.1 is normally unroutable (the connect
    // hangs); if the environment rejects it instantly the call still fails, which is fine.
    std::atomic<bool> cancel{false};
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(200ms);
        cancel = true;
    });
    const auto start = std::chrono::steady_clock::now();
    status = net::connectTcp("10.255.255.1", 22, 10000ms, socket, &cancel);
    canceller.join();
    WT_CHECK(!status.isOk());
    WT_CHECK(std::chrono::steady_clock::now() - start < 5s);
}

WT_TEST(net_socks5_proxy_without_and_with_auth) {
    test::EchoServer echo;
    WT_REQUIRE(echo.start());
    test::ProxyServer open(test::ProxyServer::Kind::Socks5);
    WT_REQUIRE(open.start());
    net::ProxyConfig proxy{net::ProxyType::Socks5, "127.0.0.1", open.port(), {}, {}};
    net::Socket socket;
    auto status = net::connectThrough(proxy, "127.0.0.1", echo.port(), 2000ms, socket);
    WT_REQUIRE(status.isOk());
    WT_CHECK(echoRoundTrip(socket, "through socks"));
    WT_CHECK_EQ(open.lastTarget(), "127.0.0.1:" + std::to_string(echo.port()));

    test::ProxyServer guarded(test::ProxyServer::Kind::Socks5, "alice", "s3cret");
    WT_REQUIRE(guarded.start());
    proxy = {net::ProxyType::Socks5, "127.0.0.1", guarded.port(), "alice", "s3cret"};
    net::Socket authed;
    WT_REQUIRE(net::connectThrough(proxy, "localhost", echo.port(), 2000ms, authed).isOk());
    WT_CHECK(echoRoundTrip(authed, "authenticated"));

    proxy.password = "wrong";
    net::Socket rejected;
    status = net::connectThrough(proxy, "localhost", echo.port(), 2000ms, rejected);
    WT_CHECK(!status.isOk());
    WT_CHECK(status.message().find("authentication failed") != std::string::npos);
    proxy.username.clear();
    status = net::connectThrough(proxy, "localhost", echo.port(), 2000ms, rejected);
    WT_CHECK(status.message().find("requires authentication") != std::string::npos);
}

WT_TEST(net_socks5_reports_unreachable_target) {
    test::ProxyServer proxyServer(test::ProxyServer::Kind::Socks5);
    WT_REQUIRE(proxyServer.start());
    std::uint16_t closedPort = 0;
    {
        net::Socket listener;
        WT_REQUIRE(net::listenLoopback(0, listener, closedPort).isOk());
    }
    net::Socket socket;
    const auto status = net::connectThrough({net::ProxyType::Socks5, "127.0.0.1", proxyServer.port(), {}, {}},
                                            "127.0.0.1", closedPort, 2000ms, socket);
    WT_CHECK(!status.isOk());
    WT_CHECK(status.message().find("connection refused") != std::string::npos);
}

WT_TEST(net_http_connect_proxy) {
    test::EchoServer echo;
    WT_REQUIRE(echo.start());
    test::ProxyServer http(test::ProxyServer::Kind::Http, "bob", "pw");
    WT_REQUIRE(http.start());
    net::ProxyConfig proxy{net::ProxyType::HttpConnect, "127.0.0.1", http.port(), "bob", "pw"};
    net::Socket socket;
    WT_REQUIRE(net::connectThrough(proxy, "127.0.0.1", echo.port(), 2000ms, socket).isOk());
    WT_CHECK(echoRoundTrip(socket, "through http"));

    proxy.password = "nope";
    net::Socket denied;
    const auto status = net::connectThrough(proxy, "127.0.0.1", echo.port(), 2000ms, denied);
    WT_CHECK(!status.isOk());
    WT_CHECK(status.message().find("407") != std::string::npos);
}

WT_TEST(net_socket_pair_and_drop) {
    net::Socket a;
    net::Socket b;
    WT_REQUIRE(net::socketPair(a, b).isOk());
    WT_CHECK(a.sendAll("ping").isOk());
    char buffer[4];
    WT_REQUIRE(b.receiveExact(buffer, 4, 1000ms).isOk());
    WT_CHECK_EQ(std::string(buffer, 4), std::string("ping"));
    WT_CHECK(!net::waitReadable(a.native(), 50ms));
    b.close();
    std::size_t n = 1;
    WT_CHECK(a.receiveSome(buffer, sizeof(buffer), n, 1000ms).isOk());
    WT_CHECK_EQ(n, std::size_t(0)); // EOF

    test::EchoServer echo;
    WT_REQUIRE(echo.start());
    net::Socket client;
    WT_REQUIRE(net::connectTcp("127.0.0.1", echo.port(), 2000ms, client).isOk());
    WT_CHECK(echoRoundTrip(client, "x"));
    echo.dropAll();
    WT_CHECK(!echoRoundTrip(client, "y"));
}

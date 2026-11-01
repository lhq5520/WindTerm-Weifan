// Unit tests for the server side of the SOCKS handshake (SSH dynamic port forwarding).

#include "WtTest.h"

#include "wt/net/SocksServer.h"

using namespace wt::net;
using State = SocksServerHandshake::State;

namespace {

std::string bytes(std::initializer_list<int> values) {
    std::string out;
    for (const int v : values) {
        out += static_cast<char>(v);
    }
    return out;
}

} // namespace

WT_TEST(socks5_domain_connect) {
    SocksServerHandshake handshake;
    std::string reply;
    WT_CHECK(handshake.feed(bytes({5, 2, 0, 2}), reply) == State::NeedMore);
    WT_CHECK_EQ(reply, bytes({5, 0})); // no authentication chosen
    reply.clear();
    std::string request = bytes({5, 1, 0, 3, 11}) + "example.org" + bytes({0x01, 0xBB}) + "GET /";
    // Byte by byte: the parser must cope with any split.
    State state = State::NeedMore;
    for (const char c : request) {
        state = handshake.feed(std::string_view(&c, 1), reply);
    }
    WT_CHECK(state == State::Ready);
    WT_CHECK_EQ(handshake.version(), 5);
    WT_CHECK_EQ(handshake.host(), std::string("example.org"));
    WT_CHECK_EQ(handshake.port(), std::uint16_t(443));
    WT_CHECK_EQ(handshake.takeLeftover(), std::string("GET /"));
    WT_CHECK_EQ(handshake.finish(true), bytes({5, 0, 0, 1, 0, 0, 0, 0, 0, 0}));
    WT_CHECK_EQ(static_cast<int>(static_cast<unsigned char>(handshake.finish(false)[1])), 5);
}

WT_TEST(socks5_ip_addresses) {
    SocksServerHandshake v4;
    std::string reply;
    v4.feed(bytes({5, 1, 0}), reply);
    WT_CHECK(v4.feed(bytes({5, 1, 0, 1, 10, 0, 0, 7, 0, 22}), reply) == State::Ready);
    WT_CHECK_EQ(v4.host(), std::string("10.0.0.7"));
    WT_CHECK_EQ(v4.port(), std::uint16_t(22));

    SocksServerHandshake v6;
    v6.feed(bytes({5, 1, 0}), reply);
    WT_CHECK(v6.feed(bytes({5, 1, 0, 4, 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 80}), reply) ==
             State::Ready);
    WT_CHECK_EQ(v6.host(), std::string("2001:db8:0:0:0:0:0:1"));
    WT_CHECK_EQ(v6.port(), std::uint16_t(80));
}

WT_TEST(socks5_rejections) {
    std::string reply;
    SocksServerHandshake auth;
    WT_CHECK(auth.feed(bytes({5, 1, 2}), reply) == State::Failed); // username/password only
    WT_CHECK_EQ(reply, bytes({5, 0xFF}));

    reply.clear();
    SocksServerHandshake bind;
    bind.feed(bytes({5, 1, 0}), reply);
    reply.clear();
    WT_CHECK(bind.feed(bytes({5, 2, 0, 1, 1, 2, 3, 4, 0, 80}), reply) == State::Failed); // BIND
    WT_CHECK_EQ(static_cast<int>(static_cast<unsigned char>(reply[1])), 7);

    reply.clear();
    SocksServerHandshake type;
    type.feed(bytes({5, 1, 0}), reply);
    reply.clear();
    WT_CHECK(type.feed(bytes({5, 1, 0, 9, 1, 2}), reply) == State::Failed);
    WT_CHECK_EQ(static_cast<int>(static_cast<unsigned char>(reply[1])), 8);

    SocksServerHandshake garbage;
    WT_CHECK(garbage.feed("GET / HTTP/1.1\r\n", reply) == State::Failed);
    WT_CHECK(!garbage.error().empty());
}

WT_TEST(socks4_and_4a) {
    std::string reply;
    SocksServerHandshake v4;
    WT_CHECK(v4.feed(bytes({4, 1, 0x1F, 0x90, 192, 168, 1, 2}) + "user", reply) == State::NeedMore);
    WT_CHECK(v4.feed(bytes({0}), reply) == State::Ready);
    WT_CHECK_EQ(v4.host(), std::string("192.168.1.2"));
    WT_CHECK_EQ(v4.port(), std::uint16_t(8080));
    WT_CHECK_EQ(v4.finish(true), bytes({0, 0x5A, 0, 0, 0, 0, 0, 0}));
    WT_CHECK_EQ(v4.finish(false), bytes({0, 0x5B, 0, 0, 0, 0, 0, 0}));

    SocksServerHandshake v4a;
    WT_CHECK(v4a.feed(bytes({4, 1, 0, 80, 0, 0, 0, 1, 0}) + "intranet.local" + bytes({0}) + "x", reply) ==
             State::Ready);
    WT_CHECK_EQ(v4a.host(), std::string("intranet.local"));
    WT_CHECK_EQ(v4a.port(), std::uint16_t(80));
    WT_CHECK_EQ(v4a.takeLeftover(), std::string("x"));

    SocksServerHandshake bind;
    reply.clear();
    WT_CHECK(bind.feed(bytes({4, 2, 0, 80, 1, 2, 3, 4, 0}), reply) == State::Failed);
    WT_CHECK_EQ(reply, bytes({0, 0x5B, 0, 0, 0, 0, 0, 0}));
}

WT_TEST(socks_oversized_requests_fail) {
    SocksServerHandshake handshake;
    std::string reply;
    // SOCKS4 user id without terminator, endlessly.
    State state = handshake.feed(bytes({4, 1, 0, 80, 1, 2, 3, 4}), reply);
    for (int i = 0; i < 200 && state == State::NeedMore; ++i) {
        state = handshake.feed(std::string(16, 'u'), reply);
    }
    WT_CHECK(state == State::Failed);
}

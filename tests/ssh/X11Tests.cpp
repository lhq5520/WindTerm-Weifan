// Unit tests for the X11 forwarding helpers (no SSH server needed).

#include "WtTest.h"

#include "wt/ssh/SshTypes.h"
#include "wt/ssh/X11.h"

using namespace wt::ssh;

namespace {

std::string counted(const std::string& text) {
    return std::string{static_cast<char>(text.size() >> 8), static_cast<char>(text.size() & 0xFF)} + text;
}

std::string xauthRecord(std::uint16_t family, const std::string& address, const std::string& number,
                        const std::string& name, const std::string& data) {
    return std::string{static_cast<char>(family >> 8), static_cast<char>(family & 0xFF)} + counted(address) +
           counted(number) + counted(name) + counted(data);
}

std::string pad(std::string text) {
    text.append((4 - text.size() % 4) % 4, '\0');
    return text;
}

// An X11 connection setup (little-endian) carrying `name`/`data` as authentication.
std::string setup(const std::string& name, const std::string& data) {
    std::string out{'l', 0, 11, 0, 0, 0};
    out += static_cast<char>(name.size());
    out += '\0';
    out += static_cast<char>(data.size());
    out += '\0';
    out += std::string(2, '\0');
    return out + pad(name) + pad(data);
}

} // namespace

WT_TEST(x11_display_parsing) {
    auto local = parseX11Display(":0");
    WT_REQUIRE(local.has_value());
    WT_CHECK(local->host.empty());
    WT_CHECK_EQ(local->socketPath, std::string("/tmp/.X11-unix/X0"));
    WT_CHECK_EQ(local->number, 0);

    auto screen = parseX11Display("unix:3.1");
    WT_REQUIRE(screen.has_value());
    WT_CHECK_EQ(screen->socketPath, std::string("/tmp/.X11-unix/X3"));
    WT_CHECK_EQ(screen->screen, 1);

    auto tcp = parseX11Display("localhost:10.0");
    WT_REQUIRE(tcp.has_value());
    WT_CHECK_EQ(tcp->host, std::string("localhost"));
    WT_CHECK(tcp->socketPath.empty());
    WT_CHECK_EQ(tcp->number, 10);

    auto quartz = parseX11Display("/private/tmp/com.apple.launchd.AbC/org.xquartz:0");
    WT_REQUIRE(quartz.has_value());
    WT_CHECK_EQ(quartz->socketPath, std::string("/private/tmp/com.apple.launchd.AbC/org.xquartz"));
    WT_CHECK(quartz->host.empty());

    WT_CHECK(!parseX11Display(""));
    WT_CHECK(!parseX11Display("nocolon"));
    WT_CHECK(!parseX11Display(":x"));
    WT_CHECK(!parseX11Display(":1.y"));
}

WT_TEST(x11_xauthority_lookup) {
    const std::string cookieLocal(16, '\x11');
    const std::string cookieWild(16, '\x22');
    const std::string cookieOther(16, '\x33');
    const std::string file = xauthRecord(256, "myhost", "0", "MIT-MAGIC-COOKIE-1", cookieLocal) +
                             xauthRecord(256, "otherhost", "1", "MIT-MAGIC-COOKIE-1", cookieOther) +
                             xauthRecord(65535, "", "1", "MIT-MAGIC-COOKIE-1", cookieWild) +
                             xauthRecord(256, "myhost", "2", "XDM-AUTHORIZATION-1", "x") +
                             std::string("\x01", 1); // truncated trailing record is ignored
    const auto entries = parseXauthority(file);
    WT_REQUIRE(entries.size() == 4u);
    WT_CHECK_EQ(entries[0].address, std::string("myhost"));
    WT_CHECK_EQ(entries[2].family, std::uint16_t(65535));

    const auto zero = findX11Cookie(entries, *parseX11Display(":0"), "myhost");
    WT_REQUIRE(zero.has_value());
    WT_CHECK_EQ(zero->data, cookieLocal);
    const auto one = findX11Cookie(entries, *parseX11Display("localhost:1"), "myhost");
    WT_REQUIRE(one.has_value());
    WT_CHECK_EQ(one->data, cookieWild); // the wildcard entry beats another host's
    WT_CHECK(!findX11Cookie(entries, *parseX11Display(":2"), "myhost")); // not a magic cookie
    WT_CHECK(!findX11Cookie(entries, *parseX11Display(":7"), "myhost"));
}

WT_TEST(x11_cookie_substitution) {
    const std::string fake = randomX11Cookie();
    WT_CHECK_EQ(fake.size(), std::size_t(16));
    WT_CHECK_EQ(hexEncode(std::string("\x01\xab", 2)), std::string("01ab"));
    const std::string real(16, '\x5a');
    X11AuthRewriter rewriter(fake, "MIT-MAGIC-COOKIE-1", real);
    const std::string client = setup("MIT-MAGIC-COOKIE-1", fake) + "REQUESTS";
    std::string output;
    // Split in two to exercise buffering.
    WT_CHECK(rewriter.feed(std::string_view(client).substr(0, 20), output) == X11AuthRewriter::State::NeedMore);
    WT_CHECK(output.empty());
    WT_CHECK(rewriter.feed(std::string_view(client).substr(20), output) == X11AuthRewriter::State::Done);
    WT_CHECK_EQ(output, setup("MIT-MAGIC-COOKIE-1", real) + "REQUESTS");
    output.clear();
    WT_CHECK(rewriter.feed("more", output) == X11AuthRewriter::State::Done);
    WT_CHECK_EQ(output, std::string("more"));

    // No local cookie: the setup goes out without authentication.
    X11AuthRewriter open(fake, "", "");
    output.clear();
    WT_CHECK(open.feed(setup("MIT-MAGIC-COOKIE-1", fake), output) == X11AuthRewriter::State::Done);
    WT_CHECK_EQ(output, setup("", ""));

    // A client with another cookie is refused.
    X11AuthRewriter strict(fake, "MIT-MAGIC-COOKIE-1", real);
    output.clear();
    WT_CHECK(strict.feed(setup("MIT-MAGIC-COOKIE-1", std::string(16, 'x')), output) == X11AuthRewriter::State::Rejected);
    WT_CHECK(output.empty());
    X11AuthRewriter bogus(fake, "MIT-MAGIC-COOKIE-1", real);
    WT_CHECK(bogus.feed(std::string(12, 'Q'), output) == X11AuthRewriter::State::Rejected);
}

WT_TEST(port_forward_descriptions) {
    PortForward local{ForwardKind::Local, "", 8080, "db", 5432};
    WT_CHECK_EQ(local.describe(), std::string("L 127.0.0.1:8080 -> db:5432"));
    PortForward remote{ForwardKind::Remote, "", 9000, "127.0.0.1", 3000};
    WT_CHECK_EQ(remote.describe(), std::string("R localhost:9000 -> 127.0.0.1:3000"));
    PortForward dynamic{ForwardKind::Dynamic, "0.0.0.0", 1080, "", 0};
    WT_CHECK_EQ(dynamic.describe(), std::string("D 0.0.0.0:1080 (SOCKS)"));
}

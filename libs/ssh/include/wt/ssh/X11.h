#pragma once

#include "wt/core/Status.h"
#include "wt/net/Socket.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wt::ssh {

// A parsed DISPLAY value.
struct X11Display {
    std::string host;       // TCP host; empty for a local (Unix socket) display
    std::string socketPath; // Unix socket of a local display
    int number = 0;
    int screen = 0;
};

// ":0", ":0.1", "unix:0", "localhost:10.0", "host:1", or XQuartz's
// "/private/tmp/com.apple.launchd.XXXX/org.xquartz:0".
std::optional<X11Display> parseX11Display(std::string_view display);

struct XauthEntry {
    std::uint16_t family = 0; // 256 = local (address is the host name), 65535 = wildcard, 0 = IPv4
    std::string address;
    std::string number; // display number as text
    std::string name;   // "MIT-MAGIC-COOKIE-1"
    std::string data;   // the cookie (raw bytes)
};

// Entries of an Xauthority file (big-endian binary records); stops at the first truncated one.
std::vector<XauthEntry> parseXauthority(std::string_view contents);
// $XAUTHORITY, else ~/.Xauthority.
std::string defaultXauthorityPath();
// The MIT-MAGIC-COOKIE-1 entry for `display` (`hostname`: this machine's name, for local displays).
std::optional<XauthEntry> findX11Cookie(const std::vector<XauthEntry>& entries, const X11Display& display,
                                        const std::string& hostname);

// OpenSSH's X11 forwarding scheme: the server gets a random fake cookie; when a remote X client
// connects through the SSH channel, its connection setup is checked against the fake cookie and
// rewritten to carry the real one (or none), so the real cookie never leaves this machine.
class X11AuthRewriter {
public:
    enum class State { NeedMore, Done, Rejected };

    X11AuthRewriter(std::string fakeCookie, std::string realName, std::string realData);
    // Feeds bytes from the channel. Once Done, `output` holds the rewritten setup plus everything
    // after it; later calls pass data through unchanged.
    State feed(std::string_view data, std::string& output);
    State state() const { return m_state; }

private:
    std::string m_fake;
    std::string m_realName;
    std::string m_realData;
    std::string m_buffer;
    State m_state = State::NeedMore;
};

// Random fake cookie (16 bytes) and its lowercase hex form for the x11-req.
std::string randomX11Cookie();
std::string hexEncode(std::string_view bytes);

// Connects to the local X server of `display`: its Unix socket (falling back to Linux's abstract
// socket) or TCP port 6000 + number.
core::Status connectX11(const X11Display& display, net::Socket& out);

} // namespace wt::ssh

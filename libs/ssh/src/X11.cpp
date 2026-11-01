#include "wt/ssh/X11.h"

#include "wt/platform/Files.h"

#include <cstdlib>
#include <random>

namespace wt::ssh {
namespace {

constexpr std::uint16_t kFamilyInternet = 0;
constexpr std::uint16_t kFamilyLocal = 256;
constexpr std::uint16_t kFamilyWild = 65535;
const std::string kMitCookie = "MIT-MAGIC-COOKIE-1";

bool parseNumber(std::string_view text, int& out) {
    if (text.empty() || text.size() > 6) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

std::size_t pad4(std::size_t n) {
    return (n + 3) & ~std::size_t(3);
}

std::uint16_t readCard16(std::string_view bytes, std::size_t offset, bool bigEndian) {
    const auto a = static_cast<std::uint8_t>(bytes[offset]);
    const auto b = static_cast<std::uint8_t>(bytes[offset + 1]);
    return static_cast<std::uint16_t>(bigEndian ? (a << 8) | b : (b << 8) | a);
}

void appendCard16(std::string& out, std::uint16_t value, bool bigEndian) {
    const char hi = static_cast<char>(value >> 8);
    const char lo = static_cast<char>(value & 0xFF);
    out += bigEndian ? hi : lo;
    out += bigEndian ? lo : hi;
}

} // namespace

std::optional<X11Display> parseX11Display(std::string_view display) {
    const auto colon = display.rfind(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view host = display.substr(0, colon);
    std::string_view rest = display.substr(colon + 1);
    X11Display parsed;
    const auto dot = rest.find('.');
    if (!parseNumber(rest.substr(0, dot), parsed.number)) {
        return std::nullopt;
    }
    if (dot != std::string_view::npos && !parseNumber(rest.substr(dot + 1), parsed.screen)) {
        return std::nullopt;
    }
    if (!host.empty() && host.front() == '/') {
        parsed.socketPath = std::string(host); // XQuartz/launchd socket
    } else if (host.empty() || host == "unix") {
        parsed.socketPath = "/tmp/.X11-unix/X" + std::to_string(parsed.number);
    } else {
        parsed.host = std::string(host);
    }
    return parsed;
}

std::vector<XauthEntry> parseXauthority(std::string_view contents) {
    std::vector<XauthEntry> entries;
    std::size_t pos = 0;
    auto readCounted = [&](std::string& out) {
        if (pos + 2 > contents.size()) {
            return false;
        }
        const std::size_t length = readCard16(contents, pos, true);
        pos += 2;
        if (pos + length > contents.size()) {
            return false;
        }
        out.assign(contents.substr(pos, length));
        pos += length;
        return true;
    };
    while (pos + 2 <= contents.size()) {
        XauthEntry entry;
        entry.family = readCard16(contents, pos, true);
        pos += 2;
        if (!readCounted(entry.address) || !readCounted(entry.number) || !readCounted(entry.name) ||
            !readCounted(entry.data)) {
            break;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::string defaultXauthorityPath() {
    if (const char* path = std::getenv("XAUTHORITY"); path && *path) {
        return path;
    }
    const std::string home = platform::homeDirectory();
    return home.empty() ? std::string() : home + "/.Xauthority";
}

std::optional<XauthEntry> findX11Cookie(const std::vector<XauthEntry>& entries, const X11Display& display,
                                        const std::string& hostname) {
    const std::string number = std::to_string(display.number);
    const bool local = display.host.empty() || display.host == "localhost" || display.host == "127.0.0.1";
    const XauthEntry* fallback = nullptr;
    for (const XauthEntry& entry : entries) {
        if (entry.number != number || entry.name != kMitCookie) {
            continue;
        }
        if (entry.family == kFamilyWild) {
            return entry;
        }
        if (local && entry.family == kFamilyLocal && entry.address == hostname) {
            return entry;
        }
        if (local && entry.family == kFamilyInternet && entry.address == std::string("\x7f\x00\x00\x01", 4)) {
            return entry;
        }
        if (!local && entry.family == kFamilyLocal && entry.address == display.host) {
            return entry;
        }
        if (!fallback) {
            fallback = &entry;
        }
    }
    if (fallback) {
        return *fallback;
    }
    return std::nullopt;
}

X11AuthRewriter::X11AuthRewriter(std::string fakeCookie, std::string realName, std::string realData)
    : m_fake(std::move(fakeCookie)), m_realName(std::move(realName)), m_realData(std::move(realData)) {}

X11AuthRewriter::State X11AuthRewriter::feed(std::string_view data, std::string& output) {
    if (m_state == State::Done) {
        output.append(data);
        return m_state;
    }
    if (m_state == State::Rejected) {
        return m_state;
    }
    m_buffer.append(data);
    if (m_buffer.size() < 12) {
        return m_state;
    }
    const char order = m_buffer[0];
    if (order != 'B' && order != 'l') {
        m_state = State::Rejected;
        return m_state;
    }
    const bool bigEndian = order == 'B';
    const std::size_t nameLength = readCard16(m_buffer, 6, bigEndian);
    const std::size_t dataLength = readCard16(m_buffer, 8, bigEndian);
    const std::size_t total = 12 + pad4(nameLength) + pad4(dataLength);
    if (m_buffer.size() < total) {
        if (m_buffer.size() > 4096) {
            m_state = State::Rejected;
        }
        return m_state;
    }
    const std::string_view name = std::string_view(m_buffer).substr(12, nameLength);
    const std::string_view cookie = std::string_view(m_buffer).substr(12 + pad4(nameLength), dataLength);
    if (name != kMitCookie || cookie != m_fake) {
        m_state = State::Rejected; // not from a client the server gave our fake cookie to
        return m_state;
    }
    std::string setup;
    setup.append(m_buffer, 0, 6); // byte order, pad, protocol version
    appendCard16(setup, static_cast<std::uint16_t>(m_realName.size()), bigEndian);
    appendCard16(setup, static_cast<std::uint16_t>(m_realData.size()), bigEndian);
    setup.append(2, '\0');
    setup += m_realName;
    setup.append(pad4(m_realName.size()) - m_realName.size(), '\0');
    setup += m_realData;
    setup.append(pad4(m_realData.size()) - m_realData.size(), '\0');
    output += setup;
    output.append(m_buffer, total, std::string::npos);
    m_buffer.clear();
    m_state = State::Done;
    return m_state;
}

std::string randomX11Cookie() {
    std::random_device device;
    std::string cookie(16, '\0');
    for (char& c : cookie) {
        c = static_cast<char>(device() & 0xFF);
    }
    return cookie;
}

std::string hexEncode(std::string_view bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        const auto byte = static_cast<std::uint8_t>(c);
        out += digits[byte >> 4];
        out += digits[byte & 0xF];
    }
    return out;
}

core::Status connectX11(const X11Display& display, net::Socket& out) {
    if (!display.socketPath.empty()) {
        auto status = net::connectUnix(display.socketPath, out);
#ifdef __linux__
        if (!status && display.socketPath.rfind("/tmp/.X11-unix/", 0) == 0) {
            status = net::connectUnix("@" + display.socketPath, out); // abstract socket of the same display
        }
#endif
        return status;
    }
    return net::connectTcp(display.host, static_cast<std::uint16_t>(6000 + display.number), std::chrono::seconds(3), out);
}

} // namespace wt::ssh

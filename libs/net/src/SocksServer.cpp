#include "wt/net/SocksServer.h"

#include <cstdio>

namespace wt::net {
namespace {

constexpr std::size_t kMaxRequest = 1024;

std::uint16_t readPort(std::string_view bytes) {
    return static_cast<std::uint16_t>((static_cast<std::uint8_t>(bytes[0]) << 8) | static_cast<std::uint8_t>(bytes[1]));
}

std::string ipv4Text(std::string_view bytes) {
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", static_cast<std::uint8_t>(bytes[0]),
                  static_cast<std::uint8_t>(bytes[1]), static_cast<std::uint8_t>(bytes[2]),
                  static_cast<std::uint8_t>(bytes[3]));
    return text;
}

std::string ipv6Text(std::string_view bytes) {
    std::string text;
    char group[8];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(group, sizeof(group), "%x",
                      (static_cast<std::uint8_t>(bytes[static_cast<std::size_t>(2 * i)]) << 8) |
                          static_cast<std::uint8_t>(bytes[static_cast<std::size_t>(2 * i + 1)]));
        text += (i ? ":" : "") + std::string(group);
    }
    return text;
}

const std::string_view kSocks5GeneralFailure("\x05\x01\x00\x01\x00\x00\x00\x00\x00\x00", 10);
const std::string_view kSocks5CommandNotSupported("\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00", 10);
const std::string_view kSocks5AddressNotSupported("\x05\x08\x00\x01\x00\x00\x00\x00\x00\x00", 10);
const std::string_view kSocks4Rejected("\x00\x5b\x00\x00\x00\x00\x00\x00", 8);

} // namespace

SocksServerHandshake::State SocksServerHandshake::fail(std::string message, std::string& reply,
                                                       std::string_view failureReply) {
    m_error = std::move(message);
    reply.append(failureReply);
    m_phase = Phase::Done;
    m_state = State::Failed;
    return m_state;
}

SocksServerHandshake::State SocksServerHandshake::feed(std::string_view data, std::string& reply) {
    if (m_state != State::NeedMore) {
        if (m_state == State::Ready) {
            m_buffer.append(data); // early data after the request
        }
        return m_state;
    }
    m_buffer.append(data);
    if (m_buffer.size() > kMaxRequest) {
        return fail("SOCKS request too long", reply, {});
    }
    return parse(reply);
}

SocksServerHandshake::State SocksServerHandshake::parse(std::string& reply) {
    for (;;) {
        switch (m_phase) {
        case Phase::Version:
            if (m_buffer.empty()) {
                return m_state;
            }
            m_version = static_cast<std::uint8_t>(m_buffer[0]);
            if (m_version == 5) {
                m_phase = Phase::Socks5Methods;
            } else if (m_version == 4) {
                m_phase = Phase::Socks4Request;
            } else {
                return fail("not a SOCKS request (version " + std::to_string(m_version) + ")", reply, {});
            }
            break;
        case Phase::Socks5Methods: {
            if (m_buffer.size() < 2) {
                return m_state;
            }
            const std::size_t count = static_cast<std::uint8_t>(m_buffer[1]);
            if (m_buffer.size() < 2 + count) {
                return m_state;
            }
            const std::string_view methods = std::string_view(m_buffer).substr(2, count);
            m_buffer.erase(0, 2 + count);
            if (methods.find('\0') == std::string_view::npos) {
                return fail("the SOCKS client requires authentication", reply, std::string_view("\x05\xff", 2));
            }
            reply.append("\x05\x00", 2);
            m_phase = Phase::Socks5Request;
            break;
        }
        case Phase::Socks5Request: {
            if (m_buffer.size() < 5) {
                return m_state;
            }
            if (static_cast<std::uint8_t>(m_buffer[0]) != 5) {
                return fail("malformed SOCKS5 request", reply, kSocks5GeneralFailure);
            }
            const auto command = static_cast<std::uint8_t>(m_buffer[1]);
            const auto type = static_cast<std::uint8_t>(m_buffer[3]);
            std::size_t addressLength = 0;
            std::size_t addressStart = 4;
            if (type == 1) {
                addressLength = 4;
            } else if (type == 4) {
                addressLength = 16;
            } else if (type == 3) {
                addressLength = static_cast<std::uint8_t>(m_buffer[4]);
                addressStart = 5;
            } else {
                return fail("unsupported SOCKS5 address type", reply, kSocks5AddressNotSupported);
            }
            const std::size_t total = addressStart + addressLength + 2;
            if (m_buffer.size() < total) {
                return m_state;
            }
            if (command != 1) {
                return fail("only SOCKS CONNECT is supported", reply, kSocks5CommandNotSupported);
            }
            const std::string_view address = std::string_view(m_buffer).substr(addressStart, addressLength);
            m_host = type == 1 ? ipv4Text(address) : type == 4 ? ipv6Text(address) : std::string(address);
            m_port = readPort(std::string_view(m_buffer).substr(addressStart + addressLength, 2));
            m_buffer.erase(0, total);
            if (m_host.empty()) {
                return fail("empty SOCKS5 host name", reply, kSocks5AddressNotSupported);
            }
            m_phase = Phase::Done;
            m_state = State::Ready;
            return m_state;
        }
        case Phase::Socks4Request: {
            // VN CD DSTPORT(2) DSTIP(4) USERID NUL [HOST NUL for 4a]
            if (m_buffer.size() < 9) {
                return m_state;
            }
            const auto userEnd = m_buffer.find('\0', 8);
            if (userEnd == std::string::npos) {
                return m_state;
            }
            const std::string_view ip = std::string_view(m_buffer).substr(4, 4);
            const bool socks4a = ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] != 0;
            std::size_t end = userEnd + 1;
            if (socks4a) {
                const auto hostEnd = m_buffer.find('\0', end);
                if (hostEnd == std::string::npos) {
                    return m_state;
                }
                m_host = m_buffer.substr(end, hostEnd - end);
                end = hostEnd + 1;
            } else {
                m_host = ipv4Text(ip);
            }
            if (static_cast<std::uint8_t>(m_buffer[1]) != 1) {
                return fail("only SOCKS CONNECT is supported", reply, kSocks4Rejected);
            }
            m_port = readPort(std::string_view(m_buffer).substr(2, 2));
            m_buffer.erase(0, end);
            if (m_host.empty()) {
                return fail("empty SOCKS4a host name", reply, kSocks4Rejected);
            }
            m_phase = Phase::Done;
            m_state = State::Ready;
            return m_state;
        }
        case Phase::Done: return m_state;
        }
    }
}

std::string SocksServerHandshake::finish(bool success) const {
    if (m_version == 4) {
        std::string reply("\x00\x5a\x00\x00\x00\x00\x00\x00", 8);
        if (!success) {
            reply[1] = '\x5b';
        }
        return reply;
    }
    std::string reply("\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10);
    if (!success) {
        reply[1] = '\x05'; // connection refused
    }
    return reply;
}

std::string SocksServerHandshake::takeLeftover() {
    std::string leftover;
    leftover.swap(m_buffer);
    return leftover;
}

} // namespace wt::net

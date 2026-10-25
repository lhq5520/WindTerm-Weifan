#include "wt/core/Base64.h"

#include <cstdint>

namespace wt::core {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '-') { // also accept the URL-safe alphabet
        return 62;
    }
    if (c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string base64Encode(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                                (static_cast<std::uint8_t>(bytes[i + 1]) << 8) | static_cast<std::uint8_t>(bytes[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest > 0) {
        std::uint32_t v = static_cast<std::uint8_t>(bytes[i]) << 16;
        if (rest == 2) {
            v |= static_cast<std::uint8_t>(bytes[i + 1]) << 8;
        }
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(rest == 2 ? kAlphabet[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view text) {
    std::string out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    int padding = 0;
    std::size_t symbols = 0;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        if (c == '=') {
            ++padding;
            continue;
        }
        if (padding > 0) {
            return std::nullopt; // data after padding
        }
        const int value = decodeChar(c);
        if (value < 0) {
            return std::nullopt;
        }
        ++symbols;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    if (symbols % 4 == 1 || padding > 2 || (padding > 0 && (symbols + static_cast<std::size_t>(padding)) % 4 != 0)) {
        return std::nullopt;
    }
    return out;
}

std::string hexEncode(std::string_view bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (char c : bytes) {
        const auto b = static_cast<std::uint8_t>(c);
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0xF]);
    }
    return out;
}

std::optional<std::string> hexDecode(std::string_view text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = hexValue(text[i]);
        const int lo = hexValue(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

} // namespace wt::core

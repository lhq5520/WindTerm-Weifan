#include "wt/core/Utf8.h"

namespace wt::core {

void appendUtf8(std::string& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = kReplacementChar;
    }
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string toUtf8(std::u32string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char32_t cp : text) {
        appendUtf8(out, cp);
    }
    return out;
}

std::u32string fromUtf8(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    Utf8Decoder decoder;
    decoder.decode(text, [&out](char32_t cp) { out.push_back(cp); });
    if (decoder.hasPendingSequence()) {
        out.push_back(kReplacementChar);
    }
    return out;
}

} // namespace wt::core

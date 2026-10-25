#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace wt::core {

inline constexpr char32_t kReplacementChar = 0xFFFD;

// Streaming UTF-8 decoder following the WHATWG "UTF-8 decode" algorithm:
// invalid or truncated sequences yield U+FFFD, and a multi-byte sequence may be
// split across successive decode() calls (as happens with PTY reads).
class Utf8Decoder {
public:
    template <typename Emit>
    void decode(std::string_view bytes, Emit&& emit) {
        for (std::size_t i = 0; i < bytes.size();) {
            const auto b = static_cast<std::uint8_t>(bytes[i]);
            if (m_needed == 0) {
                ++i;
                if (b <= 0x7F) {
                    emit(static_cast<char32_t>(b));
                } else if (b >= 0xC2 && b <= 0xDF) {
                    m_needed = 1;
                    m_codepoint = b & 0x1Fu;
                } else if (b >= 0xE0 && b <= 0xEF) {
                    if (b == 0xE0) {
                        m_lower = 0xA0;
                    } else if (b == 0xED) {
                        m_upper = 0x9F;
                    }
                    m_needed = 2;
                    m_codepoint = b & 0x0Fu;
                } else if (b >= 0xF0 && b <= 0xF4) {
                    if (b == 0xF0) {
                        m_lower = 0x90;
                    } else if (b == 0xF4) {
                        m_upper = 0x8F;
                    }
                    m_needed = 3;
                    m_codepoint = b & 0x07u;
                } else {
                    emit(kReplacementChar);
                }
                continue;
            }

            if (b < m_lower || b > m_upper) {
                // Abort the current sequence and reprocess this byte from scratch.
                reset();
                emit(kReplacementChar);
                continue;
            }

            ++i;
            m_lower = 0x80;
            m_upper = 0xBF;
            m_codepoint = (m_codepoint << 6) | (b & 0x3Fu);
            if (++m_seen == m_needed) {
                const char32_t cp = m_codepoint;
                reset();
                emit(cp);
            }
        }
    }

    // True while a multi-byte sequence is incomplete.
    bool hasPendingSequence() const { return m_needed != 0; }
    void reset() {
        m_codepoint = 0;
        m_needed = 0;
        m_seen = 0;
        m_lower = 0x80;
        m_upper = 0xBF;
    }

private:
    char32_t m_codepoint = 0;
    std::uint8_t m_needed = 0;
    std::uint8_t m_seen = 0;
    std::uint8_t m_lower = 0x80;
    std::uint8_t m_upper = 0xBF;
};

// Appends the UTF-8 encoding of `cp` to `out`. Invalid code points encode as U+FFFD.
void appendUtf8(std::string& out, char32_t cp);
std::string toUtf8(std::u32string_view text);
std::u32string fromUtf8(std::string_view text);

} // namespace wt::core

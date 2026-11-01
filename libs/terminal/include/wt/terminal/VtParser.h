#pragma once

#include "wt/core/Utf8.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace wt::terminal {

// Numeric parameters of a CSI sequence. Missing values read as 0, matching the
// "default parameter" convention; at() substitutes the caller's default for 0.
// Sub-parameters separated by ':' (e.g. SGR 38:2::r:g:b) are kept in order and flagged.
class CsiParams {
public:
    static constexpr std::size_t kMaxParams = 32;

    std::size_t size() const { return m_count; }
    bool empty() const { return m_count == 0; }
    std::uint16_t raw(std::size_t i) const { return i < m_count ? m_values[i] : 0; }
    int at(std::size_t i, int defaultValue) const {
        const int v = raw(i);
        return v == 0 ? defaultValue : v;
    }
    // True if parameter i was introduced by ':' rather than ';'.
    bool isSubParam(std::size_t i) const { return i < m_count && (m_subMask >> i) & 1u; }

    void clear() {
        m_count = 0;
        m_subMask = 0;
        m_current = 0;
        m_hasCurrent = false;
        m_nextIsSub = false;
    }
    void addDigit(int digit);
    void separator(bool colon);
    void finish();

private:
    void push();

    std::array<std::uint16_t, kMaxParams> m_values{};
    std::size_t m_count = 0;
    std::uint32_t m_subMask = 0;
    std::uint32_t m_current = 0;
    bool m_hasCurrent = false;
    bool m_nextIsSub = false;
};

class VtHandler {
public:
    virtual ~VtHandler() = default;
    virtual void print(char32_t cp) = 0;
    // A run of printable ASCII (0x20-0x7E) in the ground state; same effect as print() per byte.
    virtual void printAscii(std::string_view text) {
        for (char c : text) {
            print(static_cast<unsigned char>(c));
        }
    }
    // C0 control (0x00-0x1F) other than ESC/CAN/SUB handling performed by the parser.
    virtual void execute(std::uint8_t control) = 0;
    // `prefix` is the private marker ('?', '>', '<', '=') or 0.
    virtual void csiDispatch(const CsiParams& params, char prefix, std::string_view intermediates,
                             char final) = 0;
    virtual void escDispatch(std::string_view intermediates, char final) = 0;
    // Payload of OSC, UTF-8, without the terminator. `bellTerminated` is true for BEL (xterm
    // style) rather than ST, so replies can use the same terminator.
    virtual void oscDispatch(std::string_view payload, bool bellTerminated) = 0;
    // Complete DCS string: parameters/prefix/intermediates/final as for CSI, then the data up to
    // (not including) ST.
    virtual void dcsDispatch(const CsiParams& params, char prefix, std::string_view intermediates, char final,
                             std::string_view data) {
        (void)params, (void)prefix, (void)intermediates, (void)final, (void)data;
    }
    // Complete APC string (ESC _ ... ST) without the terminator, e.g. kitty graphics ("G...").
    virtual void apcDispatch(std::string_view data) { (void)data; }
};

// DEC/ANSI escape sequence parser based on Paul Williams' VT500 state machine
// (https://vt100.net/emu/dec_ansi_parser), operating on UTF-8 decoded code points.
// DCS and APC strings are collected (bounded) and dispatched; SOS/PM strings are consumed and ignored.
class VtParser {
public:
    static constexpr std::size_t kMaxOscLength = 64 * 1024;
    static constexpr std::size_t kMaxDcsLength = 64 * 1024;
    static constexpr std::size_t kMaxSixelLength = 32 * 1024 * 1024;
    static constexpr std::size_t kMaxApcLength = 32 * 1024 * 1024; // kitty graphics in one chunk
    static constexpr std::size_t kMaxIntermediates = 2;

    explicit VtParser(VtHandler& handler) : m_handler(handler) {}

    void feed(std::string_view bytes);
    void reset();

private:
    enum class State : std::uint8_t {
        Ground,
        Escape,
        EscapeIntermediate,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        CsiIgnore,
        OscString,
        DcsEntry,
        DcsParam,
        DcsIntermediate,
        DcsPassthrough,
        DcsIgnore,     // malformed DCS: swallow until ST
        ApcString,
        IgnoredString, // SOS, PM: swallow until ST
    };

    void advance(char32_t cp);
    void clearSequence();
    void collect(char32_t cp);
    void enterEscape();
    void dispatchCsi(char32_t final);
    void finishOsc(bool bellTerminated);
    void finishDcs();
    void finishApc();
    // Common parameter/intermediate handling for CSI and DCS headers; returns false for bytes it
    // does not handle.
    bool collectParam(char32_t cp, State paramState, State intermediateState, State ignoreState);

    VtHandler& m_handler;
    core::Utf8Decoder m_decoder;
    State m_state = State::Ground;
    CsiParams m_params;
    char m_prefix = 0;
    std::string m_intermediates;
    bool m_intermediateOverflow = false;
    std::string m_osc;
    std::string m_dcs;
    char m_dcsFinal = 0;
    bool m_dcsOverflow = false;
    std::string m_apc;
    bool m_apcOverflow = false;
};

} // namespace wt::terminal

#include "wt/terminal/VtParser.h"

#include <algorithm>

namespace wt::terminal {

void CsiParams::addDigit(int digit) {
    m_current = m_current * 10 + static_cast<std::uint32_t>(digit);
    if (m_current > 0xFFFF) {
        m_current = 0xFFFF;
    }
    m_hasCurrent = true;
}

void CsiParams::separator(bool colon) {
    push();
    m_nextIsSub = colon;
}

void CsiParams::finish() {
    // A trailing parameter is only recorded if something was written for it, so "CSI m" has no
    // parameters while "CSI ;m" has two empty ones.
    if (m_hasCurrent || m_count > 0) {
        push();
    }
}

void CsiParams::push() {
    if (m_count < kMaxParams) {
        m_values[m_count] = static_cast<std::uint16_t>(m_current);
        if (m_nextIsSub) {
            m_subMask |= 1u << m_count;
        }
        ++m_count;
    }
    m_current = 0;
    m_hasCurrent = false;
    m_nextIsSub = false;
}

void VtParser::feed(std::string_view bytes) {
    auto isPrintableAscii = [](char c) { return c >= 0x20 && c < 0x7F; };
    std::size_t i = 0;
    while (i < bytes.size()) {
        if (m_state == State::Ground && !m_decoder.hasPendingSequence() && isPrintableAscii(bytes[i])) {
            // Fast path: hand whole runs of plain text to the handler.
            std::size_t end = i + 1;
            while (end < bytes.size() && isPrintableAscii(bytes[end])) {
                ++end;
            }
            m_handler.printAscii(bytes.substr(i, end - i));
            i = end;
            continue;
        }
        // Everything else goes through the UTF-8 decoder and the state machine, up to the next
        // byte that could start a plain-text run.
        std::size_t end = i + 1;
        while (end < bytes.size() && !isPrintableAscii(bytes[end])) {
            ++end;
        }
        if (m_state != State::Ground) {
            // Inside a sequence printable bytes are parameters/data (no fast path); decode a
            // bounded chunk; text after the sequence ends within it simply takes the slow path.
            end = std::min(bytes.size(), i + 4096);
        }
        m_decoder.decode(bytes.substr(i, end - i), [this](char32_t cp) { advance(cp); });
        i = end;
    }
}

void VtParser::reset() {
    m_decoder.reset();
    m_state = State::Ground;
    clearSequence();
    m_osc.clear();
    m_dcs.clear();
    m_apc.clear();
}

void VtParser::clearSequence() {
    m_params.clear();
    m_prefix = 0;
    m_intermediates.clear();
    m_intermediateOverflow = false;
}

void VtParser::collect(char32_t cp) {
    if (m_intermediates.size() < kMaxIntermediates) {
        m_intermediates.push_back(static_cast<char>(cp));
    } else {
        m_intermediateOverflow = true;
    }
}

void VtParser::enterEscape() {
    if (m_state == State::OscString) {
        finishOsc(false); // ESC begins the ST terminator (ESC \)
    } else if (m_state == State::DcsPassthrough) {
        finishDcs();
    } else if (m_state == State::ApcString) {
        finishApc();
    }
    m_state = State::Escape;
    clearSequence();
}

void VtParser::dispatchCsi(char32_t final) {
    m_params.finish();
    if (!m_intermediateOverflow) {
        m_handler.csiDispatch(m_params, m_prefix, m_intermediates, static_cast<char>(final));
    }
    m_state = State::Ground;
}

void VtParser::finishOsc(bool bellTerminated) {
    m_handler.oscDispatch(m_osc, bellTerminated);
    m_osc.clear();
}

void VtParser::finishDcs() {
    if (!m_dcsOverflow && !m_intermediateOverflow) {
        m_handler.dcsDispatch(m_params, m_prefix, m_intermediates, m_dcsFinal, m_dcs);
    }
    m_dcs.clear();
    m_dcsOverflow = false;
}

void VtParser::finishApc() {
    if (!m_apcOverflow) {
        m_handler.apcDispatch(m_apc);
    }
    m_apc.clear();
    m_apcOverflow = false;
}

bool VtParser::collectParam(char32_t cp, State paramState, State intermediateState, State ignoreState) {
    if (cp >= '0' && cp <= '9') {
        m_params.addDigit(static_cast<int>(cp - '0'));
        m_state = paramState;
    } else if (cp == ';' || cp == ':') {
        m_params.separator(cp == ':');
        m_state = paramState;
    } else if (cp >= 0x3C && cp <= 0x3F) {
        if (m_state != paramState) {
            m_prefix = static_cast<char>(cp);
            m_state = paramState;
        } else {
            m_state = ignoreState;
        }
    } else if (cp >= 0x20 && cp <= 0x2F) {
        collect(cp);
        m_state = intermediateState;
    } else {
        return false;
    }
    return true;
}

void VtParser::advance(char32_t cp) {
    // Transitions valid from any state.
    if (cp == 0x1B) {
        enterEscape();
        return;
    }
    if (cp == 0x18 || cp == 0x1A) { // CAN, SUB abort the current sequence
        m_osc.clear();
        m_dcs.clear();
        m_apc.clear();
        m_apcOverflow = false;
        m_state = State::Ground;
        m_handler.execute(static_cast<std::uint8_t>(cp));
        return;
    }
    if (cp == 0x9C) { // C1 ST
        if (m_state == State::OscString) {
            finishOsc(false);
        } else if (m_state == State::DcsPassthrough) {
            finishDcs();
        } else if (m_state == State::ApcString) {
            finishApc();
        }
        m_state = State::Ground;
        return;
    }

    const bool c0 = cp < 0x20;
    switch (m_state) {
    case State::Ground:
        if (c0) {
            m_handler.execute(static_cast<std::uint8_t>(cp));
        } else if (cp != 0x7F && (cp < 0x80 || cp >= 0xA0)) {
            m_handler.print(cp);
        }
        return;

    case State::Escape:
        if (c0) {
            m_handler.execute(static_cast<std::uint8_t>(cp));
        } else if (cp >= 0x20 && cp <= 0x2F) {
            collect(cp);
            m_state = State::EscapeIntermediate;
        } else if (cp == '[') {
            clearSequence();
            m_state = State::CsiEntry;
        } else if (cp == ']') {
            m_osc.clear();
            m_state = State::OscString;
        } else if (cp == 'P') {
            clearSequence();
            m_dcs.clear();
            m_dcsOverflow = false;
            m_state = State::DcsEntry;
        } else if (cp == '_') {
            m_apc.clear();
            m_apcOverflow = false;
            m_state = State::ApcString;
        } else if (cp == 'X' || cp == '^') {
            m_state = State::IgnoredString;
        } else if (cp >= 0x30 && cp <= 0x7E) {
            m_handler.escDispatch({}, static_cast<char>(cp));
            m_state = State::Ground;
        } else if (cp != 0x7F) {
            m_state = State::Ground;
        }
        return;

    case State::EscapeIntermediate:
        if (c0) {
            m_handler.execute(static_cast<std::uint8_t>(cp));
        } else if (cp >= 0x20 && cp <= 0x2F) {
            collect(cp);
        } else if (cp >= 0x30 && cp <= 0x7E) {
            if (!m_intermediateOverflow) {
                m_handler.escDispatch(m_intermediates, static_cast<char>(cp));
            }
            m_state = State::Ground;
        } else if (cp != 0x7F) {
            m_state = State::Ground;
        }
        return;

    case State::CsiEntry:
    case State::CsiParam:
        if (c0) {
            m_handler.execute(static_cast<std::uint8_t>(cp));
        } else if (collectParam(cp, State::CsiParam, State::CsiIntermediate, State::CsiIgnore)) {
            // parameter, prefix or intermediate
        } else if (cp >= 0x40 && cp <= 0x7E) {
            dispatchCsi(cp);
        } else if (cp != 0x7F) {
            m_state = State::CsiIgnore;
        }
        return;

    case State::CsiIntermediate:
        if (c0) {
            m_handler.execute(static_cast<std::uint8_t>(cp));
        } else if (cp >= 0x20 && cp <= 0x2F) {
            collect(cp);
        } else if (cp >= 0x40 && cp <= 0x7E) {
            dispatchCsi(cp);
        } else if (cp != 0x7F) {
            m_state = State::CsiIgnore;
        }
        return;

    case State::CsiIgnore:
        if (c0) {
            m_handler.execute(static_cast<std::uint8_t>(cp));
        } else if (cp >= 0x40 && cp <= 0x7E) {
            m_state = State::Ground;
        }
        return;

    case State::OscString:
        if (cp == 0x07) { // BEL terminator (xterm)
            finishOsc(true);
            m_state = State::Ground;
        } else if (!c0 && m_osc.size() < kMaxOscLength) {
            core::appendUtf8(m_osc, cp);
        }
        return;

    case State::DcsEntry:
    case State::DcsParam:
        if (c0 || cp == 0x7F) {
            return; // ignored inside a DCS header
        }
        if (collectParam(cp, State::DcsParam, State::DcsIntermediate, State::DcsIgnore)) {
            return;
        }
        if (cp >= 0x40 && cp <= 0x7E) {
            m_params.finish();
            m_dcsFinal = static_cast<char>(cp);
            m_state = State::DcsPassthrough;
        } else {
            m_state = State::DcsIgnore;
        }
        return;

    case State::DcsIntermediate:
        if (c0 || cp == 0x7F) {
            return;
        }
        if (cp >= 0x20 && cp <= 0x2F) {
            collect(cp);
        } else if (cp >= 0x40 && cp <= 0x7E) {
            m_params.finish();
            m_dcsFinal = static_cast<char>(cp);
            m_state = State::DcsPassthrough;
        } else {
            m_state = State::DcsIgnore;
        }
        return;

    case State::DcsPassthrough:
        if (cp == 0x7F) {
            return;
        }
        // Sixel images (DCS ... q) are much larger than other device control strings.
        if (m_dcs.size() < (m_dcsFinal == 'q' && m_intermediates.empty() && m_prefix == 0 ? kMaxSixelLength
                                                                                           : kMaxDcsLength)) {
            core::appendUtf8(m_dcs, cp);
        } else {
            m_dcsOverflow = true;
        }
        return;

    case State::ApcString:
        if (c0 || cp == 0x7F) {
            return;
        }
        if (m_apc.size() < kMaxApcLength) {
            core::appendUtf8(m_apc, cp);
        } else {
            m_apcOverflow = true;
        }
        return;

    case State::DcsIgnore:
    case State::IgnoredString:
        return;
    }
}

} // namespace wt::terminal

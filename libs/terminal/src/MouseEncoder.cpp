#include "wt/terminal/MouseEncoder.h"

#include "wt/core/Utf8.h"

#include <algorithm>

namespace wt::terminal {
namespace {

bool isWheel(MouseButton button) {
    return button >= MouseButton::WheelUp;
}

int buttonCode(MouseButton button) {
    switch (button) {
    case MouseButton::Left: return 0;
    case MouseButton::Middle: return 1;
    case MouseButton::Right: return 2;
    case MouseButton::None: return 3;
    case MouseButton::WheelUp: return 64;
    case MouseButton::WheelDown: return 65;
    case MouseButton::WheelLeft: return 66;
    case MouseButton::WheelRight: return 67;
    }
    return 3;
}

} // namespace

std::string encodeMouse(const MouseEvent& event, MouseTracking tracking, MouseEncoding encoding) {
    switch (tracking) {
    case MouseTracking::None: return {};
    case MouseTracking::X10:
        // X10 compatibility: button presses only, no modifiers, no wheel.
        if (event.action != MouseAction::Press || isWheel(event.button) || event.button == MouseButton::None) {
            return {};
        }
        break;
    case MouseTracking::Normal:
        if (event.action == MouseAction::Motion) {
            return {};
        }
        break;
    case MouseTracking::ButtonEvent:
        if (event.action == MouseAction::Motion && event.button == MouseButton::None) {
            return {};
        }
        break;
    case MouseTracking::AnyEvent: break;
    }
    if (isWheel(event.button) && event.action == MouseAction::Release) {
        return {}; // wheel "buttons" have no release
    }
    if (event.action == MouseAction::Press && event.button == MouseButton::None) {
        return {};
    }

    const bool sgr = encoding == MouseEncoding::Sgr;
    int code = buttonCode(event.button);
    if (event.action == MouseAction::Release && !sgr) {
        code = 3; // legacy encodings cannot tell which button was released
    }
    if (event.action == MouseAction::Motion) {
        code += 32;
    }
    if (tracking != MouseTracking::X10) {
        if (event.modifiers & ShiftModifier) {
            code += 4;
        }
        if (event.modifiers & (AltModifier | MetaModifier)) {
            code += 8;
        }
        if (event.modifiers & ControlModifier) {
            code += 16;
        }
    }
    const int x = std::max(0, event.column) + 1;
    const int y = std::max(0, event.row) + 1;

    switch (encoding) {
    case MouseEncoding::Sgr:
        return "\x1b[<" + std::to_string(code) + ";" + std::to_string(x) + ";" + std::to_string(y) +
               (event.action == MouseAction::Release ? "m" : "M");
    case MouseEncoding::Urxvt:
        return "\x1b[" + std::to_string(code + 32) + ";" + std::to_string(x) + ";" + std::to_string(y) + "M";
    case MouseEncoding::Utf8: {
        constexpr int kMax = 2047 - 32;
        if (x > kMax || y > kMax) {
            return {};
        }
        std::string out = "\x1b[M";
        core::appendUtf8(out, static_cast<char32_t>(code + 32));
        core::appendUtf8(out, static_cast<char32_t>(x + 32));
        core::appendUtf8(out, static_cast<char32_t>(y + 32));
        return out;
    }
    case MouseEncoding::Default: {
        constexpr int kMax = 255 - 32;
        if (x > kMax || y > kMax) {
            return {}; // not representable in a single byte
        }
        std::string out = "\x1b[M";
        out.push_back(static_cast<char>(code + 32));
        out.push_back(static_cast<char>(x + 32));
        out.push_back(static_cast<char>(y + 32));
        return out;
    }
    }
    return {};
}

} // namespace wt::terminal

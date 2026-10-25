#pragma once

#include "wt/terminal/KeyEncoder.h"
#include "wt/terminal/Terminal.h"

#include <string>

namespace wt::terminal {

enum class MouseButton : std::uint8_t {
    Left, Middle, Right,
    None, // motion with no button held
    WheelUp, WheelDown, WheelLeft, WheelRight,
};

enum class MouseAction : std::uint8_t { Press, Release, Motion };

struct MouseEvent {
    MouseAction action = MouseAction::Press;
    MouseButton button = MouseButton::Left;
    unsigned modifiers = NoModifier; // KeyEncoder Modifier flags
    int row = 0;                     // 0-based cell coordinates
    int column = 0;
};

// Encodes a mouse event for the application according to the tracking mode (?9/1000/1002/1003)
// and coordinate encoding (default, ?1005 UTF-8, ?1006 SGR, ?1015 urxvt). Returns an empty string
// when the current mode does not report this event (or coordinates cannot be encoded). Callers
// should suppress repeated motion events within the same cell.
std::string encodeMouse(const MouseEvent& event, MouseTracking tracking, MouseEncoding encoding);

} // namespace wt::terminal

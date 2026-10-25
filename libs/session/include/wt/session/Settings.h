#pragma once

#include "wt/core/Json.h"
#include "wt/session/Keymap.h"
#include "wt/session/Theme.h"

#include <string>
#include <vector>

namespace wt::session {

enum class CursorStyle { Block, Underline, Bar };
enum class BellStyle { None, Visual, Audible };

struct TerminalSettings {
    std::string fontFamily; // empty: the platform's fixed-width font
    double fontSize = 0;    // points; 0: platform default
    std::string theme = std::string(kDefaultThemeName);
    int scrollbackLines = 10000;
    CursorStyle cursorStyle = CursorStyle::Block;
    bool cursorBlink = false;
    bool optionAsMeta = false;        // macOS: Option sends ESC-prefixed keys instead of composing
    bool copyOnSelect = false;
    bool allowClipboardWrite = false; // OSC 52
    BellStyle bell = BellStyle::Visual;

    friend bool operator==(const TerminalSettings&, const TerminalSettings&) = default;
};

// Application settings persisted as settings.json.
struct Settings {
    static constexpr int kVersion = 1;
    static constexpr int kMinScrollback = 0;
    static constexpr int kMaxScrollback = 1000000;

    TerminalSettings terminal;
    Keymap keymap = Keymap::defaults();
    std::vector<Theme> customThemes;
    bool confirmCloseRunning = true;
    std::string knownHostsFile; // empty: ~/.ssh/known_hosts

    core::Json toJson() const;
    // Unknown keys are ignored and out-of-range values clamped; `warnings` lists what was fixed.
    static Settings fromJson(const core::Json& json, std::vector<std::string>* warnings = nullptr);

    // The active theme (falls back to the default theme if the name is unknown).
    Theme activeTheme() const;

    friend bool operator==(const Settings&, const Settings&) = default;
};

} // namespace wt::session

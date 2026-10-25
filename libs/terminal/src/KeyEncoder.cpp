#include "wt/terminal/KeyEncoder.h"

#include "wt/core/Utf8.h"

#include <optional>

namespace wt::terminal {
namespace {

unsigned modifierParameter(unsigned modifiers) {
    unsigned value = 1;
    if (modifiers & ShiftModifier) {
        value += 1;
    }
    if (modifiers & AltModifier) {
        value += 2;
    }
    if (modifiers & ControlModifier) {
        value += 4;
    }
    if (modifiers & MetaModifier) {
        value += 8;
    }
    return value;
}

// Keys reported as CSI <letter> / SS3 <letter> (cursor keys, Home/End, F1-F4).
std::string letterKey(char letter, unsigned modifiers, bool application) {
    const unsigned mod = modifierParameter(modifiers);
    if (mod > 1) {
        return "\x1b[1;" + std::to_string(mod) + letter;
    }
    return std::string(application ? "\x1bO" : "\x1b[") + letter;
}

// Keys reported as CSI <n> ~ (editing keypad, F5-F12).
std::string tildeKey(int code, unsigned modifiers) {
    const unsigned mod = modifierParameter(modifiers);
    std::string s = "\x1b[" + std::to_string(code);
    if (mod > 1) {
        s += ";" + std::to_string(mod);
    }
    return s + "~";
}

std::string withAlt(std::string bytes, unsigned modifiers) {
    if (modifiers & AltModifier) {
        bytes.insert(bytes.begin(), '\x1b');
    }
    return bytes;
}

std::string keypadKey(Key key, unsigned modifiers, const KeyboardModes& modes) {
    struct Mapping {
        char plain;
        char application; // final byte of ESC O <x>
    };
    static constexpr Mapping kKeypad[] = {
        {'0', 'p'}, {'1', 'q'}, {'2', 'r'}, {'3', 's'}, {'4', 't'}, {'5', 'u'}, {'6', 'v'}, {'7', 'w'},
        {'8', 'x'}, {'9', 'y'}, {'.', 'n'}, {'\r', 'M'}, {'+', 'k'}, {'-', 'm'}, {'*', 'j'}, {'/', 'o'},
        {'=', 'X'},
    };
    const Mapping& m = kKeypad[static_cast<int>(key) - static_cast<int>(Key::Keypad0)];
    if (modes.keypadApplication) {
        return withAlt(std::string("\x1bO") + m.application, modifiers);
    }
    if (key == Key::KeypadEnter) {
        return withAlt(modes.newLineMode ? "\r\n" : "\r", modifiers);
    }
    return withAlt(std::string(1, m.plain), modifiers);
}

// CSI <code> [; <mods>] u
std::string kittyKey(unsigned code, unsigned modifiers) {
    const unsigned mod = modifierParameter(modifiers);
    std::string s = "\x1b[" + std::to_string(code);
    if (mod > 1) {
        s += ";" + std::to_string(mod);
    }
    return s + "u";
}

// Enter, Tab, Backspace and Escape under the kitty protocol, or nullopt for the legacy encoding.
std::optional<std::string> kittyTextKey(Key key, unsigned modifiers, const KeyboardModes& modes) {
    unsigned code = 0;
    switch (key) {
    case Key::Enter: code = 13; break;
    case Key::Tab: code = 9; break;
    case Key::Backspace: code = 127; break;
    case Key::Escape: code = 27; break;
    default: return std::nullopt;
    }
    const bool all = modes.kittyFlags & KittyReportAllKeys;
    if (!all && !(modes.kittyFlags & KittyDisambiguate)) {
        return std::nullopt;
    }
    if (all || key == Key::Escape || modifiers != NoModifier) {
        return kittyKey(code, modifiers);
    }
    return std::nullopt;
}

} // namespace

std::string encodeKey(Key key, unsigned modifiers, const KeyboardModes& modes) {
    const bool app = modes.cursorKeysApplication;
    if (auto kitty = kittyTextKey(key, modifiers, modes)) {
        return *kitty;
    }
    switch (key) {
    case Key::Enter: return withAlt(modes.newLineMode ? "\r\n" : "\r", modifiers);
    case Key::Tab: return (modifiers & ShiftModifier) ? std::string("\x1b[Z") : withAlt("\t", modifiers);
    case Key::Backspace: return withAlt((modifiers & ControlModifier) ? "\x08" : "\x7f", modifiers);
    case Key::Escape: return withAlt("\x1b", modifiers);
    case Key::Up: return letterKey('A', modifiers, app);
    case Key::Down: return letterKey('B', modifiers, app);
    case Key::Right: return letterKey('C', modifiers, app);
    case Key::Left: return letterKey('D', modifiers, app);
    case Key::Home: return letterKey('H', modifiers, app);
    case Key::End: return letterKey('F', modifiers, app);
    case Key::F1: return letterKey('P', modifiers, true);
    case Key::F2: return letterKey('Q', modifiers, true);
    // CSI R would be read as a cursor position report, so the kitty protocol uses CSI 13 ~.
    case Key::F3: return modes.kittyFlags ? tildeKey(13, modifiers) : letterKey('R', modifiers, true);
    case Key::F4: return letterKey('S', modifiers, true);
    case Key::Insert: return tildeKey(2, modifiers);
    case Key::Delete: return tildeKey(3, modifiers);
    case Key::PageUp: return tildeKey(5, modifiers);
    case Key::PageDown: return tildeKey(6, modifiers);
    case Key::F5: return tildeKey(15, modifiers);
    case Key::F6: return tildeKey(17, modifiers);
    case Key::F7: return tildeKey(18, modifiers);
    case Key::F8: return tildeKey(19, modifiers);
    case Key::F9: return tildeKey(20, modifiers);
    case Key::F10: return tildeKey(21, modifiers);
    case Key::F11: return tildeKey(23, modifiers);
    case Key::F12: return tildeKey(24, modifiers);
    case Key::Keypad0:
    case Key::Keypad1:
    case Key::Keypad2:
    case Key::Keypad3:
    case Key::Keypad4:
    case Key::Keypad5:
    case Key::Keypad6:
    case Key::Keypad7:
    case Key::Keypad8:
    case Key::Keypad9:
    case Key::KeypadDecimal:
    case Key::KeypadEnter:
    case Key::KeypadPlus:
    case Key::KeypadMinus:
    case Key::KeypadMultiply:
    case Key::KeypadDivide:
    case Key::KeypadEqual: return keypadKey(key, modifiers, modes);
    }
    return {};
}

std::string encodeChar(char32_t ch, unsigned modifiers) {
    std::string bytes;
    if (modifiers & ControlModifier) {
        if (ch >= U'a' && ch <= U'z') {
            ch = ch - U'a' + 1;
        } else if (ch >= U'@' && ch <= U'_') { // @ A-Z [ \ ] ^ _
            ch = ch - U'@';
        } else if (ch == U' ' || ch == U'2') {
            ch = 0;
        } else if (ch == U'?' || ch == U'8') {
            ch = 0x7F;
        } else if (ch >= U'3' && ch <= U'7') { // xterm: ^3 = ESC ... ^7 = ^_
            ch = ch - U'3' + 0x1B;
        } else if (ch == U'/') {
            ch = 0x1F;
        }
    }
    core::appendUtf8(bytes, ch);
    return withAlt(std::move(bytes), modifiers);
}

std::string encodeCharKey(char32_t ch, unsigned modifiers, const KeyboardModes& modes) {
    const unsigned chordMods = modifiers & (ControlModifier | AltModifier | MetaModifier);
    // Kitty reports the unshifted key; for letters that is the lower-case form.
    const char32_t base = (ch >= U'A' && ch <= U'Z') ? ch - U'A' + U'a' : ch;
    if ((modes.kittyFlags & KittyReportAllKeys) ||
        ((modes.kittyFlags & KittyDisambiguate) && chordMods != NoModifier)) {
        return kittyKey(static_cast<unsigned>(base), modifiers);
    }
    if (modes.modifyOtherKeys > 0 && chordMods != NoModifier && ch >= 0x20 && ch != 0x7F) {
        // Level 1 only rewrites Control chords without a C0 equivalent (e.g. Ctrl+, or
        // Ctrl+Shift+A); level 2 rewrites every chord.
        bool rewrite = modes.modifyOtherKeys >= 2;
        if (!rewrite && (modifiers & ControlModifier)) {
            const std::string control = encodeChar(ch, ControlModifier);
            const auto first = static_cast<unsigned char>(control.front());
            const bool isControlCode = control.size() == 1 && (first < 0x20 || first == 0x7F);
            const bool shiftedLetter = (modifiers & ShiftModifier) && ((ch >= U'a' && ch <= U'z') || (ch >= U'A' && ch <= U'Z'));
            rewrite = !isControlCode || shiftedLetter;
        }
        if (rewrite) {
            return "\x1b[27;" + std::to_string(modifierParameter(modifiers)) + ";" +
                   std::to_string(static_cast<unsigned>(ch)) + "~";
        }
    }
    return encodeChar(ch, modifiers & ~(ShiftModifier | MetaModifier));
}

std::string encodePaste(std::string_view text, bool bracketed) {
    static constexpr std::string_view kEnd = "\x1b[201~";
    std::string out;
    out.reserve(text.size() + 12);
    if (bracketed) {
        out += "\x1b[200~";
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (bracketed && text.compare(i, kEnd.size(), kEnd) == 0) {
            i += kEnd.size() - 1; // drop embedded end markers so the paste cannot escape
            continue;
        }
        if (bracketed && c == '\x1b') {
            // No other escape characters either, so an end marker cannot be reassembled out of
            // fragments that only join up once a marker was removed ("\e[20\e[201~1~").
            continue;
        }
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue; // CRLF -> CR
        }
        out.push_back(c == '\n' ? '\r' : c);
    }
    if (bracketed) {
        out += kEnd;
    }
    return out;
}

} // namespace wt::terminal

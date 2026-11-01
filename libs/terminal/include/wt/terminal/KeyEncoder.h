#pragma once

#include <string>
#include <string_view>

namespace wt::terminal {

enum class Key {
    Enter, Tab, Backspace, Escape,
    Up, Down, Left, Right,
    Home, End, Insert, Delete, PageUp, PageDown,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    // Numeric keypad (with NumLock on). They send plain characters unless DECKPAM is active.
    Keypad0, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadDecimal, KeypadEnter, KeypadPlus, KeypadMinus, KeypadMultiply, KeypadDivide, KeypadEqual,
};

enum Modifier : unsigned {
    NoModifier = 0,
    ShiftModifier = 1u << 0,
    AltModifier = 1u << 1,
    ControlModifier = 1u << 2,
    MetaModifier = 1u << 3,
};

// Kitty keyboard protocol flags (https://sw.kovidgoyal.net/kitty/keyboard-protocol/).
enum KittyKeyboardFlag : unsigned {
    KittyDisambiguate = 1u << 0,     // escape codes for ambiguous keys and chords
    KittyReportEvents = 1u << 1,     // repeat and release events
    KittyReportAlternates = 1u << 2, // the shifted key next to the base key
    KittyReportAllKeys = 1u << 3,    // every key (also text and modifier keys) as an escape code
    KittyReportText = 1u << 4,       // the text a key produces (with ReportAllKeys)
};

enum class KeyAction { Press, Repeat, Release };

// Modifier keys pressed on their own (reported with KittyReportAllKeys only).
enum class ModifierKey { Shift, Control, Alt, Super, CapsLock, NumLock };

// What a character key produced, for the kitty protocol's alternates and text fields.
struct KeyEventInfo {
    KeyAction action = KeyAction::Press;
    char32_t shifted = 0;  // the shifted form of the key (e.g. 'A' for Shift+a), 0 if none
    std::u32string text;   // text the key generated (empty for releases and chords)
};

struct KeyboardModes {
    bool cursorKeysApplication = false; // DECCKM
    bool newLineMode = false;           // LNM: Enter sends CR LF
    bool keypadApplication = false;     // DECKPAM: keypad sends ESC O <code>
    unsigned kittyFlags = 0;            // CSI > flags u
    int modifyOtherKeys = 0;            // XTMODKEYS level (CSI > 4 ; n m)
};

// Translates keyboard input into the byte sequences xterm sends to the application.
// Modifier encoding follows xterm's "CSI 1 ; <1 + mods> X" convention. Repeats are sent like
// presses and releases are dropped unless the kitty "report events" flag asks for them.
std::string encodeKey(Key key, unsigned modifiers, const KeyboardModes& modes, KeyAction action = KeyAction::Press);

// Encodes a typed character. Control maps letters and @[\]^_? to C0 codes; Alt prefixes ESC.
std::string encodeChar(char32_t ch, unsigned modifiers);

// Encodes a character key honouring the kitty keyboard protocol and xterm's modifyOtherKeys
// (CSI 27 ; mods ; code ~); falls back to encodeChar().
std::string encodeCharKey(char32_t ch, unsigned modifiers, const KeyboardModes& modes, const KeyEventInfo& event = {});

// A modifier key on its own: an escape code under KittyReportAllKeys, nothing otherwise.
// `modifiers` is the state after the event (Shift is set while Shift is held, clear on release).
std::string encodeModifierKey(ModifierKey key, unsigned modifiers, const KeyboardModes& modes, KeyAction action);

// Prepares clipboard text for the PTY: newlines become CR (like typing Enter) and, in bracketed
// paste mode, the text is wrapped in ESC[200~ ... ESC[201~ with embedded end markers removed.
std::string encodePaste(std::string_view text, bool bracketed);

} // namespace wt::terminal

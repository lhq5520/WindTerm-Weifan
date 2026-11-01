#pragma once

#include "wt/core/Json.h"
#include "wt/session/Keymap.h"
#include "wt/session/SessionLog.h"
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

enum class TriggerAction { Highlight, Notify, Beep, Send };

// A rule applied to every line of terminal output (and to the line being written, so prompts
// can trigger it): highlight the matches, notify, beep, or send text back (auto-answer).
struct TriggerRule {
    std::string name;
    std::string pattern; // regular expression (PCRE syntax)
    bool caseSensitive = false;
    bool enabled = true;
    TriggerAction action = TriggerAction::Highlight;
    std::string foreground; // Highlight: "#rrggbb", empty = unchanged
    std::string background; // Highlight: "#rrggbb", empty = unchanged
    std::string text;       // Send: text with \r \n \t \e \\ escapes; Notify: message (empty: the line)

    friend bool operator==(const TriggerRule&, const TriggerRule&) = default;
};

// "\\r" -> CR, "\\n" -> LF, "\\t" -> tab, "\\e" -> ESC, "\\\\" -> backslash.
std::string unescapeTriggerText(std::string_view text);

struct LoggingSettings {
    bool autoStart = false;   // log every session from its start
    std::string directory;    // empty: <Documents>/WindTerm-Weifan/Logs
    std::string fileName = "%Y-%m-%d_%H-%M-%S_%N.log"; // see expandLogFileName()
    LogFormat format = LogFormat::Text;
    bool timestamps = true; // text logs: prefix every line with the time

    friend bool operator==(const LoggingSettings&, const LoggingSettings&) = default;
};

enum class CredentialStoreKind { Vault, System };

struct SecuritySettings {
    // Where remembered passwords go: the encrypted vault (master password) or the operating
    // system's store (macOS Keychain, Windows Credential Manager, Secret Service).
    CredentialStoreKind credentialStore = CredentialStoreKind::Vault;
    int vaultLockMinutes = 30; // lock the vault after this much inactivity; 0 = never

    friend bool operator==(const SecuritySettings&, const SecuritySettings&) = default;
};

struct CompletionSettings {
    bool enabled = true;   // suggest commands from the history while typing at a prompt
    int minimumPrefix = 2; // characters typed before suggestions appear

    friend bool operator==(const CompletionSettings&, const CompletionSettings&) = default;
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
    LoggingSettings logging;
    std::vector<TriggerRule> triggers;
    CompletionSettings completion;
    SecuritySettings security;
    bool restoreWorkspace = true; // reopen the last window layout and sessions on start

    core::Json toJson() const;
    // Unknown keys are ignored and out-of-range values clamped; `warnings` lists what was fixed.
    static Settings fromJson(const core::Json& json, std::vector<std::string>* warnings = nullptr);

    // The active theme (falls back to the default theme if the name is unknown).
    Theme activeTheme() const;

    friend bool operator==(const Settings&, const Settings&) = default;
};

} // namespace wt::session

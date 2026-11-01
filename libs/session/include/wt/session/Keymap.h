#pragma once

#include "wt/core/Json.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wt::session {

// Application commands that can be bound to keys.
enum class Action {
    NewTab,
    CloseTab,
    NextTab,
    PreviousTab,
    SplitRight,
    SplitDown,
    ClosePane,
    FocusNextPane,
    FocusPreviousPane,
    Find,
    FindNext,
    FindPrevious,
    Copy,
    Paste,
    SelectAll,
    ClearScrollback,
    ZoomIn,
    ZoomOut,
    ZoomReset,
    NewSshSession,
    SessionManager,
    Settings,
    ToggleFileBrowser,
    Reconnect,
    NewTelnetSession,
    NewSerialSession,
    SendBreak,
    PortForwarding,
    Count_ // number of actions; not an action
};

struct ActionInfo {
    Action action;
    std::string_view id;    // stable identifier used in settings files, e.g. "new-tab"
    std::string_view title; // English menu text
};

const std::vector<ActionInfo>& allActions();
std::optional<Action> actionFromId(std::string_view id);
std::string_view actionId(Action action);

enum class KeymapFlavor { Standard, Mac };
KeymapFlavor nativeKeymapFlavor();

// Key sequences use Qt's portable text format ("Ctrl+Shift+T"; on macOS "Ctrl" is Command and
// "Meta" is the Control key). Several sequences per action are allowed; an empty list unbinds it.
class Keymap {
public:
    static Keymap defaults(KeymapFlavor flavor = nativeKeymapFlavor());

    const std::vector<std::string>& sequences(Action action) const;
    void setSequences(Action action, std::vector<std::string> sequences);

    // Sequences bound to more than one action: (sequence, actions).
    std::vector<std::pair<std::string, std::vector<Action>>> conflicts() const;

    // Only actions that differ from `base` are written; reading applies them on top of `base`.
    core::Json toJson(const Keymap& base) const;
    static Keymap fromJson(const core::Json& json, const Keymap& base, std::vector<std::string>* warnings = nullptr);

    friend bool operator==(const Keymap&, const Keymap&) = default;

private:
    std::vector<std::vector<std::string>> m_bindings = std::vector<std::vector<std::string>>(
        static_cast<std::size_t>(Action::Count_));
};

} // namespace wt::session

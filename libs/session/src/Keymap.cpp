#include "wt/session/Keymap.h"

#include <algorithm>
#include <map>

namespace wt::session {

const std::vector<ActionInfo>& allActions() {
    static const std::vector<ActionInfo> actions = {
        {Action::NewTab, "new-tab", "New Tab"},
        {Action::CloseTab, "close-tab", "Close Tab"},
        {Action::NextTab, "next-tab", "Next Tab"},
        {Action::PreviousTab, "previous-tab", "Previous Tab"},
        {Action::SplitRight, "split-right", "Split Right"},
        {Action::SplitDown, "split-down", "Split Down"},
        {Action::ClosePane, "close-pane", "Close Pane"},
        {Action::FocusNextPane, "focus-next-pane", "Next Pane"},
        {Action::FocusPreviousPane, "focus-previous-pane", "Previous Pane"},
        {Action::Find, "find", "Find..."},
        {Action::FindNext, "find-next", "Find Next"},
        {Action::FindPrevious, "find-previous", "Find Previous"},
        {Action::Copy, "copy", "Copy"},
        {Action::Paste, "paste", "Paste"},
        {Action::SelectAll, "select-all", "Select All"},
        {Action::ClearScrollback, "clear-scrollback", "Clear Scrollback"},
        {Action::ZoomIn, "zoom-in", "Zoom In"},
        {Action::ZoomOut, "zoom-out", "Zoom Out"},
        {Action::ZoomReset, "zoom-reset", "Actual Size"},
        {Action::NewSshSession, "new-ssh-session", "New SSH Session..."},
        {Action::SessionManager, "session-manager", "Session Manager..."},
        {Action::Settings, "settings", "Settings..."},
        {Action::ToggleFileBrowser, "toggle-file-browser", "File Browser (SFTP)"},
        {Action::Reconnect, "reconnect", "Reconnect"},
        {Action::NewTelnetSession, "new-telnet-session", "New Telnet Session..."},
        {Action::NewSerialSession, "new-serial-session", "New Serial Session..."},
        {Action::SendBreak, "send-break", "Send Break"},
        {Action::PortForwarding, "port-forwarding", "Port Forwarding..."},
    };
    return actions;
}

std::optional<Action> actionFromId(std::string_view id) {
    for (const ActionInfo& info : allActions()) {
        if (info.id == id) {
            return info.action;
        }
    }
    return std::nullopt;
}

std::string_view actionId(Action action) {
    for (const ActionInfo& info : allActions()) {
        if (info.action == action) {
            return info.id;
        }
    }
    return {};
}

KeymapFlavor nativeKeymapFlavor() {
#ifdef __APPLE__
    return KeymapFlavor::Mac;
#else
    return KeymapFlavor::Standard;
#endif
}

Keymap Keymap::defaults(KeymapFlavor flavor) {
    Keymap map;
    auto bind = [&map](Action action, std::vector<std::string> sequences) {
        map.setSequences(action, std::move(sequences));
    };
    if (flavor == KeymapFlavor::Mac) {
        // Qt's "Ctrl" is the Command key on macOS; shortcuts follow Terminal.app / iTerm2.
        bind(Action::NewTab, {"Ctrl+T"});
        bind(Action::CloseTab, {"Ctrl+Shift+W"});
        bind(Action::NextTab, {"Ctrl+Shift+]"});
        bind(Action::PreviousTab, {"Ctrl+Shift+["});
        bind(Action::SplitRight, {"Ctrl+D"});
        bind(Action::SplitDown, {"Ctrl+Shift+D"});
        bind(Action::ClosePane, {"Ctrl+W"});
        bind(Action::FocusNextPane, {"Ctrl+]"});
        bind(Action::FocusPreviousPane, {"Ctrl+["});
        bind(Action::Find, {"Ctrl+F"});
        bind(Action::FindNext, {"Ctrl+G"});
        bind(Action::FindPrevious, {"Ctrl+Shift+G"});
        bind(Action::Copy, {"Ctrl+C"});
        bind(Action::Paste, {"Ctrl+V"});
        bind(Action::SelectAll, {"Ctrl+A"});
        bind(Action::ClearScrollback, {"Ctrl+K"});
        bind(Action::ZoomIn, {"Ctrl+=", "Ctrl++"});
        bind(Action::ZoomOut, {"Ctrl+-"});
        bind(Action::ZoomReset, {"Ctrl+0"});
        bind(Action::NewSshSession, {"Ctrl+Shift+N"});
        bind(Action::SessionManager, {"Ctrl+Shift+O"});
        bind(Action::Settings, {"Ctrl+,"});
        bind(Action::ToggleFileBrowser, {"Ctrl+Shift+B"});
        bind(Action::Reconnect, {"Ctrl+Shift+R"});
    } else {
        // Control+Shift chords leave plain Control keys to the terminal application.
        bind(Action::NewTab, {"Ctrl+Shift+T"});
        bind(Action::CloseTab, {"Ctrl+Shift+Q"});
        bind(Action::NextTab, {"Ctrl+Tab", "Ctrl+PgDown"});
        bind(Action::PreviousTab, {"Ctrl+Shift+Tab", "Ctrl+PgUp"});
        bind(Action::SplitRight, {"Ctrl+Shift+E"});
        bind(Action::SplitDown, {"Ctrl+Shift+O"});
        bind(Action::ClosePane, {"Ctrl+Shift+W"});
        bind(Action::FocusNextPane, {"Ctrl+Shift+Right"});
        bind(Action::FocusPreviousPane, {"Ctrl+Shift+Left"});
        bind(Action::Find, {"Ctrl+Shift+F"});
        bind(Action::FindNext, {"F3"});
        bind(Action::FindPrevious, {"Shift+F3"});
        bind(Action::Copy, {"Ctrl+Shift+C", "Ctrl+Ins"});
        bind(Action::Paste, {"Ctrl+Shift+V", "Shift+Ins"});
        bind(Action::SelectAll, {"Ctrl+Shift+A"});
        bind(Action::ClearScrollback, {"Ctrl+Shift+K"});
        bind(Action::ZoomIn, {"Ctrl+=", "Ctrl++"});
        bind(Action::ZoomOut, {"Ctrl+-"});
        bind(Action::ZoomReset, {"Ctrl+0"});
        bind(Action::NewSshSession, {"Ctrl+Shift+N"});
        bind(Action::SessionManager, {"Ctrl+Shift+M"});
        bind(Action::Settings, {"Ctrl+,"});
        bind(Action::ToggleFileBrowser, {"Ctrl+Shift+B"});
        bind(Action::Reconnect, {"Ctrl+Shift+R"});
    }
    return map;
}

const std::vector<std::string>& Keymap::sequences(Action action) const {
    return m_bindings[static_cast<std::size_t>(action)];
}

void Keymap::setSequences(Action action, std::vector<std::string> sequences) {
    sequences.erase(std::remove(sequences.begin(), sequences.end(), std::string()), sequences.end());
    m_bindings[static_cast<std::size_t>(action)] = std::move(sequences);
}

std::vector<std::pair<std::string, std::vector<Action>>> Keymap::conflicts() const {
    std::map<std::string, std::vector<Action>> owners;
    for (std::size_t i = 0; i < m_bindings.size(); ++i) {
        for (const std::string& sequence : m_bindings[i]) {
            auto& list = owners[sequence];
            if (std::find(list.begin(), list.end(), static_cast<Action>(i)) == list.end()) {
                list.push_back(static_cast<Action>(i));
            }
        }
    }
    std::vector<std::pair<std::string, std::vector<Action>>> result;
    for (auto& [sequence, actions] : owners) {
        if (actions.size() > 1) {
            result.emplace_back(sequence, std::move(actions));
        }
    }
    return result;
}

core::Json Keymap::toJson(const Keymap& base) const {
    core::Json json = core::Json::object();
    for (const ActionInfo& info : allActions()) {
        if (sequences(info.action) == base.sequences(info.action)) {
            continue;
        }
        core::Json list = core::Json::array();
        for (const std::string& sequence : sequences(info.action)) {
            list.push(sequence);
        }
        json[info.id] = std::move(list);
    }
    return json;
}

Keymap Keymap::fromJson(const core::Json& json, const Keymap& base, std::vector<std::string>* warnings) {
    Keymap map = base;
    for (const auto& [id, value] : json.fields()) {
        const auto action = actionFromId(id);
        if (!action) {
            if (warnings) {
                warnings->push_back("unknown action \"" + id + "\" in keybindings");
            }
            continue;
        }
        std::vector<std::string> sequences;
        if (value.isString()) {
            sequences.push_back(value.toString());
        } else {
            for (const core::Json& item : value.items()) {
                if (item.isString()) {
                    sequences.push_back(item.toString());
                }
            }
        }
        map.setSequences(*action, std::move(sequences));
    }
    return map;
}

} // namespace wt::session

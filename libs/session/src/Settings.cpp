#include "wt/session/Settings.h"

#include <algorithm>
#include <cmath>

namespace wt::session {
namespace {

template <typename Enum>
struct EnumName {
    Enum value;
    const char* name;
};

constexpr EnumName<CursorStyle> kCursorStyles[] = {
    {CursorStyle::Block, "block"}, {CursorStyle::Underline, "underline"}, {CursorStyle::Bar, "bar"}};
constexpr EnumName<BellStyle> kBellStyles[] = {
    {BellStyle::None, "none"}, {BellStyle::Visual, "visual"}, {BellStyle::Audible, "audible"}};

template <typename Enum, std::size_t N>
const char* enumToString(const EnumName<Enum> (&table)[N], Enum value) {
    for (const auto& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return table[0].name;
}

template <typename Enum, std::size_t N>
Enum enumFromJson(const EnumName<Enum> (&table)[N], const core::Json& json, Enum fallback, const char* key,
                  std::vector<std::string>* warnings) {
    if (json.isNull()) {
        return fallback;
    }
    const std::string text = json.toString();
    for (const auto& entry : table) {
        if (text == entry.name) {
            return entry.value;
        }
    }
    if (warnings) {
        warnings->push_back(std::string("invalid value for ") + key);
    }
    return fallback;
}

} // namespace

core::Json Settings::toJson() const {
    core::Json json;
    json["version"] = kVersion;
    core::Json& t = json["terminal"];
    t["fontFamily"] = terminal.fontFamily;
    t["fontSize"] = terminal.fontSize;
    t["theme"] = terminal.theme;
    t["scrollbackLines"] = terminal.scrollbackLines;
    t["cursorStyle"] = enumToString(kCursorStyles, terminal.cursorStyle);
    t["cursorBlink"] = terminal.cursorBlink;
    t["optionAsMeta"] = terminal.optionAsMeta;
    t["copyOnSelect"] = terminal.copyOnSelect;
    t["allowClipboardWrite"] = terminal.allowClipboardWrite;
    t["bell"] = enumToString(kBellStyles, terminal.bell);
    json["keybindings"] = keymap.toJson(Keymap::defaults());
    core::Json themes = core::Json::array();
    for (const Theme& theme : customThemes) {
        themes.push(themeToJson(theme));
    }
    json["customThemes"] = std::move(themes);
    json["confirmCloseRunning"] = confirmCloseRunning;
    json["knownHostsFile"] = knownHostsFile;
    return json;
}

Settings Settings::fromJson(const core::Json& json, std::vector<std::string>* warnings) {
    Settings s;
    const core::Json& t = json["terminal"];
    s.terminal.fontFamily = t["fontFamily"].toString();
    s.terminal.fontSize = t["fontSize"].toDouble(0);
    if (!std::isfinite(s.terminal.fontSize) || s.terminal.fontSize < 0 || s.terminal.fontSize > 200) {
        if (warnings) {
            warnings->push_back("fontSize out of range");
        }
        s.terminal.fontSize = 0;
    }
    s.terminal.theme = t["theme"].toString(kDefaultThemeName);
    const auto scrollback = t["scrollbackLines"].toInt(s.terminal.scrollbackLines);
    s.terminal.scrollbackLines = static_cast<int>(std::clamp<std::int64_t>(scrollback, kMinScrollback, kMaxScrollback));
    if (s.terminal.scrollbackLines != scrollback && warnings) {
        warnings->push_back("scrollbackLines clamped");
    }
    s.terminal.cursorStyle = enumFromJson(kCursorStyles, t["cursorStyle"], CursorStyle::Block, "cursorStyle", warnings);
    s.terminal.cursorBlink = t["cursorBlink"].toBool(false);
    s.terminal.optionAsMeta = t["optionAsMeta"].toBool(false);
    s.terminal.copyOnSelect = t["copyOnSelect"].toBool(false);
    s.terminal.allowClipboardWrite = t["allowClipboardWrite"].toBool(false);
    s.terminal.bell = enumFromJson(kBellStyles, t["bell"], BellStyle::Visual, "bell", warnings);
    s.keymap = Keymap::fromJson(json["keybindings"], Keymap::defaults(), warnings);
    for (const core::Json& item : json["customThemes"].items()) {
        std::string error;
        if (auto theme = themeFromJson(item, &error)) {
            s.customThemes.push_back(std::move(*theme));
        } else if (warnings) {
            warnings->push_back(error);
        }
    }
    s.confirmCloseRunning = json["confirmCloseRunning"].toBool(true);
    s.knownHostsFile = json["knownHostsFile"].toString();
    if (!findTheme(s.terminal.theme, s.customThemes)) {
        if (warnings) {
            warnings->push_back("unknown theme \"" + s.terminal.theme + "\"");
        }
        s.terminal.theme = std::string(kDefaultThemeName);
    }
    return s;
}

Theme Settings::activeTheme() const {
    if (auto theme = findTheme(terminal.theme, customThemes)) {
        return *theme;
    }
    return builtinThemes().front();
}

} // namespace wt::session

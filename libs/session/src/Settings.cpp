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
constexpr EnumName<LogFormat> kLogFormats[] = {{LogFormat::Text, "text"}, {LogFormat::Raw, "raw"}};
constexpr EnumName<TriggerAction> kTriggerActions[] = {{TriggerAction::Highlight, "highlight"},
                                                       {TriggerAction::Notify, "notify"},
                                                       {TriggerAction::Beep, "beep"},
                                                       {TriggerAction::Send, "send"}};

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
    core::Json& l = json["logging"];
    l["autoStart"] = logging.autoStart;
    l["directory"] = logging.directory;
    l["fileName"] = logging.fileName;
    l["format"] = enumToString(kLogFormats, logging.format);
    l["timestamps"] = logging.timestamps;
    core::Json rules = core::Json::array();
    for (const TriggerRule& rule : triggers) {
        core::Json r;
        r["name"] = rule.name;
        r["pattern"] = rule.pattern;
        r["caseSensitive"] = rule.caseSensitive;
        r["enabled"] = rule.enabled;
        r["action"] = enumToString(kTriggerActions, rule.action);
        r["foreground"] = rule.foreground;
        r["background"] = rule.background;
        r["text"] = rule.text;
        rules.push(std::move(r));
    }
    json["triggers"] = std::move(rules);
    core::Json& c = json["completion"];
    c["enabled"] = completion.enabled;
    c["minimumPrefix"] = completion.minimumPrefix;
    return json;
}

std::string unescapeTriggerText(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i];
            continue;
        }
        switch (text[++i]) {
        case 'r': out += '\r'; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'e': out += '\x1b'; break;
        case '\\': out += '\\'; break;
        default:
            out += '\\';
            out += text[i];
            break;
        }
    }
    return out;
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
    const core::Json& l = json["logging"];
    s.logging.autoStart = l["autoStart"].toBool(false);
    s.logging.directory = l["directory"].toString();
    s.logging.fileName = l["fileName"].toString(LoggingSettings{}.fileName);
    if (s.logging.fileName.empty()) {
        s.logging.fileName = LoggingSettings{}.fileName;
    }
    s.logging.format = enumFromJson(kLogFormats, l["format"], LogFormat::Text, "logging.format", warnings);
    s.logging.timestamps = l["timestamps"].toBool(true);
    for (const core::Json& r : json["triggers"].items()) {
        TriggerRule rule;
        rule.name = r["name"].toString();
        rule.pattern = r["pattern"].toString();
        if (rule.pattern.empty()) {
            if (warnings) {
                warnings->push_back("trigger \"" + rule.name + "\" has no pattern");
            }
            continue;
        }
        rule.caseSensitive = r["caseSensitive"].toBool(false);
        rule.enabled = r["enabled"].toBool(true);
        rule.action = enumFromJson(kTriggerActions, r["action"], TriggerAction::Highlight, "trigger action", warnings);
        rule.foreground = r["foreground"].toString();
        rule.background = r["background"].toString();
        for (std::string* color : {&rule.foreground, &rule.background}) {
            if (!color->empty() && !parseHexColor(*color)) {
                if (warnings) {
                    warnings->push_back("trigger \"" + rule.name + "\": invalid colour " + *color);
                }
                color->clear();
            }
        }
        rule.text = r["text"].toString();
        s.triggers.push_back(std::move(rule));
    }
    const core::Json& c = json["completion"];
    s.completion.enabled = c["enabled"].toBool(true);
    s.completion.minimumPrefix = static_cast<int>(std::clamp<std::int64_t>(c["minimumPrefix"].toInt(2), 1, 20));
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

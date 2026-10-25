#include "wt/session/Theme.h"

#include <array>
#include <charconv>

namespace wt::session {
namespace {

Theme makeTheme(std::string name, bool dark, std::uint32_t foreground, std::uint32_t background,
                std::uint32_t cursor, std::uint32_t selection, const std::array<std::uint32_t, 16>& ansi) {
    Theme theme;
    theme.name = std::move(name);
    theme.dark = dark;
    theme.palette = terminal::ColorPalette::xterm();
    for (std::size_t i = 0; i < ansi.size(); ++i) {
        theme.palette.indexed[i] = ansi[i];
    }
    theme.palette.foreground = foreground;
    theme.palette.background = background;
    theme.palette.cursor = cursor;
    theme.selectionBackground = selection;
    return theme;
}

std::vector<Theme> createBuiltins() {
    std::vector<Theme> themes;
    {
        Theme windterm;
        windterm.name = std::string(kDefaultThemeName);
        themes.push_back(windterm);
    }
    themes.push_back(makeTheme("Campbell", true, 0xCCCCCC, 0x0C0C0C, 0xFFFFFF, 0x264F78,
                               {0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
                                0x767676, 0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2}));
    themes.push_back(makeTheme("Solarized Dark", true, 0x839496, 0x002B36, 0x93A1A1, 0x274642,
                               {0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5,
                                0x002B36, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3}));
    themes.push_back(makeTheme("Solarized Light", false, 0x657B83, 0xFDF6E3, 0x586E75, 0xEEE8D5,
                               {0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5,
                                0x002B36, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3}));
    themes.push_back(makeTheme("Dracula", true, 0xF8F8F2, 0x282A36, 0xF8F8F2, 0x44475A,
                               {0x21222C, 0xFF5555, 0x50FA7B, 0xF1FA8C, 0xBD93F9, 0xFF79C6, 0x8BE9FD, 0xF8F8F2,
                                0x6272A4, 0xFF6E6E, 0x69FF94, 0xFFFFA5, 0xD6ACFF, 0xFF92DF, 0xA4FFFF, 0xFFFFFF}));
    themes.push_back(makeTheme("Monokai", true, 0xF8F8F2, 0x272822, 0xF8F8F0, 0x49483E,
                               {0x272822, 0xF92672, 0xA6E22E, 0xF4BF75, 0x66D9EF, 0xAE81FF, 0xA1EFE4, 0xF8F8F2,
                                0x75715E, 0xF92672, 0xA6E22E, 0xF4BF75, 0x66D9EF, 0xAE81FF, 0xA1EFE4, 0xF9F8F5}));
    themes.push_back(makeTheme("Gruvbox Dark", true, 0xEBDBB2, 0x282828, 0xEBDBB2, 0x504945,
                               {0x282828, 0xCC241D, 0x98971A, 0xD79921, 0x458588, 0xB16286, 0x689D6A, 0xA89984,
                                0x928374, 0xFB4934, 0xB8BB26, 0xFABD2F, 0x83A598, 0xD3869B, 0x8EC07C, 0xEBDBB2}));
    themes.push_back(makeTheme("One Dark", true, 0xABB2BF, 0x282C34, 0x528BFF, 0x3E4451,
                               {0x282C34, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xABB2BF,
                                0x5C6370, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xFFFFFF}));
    themes.push_back(makeTheme("Nord", true, 0xD8DEE9, 0x2E3440, 0xD8DEE9, 0x434C5E,
                               {0x3B4252, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x88C0D0, 0xE5E9F0,
                                0x4C566A, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x8FBCBB, 0xECEFF4}));
    themes.push_back(makeTheme("Tango Light", false, 0x2E3436, 0xFFFFFF, 0x2E3436, 0xB5D5FF,
                               {0x000000, 0xCC0000, 0x4E9A06, 0xC4A000, 0x3465A4, 0x75507B, 0x06989A, 0xD3D7CF,
                                0x555753, 0xEF2929, 0x8AE234, 0xFCE94F, 0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEC}));
    themes.push_back(makeTheme("xterm", true, 0xE5E5E5, 0x000000, 0xE5E5E5, 0x264F78,
                               {0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
                                0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF}));
    return themes;
}

} // namespace

const std::vector<Theme>& builtinThemes() {
    static const std::vector<Theme> themes = createBuiltins();
    return themes;
}

std::optional<Theme> findTheme(std::string_view name, const std::vector<Theme>& custom) {
    for (const Theme& theme : custom) {
        if (theme.name == name) {
            return theme;
        }
    }
    for (const Theme& theme : builtinThemes()) {
        if (theme.name == name) {
            return theme;
        }
    }
    return std::nullopt;
}

std::string formatHexColor(std::uint32_t rgb) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "#";
    for (int shift = 20; shift >= 0; shift -= 4) {
        out.push_back(kDigits[(rgb >> shift) & 0xF]);
    }
    return out;
}

std::optional<std::uint32_t> parseHexColor(std::string_view text) {
    if (!text.empty() && text[0] == '#') {
        text.remove_prefix(1);
    }
    if (text.size() != 6) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

core::Json themeToJson(const Theme& theme) {
    core::Json json;
    json["name"] = theme.name;
    json["dark"] = theme.dark;
    json["foreground"] = formatHexColor(theme.palette.foreground);
    json["background"] = formatHexColor(theme.palette.background);
    json["cursor"] = formatHexColor(theme.palette.cursor);
    json["selection"] = formatHexColor(theme.selectionBackground);
    core::Json ansi = core::Json::array();
    for (std::size_t i = 0; i < 16; ++i) {
        ansi.push(formatHexColor(theme.palette.indexed[i]));
    }
    json["ansi"] = std::move(ansi);
    return json;
}

std::optional<Theme> themeFromJson(const core::Json& json, std::string* error) {
    auto fail = [error](std::string message) -> std::optional<Theme> {
        if (error) {
            *error = std::move(message);
        }
        return std::nullopt;
    };
    Theme theme;
    theme.name = json["name"].toString();
    if (theme.name.empty()) {
        return fail("theme without a name");
    }
    theme.dark = json["dark"].toBool(true);
    struct Field {
        const char* key;
        std::uint32_t* target;
    };
    const Field fields[] = {{"foreground", &theme.palette.foreground},
                            {"background", &theme.palette.background},
                            {"cursor", &theme.palette.cursor},
                            {"selection", &theme.selectionBackground}};
    for (const Field& field : fields) {
        if (!json.contains(field.key)) {
            continue; // keep the default
        }
        const auto value = parseHexColor(json[field.key].toString());
        if (!value) {
            return fail("theme \"" + theme.name + "\": invalid colour for " + field.key);
        }
        *field.target = *value;
    }
    const auto& ansi = json["ansi"].items();
    if (!ansi.empty() && ansi.size() != 16) {
        return fail("theme \"" + theme.name + "\": \"ansi\" needs 16 colours");
    }
    for (std::size_t i = 0; i < ansi.size(); ++i) {
        const auto value = parseHexColor(ansi[i].toString());
        if (!value) {
            return fail("theme \"" + theme.name + "\": invalid ANSI colour " + std::to_string(i));
        }
        theme.palette.indexed[i] = *value;
    }
    return theme;
}

} // namespace wt::session

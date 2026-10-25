#pragma once

#include "wt/core/Json.h"
#include "wt/terminal/Cell.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wt::session {

// A colour scheme: the 16 ANSI colours (the 240 extended colours keep their xterm values), default
// foreground/background, cursor and selection colours.
struct Theme {
    std::string name;
    terminal::ColorPalette palette = terminal::ColorPalette::xterm();
    std::uint32_t selectionBackground = 0x264F78;
    bool dark = true;

    friend bool operator==(const Theme&, const Theme&) = default;
};

inline constexpr std::string_view kDefaultThemeName = "WindTerm Dark";

const std::vector<Theme>& builtinThemes();
// Looks `name` up in `custom` first, then in the built-in themes.
std::optional<Theme> findTheme(std::string_view name, const std::vector<Theme>& custom = {});

// JSON form: {"name", "foreground", "background", "cursor", "selection", "ansi": [16 x "#rrggbb"]}.
core::Json themeToJson(const Theme& theme);
std::optional<Theme> themeFromJson(const core::Json& json, std::string* error = nullptr);

std::string formatHexColor(std::uint32_t rgb);              // "#rrggbb"
std::optional<std::uint32_t> parseHexColor(std::string_view); // "#rrggbb" or "rrggbb"

} // namespace wt::session

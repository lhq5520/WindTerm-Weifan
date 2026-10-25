#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace wt::terminal {

struct Color {
    enum class Kind : std::uint8_t { Default, Indexed, Rgb };

    Kind kind = Kind::Default;
    std::uint8_t index = 0; // Indexed: 0-255
    std::uint8_t r = 0, g = 0, b = 0;

    static Color defaultColor() { return {}; }
    static Color indexed(std::uint8_t i) { return {Kind::Indexed, i, 0, 0, 0}; }
    static Color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) { return {Kind::Rgb, 0, r, g, b}; }

    bool isDefault() const { return kind == Kind::Default; }
    friend bool operator==(const Color&, const Color&) = default;
};

enum CellFlag : std::uint16_t {
    Bold = 1u << 0,
    Dim = 1u << 1,
    Italic = 1u << 2,
    Underline = 1u << 3, // any underline; the style is in the UnderlineStyle bits
    Blink = 1u << 4,
    Inverse = 1u << 5,
    Hidden = 1u << 6,
    Strikethrough = 1u << 7,
    Overline = 1u << 8,
    // Blank left at the end of a line when a wide character wrapped early. It is not part of the
    // text: copy and reflow skip it.
    WidePadding = 1u << 9,
};

// SGR 4:x underline styles, stored in CellAttributes::flags bits 12-14.
enum class UnderlineStyle : std::uint8_t { None = 0, Single, Double, Curly, Dotted, Dashed };

struct CellAttributes {
    Color foreground;
    Color background;
    Color underlineColor; // SGR 58/59; default means "use the foreground"
    std::uint16_t flags = 0;
    std::uint16_t hyperlink = 0; // OSC 8 link id (Terminal::hyperlinkUri), 0 = none

    static constexpr unsigned kUnderlineStyleShift = 12;
    static constexpr std::uint16_t kUnderlineStyleMask = 7u << kUnderlineStyleShift;

    bool has(CellFlag f) const { return (flags & f) != 0; }
    UnderlineStyle underlineStyle() const {
        if (!has(Underline)) {
            return UnderlineStyle::None;
        }
        const auto style = static_cast<UnderlineStyle>((flags & kUnderlineStyleMask) >> kUnderlineStyleShift);
        return style == UnderlineStyle::None ? UnderlineStyle::Single : style;
    }
    void setUnderlineStyle(UnderlineStyle style) {
        flags = static_cast<std::uint16_t>(flags & ~(kUnderlineStyleMask | Underline));
        if (style != UnderlineStyle::None) {
            flags = static_cast<std::uint16_t>(flags | Underline |
                                               (static_cast<unsigned>(style) << kUnderlineStyleShift));
        }
    }
    friend bool operator==(const CellAttributes&, const CellAttributes&) = default;
};

// Cell::ch values at or above this mark refer to a grapheme cluster (base character followed by
// combining marks) interned by the Terminal; resolve them with Terminal::cellText().
inline constexpr char32_t kClusterBase = 0x110000;

struct Cell {
    char32_t ch = U' ';
    CellAttributes attributes;
    // 1 for normal cells, 2 for the leading half of a wide character,
    // 0 for the trailing half (its `ch` is 0 and it should not be drawn).
    std::uint8_t width = 1;

    bool isCluster() const { return ch >= kClusterBase; }
    friend bool operator==(const Cell&, const Cell&) = default;
};

// DECDWL / DECDHL line sizes (ESC # 6, ESC # 3/4). Such lines show every cell twice as wide.
enum class LineSize : std::uint8_t { Normal, DoubleWidth, DoubleHeightTop, DoubleHeightBottom };

struct Line {
    std::vector<Cell> cells;
    // True if this line continues on the next one because of autowrap (needed for reflow/copy).
    bool wrapped = false;
    LineSize size = LineSize::Normal;
};

// xterm default 256-colour palette as 0xRRGGBB.
std::uint32_t paletteColor(std::uint8_t index);

// Colours an application can query and change at runtime (OSC 4/10/11/12 and their resets).
struct ColorPalette {
    std::array<std::uint32_t, 256> indexed{};
    std::uint32_t foreground = 0xD4D4D4;
    std::uint32_t background = 0x1E1E1E;
    std::uint32_t cursor = 0xAEAFAD;

    static ColorPalette xterm();
    // Resolves a cell colour; `bold` brightens the 8 base colours (xterm "bold is bright").
    std::uint32_t resolve(const Color& color, bool foreground, bool bold = false) const;
    friend bool operator==(const ColorPalette&, const ColorPalette&) = default;
};

} // namespace wt::terminal

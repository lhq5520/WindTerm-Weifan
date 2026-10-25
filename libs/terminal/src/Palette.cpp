#include "wt/terminal/Cell.h"

namespace wt::terminal {

std::uint32_t paletteColor(std::uint8_t index) {
    static constexpr std::uint32_t kAnsi[16] = {
        0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
        0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    };
    if (index < 16) {
        return kAnsi[index];
    }
    if (index < 232) { // 6x6x6 colour cube
        static constexpr std::uint32_t kLevels[6] = {0x00, 0x5F, 0x87, 0xAF, 0xD7, 0xFF};
        const int i = index - 16;
        return (kLevels[i / 36] << 16) | (kLevels[(i / 6) % 6] << 8) | kLevels[i % 6];
    }
    const std::uint32_t gray = 8 + 10u * (index - 232u); // 24-step grayscale ramp
    return (gray << 16) | (gray << 8) | gray;
}

ColorPalette ColorPalette::xterm() {
    ColorPalette palette;
    for (int i = 0; i < 256; ++i) {
        palette.indexed[static_cast<std::size_t>(i)] = paletteColor(static_cast<std::uint8_t>(i));
    }
    return palette;
}

std::uint32_t ColorPalette::resolve(const Color& color, bool isForeground, bool bold) const {
    switch (color.kind) {
    case Color::Kind::Default: return isForeground ? foreground : background;
    case Color::Kind::Indexed: {
        const std::size_t index = (isForeground && bold && color.index < 8) ? color.index + 8u : color.index;
        return indexed[index];
    }
    case Color::Kind::Rgb:
        return (std::uint32_t{color.r} << 16) | (std::uint32_t{color.g} << 8) | std::uint32_t{color.b};
    }
    return foreground;
}

} // namespace wt::terminal

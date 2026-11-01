#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace wt::terminal {

// An RGBA image: pixels are 0xAARRGGBB, row by row.
struct Bitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
};

struct SixelOptions {
    // DCS P2 = 1: pixels that are not drawn stay transparent; otherwise they get `background`.
    bool transparentBackground = false;
    std::uint32_t background = 0xFF000000;
    int maxWidth = 4096;
    int maxHeight = 4096;
};

// Decodes sixel data (what follows "DCS P1;P2;P3 q" up to ST): raster attributes ("), colour
// definitions/selections (#, RGB and HLS), repeats (!), graphics CR ($) and NL (-). Colours start
// from the VT340 palette. Pixels outside the size limits are dropped. nullopt for empty images.
std::optional<Bitmap> decodeSixel(std::string_view data, const SixelOptions& options = {});

} // namespace wt::terminal

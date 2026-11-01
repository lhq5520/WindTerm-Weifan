#pragma once

// The kitty terminal graphics protocol (https://sw.kovidgoyal.net/kitty/graphics-protocol/):
// command parsing and pixel data decoding. Storing and placing images is up to Terminal.

#include "wt/terminal/Sixel.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace wt::terminal {

struct KittyCommand {
    char action = 't';      // a: t transmit, T transmit + put, p put, d delete, q query, f/a/c animation
    char medium = 'd';      // t: d direct, f file, t temporary file, s shared memory
    char compression = 0;   // o: 'z' = zlib (RFC 1950)
    char deleteWhat = 'a';  // d: what a delete command removes
    int format = 32;        // f: 24 RGB, 32 RGBA, 100 PNG
    std::uint32_t id = 0;        // i: image id chosen by the client
    std::uint32_t number = 0;    // I: image number (the terminal picks the id)
    std::uint32_t placement = 0; // p
    bool more = false;           // m=1: more chunks follow
    int quiet = 0;               // q: 1 = no OK responses, 2 = no responses at all
    int width = 0;               // s, v: pixel size of RGB/RGBA data
    int height = 0;
    int x = 0; // x, y, w, h: source rectangle (for deletes: the cell / range)
    int y = 0;
    int w = 0;
    int h = 0;
    int offsetX = 0; // X, Y: pixel offset inside the first cell
    int offsetY = 0;
    int columns = 0; // c, r: cells to fill (0 = from the image size)
    int rows = 0;
    bool keepCursor = false;         // C=1: the cursor does not move
    bool unicodePlaceholder = false; // U=1: virtual placement for Unicode placeholders
    std::int32_t z = 0;              // z-index; below zero the image is drawn under the text
    std::string payload;             // base64 data after ';'
};

// Parses the APC data after 'G': "key=value,...;payload". Unknown keys are ignored.
std::optional<KittyCommand> parseKittyCommand(std::string_view data);

struct KittyDecoders {
    // PNG -> bitmap, provided by the GUI layer; without it PNG data is refused.
    std::function<std::optional<Bitmap>(std::string_view png)> png;
};

struct KittyLimits {
    int maxSide = 10000;                      // pixels per side
    std::size_t maxPixels = 16u * 1024 * 1024; // 64 MB as ARGB
};

struct KittyDecodeResult {
    std::optional<Bitmap> bitmap;
    std::string error; // "ECODE:message" when bitmap is empty
};

// Decodes transmitted (base64-decoded) data according to the command's format and compression.
KittyDecodeResult decodeKittyImage(const KittyCommand& command, std::string data, const KittyDecoders& decoders,
                                   const KittyLimits& limits = {});

// zlib inflate that gives up beyond `limit` output bytes; nullopt on errors or without zlib.
std::optional<std::string> inflateZlib(std::string_view data, std::size_t limit);
bool zlibAvailable();

} // namespace wt::terminal

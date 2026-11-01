// Sixel decoding and image placement in the terminal model.

#include "WtTest.h"

#include "wt/terminal/Sixel.h"
#include "wt/terminal/Terminal.h"

#include <string>

using namespace wt::terminal;

#define CSI "\x1b["
#define ST "\x1b\\"

namespace {

std::uint32_t pixel(const Bitmap& b, int x, int y) {
    return b.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(b.width) + static_cast<std::size_t>(x)];
}

} // namespace

WT_TEST(sixel_decodes_colours_repeats_and_bands) {
    // Register 1 = pure red (RGB percentages), four full columns, then a graphics newline and a
    // second band in register 2 (HLS green) with a repeat count.
    const auto bitmap = decodeSixel("#1;2;100;0;0#1~~~~-#2;1;240;50;100!3~");
    WT_REQUIRE(bitmap.has_value());
    WT_CHECK_EQ(bitmap->width, 4);
    WT_CHECK_EQ(bitmap->height, 12);
    WT_CHECK_EQ(pixel(*bitmap, 0, 0), 0xFFFF0000u);
    WT_CHECK_EQ(pixel(*bitmap, 3, 5), 0xFFFF0000u);
    WT_CHECK_EQ(pixel(*bitmap, 2, 6), 0xFF00FF00u); // DEC hue 240 is green
    WT_CHECK_EQ(pixel(*bitmap, 3, 11), 0xFF000000u); // not drawn: the (opaque) background
    // '$' returns to the band start: overprint a single pixel (bit 0) in register 3.
    const auto overprint = decodeSixel("#3;2;0;0;100~~$#3@");
    WT_REQUIRE(overprint.has_value());
    WT_CHECK_EQ(pixel(*overprint, 0, 0), 0xFF0000FFu);
    WT_CHECK_EQ(pixel(*overprint, 1, 0), 0xFF0000FFu);
    // Default palette: register 2 is the VT340 red.
    const auto palette = decodeSixel("#2~");
    WT_REQUIRE(palette.has_value());
    WT_CHECK_EQ(pixel(*palette, 0, 0), 0xFFCC2121u);
}

WT_TEST(sixel_raster_attributes_transparency_and_limits) {
    SixelOptions transparent;
    transparent.transparentBackground = true;
    const auto sized = decodeSixel("\"1;1;8;12#1;2;0;100;0@", transparent);
    WT_REQUIRE(sized.has_value());
    WT_CHECK_EQ(sized->width, 8); // the raster size wins over the drawn extent
    WT_CHECK_EQ(sized->height, 12);
    WT_CHECK_EQ(pixel(*sized, 0, 0), 0xFF00FF00u);
    WT_CHECK_EQ(pixel(*sized, 7, 11), 0u); // transparent
    SixelOptions small;
    small.maxWidth = 3;
    small.maxHeight = 6;
    const auto clipped = decodeSixel("!100~-~~", small);
    WT_REQUIRE(clipped.has_value());
    WT_CHECK_EQ(clipped->width, 3);
    WT_CHECK_EQ(clipped->height, 6);
    WT_CHECK(!decodeSixel("").has_value());
    WT_CHECK(!decodeSixel("#1;2;3;4;5$-").has_value()); // nothing drawn
    const auto junk = decodeSixel("\x01xyz!!#;;;~ \n~");
    WT_REQUIRE(junk.has_value()); // garbage is skipped
}

WT_TEST(sixel_images_are_placed_and_scroll_with_the_text) {
    Terminal t(5, 20);
    t.setScrollbackLimit(100);
    t.setCellPixelSize(10, 20);
    t.feed("ab");
    // 25 x 30 pixels: 3 columns, 2 rows.
    t.feed("\x1bPq\"1;1;25;30#1;2;100;0;0~" ST);
    auto images = t.visibleImages();
    WT_REQUIRE(images.size() == 1u);
    WT_CHECK_EQ(images[0]->row, t.screenStart());
    WT_CHECK_EQ(images[0]->column, 2);
    WT_CHECK_EQ(images[0]->columns, 3);
    WT_CHECK_EQ(images[0]->rows, 2);
    WT_CHECK_EQ(images[0]->bitmap->width, 25);
    // The cursor continues below the image, in its first column.
    WT_CHECK_EQ(t.cursor().row, 2);
    WT_CHECK_EQ(t.cursor().column, 2);
    // Scrolling keeps the image on its text; it is dropped once it leaves the history.
    const std::int64_t row = images[0]->row;
    for (int i = 0; i < 10; ++i) {
        t.feed("\r\nline");
    }
    WT_REQUIRE(t.visibleImages().size() == 1u);
    WT_CHECK_EQ(t.visibleImages()[0]->row, row);
    t.setScrollbackLimit(2);
    t.feed("\r\n\r\n\r\n");
    WT_CHECK_EQ(t.imageCount(), std::size_t(1)); // pruned lazily on the next image...
    t.feed("\x1bPq#0~" ST);
    WT_CHECK_EQ(t.imageCount(), std::size_t(1)); // ...which keeps only itself
    // ED 2 removes images on the screen; the alternate screen's images do not leak.
    t.feed(CSI "2J");
    WT_CHECK_EQ(t.imageCount(), std::size_t(0));
    t.feed(CSI "?1049h" "\x1bPq#0~" ST);
    WT_CHECK_EQ(t.visibleImages().size(), std::size_t(1));
    WT_CHECK(t.visibleImages()[0]->alternate);
    t.feed(CSI "?1049l");
    WT_CHECK_EQ(t.imageCount(), std::size_t(0));
    // DECSDM: top-left corner, the cursor does not move.
    t.feed(CSI "?80h" CSI "3;4H" "\x1bPq#0~~" ST);
    WT_REQUIRE(t.visibleImages().size() == 1u);
    WT_CHECK_EQ(t.visibleImages()[0]->row, t.screenStart());
    WT_CHECK_EQ(t.visibleImages()[0]->column, 0);
    WT_CHECK_EQ(t.cursor().row, 2);
    WT_CHECK_EQ(t.cursor().column, 3);
    // A different width reflows the text: images are dropped. RIS too.
    t.resize(5, 30);
    WT_CHECK_EQ(t.imageCount(), std::size_t(0));
    t.feed("\x1bPq#0~" ST "\x1b" "c");
    WT_CHECK_EQ(t.imageCount(), std::size_t(0));
}

WT_TEST(sixel_random_input_stays_within_limits) {
    // Deterministic garbage made of sixel syntax: must never crash or exceed the limits.
    static constexpr char kAlphabet[] = "#;!$-\"0123456789?~@AZ^_`|0;;99999999 \x1b\x01";
    std::uint32_t state = 12345;
    auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    SixelOptions limits;
    limits.maxWidth = 64;
    limits.maxHeight = 48;
    Terminal t(6, 20);
    t.setScrollbackLimit(10);
    for (int round = 0; round < 3000; ++round) {
        std::string data;
        const std::uint32_t length = next() % 160;
        for (std::uint32_t i = 0; i < length; ++i) {
            data.push_back(kAlphabet[next() % (sizeof(kAlphabet) - 1)]);
        }
        if (const auto bitmap = decodeSixel(data, limits)) {
            WT_CHECK(bitmap->width > 0 && bitmap->width <= limits.maxWidth);
            WT_CHECK(bitmap->height > 0 && bitmap->height <= limits.maxHeight);
            WT_CHECK_EQ(bitmap->pixels.size(),
                        static_cast<std::size_t>(bitmap->width) * static_cast<std::size_t>(bitmap->height));
        }
        t.feed("\x1bP" + std::to_string(next() % 3) + ";" + std::to_string(next() % 3) + "q" + data + ST);
    }
    for (const Terminal::Image* image : t.visibleImages()) {
        WT_CHECK(image->column >= 0 && image->column + image->columns <= t.columns());
        WT_CHECK(image->rows > 0);
    }
}

WT_TEST(sixel_capability_queries) {
    Terminal t(24, 80);
    t.setCellPixelSize(8, 16);
    std::string responses;
    t.setResponseHandler([&](std::string_view bytes) { responses.append(bytes); });
    t.feed(CSI "c");
    WT_CHECK(responses.find(";4;") != std::string::npos); // DA1 advertises sixel
    responses.clear();
    t.feed(CSI "?1;1S" CSI "?2;1S" CSI "?2;4S" CSI "?3;1S");
    WT_CHECK_EQ(responses, std::string(CSI "?1;0;256S" CSI "?2;0;640;384S" CSI "?2;0;4096;4096S" CSI "?3;1S"));
    responses.clear();
    t.feed(CSI "?80$p");
    WT_CHECK_EQ(responses, std::string(CSI "?80;2$y"));
}

// Kitty graphics protocol: commands, transmission, placement, deletion and responses.

#include "WtTest.h"

#include "wt/core/Base64.h"
#include "wt/terminal/KittyGraphics.h"
#include "wt/terminal/Terminal.h"

#include <limits>
#include <string>
#include <utility>

using namespace wt::terminal;

#define APC "\x1b_G"
#define ST "\x1b\\"
#define CSI "\x1b["

namespace {

struct Fixture {
    Terminal t{10, 20};
    std::string responses;

    Fixture() {
        t.setCellPixelSize(10, 20);
        t.setScrollbackLimit(100);
        t.setResponseHandler([this](std::string_view bytes) { responses.append(bytes); });
    }
    std::string take() { return std::exchange(responses, {}); }
    const Terminal::Image* image(std::uint32_t kittyImage) const {
        for (const Terminal::Image* placed : t.visibleImages()) {
            if (placed->kittyImage == kittyImage) {
                return placed;
            }
        }
        return nullptr;
    }
};

std::uint32_t pixel(const Bitmap& b, int x, int y) {
    return b.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(b.width) + static_cast<std::size_t>(x)];
}

std::string be32(std::uint32_t v) {
    return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}

// The start of a PNG file: signature and IHDR chunk (enough for the size check).
std::string pngHeader(std::uint32_t width, std::uint32_t height) {
    return std::string("\x89PNG\r\n\x1a\n", 8) + be32(13) + "IHDR" + be32(width) + be32(height) +
           std::string("\x08\x06\0\0\0", 5) + be32(0);
}

} // namespace

WT_TEST(kitty_command_parsing) {
    const auto c = parseKittyCommand("a=T,f=24,s=2,v=1,i=5,p=7,z=-3,c=4,r=2,X=3,Y=4,C=1,q=2,m=1,o=z,t=d,bogus,k=;AAAA");
    WT_REQUIRE(c.has_value());
    WT_CHECK_EQ(c->action, 'T');
    WT_CHECK_EQ(c->format, 24);
    WT_CHECK_EQ(c->width, 2);
    WT_CHECK_EQ(c->height, 1);
    WT_CHECK_EQ(c->id, 5u);
    WT_CHECK_EQ(c->placement, 7u);
    WT_CHECK_EQ(c->z, -3);
    WT_CHECK_EQ(c->columns, 4);
    WT_CHECK_EQ(c->rows, 2);
    WT_CHECK_EQ(c->offsetX, 3);
    WT_CHECK_EQ(c->offsetY, 4);
    WT_CHECK(c->keepCursor);
    WT_CHECK_EQ(c->quiet, 2);
    WT_CHECK(c->more);
    WT_CHECK_EQ(c->compression, 'z');
    WT_CHECK_EQ(c->medium, 'd');
    WT_CHECK_EQ(c->payload, std::string("AAAA"));
    const auto d = parseKittyCommand("a=d,d=I,i=4294967295,x=99999999999");
    WT_REQUIRE(d.has_value());
    WT_CHECK_EQ(d->deleteWhat, 'I');
    WT_CHECK_EQ(d->id, 4294967295u);
    WT_CHECK_EQ(d->x, std::numeric_limits<int>::max()); // saturated
    const auto defaults = parseKittyCommand("");
    WT_REQUIRE(defaults.has_value());
    WT_CHECK_EQ(defaults->action, 't');
    WT_CHECK_EQ(defaults->format, 32);
}

WT_TEST(kitty_transmit_and_display) {
    Fixture f;
    f.t.feed("ab");
    f.t.feed(APC "a=T,f=24,s=2,v=1,i=5;/wAAAP8A" ST); // red, green
    WT_CHECK_EQ(f.take(), std::string(APC "i=5;OK" ST));
    const auto* image = f.image(5);
    WT_REQUIRE(image != nullptr);
    WT_CHECK_EQ(image->row, f.t.screenStart());
    WT_CHECK_EQ(image->column, 2);
    WT_CHECK_EQ(image->columns, 1);
    WT_CHECK_EQ(image->rows, 1);
    WT_CHECK_EQ(image->pixelWidth, 2);
    WT_CHECK_EQ(image->bitmap->width, 2);
    WT_CHECK_EQ(pixel(*image->bitmap, 0, 0), 0xFFFF0000u);
    WT_CHECK_EQ(pixel(*image->bitmap, 1, 0), 0xFF00FF00u);
    // The cursor moves past the image (its last row, after its last column).
    WT_CHECK_EQ(f.t.cursor().row, 0);
    WT_CHECK_EQ(f.t.cursor().column, 3);
    WT_CHECK_EQ(f.t.kittyImageCount(), std::size_t(1));
    // RGBA keeps its alpha; C=1 leaves the cursor alone.
    f.t.feed(APC "a=T,f=32,s=1,v=1,i=6,C=1;AAD/gA==" ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=6;OK" ST));
    WT_REQUIRE(f.image(6) != nullptr);
    WT_CHECK_EQ(pixel(*f.image(6)->bitmap, 0, 0), 0x800000FFu);
    WT_CHECK_EQ(f.t.cursor().column, 3);
}

WT_TEST(kitty_chunked_transmission_and_put) {
    Fixture f;
    // 40 x 30 RGB in two chunks: no answer before the last one; a=t places nothing.
    const std::string data = wt::core::base64Encode(std::string(40 * 30 * 3, '\x7f'));
    f.t.feed(std::string(APC "a=t,f=24,s=40,v=30,i=7,m=1;") + data.substr(0, 4096) + ST);
    WT_CHECK_EQ(f.take(), std::string());
    f.t.feed(std::string(APC "m=0;") + data.substr(4096) + ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=7;OK" ST));
    WT_CHECK_EQ(f.t.kittyImageCount(), std::size_t(1));
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(0));
    // Natural size with 10 x 20 cells: 4 columns, 2 rows; the cursor ends on the last row.
    f.t.feed(APC "a=p,i=7,p=1;" ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=7,p=1;OK" ST));
    WT_REQUIRE(f.image(7) != nullptr);
    WT_CHECK_EQ(f.image(7)->columns, 4);
    WT_CHECK_EQ(f.image(7)->rows, 2);
    WT_CHECK_EQ(f.t.cursor().row, 1);
    WT_CHECK_EQ(f.t.cursor().column, 4);
    // The same placement id moves the placement; c and r scale it to the cells.
    f.t.feed("\r\n" APC "a=p,i=7,p=1,c=2,r=3;" ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=7,p=1;OK" ST));
    WT_REQUIRE(f.t.visibleImages().size() == 1u);
    WT_CHECK_EQ(f.image(7)->row, f.t.screenStart() + 2);
    WT_CHECK_EQ(f.image(7)->columns, 2);
    WT_CHECK_EQ(f.image(7)->rows, 3);
    WT_CHECK_EQ(f.image(7)->pixelWidth, 20);
    WT_CHECK_EQ(f.image(7)->pixelHeight, 60);
    // Only one side given: the other keeps the aspect ratio (40 x 30 -> 20 x 15).
    f.t.feed(APC "a=p,i=7,p=2,c=2,C=1,q=1;" ST);
    WT_CHECK_EQ(f.take(), std::string()); // q=1: no OK
    WT_REQUIRE(f.t.visibleImages().size() == 2u);
    const Terminal::Image* scaled = f.t.visibleImages()[1];
    WT_CHECK_EQ(scaled->pixelWidth, 20);
    WT_CHECK_EQ(scaled->pixelHeight, 15);
    WT_CHECK_EQ(scaled->rows, 1);
    // A source rectangle and an offset inside the cell.
    f.t.feed(APC "a=p,i=7,p=3,x=10,y=5,w=8,h=6,X=4,Y=3,C=1,q=1;" ST);
    const Terminal::Image* cropped = f.t.visibleImages()[2];
    WT_CHECK_EQ(cropped->sourceX, 10);
    WT_CHECK_EQ(cropped->sourceY, 5);
    WT_CHECK_EQ(cropped->sourceWidth, 8);
    WT_CHECK_EQ(cropped->sourceHeight, 6);
    WT_CHECK_EQ(cropped->offsetX, 4);
    WT_CHECK_EQ(cropped->columns, 2); // 8 + 4 pixels
    // Unknown images; q=2 silences errors as well.
    f.t.feed(APC "a=p,i=99;" ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=99;ENOENT:no such image" ST));
    f.t.feed(APC "a=p,i=99,q=2;" ST);
    WT_CHECK_EQ(f.take(), std::string());
}

WT_TEST(kitty_numbers_queries_and_errors) {
    Fixture f;
    // I without i: the terminal picks the id and reports it with the number.
    f.t.feed(APC "a=t,f=24,s=2,v=1,I=13;/wAAAP8A" ST);
    const std::string reply = f.take();
    WT_CHECK(reply.rfind(APC "i=", 0) == 0);
    WT_CHECK(reply.find(",I=13;OK" ST) != std::string::npos);
    f.t.feed(APC "a=p,I=13;" ST);
    WT_CHECK(f.take().find(",I=13;OK" ST) != std::string::npos);
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(1));
    // The usual support check: answered, nothing stored.
    f.t.feed(APC "i=31,s=1,v=1,a=q,t=d,f=24;AAAA" ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=31;OK" ST));
    WT_CHECK_EQ(f.t.kittyImageCount(), std::size_t(1));
    // Errors.
    f.t.feed(APC "a=t,t=f,i=40;L3RtcC94" ST); // a file name: refused
    WT_CHECK_EQ(f.take(), std::string(APC "i=40;EINVAL:only direct transmission (t=d) is supported" ST));
    f.t.feed(APC "a=t,f=24,s=4,v=4,i=41;/wAA" ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=41;ENODATA:insufficient image data" ST));
    f.t.feed(APC "a=t,f=24,s=20000,v=1,i=42;/wAA" ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=42;EFBIG:image too large" ST));
    f.t.feed(APC "a=t,f=24,s=1,v=1,i=43;@@@@" ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=43;EINVAL:invalid or too much data" ST));
    f.t.feed(std::string(APC "a=t,f=100,i=44;") + wt::core::base64Encode(pngHeader(2, 2)) + ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=44;EINVAL:PNG is not supported" ST)); // no decoder installed
    f.t.feed(APC "a=T,f=24,s=1,v=1,i=45,U=1;AAAA" ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=45;EINVAL:Unicode placeholders are not supported" ST));
    f.t.feed(APC "a=f,i=45;" ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=45;EINVAL:animation is not supported" ST));
    // Anonymous images are shown without any answer.
    f.t.feed(APC "a=T,f=24,s=1,v=1;AAAA" ST);
    WT_CHECK_EQ(f.take(), std::string());
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(2));
}

WT_TEST(kitty_png_and_zlib) {
    Fixture f;
    int decoded = 0;
    f.t.setPngDecoder([&decoded](std::string_view) -> std::optional<Bitmap> {
        ++decoded;
        Bitmap bitmap;
        bitmap.width = 3;
        bitmap.height = 2;
        bitmap.pixels.assign(6, 0xFF123456u);
        return bitmap;
    });
    f.t.feed(std::string(APC "a=T,f=100,i=50;") + wt::core::base64Encode(pngHeader(3, 2) + "IDAT...") + ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=50;OK" ST));
    WT_CHECK_EQ(decoded, 1);
    WT_REQUIRE(f.image(50) != nullptr);
    WT_CHECK_EQ(f.image(50)->bitmap->width, 3);
    // The size in the header is checked before decoding.
    f.t.feed(std::string(APC "a=T,f=100,i=51;") + wt::core::base64Encode(pngHeader(20000, 2)) + ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=51;EFBIG:image too large" ST));
    f.t.feed(std::string(APC "a=T,f=100,i=52;") + wt::core::base64Encode("not a png at all, really not") + ST);
    WT_CHECK_EQ(f.take(), std::string(APC "i=52;EBADPNG:not a PNG image" ST));
    WT_CHECK_EQ(decoded, 1);

    // zlib-compressed RGB (red, green).
    f.t.feed(APC "a=T,f=24,s=2,v=1,o=z,i=53;eJz7z8DA8J8BAAf+Af8=" ST);
    if (zlibAvailable()) {
        WT_CHECK_EQ(f.take(), std::string(APC "i=53;OK" ST));
        WT_REQUIRE(f.image(53) != nullptr);
        WT_CHECK_EQ(pixel(*f.image(53)->bitmap, 1, 0), 0xFF00FF00u);
        // More output than the image needs is refused (decompression bombs).
        f.t.feed(APC "a=T,f=24,s=1,v=1,o=z,i=54;eJz7z8DA8J8BAAf+Af8=" ST);
        WT_CHECK_EQ(f.take(), std::string(APC "i=54;EINVAL:cannot decompress the data" ST));
        WT_CHECK(!inflateZlib("not zlib", 100).has_value());
    } else {
        WT_CHECK_EQ(f.take(), std::string(APC "i=53;EINVAL:zlib compression is not supported" ST));
    }
}

WT_TEST(kitty_delete_commands) {
    Fixture f;
    f.t.feed(APC "a=t,f=24,s=1,v=1,i=1,q=1;AAAA" ST APC "a=t,f=24,s=1,v=1,i=2,q=1;AAAA" ST);
    f.t.feed(CSI "1;1H" APC "a=p,i=1,p=1,q=1;" ST);       // row 0, column 0
    f.t.feed(CSI "3;5H" APC "a=p,i=1,p=2,z=-1,q=1;" ST);  // row 2, column 4
    f.t.feed(CSI "5;10H" APC "a=p,i=2,q=1;" ST);          // row 4, column 9
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(3));
    f.t.feed(APC "a=d,d=p,x=5,y=3;" ST); // the cell at column 5, row 3 (1-based)
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(2));
    f.t.feed(APC "a=d,d=i,i=1,p=1;" ST);
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(1));
    WT_CHECK_EQ(f.t.kittyImageCount(), std::size_t(2)); // lower case keeps the data
    f.t.feed(APC "a=d,d=I,i=1;" ST);
    WT_CHECK_EQ(f.t.kittyImageCount(), std::size_t(1));
    f.t.feed(CSI "5;10H" APC "a=d,d=c;" ST); // at the cursor
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(0));
    WT_CHECK_EQ(f.t.kittyImageCount(), std::size_t(1));
    f.t.feed(CSI "1;1H" APC "a=p,i=2,q=1,z=7;" ST APC "a=d,d=z,z=6;" ST);
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(1));
    f.t.feed(APC "a=d,d=A;" ST); // everything, data too
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(0));
    WT_CHECK_EQ(f.t.kittyImageCount(), std::size_t(0));
    // Sixel images are not kitty placements; RIS clears everything.
    f.t.feed("\x1bPq#0~" ST APC "a=d;" ST);
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(1));
    f.t.feed(APC "a=t,f=24,s=1,v=1,i=9,q=1;AAAA" ST "\x1b" "c");
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(0));
    WT_CHECK_EQ(f.t.kittyImageCount(), std::size_t(0));
    WT_CHECK_EQ(f.take(), std::string()); // deletes are not answered
}

WT_TEST(kitty_placements_scroll_and_stack) {
    Fixture f;
    f.t.feed(CSI "?1049h" APC "a=T,f=24,s=1,v=1,i=3,q=2;AAAA" ST);
    WT_CHECK_EQ(f.t.visibleImages().size(), std::size_t(1));
    f.t.feed(CSI "?1049l");
    WT_CHECK_EQ(f.t.imageCount(), std::size_t(0));
    WT_CHECK_EQ(f.t.kittyImageCount(), std::size_t(1)); // the data stays for later placements
    // Drawn by z-index; placements scroll with the text.
    f.t.feed(APC "a=p,i=3,z=5,C=1,q=2;" ST APC "a=p,i=3,z=-2,C=1,q=2;" ST APC "a=p,i=3,C=1,q=2;" ST);
    auto images = f.t.visibleImages();
    WT_REQUIRE(images.size() == 3u);
    WT_CHECK_EQ(images[0]->z, -2);
    WT_CHECK_EQ(images[1]->z, 0);
    WT_CHECK_EQ(images[2]->z, 5);
    const std::int64_t row = images[0]->row;
    for (int i = 0; i < 15; ++i) {
        f.t.feed("\r\nline");
    }
    WT_REQUIRE(f.t.visibleImages().size() == 3u);
    WT_CHECK_EQ(f.t.visibleImages()[0]->row, row);
    WT_CHECK(f.t.screenStart() > row);
}

WT_TEST(kitty_random_commands_are_harmless) {
    static constexpr const char* kPieces[] = {"a=T", "a=t", "a=p", "a=d", "a=q", "a=f", "f=24", "f=32", "f=100",
                                              "s=1", "s=3", "v=1", "v=2", "i=1", "i=2", "I=4", "p=1", "m=1", "m=0",
                                              "q=1", "c=3", "r=2", "x=1", "y=2", "w=5", "h=0", "X=99", "Y=7", "C=1",
                                              "z=-1", "U=1", "o=z", "t=f", "d=A", "d=i", "d=p", "d=r", "garbage", ""};
    static constexpr const char* kPayloads[] = {"", "AAAA", "/wAAAP8A", "AAAAAAAAAAAAAAAA", "@@", "eJz7z8DA8J8BAAf+Af8="};
    std::uint32_t state = 777;
    auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    Fixture f;
    for (int round = 0; round < 4000; ++round) {
        std::string control;
        const std::uint32_t pieces = next() % 7;
        for (std::uint32_t i = 0; i < pieces; ++i) {
            control += (i ? "," : "") + std::string(kPieces[next() % std::size(kPieces)]);
        }
        f.t.feed(std::string(APC) + control + ";" + kPayloads[next() % std::size(kPayloads)] + ST);
        if (next() % 50 == 0) {
            f.t.feed("\r\n");
        }
    }
    for (const Terminal::Image* image : f.t.visibleImages()) {
        WT_REQUIRE(image->bitmap != nullptr);
        WT_CHECK(image->rows > 0 && image->columns > 0);
        WT_CHECK(image->pixelWidth > 0 && image->pixelHeight > 0);
        WT_CHECK(image->sourceX + image->sourceWidth <= image->bitmap->width);
        WT_CHECK(image->sourceY + image->sourceHeight <= image->bitmap->height);
    }
}

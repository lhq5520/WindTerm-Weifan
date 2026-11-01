#include "wt/terminal/KittyGraphics.h"

#include <algorithm>
#include <limits>

#ifdef WT_HAVE_ZLIB
#include <zlib.h>
#endif

namespace wt::terminal {
namespace {

// Integer value of a key; out-of-range values saturate.
std::int64_t parseInteger(std::string_view text, bool& ok) {
    ok = false;
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
        negative = text[i] == '-';
        ++i;
    }
    std::int64_t value = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        value = std::min<std::int64_t>(value * 10 + (text[i] - '0'), std::int64_t(1) << 40);
        ok = true;
    }
    return negative ? -value : value;
}

int clampInt(std::int64_t value) {
    return static_cast<int>(std::clamp<std::int64_t>(value, 0, std::numeric_limits<int>::max()));
}

std::uint32_t clampU32(std::int64_t value) {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 0, std::numeric_limits<std::uint32_t>::max()));
}

bool sizeAllowed(std::int64_t width, std::int64_t height, const KittyLimits& limits) {
    return width > 0 && height > 0 && width <= limits.maxSide && height <= limits.maxSide &&
           static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) <= limits.maxPixels;
}

std::uint32_t readBigEndian32(std::string_view data, std::size_t at) {
    return (std::uint32_t(std::uint8_t(data[at])) << 24) | (std::uint32_t(std::uint8_t(data[at + 1])) << 16) |
           (std::uint32_t(std::uint8_t(data[at + 2])) << 8) | std::uint8_t(data[at + 3]);
}

} // namespace

std::optional<KittyCommand> parseKittyCommand(std::string_view data) {
    KittyCommand command;
    const auto semicolon = data.find(';');
    const std::string_view control = data.substr(0, semicolon);
    if (semicolon != std::string_view::npos) {
        command.payload = std::string(data.substr(semicolon + 1));
    }
    for (std::size_t pos = 0; pos < control.size();) {
        auto comma = control.find(',', pos);
        if (comma == std::string_view::npos) {
            comma = control.size();
        }
        const std::string_view item = control.substr(pos, comma - pos);
        pos = comma + 1;
        if (item.size() < 3 || item[1] != '=') {
            continue; // malformed key: ignored
        }
        const char key = item[0];
        const std::string_view value = item.substr(2);
        // Keys with character values.
        switch (key) {
        case 'a': command.action = value[0]; continue;
        case 't': command.medium = value[0]; continue;
        case 'o': command.compression = value[0]; continue;
        case 'd': command.deleteWhat = value[0]; continue;
        default: break;
        }
        bool ok = false;
        const std::int64_t number = parseInteger(value, ok);
        if (!ok) {
            continue;
        }
        switch (key) {
        case 'f': command.format = clampInt(number); break;
        case 'i': command.id = clampU32(number); break;
        case 'I': command.number = clampU32(number); break;
        case 'p': command.placement = clampU32(number); break;
        case 'm': command.more = number == 1; break;
        case 'q': command.quiet = clampInt(number); break;
        case 's': command.width = clampInt(number); break;
        case 'v': command.height = clampInt(number); break;
        case 'x': command.x = clampInt(number); break;
        case 'y': command.y = clampInt(number); break;
        case 'w': command.w = clampInt(number); break;
        case 'h': command.h = clampInt(number); break;
        case 'X': command.offsetX = clampInt(number); break;
        case 'Y': command.offsetY = clampInt(number); break;
        case 'c': command.columns = clampInt(number); break;
        case 'r': command.rows = clampInt(number); break;
        case 'C': command.keepCursor = number == 1; break;
        case 'U': command.unicodePlaceholder = number == 1; break;
        case 'z':
            command.z = static_cast<std::int32_t>(std::clamp<std::int64_t>(number, std::numeric_limits<std::int32_t>::min(),
                                                                           std::numeric_limits<std::int32_t>::max()));
            break;
        default: break; // S, O (file ranges) and keys we do not know
        }
    }
    return command;
}

bool zlibAvailable() {
#ifdef WT_HAVE_ZLIB
    return true;
#else
    return false;
#endif
}

std::optional<std::string> inflateZlib(std::string_view data, std::size_t limit) {
#ifdef WT_HAVE_ZLIB
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        return std::nullopt;
    }
    std::string out;
    char buffer[16384];
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(std::min<std::size_t>(data.size(), std::numeric_limits<uInt>::max()));
    int rc = Z_OK;
    while (rc == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        rc = inflate(&stream, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            break;
        }
        out.append(buffer, sizeof(buffer) - stream.avail_out);
        if (out.size() > limit) {
            rc = Z_MEM_ERROR;
            break;
        }
        if (rc == Z_OK && stream.avail_in == 0 && stream.avail_out != 0) {
            rc = Z_DATA_ERROR; // truncated stream
        }
    }
    inflateEnd(&stream);
    if (rc != Z_STREAM_END) {
        return std::nullopt;
    }
    return out;
#else
    (void)data;
    (void)limit;
    return std::nullopt;
#endif
}

KittyDecodeResult decodeKittyImage(const KittyCommand& command, std::string data, const KittyDecoders& decoders,
                                   const KittyLimits& limits) {
    KittyDecodeResult result;
    const bool raw = command.format == 24 || command.format == 32;
    if (!raw && command.format != 100) {
        result.error = "EINVAL:unknown format " + std::to_string(command.format);
        return result;
    }
    const std::size_t bytesPerPixel = command.format == 24 ? 3 : 4;
    if (raw && !sizeAllowed(command.width, command.height, limits)) {
        result.error = command.width > 0 && command.height > 0 ? "EFBIG:image too large" : "EINVAL:missing s or v";
        return result;
    }
    const std::size_t expected = raw ? static_cast<std::size_t>(command.width) * static_cast<std::size_t>(command.height) *
                                           bytesPerPixel
                                     : limits.maxPixels * 4;
    if (command.compression == 'z') {
        if (!zlibAvailable()) {
            result.error = "EINVAL:zlib compression is not supported";
            return result;
        }
        auto inflated = inflateZlib(data, expected);
        if (!inflated) {
            result.error = "EINVAL:cannot decompress the data";
            return result;
        }
        data = std::move(*inflated);
    } else if (command.compression != 0) {
        result.error = "EINVAL:unknown compression";
        return result;
    }

    if (command.format == 100) {
        // Check the size in the IHDR chunk before decoding.
        static constexpr char kSignature[] = "\x89PNG\r\n\x1a\n";
        if (data.size() < 24 || data.compare(0, 8, kSignature, 8) != 0 || data.compare(12, 4, "IHDR") != 0) {
            result.error = "EBADPNG:not a PNG image";
            return result;
        }
        if (!sizeAllowed(readBigEndian32(data, 16), readBigEndian32(data, 20), limits)) {
            result.error = "EFBIG:image too large";
            return result;
        }
        if (!decoders.png) {
            result.error = "EINVAL:PNG is not supported";
            return result;
        }
        auto bitmap = decoders.png(data);
        if (!bitmap || !sizeAllowed(bitmap->width, bitmap->height, limits) ||
            bitmap->pixels.size() != static_cast<std::size_t>(bitmap->width) * static_cast<std::size_t>(bitmap->height)) {
            result.error = "EBADPNG:cannot decode the PNG image";
            return result;
        }
        result.bitmap = std::move(bitmap);
        return result;
    }

    if (data.size() < expected) {
        result.error = "ENODATA:insufficient image data";
        return result;
    }
    Bitmap bitmap;
    bitmap.width = command.width;
    bitmap.height = command.height;
    bitmap.pixels.resize(static_cast<std::size_t>(command.width) * static_cast<std::size_t>(command.height));
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    for (std::size_t i = 0; i < bitmap.pixels.size(); ++i) {
        const unsigned char* p = bytes + i * bytesPerPixel;
        const std::uint32_t alpha = bytesPerPixel == 4 ? p[3] : 0xFFu;
        bitmap.pixels[i] = (alpha << 24) | (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
    }
    result.bitmap = std::move(bitmap);
    return result;
}

} // namespace wt::terminal

#include "wt/terminal/Sixel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace wt::terminal {
namespace {

std::uint32_t rgb(int r, int g, int b) {
    return 0xFF000000u | (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) |
           static_cast<std::uint32_t>(b);
}

// Percentages (0-100) to 0-255.
int scalePercent(int value) {
    return static_cast<int>(std::lround(std::clamp(value, 0, 100) * 255.0 / 100.0));
}

// Sixel HLS: hue 0-360 with 0 = blue (DEC's convention, 120 degrees off the usual one),
// lightness and saturation 0-100.
std::uint32_t hls(int hue, int lightness, int saturation) {
    const double h = std::fmod((std::clamp(hue, 0, 360) + 240) % 360, 360.0) / 360.0;
    const double l = std::clamp(lightness, 0, 100) / 100.0;
    const double s = std::clamp(saturation, 0, 100) / 100.0;
    if (s == 0) {
        const int v = static_cast<int>(std::lround(l * 255));
        return rgb(v, v, v);
    }
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const double p = 2 * l - q;
    auto channel = [&](double t) {
        if (t < 0) {
            t += 1;
        }
        if (t > 1) {
            t -= 1;
        }
        double v = p;
        if (t < 1.0 / 6) {
            v = p + (q - p) * 6 * t;
        } else if (t < 0.5) {
            v = q;
        } else if (t < 2.0 / 3) {
            v = p + (q - p) * (2.0 / 3 - t) * 6;
        }
        return static_cast<int>(std::lround(v * 255));
    };
    return rgb(channel(h + 1.0 / 3), channel(h), channel(h - 1.0 / 3));
}

std::array<std::uint32_t, 256> defaultPalette() {
    std::array<std::uint32_t, 256> palette{};
    static constexpr int kVt340[16][3] = {{0, 0, 0},    {20, 20, 80}, {80, 13, 13}, {20, 80, 20}, {80, 20, 80}, {20, 80, 80},
                                          {80, 80, 20}, {53, 53, 53}, {26, 26, 26}, {33, 33, 60}, {60, 26, 26}, {33, 60, 33},
                                          {60, 33, 60}, {33, 60, 60}, {60, 60, 33}, {80, 80, 80}};
    for (int i = 0; i < 256; ++i) {
        const int* c = kVt340[i % 16];
        palette[static_cast<std::size_t>(i)] = rgb(scalePercent(c[0]), scalePercent(c[1]), scalePercent(c[2]));
    }
    return palette;
}

class Canvas {
public:
    Canvas(const SixelOptions& options) : m_options(options) {}

    void reserve(int width, int height) {
        grow(std::min(width, m_options.maxWidth), std::min(height, m_options.maxHeight));
        m_rasterWidth = std::min(width, m_options.maxWidth);
        m_rasterHeight = std::min(height, m_options.maxHeight);
    }

    void set(int x, int y, std::uint32_t color) {
        if (x < 0 || y < 0 || x >= m_options.maxWidth || y >= m_options.maxHeight) {
            return;
        }
        if (x >= m_width || y >= m_height) {
            // Grow only what is needed, geometrically.
            grow(x >= m_width ? std::min(m_options.maxWidth, std::max(x + 1, m_width * 2)) : m_width,
                 y >= m_height ? std::min(m_options.maxHeight, std::max(y + 6, m_height * 2)) : m_height);
        }
        m_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x)] = color;
        m_usedWidth = std::max(m_usedWidth, x + 1);
        m_usedHeight = std::max(m_usedHeight, y + 1);
    }

    std::optional<Bitmap> finish() {
        const int width = std::max(m_usedWidth, m_rasterWidth);
        const int height = std::max(m_usedHeight, m_rasterHeight);
        if (width <= 0 || height <= 0) {
            return std::nullopt;
        }
        Bitmap bitmap;
        bitmap.width = width;
        bitmap.height = height;
        bitmap.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::uint32_t value = (x < m_width && y < m_height)
                                                ? m_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) +
                                                           static_cast<std::size_t>(x)]
                                                : kUnset;
                bitmap.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                    value == kUnset ? (m_options.transparentBackground ? 0u : m_options.background) : value;
            }
        }
        return bitmap;
    }

private:
    // Marks pixels no sixel has touched (real colours always have alpha 0xFF).
    static constexpr std::uint32_t kUnset = 0x00FFFFFFu;

    void grow(int width, int height) {
        if (width <= m_width && height <= m_height) {
            return;
        }
        width = std::max(width, m_width);
        height = std::max(height, m_height);
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), kUnset);
        for (int y = 0; y < m_height; ++y) {
            std::copy_n(m_pixels.begin() + static_cast<std::ptrdiff_t>(y) * m_width, m_width,
                        pixels.begin() + static_cast<std::ptrdiff_t>(y) * width);
        }
        m_pixels.swap(pixels);
        m_width = width;
        m_height = height;
    }

    const SixelOptions& m_options;
    std::vector<std::uint32_t> m_pixels;
    int m_width = 0;
    int m_height = 0;
    int m_usedWidth = 0;
    int m_usedHeight = 0;
    int m_rasterWidth = 0;
    int m_rasterHeight = 0;
};

} // namespace

std::optional<Bitmap> decodeSixel(std::string_view data, const SixelOptions& options) {
    std::array<std::uint32_t, 256> palette = defaultPalette();
    Canvas canvas(options);
    int x = 0;
    int y = 0; // top of the current six-pixel band
    std::uint32_t color = palette[0];
    std::size_t i = 0;
    auto readNumbers = [&](std::vector<int>& numbers) {
        numbers.clear();
        int value = 0;
        bool any = false;
        while (i < data.size()) {
            const char c = data[i];
            if (c >= '0' && c <= '9') {
                value = std::min(value * 10 + (c - '0'), 1000000);
                any = true;
            } else if (c == ';') {
                numbers.push_back(any ? value : 0);
                value = 0;
                any = false;
            } else {
                break;
            }
            ++i;
        }
        if (any || !numbers.empty()) {
            numbers.push_back(any ? value : 0);
        }
    };
    std::vector<int> numbers;
    while (i < data.size()) {
        const char c = data[i++];
        if (c >= '?' && c <= '~') {
            const int bits = c - '?';
            for (int bit = 0; bit < 6; ++bit) {
                if (bits & (1 << bit)) {
                    canvas.set(x, y + bit, color);
                }
            }
            ++x;
            continue;
        }
        switch (c) {
        case '!': { // repeat introducer: !<count><sixel>
            readNumbers(numbers);
            const int count = numbers.empty() ? 1 : std::clamp(numbers[0], 1, options.maxWidth);
            if (i < data.size() && data[i] >= '?' && data[i] <= '~') {
                const int bits = data[i++] - '?';
                for (int n = 0; n < count; ++n, ++x) {
                    for (int bit = 0; bit < 6; ++bit) {
                        if (bits & (1 << bit)) {
                            canvas.set(x, y + bit, color);
                        }
                    }
                    if (x >= options.maxWidth) {
                        break;
                    }
                }
            }
            break;
        }
        case '#': { // colour: #Pc selects, #Pc;Pu;Px;Py;Pz defines (Pu 1 = HLS, 2 = RGB)
            readNumbers(numbers);
            if (numbers.empty()) {
                break;
            }
            const auto index = static_cast<std::size_t>(numbers[0] & 0xFF);
            if (numbers.size() >= 5) {
                if (numbers[1] == 1) {
                    palette[index] = hls(numbers[2], numbers[3], numbers[4]);
                } else if (numbers[1] == 2) {
                    palette[index] = rgb(scalePercent(numbers[2]), scalePercent(numbers[3]), scalePercent(numbers[4]));
                }
            }
            color = palette[index];
            break;
        }
        case '"': // raster attributes: "Pan;Pad;Ph;Pv
            readNumbers(numbers);
            if (numbers.size() >= 4 && numbers[2] > 0 && numbers[3] > 0) {
                canvas.reserve(numbers[2], numbers[3]);
            }
            break;
        case '$': x = 0; break;
        case '-':
            x = 0;
            y += 6;
            if (y >= options.maxHeight) {
                return canvas.finish(); // everything further down would be dropped anyway
            }
            break;
        default: break; // whitespace and anything unknown is ignored
        }
    }
    return canvas.finish();
}

} // namespace wt::terminal

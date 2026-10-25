#include "wt/core/CharWidth.h"

#include <algorithm>
#include <iterator>

namespace wt::core {
namespace {

struct Range {
    char32_t first;
    char32_t last;
};

#include "CharWidthTables.inc"

template <std::size_t N>
bool inTable(const Range (&table)[N], char32_t cp) {
    const auto it = std::upper_bound(std::begin(table), std::end(table), cp,
                                     [](char32_t value, const Range& r) { return value < r.first; });
    return it != std::begin(table) && cp <= std::prev(it)->last;
}

} // namespace

int charWidth(char32_t cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
        return 0;
    }
    if (cp < 0x300) {
        return 1;
    }
    if (inTable(kZeroWidth, cp)) {
        return 0;
    }
    if (inTable(kWide, cp)) {
        return 2;
    }
    return 1;
}

} // namespace wt::core

#pragma once

#include <algorithm>
#include <cstdint>
#include <tuple>

namespace wt::terminal {

// Position in the terminal's absolute line space (see Terminal::historyStart()). Absolute rows stay
// attached to the same line while it scrolls from the screen into the scrollback.
struct GridPoint {
    std::int64_t row = 0;
    int column = 0;

    friend bool operator==(const GridPoint&, const GridPoint&) = default;
    friend bool operator<(const GridPoint& a, const GridPoint& b) {
        return std::tie(a.row, a.column) < std::tie(b.row, b.column);
    }
};

enum class SelectionMode : std::uint8_t {
    Character, // plain drag
    Word,      // double click: snaps to word boundaries
    Line,      // triple click: whole logical (unwrapped) lines
    Block,     // rectangular (Alt+drag)
};

// Normalised, inclusive selection range after word/line expansion.
struct SelectionRange {
    GridPoint start;
    GridPoint end;
    bool block = false;

    bool contains(std::int64_t row, int column) const {
        if (row < start.row || row > end.row) {
            return false;
        }
        if (block) {
            return column >= std::min(start.column, end.column) && column <= std::max(start.column, end.column);
        }
        if (row == start.row && column < start.column) {
            return false;
        }
        return !(row == end.row && column > end.column);
    }
};

} // namespace wt::terminal

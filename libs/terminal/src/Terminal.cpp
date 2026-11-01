#include "wt/terminal/Terminal.h"

#include "wt/core/Base64.h"
#include "wt/core/CharWidth.h"
#include "wt/core/Utf8.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <string>
#include <utility>

namespace wt::terminal {
namespace {

constexpr int kTabWidth = 8;
constexpr std::size_t kMaxClusters = 1u << 16;   // distinct grapheme clusters kept per terminal
constexpr std::size_t kMaxClusterLength = 16;    // code points per cluster (base + marks)
constexpr std::size_t kMaxTitleStack = 10;
constexpr std::size_t kMaxHyperlinks = 0xFFFF;   // ids are 16-bit, 0 means "no link"
constexpr std::size_t kMaxHyperlinkLength = 4096;  // URI bytes
constexpr std::size_t kMaxHyperlinkIdLength = 250; // the "id=" parameter (same limit as kitty)
constexpr std::size_t kMaxHyperlinkBytes = 8u << 20; // all interned id+URI keys together
constexpr std::size_t kMaxKittyStack = 16;

// VT100 "DEC Special Graphics" (ESC ( 0) mapping for 0x60-0x7E, used for line drawing.
char32_t decSpecialGraphics(char32_t cp) {
    static constexpr char32_t kTable[] = {
        U'◆', U'▒', U'␉', U'␌', U'␍', U'␊', U'°', U'±', U'␤', U'␋', U'┘', U'┐', U'┌', U'└', U'┼', U'⎺',
        U'⎻', U'─', U'⎼', U'⎽', U'├', U'┤', U'┴', U'┬', U'│', U'≤', U'≥', U'π', U'≠', U'£', U'·',
    };
    if (cp >= 0x60 && cp <= 0x7E) {
        return kTable[cp - 0x60];
    }
    return cp;
}

std::uint8_t clampByte(int v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

bool isBlankLine(const Line& line) {
    return std::all_of(line.cells.begin(), line.cells.end(), [](const Cell& c) { return c == Cell{}; });
}

std::optional<int> parseNumber(std::string_view text) {
    int value = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc() || ptr != end || text.empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<unsigned> parseHex(std::string_view text) {
    unsigned value = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, 16);
    if (ec != std::errc() || ptr != end || text.empty()) {
        return std::nullopt;
    }
    return value;
}

// X11 colour specifications as accepted by xterm: rgb:R/G/B (1-4 hex digits per component) and
// #RGB / #RRGGBB / #RRRGGGBBB / #RRRRGGGGBBBB.
std::optional<std::uint32_t> parseColorSpec(std::string_view spec) {
    std::uint32_t rgb = 0;
    if (spec.substr(0, 4) == "rgb:") {
        spec.remove_prefix(4);
        for (int i = 0; i < 3; ++i) {
            const auto slash = spec.find('/');
            const std::string_view part = spec.substr(0, slash);
            if (part.empty() || part.size() > 4 || (i < 2) == (slash == std::string_view::npos)) {
                return std::nullopt;
            }
            const auto value = parseHex(part);
            if (!value) {
                return std::nullopt;
            }
            const unsigned max = (1u << (4 * part.size())) - 1;
            rgb = (rgb << 8) | ((*value * 255 + max / 2) / max);
            spec.remove_prefix(slash == std::string_view::npos ? spec.size() : slash + 1);
        }
        return rgb;
    }
    if (!spec.empty() && spec[0] == '#') {
        spec.remove_prefix(1);
        if (spec.empty() || spec.size() % 3 != 0 || spec.size() > 12) {
            return std::nullopt;
        }
        const std::size_t digits = spec.size() / 3;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto value = parseHex(spec.substr(i * digits, digits));
            if (!value) {
                return std::nullopt;
            }
            // The given digits are the most significant bits of the component.
            const unsigned v = digits >= 2 ? *value >> (4 * (digits - 2)) : *value << 4;
            rgb = (rgb << 8) | (v & 0xFF);
        }
        return rgb;
    }
    return std::nullopt;
}

std::string formatColorSpec(std::uint32_t rgb) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "rgb:%04x/%04x/%04x", ((rgb >> 16) & 0xFF) * 0x101u,
                  ((rgb >> 8) & 0xFF) * 0x101u, (rgb & 0xFF) * 0x101u);
    return buffer;
}

std::string percentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            if (const auto value = parseHex(text.substr(i + 1, 2))) {
                out.push_back(static_cast<char>(*value));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

// A position inside a logical line that must follow the text through a rewrap (cursor,
// selection end points). `cell` indexes the logical line's cells; the result is an index into the
// rewrapped lines plus a column.
struct TrackedCell {
    int cell = -1;
    int row = -1;
    int column = 0;
};

// Splits the cells of one logical (unwrapped) line into lines of `columns` cells appended to `out`.
// Wide characters never straddle a line break; padding left by an earlier early wrap is dropped.
void wrapLogicalLine(std::vector<Cell>& cells, int columns, std::vector<Line>& out,
                     std::vector<TrackedCell*>& tracked) {
    Line current;
    current.cells.reserve(static_cast<std::size_t>(columns));
    auto flush = [&](bool wrapped) {
        current.cells.resize(static_cast<std::size_t>(columns), Cell{});
        current.wrapped = wrapped;
        out.push_back(std::move(current));
        current = Line{};
        current.cells.reserve(static_cast<std::size_t>(columns));
    };
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const Cell& cell = cells[i];
        const bool padding = cell.attributes.has(WidePadding);
        if (padding) {
            // Dropped padding: points on it belong to the next character (decided after its
            // own line-break check below).
            for (TrackedCell* t : tracked) {
                if (t->cell == static_cast<int>(i) && t->row < 0) {
                    t->cell = static_cast<int>(i) + 1;
                }
            }
            continue;
        }
        if (cell.width == 0) {
            continue; // trailing halves are re-created with their leading cell
        }
        const int width = (cell.width == 2 && columns >= 2) ? 2 : 1;
        if (static_cast<int>(current.cells.size()) + width > columns) {
            flush(true);
        }
        for (TrackedCell* t : tracked) {
            // Points on a trailing half or on dropped padding map to the next drawn cell.
            const bool here = t->cell == static_cast<int>(i) ||
                              (t->cell > 0 && static_cast<int>(i) == t->cell - 1 &&
                               static_cast<std::size_t>(t->cell) < cells.size() &&
                               cells[static_cast<std::size_t>(t->cell)].width == 0 && cell.width == 2);
            if (here && t->row < 0) {
                t->row = static_cast<int>(out.size());
                t->column = std::min(static_cast<int>(current.cells.size()), columns - 1);
            }
        }
        Cell lead = cell;
        lead.width = static_cast<std::uint8_t>(width);
        current.cells.push_back(lead);
        if (width == 2) {
            current.cells.push_back(Cell{0, cell.attributes, 0});
        }
    }
    for (TrackedCell* t : tracked) {
        if (t->row < 0 && t->cell >= 0) {
            // Beyond the (trimmed) content: keep the distance from the end of the text.
            t->row = static_cast<int>(out.size());
            t->column = std::min(columns - 1, static_cast<int>(current.cells.size()) +
                                                  (t->cell - static_cast<int>(cells.size())));
        }
    }
    flush(false);
}

} // namespace

Terminal::Terminal(int rows, int columns, std::size_t scrollbackLimit)
    : m_rows(std::max(1, rows)), m_columns(std::max(1, columns)), m_scrollbackLimit(scrollbackLimit) {
    reset();
}

void Terminal::feed(std::string_view bytes) {
    m_parser.feed(bytes);
    ++m_revision;
}

void Terminal::reset() {
    clearSelection();
    m_shellMarks = {};
    m_images.clear();
    resetKittyGraphics();
    m_attributes = {};
    m_primary.assign(static_cast<std::size_t>(m_rows), blankLine());
    m_alternate.assign(static_cast<std::size_t>(m_rows), blankLine());
    m_screen = &m_primary;
    m_cursor = {};
    m_cursor.shape = m_defaultCursorShape;
    m_cursor.blinking = m_defaultCursorBlink;
    m_pendingWrap = false;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_modes = {};
    m_saved[0].reset();
    m_saved[1].reset();
    m_charsets[0] = m_charsets[1] = Charset::Ascii;
    m_activeCharset = 0;
    m_lastPrinted = 0;
    m_titleStack.clear();
    m_palette = m_defaultPalette;
    m_kittyFlags[0].clear();
    m_kittyFlags[1].clear();
    resetTabStops();
    m_dirtyRows.assign(static_cast<std::size_t>(m_rows), false);
    markFullDamage();
    ++m_revision;
}
void Terminal::softReset() {
    // DECSTR: modes and attributes, but not the screen contents.
    m_cursor.visible = true;
    m_modes.originMode = false;
    m_modes.autoWrap = true;
    m_modes.insertMode = false;
    m_modes.cursorKeysApplication = false;
    m_modes.keypadApplication = false;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_attributes = {};
    m_charsets[0] = m_charsets[1] = Charset::Ascii;
    m_activeCharset = 0;
    m_saved[isAlternateScreen() ? 1 : 0].reset();
    m_pendingWrap = false;
}

void Terminal::setDefaultPalette(const ColorPalette& palette) {
    m_defaultPalette = palette;
    m_palette = palette;
    markFullDamage();
    ++m_revision;
}

void Terminal::setDefaultCursorStyle(CursorShape shape, bool blinking) {
    const bool wasDefault = m_cursor.shape == m_defaultCursorShape && m_cursor.blinking == m_defaultCursorBlink;
    m_defaultCursorShape = shape;
    m_defaultCursorBlink = blinking;
    if (wasDefault) {
        m_cursor.shape = shape;
        m_cursor.blinking = blinking;
    }
    ++m_revision;
}

void Terminal::setScrollbackLimit(std::size_t lines) {
    m_scrollbackLimit = lines;
    while (m_scrollback.size() > m_scrollbackLimit) {
        m_scrollback.pop_front();
        ++m_droppedLines;
    }
    if (m_selection.active && std::min(m_selection.anchor.row, m_selection.extent.row) < m_droppedLines) {
        clearSelection();
    }
    markFullDamage();
    ++m_revision;
}

Terminal::Damage Terminal::takeDamage() {
    Damage damage;
    damage.full = m_fullDamage;
    if (!damage.full) {
        for (std::size_t row = 0; row < m_dirtyRows.size(); ++row) {
            if (m_dirtyRows[row]) {
                damage.rows.push_back(static_cast<int>(row));
            }
        }
    }
    m_fullDamage = false;
    std::fill(m_dirtyRows.begin(), m_dirtyRows.end(), false);
    return damage;
}

void Terminal::markDirty(int firstRow, int lastRow) {
    firstRow = std::max(firstRow, 0);
    lastRow = std::min(lastRow, static_cast<int>(m_dirtyRows.size()) - 1);
    for (int row = firstRow; row <= lastRow; ++row) {
        m_dirtyRows[static_cast<std::size_t>(row)] = true;
    }
}

unsigned Terminal::kittyKeyboardFlags() const {
    const auto& stack = m_kittyFlags[isAlternateScreen() ? 1 : 0];
    return stack.empty() ? 0u : stack.back();
}

std::string_view Terminal::hyperlinkUri(std::uint16_t id) const {
    if (id == 0 || id > m_hyperlinks.size()) {
        return {};
    }
    const std::string& entry = m_hyperlinks[id - 1u];
    return std::string_view(entry).substr(entry.find('\n') + 1); // stored as "<id param>\n<uri>"
}

void Terminal::setCellPixelSize(int width, int height) {
    m_cellPixelWidth = std::max(0, width);
    m_cellPixelHeight = std::max(0, height);
}

void Terminal::resize(int rows, int columns) {
    rows = std::max(1, rows);
    columns = std::max(1, columns);
    if (rows == m_rows && columns == m_columns) {
        return;
    }
    // A character/word/line selection on the primary screen follows its text through the reflow.
    const bool keepSelection = m_selection.active && !isAlternateScreen() &&
                               m_selection.mode != SelectionMode::Block && columns != m_columns;
    if (!keepSelection) {
        clearSelection();
    }
    if (columns != m_columns) {
        m_images.clear(); // reflow renumbers the rows the images are anchored to
    }

    // A pending wrap (cursor parked on the last column after printing there) must survive the
    // resize, or the next character would overwrite the last one.
    const bool pendingWrap = m_pendingWrap;
    // The primary screen's cursor is the live one, or the one ?1049 saved while the alternate
    // screen is shown (without one, assume the bottom so no history is dropped).
    const bool alternate = isAlternateScreen();
    int primaryRow = m_cursor.row;
    int primaryColumn = m_cursor.column;
    if (alternate) {
        primaryRow = m_saved[0] ? m_saved[0]->row : m_rows - 1;
        primaryColumn = m_saved[0] ? m_saved[0]->column : 0;
    }
    // A cursor saved with DECSC on the primary screen follows its text like the live one (in
    // absolute rows, which also covers lines moving to and from the scrollback).
    std::optional<GridPoint> savedPoint;
    if (!alternate && m_saved[0]) {
        savedPoint = GridPoint{screenStart() + m_saved[0]->row, m_saved[0]->column};
    }

    if (columns != m_columns) {
        std::vector<GridPoint*> tracked;
        if (keepSelection) {
            tracked = {&m_selection.anchor, &m_selection.extent};
        }
        if (savedPoint) {
            tracked.push_back(&*savedPoint);
        }
        reflowPrimary(rows, columns, primaryRow, primaryColumn, std::move(tracked));
        if (keepSelection && (m_selection.anchor.row < m_droppedLines || m_selection.extent.row < m_droppedLines)) {
            clearSelection();
        }
    } else if (rows < m_rows) {
        // Keep the cursor line visible: shrinking pushes lines above it into the scrollback.
        const int excess = std::max(0, primaryRow - (rows - 1));
        for (int i = 0; i < excess; ++i) {
            pushScrollback(std::move(m_primary[static_cast<std::size_t>(i)]));
        }
        m_primary.erase(m_primary.begin(), m_primary.begin() + excess);
        m_primary.resize(static_cast<std::size_t>(rows));
        primaryRow -= excess;
    } else {
        // Growing pulls lines back from the scrollback.
        const int pull = std::min(static_cast<int>(m_scrollback.size()), rows - m_rows);
        for (int i = 0; i < pull; ++i) {
            m_primary.insert(m_primary.begin(), std::move(m_scrollback.back()));
            m_scrollback.pop_back();
        }
        Line blank;
        blank.cells.assign(static_cast<std::size_t>(columns), Cell{});
        m_primary.resize(static_cast<std::size_t>(rows), blank);
        primaryRow += pull;
    }

    // The alternate screen is not reflowed: full-screen programs redraw after SIGWINCH.
    for (Line& line : m_alternate) {
        line.cells.resize(static_cast<std::size_t>(columns), Cell{});
        if (columns < m_columns && line.cells.back().width == 2) {
            line.cells.back() = Cell{};
        }
        line.wrapped = false;
    }
    Line blank;
    blank.cells.assign(static_cast<std::size_t>(columns), Cell{});
    m_alternate.resize(static_cast<std::size_t>(rows), blank);

    if (alternate) {
        if (m_saved[0]) {
            m_saved[0]->row = primaryRow;
            m_saved[0]->column = primaryColumn;
        }
    } else {
        m_cursor.row = primaryRow;
        m_cursor.column = primaryColumn;
    }
    if (savedPoint) {
        m_saved[0]->row = static_cast<int>(std::clamp<std::int64_t>(savedPoint->row - screenStart(), -1, rows));
        m_saved[0]->column = savedPoint->column; // both clamped below
    }

    const int oldColumns = static_cast<int>(m_tabStops.size());
    m_rows = rows;
    m_columns = columns;
    m_tabStops.resize(static_cast<std::size_t>(columns), false);
    if (columns > oldColumns) {
        resetTabStops(oldColumns);
    }
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_cursor.row = std::clamp(m_cursor.row, 0, m_rows - 1);
    m_cursor.column = std::clamp(m_cursor.column, 0, m_columns - 1);
    for (auto& saved : m_saved) {
        if (saved) {
            saved->row = std::clamp(saved->row, 0, m_rows - 1);
            saved->column = std::clamp(saved->column, 0, m_columns - 1);
        }
    }
    m_pendingWrap = false;
    if (pendingWrap && !alternate) {
        if (m_cursor.column + 1 < m_columns && columns != oldColumns) {
            ++m_cursor.column; // the reflowed text no longer ends at the margin
        } else {
            m_pendingWrap = m_cursor.column == m_columns - 1;
        }
    } else if (pendingWrap && alternate) {
        m_pendingWrap = m_cursor.column == m_columns - 1;
    }
    m_dirtyRows.assign(static_cast<std::size_t>(m_rows), false);
    markFullDamage();
    ++m_revision;
}

void Terminal::reflowPrimary(int rows, int columns, int& cursorRow, int& cursorColumn,
                             std::vector<GridPoint*> tracked) {
    // Screen rows below both the cursor and the last non-blank row carry no content.
    int lastRow = std::clamp(cursorRow, 0, m_rows - 1);
    for (int row = m_rows - 1; row > lastRow; --row) {
        if (!isBlankLine(m_primary[static_cast<std::size_t>(row)])) {
            lastRow = row;
            break;
        }
    }

    // Tracked points are in absolute rows; rows below `lastRow` hold no text, so points there move
    // to the end of the last line kept.
    const std::int64_t firstAbsolute = m_droppedLines;
    const std::int64_t lastAbsolute = screenStart() + lastRow;
    std::vector<TrackedCell> trackedCells(tracked.size());

    std::vector<Line> source;
    source.reserve(m_scrollback.size() + static_cast<std::size_t>(lastRow) + 1);
    for (Line& line : m_scrollback) {
        source.push_back(std::move(line));
    }
    const int cursorSource = static_cast<int>(source.size()) + std::clamp(cursorRow, 0, m_rows - 1);
    for (int row = 0; row <= lastRow; ++row) {
        source.push_back(std::move(m_primary[static_cast<std::size_t>(row)]));
    }
    m_scrollback.clear();

    std::vector<Line> lines;
    lines.reserve(source.size());
    TrackedCell cursor;
    std::vector<TrackedCell*> active;
    std::vector<Cell> logical;
    for (std::size_t i = 0; i < source.size(); ++i) {
        Line& line = source[i];
        if (static_cast<int>(i) == cursorSource) {
            cursor.cell = static_cast<int>(logical.size()) + cursorColumn;
            active.push_back(&cursor);
        }
        for (std::size_t t = 0; t < tracked.size(); ++t) {
            const std::int64_t row = std::clamp(tracked[t]->row, firstAbsolute, lastAbsolute);
            if (row - firstAbsolute == static_cast<std::int64_t>(i)) {
                const int column = tracked[t]->row > lastAbsolute ? m_columns - 1 : tracked[t]->column;
                trackedCells[t].cell = static_cast<int>(logical.size()) + column;
                active.push_back(&trackedCells[t]);
            }
        }
        logical.insert(logical.end(), line.cells.begin(), line.cells.end());
        if (line.wrapped && i + 1 < source.size()) {
            continue;
        }
        // End of a logical line: drop trailing default blanks, but keep the cursor cell.
        std::size_t used = logical.size();
        while (used > 0 && logical[used - 1] == Cell{}) {
            --used;
        }
        if (cursor.cell >= 0 && cursor.row < 0) {
            used = std::max(used, static_cast<std::size_t>(cursor.cell) + 1);
        }
        logical.resize(used, Cell{});
        const int firstOut = static_cast<int>(lines.size());
        wrapLogicalLine(logical, columns, lines, active);
        // Rows past the logical line's last row (blank tail) stay on its last row.
        for (TrackedCell* t : active) {
            t->row = std::max(t->row, firstOut);
        }
        logical.clear();
        active.clear();
    }
    int newCursorRow = cursor.row;
    const int newCursorColumn = cursor.column;
    if (newCursorRow < 0) {
        newCursorRow = std::max(0, static_cast<int>(lines.size()) - 1);
    }

    // The last `rows` lines form the screen, as long as the cursor stays visible.
    const int total = static_cast<int>(lines.size());
    int top = std::max(0, total - rows);
    top = std::min(top, newCursorRow);
    for (int i = 0; i < top; ++i) {
        pushScrollback(std::move(lines[static_cast<std::size_t>(i)]));
    }
    Line blank;
    blank.cells.assign(static_cast<std::size_t>(columns), Cell{});
    m_primary.assign(static_cast<std::size_t>(rows), blank);
    for (int row = 0; row < rows && top + row < total; ++row) {
        m_primary[static_cast<std::size_t>(row)] = std::move(lines[static_cast<std::size_t>(top + row)]);
    }
    cursorRow = newCursorRow - top;
    cursorColumn = std::min(newCursorColumn, columns - 1);
    // Lines keep consecutive absolute numbers starting where the old history started.
    for (std::size_t t = 0; t < tracked.size(); ++t) {
        if (trackedCells[t].row >= 0) {
            tracked[t]->row = firstAbsolute + trackedCells[t].row;
            tracked[t]->column = trackedCells[t].column;
        }
    }
}

std::u32string_view Terminal::cellText(const Cell& cell) const {
    if (cell.isCluster()) {
        const std::size_t index = cell.ch - kClusterBase;
        return index < m_clusters.size() ? std::u32string_view(m_clusters[index]) : std::u32string_view(U"\uFFFD");
    }
    return {&cell.ch, 1};
}

std::optional<std::uint32_t> Terminal::internCluster(const std::u32string& text) {
    if (const auto it = m_clusterIds.find(text); it != m_clusterIds.end()) {
        return it->second;
    }
    if (m_clusters.size() >= kMaxClusters) {
        return std::nullopt;
    }
    const auto id = static_cast<std::uint32_t>(m_clusters.size());
    m_clusters.push_back(text);
    m_clusterIds.emplace(text, id);
    return id;
}

std::string Terminal::lineToText(const Line& line) const {
    std::string text;
    for (const Cell& cell : line.cells) {
        if (cell.width != 0 && !cell.attributes.has(WidePadding)) {
            for (char32_t cp : cellText(cell)) {
                core::appendUtf8(text, cp);
            }
        }
    }
    text.erase(text.find_last_not_of(' ') + 1);
    return text;
}

std::string Terminal::lineText(int row) const {
    return lineToText(line(row));
}

std::string Terminal::textFrom(GridPoint start) const {
    std::string text;
    std::int64_t row = start.row;
    std::size_t column = static_cast<std::size_t>(std::max(0, start.column));
    while (const Line* current = absoluteLine(row)) {
        for (std::size_t i = column; i < current->cells.size(); ++i) {
            const Cell& cell = current->cells[i];
            if (cell.width != 0 && !cell.attributes.has(WidePadding)) {
                for (char32_t cp : cellText(cell)) {
                    core::appendUtf8(text, cp);
                }
            }
        }
        if (!current->wrapped) {
            break;
        }
        ++row;
        column = 0;
    }
    text.erase(text.find_last_not_of(' ') + 1);
    return text;
}

std::string Terminal::screenText() const {
    std::string text;
    for (int row = 0; row < m_rows; ++row) {
        if (row > 0) {
            text.push_back('\n');
        }
        text += lineText(row);
    }
    text.erase(text.find_last_not_of('\n') + 1);
    return text;
}

// ---------------------------------------------------------------------------------------------
// Grid primitives

Cell Terminal::blankCell() const {
    // Background colour erase (bce): erased cells take the current background.
    Cell cell;
    cell.attributes.background = m_attributes.background;
    return cell;
}

Line Terminal::blankLine() const {
    Line line;
    line.cells.assign(static_cast<std::size_t>(m_columns), blankCell());
    return line;
}

void Terminal::splitWideCharAt(Line& line, int column) {
    if (column < 0 || column >= m_columns) {
        return;
    }
    auto& cells = line.cells;
    const auto c = static_cast<std::size_t>(column);
    if (cells[c].width == 0 && c > 0) {
        cells[c - 1] = blankCell();
        cells[c] = blankCell();
    } else if (cells[c].width == 2 && c + 1 < cells.size()) {
        cells[c + 1] = blankCell();
    }
}

void Terminal::eraseCells(int row, int from, int to) {
    from = std::clamp(from, 0, m_columns);
    to = std::clamp(to, 0, m_columns);
    if (from >= to) {
        return;
    }
    Line& line = (*m_screen)[static_cast<std::size_t>(row)];
    splitWideCharAt(line, from);
    splitWideCharAt(line, to);
    std::fill(line.cells.begin() + from, line.cells.begin() + to, blankCell());
    touchRows(row, row);
}

void Terminal::eraseLines(int from, int to) {
    from = std::max(0, from);
    to = std::min(to, m_rows);
    const Cell blank = blankCell();
    for (int row = from; row < to; ++row) {
        // Reuse the line's buffer: scrolling recycles lines, so this avoids an allocation per line.
        Line& line = (*m_screen)[static_cast<std::size_t>(row)];
        line.cells.assign(static_cast<std::size_t>(m_columns), blank);
        line.wrapped = false;
        line.size = LineSize::Normal;
    }
    if (from < to) {
        touchRows(from, to - 1);
    }
}

void Terminal::pushScrollback(Line&& line) {
    if (m_scrollbackLimit == 0) {
        m_spareLine = std::move(line);
        ++m_droppedLines;
    } else {
        m_scrollback.push_back(std::move(line));
        while (m_scrollback.size() > m_scrollbackLimit) {
            m_spareLine = std::move(m_scrollback.front()); // its buffer is reused by scrollUp()
            m_scrollback.pop_front();
            ++m_droppedLines;
        }
    }
    if (m_selection.active && std::min(m_selection.anchor.row, m_selection.extent.row) < m_droppedLines) {
        clearSelection(); // part of the selected text is gone
    }
}

void Terminal::clearScrollback() {
    m_droppedLines += static_cast<std::int64_t>(m_scrollback.size());
    m_scrollback.clear();
    clearSelection();
    markFullDamage();
    ++m_revision;
}

const Line* Terminal::absoluteLine(std::int64_t row) const {
    if (row < m_droppedLines) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(row - m_droppedLines);
    if (index < m_scrollback.size()) {
        return &m_scrollback[index];
    }
    const std::size_t screenRow = index - m_scrollback.size();
    return screenRow < static_cast<std::size_t>(m_rows) ? &(*m_screen)[screenRow] : nullptr;
}

void Terminal::scrollUp(int top, int bottom, int count, bool toScrollback) {
    const int height = bottom - top + 1;
    count = std::clamp(count, 0, height);
    if (count == 0) {
        return;
    }
    auto& lines = *m_screen;
    const auto first = lines.begin() + top;
    if (toScrollback && top == 0 && !isAlternateScreen()) {
        for (int i = 0; i < count; ++i) {
            // Move the line into the history and take over the buffer of the line that dropped
            // out of it (if any); eraseLines() below refills it in place.
            Line& slot = lines[static_cast<std::size_t>(top + i)];
            pushScrollback(std::move(slot));
            slot = std::move(m_spareLine);
            m_spareLine = Line{};
        }
    }
    std::rotate(first, first + count, lines.begin() + bottom + 1);
    markDirty(top, bottom);
    if (toScrollback && top == 0 && !isAlternateScreen()) {
        // Scrolled lines keep their absolute row; only lines below the region shift.
        if (bottom + 1 < m_rows) {
            touchRows(bottom + 1, m_rows - 1);
        }
    } else {
        touchRows(top, bottom);
    }
    eraseLines(bottom - count + 1, bottom + 1);
}

void Terminal::scrollDown(int top, int bottom, int count) {
    const int height = bottom - top + 1;
    count = std::clamp(count, 0, height);
    if (count == 0) {
        return;
    }
    auto& lines = *m_screen;
    std::rotate(lines.begin() + top, lines.begin() + bottom + 1 - count, lines.begin() + bottom + 1);
    touchRows(top, bottom);
    eraseLines(top, top + count);
}

void Terminal::insertCells(int count) {
    Line& line = currentLine();
    const int col = m_cursor.column;
    count = std::clamp(count, 1, m_columns - col);
    splitWideCharAt(line, col);
    auto& cells = line.cells;
    std::move_backward(cells.begin() + col, cells.end() - count, cells.end());
    std::fill(cells.begin() + col, cells.begin() + col + count, blankCell());
    if (cells.back().width == 2) {
        cells.back() = blankCell(); // its trailing half was pushed off the line
    }
    touchRows(m_cursor.row, m_cursor.row);
}

void Terminal::deleteCells(int count) {
    Line& line = currentLine();
    const int col = m_cursor.column;
    count = std::clamp(count, 1, m_columns - col);
    splitWideCharAt(line, col);
    splitWideCharAt(line, col + count);
    auto& cells = line.cells;
    std::move(cells.begin() + col + count, cells.end(), cells.begin() + col);
    std::fill(cells.end() - count, cells.end(), blankCell());
    touchRows(m_cursor.row, m_cursor.row);
}

// ---------------------------------------------------------------------------------------------
// Selection

void Terminal::startSelection(GridPoint point, SelectionMode mode) {
    m_selection.active = true;
    m_selection.mode = mode;
    m_selection.anchor = point;
    m_selection.extent = point;
    markFullDamage();
    ++m_revision;
}

void Terminal::extendSelection(GridPoint point) {
    if (m_selection.active && !(m_selection.extent == point)) {
        m_selection.extent = point;
        markFullDamage();
        ++m_revision;
    }
}

void Terminal::clearSelection() {
    if (m_selection.active) {
        m_selection.active = false;
        markFullDamage();
        ++m_revision;
    }
}

void Terminal::touchRows(int firstRow, int lastRow) {
    markDirty(firstRow, lastRow);
    if (!m_selection.active) {
        return;
    }
    const std::int64_t top = screenStart();
    const std::int64_t low = std::min(m_selection.anchor.row, m_selection.extent.row);
    const std::int64_t high = std::max(m_selection.anchor.row, m_selection.extent.row);
    if (top + lastRow >= low && top + firstRow <= high) {
        clearSelection();
    }
}

namespace {

enum class CharClass { Blank, Word, Delimiter };

CharClass classify(char32_t ch) {
    if (ch == U' ' || ch == 0) {
        return CharClass::Blank;
    }
    static constexpr std::u32string_view kDelimiters = U"\t\"'`()[]{}<>|;,│";
    return kDelimiters.find(ch) == std::u32string_view::npos ? CharClass::Word : CharClass::Delimiter;
}

CharClass classAt(const Line& line, int column) {
    if (column < 0 || column >= static_cast<int>(line.cells.size())) {
        return CharClass::Blank;
    }
    const Cell* cell = &line.cells[static_cast<std::size_t>(column)];
    if (cell->width == 0 && column > 0) {
        cell = &line.cells[static_cast<std::size_t>(column - 1)]; // trailing half of a wide char
    }
    return cell->isCluster() ? CharClass::Word : classify(cell->ch);
}

} // namespace

bool Terminal::isWordCell(const Line& line, int column) const {
    return classAt(line, column) == CharClass::Word;
}

GridPoint Terminal::wordStart(GridPoint point) const {
    const Line* line = absoluteLine(point.row);
    if (!line) {
        return point;
    }
    const CharClass cls = classAt(*line, point.column);
    if (cls == CharClass::Delimiter) {
        return point;
    }
    for (;;) {
        GridPoint prev{point.row, point.column - 1};
        if (prev.column < 0) {
            const Line* above = absoluteLine(point.row - 1);
            if (!above || !above->wrapped) {
                break;
            }
            prev = {point.row - 1, m_columns - 1};
        }
        if (classAt(*absoluteLine(prev.row), prev.column) != cls) {
            break;
        }
        point = prev;
    }
    return point;
}

GridPoint Terminal::wordEnd(GridPoint point) const {
    const Line* line = absoluteLine(point.row);
    if (!line) {
        return point;
    }
    const CharClass cls = classAt(*line, point.column);
    if (cls == CharClass::Delimiter) {
        return point;
    }
    for (;;) {
        GridPoint next{point.row, point.column + 1};
        if (next.column >= m_columns) {
            const Line* current = absoluteLine(point.row);
            if (!current->wrapped || !absoluteLine(point.row + 1)) {
                break;
            }
            next = {point.row + 1, 0};
        }
        if (classAt(*absoluteLine(next.row), next.column) != cls) {
            break;
        }
        point = next;
    }
    return point;
}

GridPoint Terminal::logicalLineStart(std::int64_t row) const {
    while (const Line* above = absoluteLine(row - 1)) {
        if (!above->wrapped) {
            break;
        }
        --row;
    }
    return {row, 0};
}

GridPoint Terminal::logicalLineEnd(std::int64_t row) const {
    while (const Line* line = absoluteLine(row)) {
        if (!line->wrapped || !absoluteLine(row + 1)) {
            break;
        }
        ++row;
    }
    return {row, m_columns - 1};
}

std::optional<SelectionRange> Terminal::selectionRange() const {
    if (!m_selection.active) {
        return std::nullopt;
    }
    const std::int64_t firstRow = historyStart();
    const std::int64_t lastRow = screenStart() + m_rows - 1;
    auto clampPoint = [&](GridPoint p) {
        p.row = std::clamp(p.row, firstRow, lastRow);
        p.column = std::clamp(p.column, 0, m_columns - 1);
        return p;
    };
    GridPoint a = clampPoint(m_selection.anchor);
    GridPoint b = clampPoint(m_selection.extent);

    if (m_selection.mode == SelectionMode::Block) {
        return SelectionRange{{std::min(a.row, b.row), std::min(a.column, b.column)},
                              {std::max(a.row, b.row), std::max(a.column, b.column)},
                              true};
    }
    if (b < a) {
        std::swap(a, b);
    }
    switch (m_selection.mode) {
    case SelectionMode::Word:
        a = wordStart(a);
        b = wordEnd(b);
        break;
    case SelectionMode::Line:
        a = logicalLineStart(a.row);
        b = logicalLineEnd(b.row);
        break;
    default: break;
    }
    // Never start on the trailing half of a wide character; always include both halves at the end.
    if (const Line* line = absoluteLine(a.row); line && a.column > 0 &&
        line->cells[static_cast<std::size_t>(a.column)].width == 0) {
        --a.column;
    }
    if (const Line* line = absoluteLine(b.row); line && b.column + 1 < m_columns &&
        line->cells[static_cast<std::size_t>(b.column)].width == 2) {
        ++b.column;
    }
    return SelectionRange{a, b, false};
}

std::string Terminal::selectedText() const {
    const auto range = selectionRange();
    if (!range) {
        return {};
    }
    std::string text;
    for (std::int64_t row = range->start.row; row <= range->end.row; ++row) {
        const Line* line = absoluteLine(row);
        if (!line) {
            continue;
        }
        const int from = range->block || row == range->start.row ? range->start.column : 0;
        int to = range->block || row == range->end.row ? range->end.column : m_columns - 1;
        to = std::min(to, static_cast<int>(line->cells.size()) - 1);
        std::string piece;
        for (int column = from; column <= to; ++column) {
            const Cell& cell = line->cells[static_cast<std::size_t>(column)];
            if (cell.width != 0 && !cell.attributes.has(WidePadding)) {
                for (char32_t cp : cellText(cell)) {
                    core::appendUtf8(piece, cp);
                }
            }
        }
        const bool joinNext = !range->block && line->wrapped && row != range->end.row;
        if (!joinNext) {
            piece.erase(piece.find_last_not_of(' ') + 1);
        }
        text += piece;
        if (row != range->end.row && !joinNext) {
            text.push_back('\n');
        }
    }
    return text;
}

// ---------------------------------------------------------------------------------------------
// Cursor movement

void Terminal::setCursor(int row, int column) {
    m_cursor.row = std::clamp(row, 0, m_rows - 1);
    m_cursor.column = std::clamp(column, 0, m_columns - 1);
    m_pendingWrap = false;
}

void Terminal::setCursorOriginRelative(int row, int column) {
    if (m_modes.originMode) {
        setCursor(std::clamp(row + m_scrollTop, m_scrollTop, m_scrollBottom), column);
    } else {
        setCursor(row, column);
    }
}

void Terminal::moveCursorUp(int count) {
    const int limit = m_cursor.row >= m_scrollTop ? m_scrollTop : 0;
    setCursor(std::max(m_cursor.row - count, limit), m_cursor.column);
}

void Terminal::moveCursorDown(int count) {
    const int limit = m_cursor.row <= m_scrollBottom ? m_scrollBottom : m_rows - 1;
    setCursor(std::min(m_cursor.row + count, limit), m_cursor.column);
}

void Terminal::lineFeed() {
    if (m_cursor.row == m_scrollBottom) {
        scrollUp(m_scrollTop, m_scrollBottom, 1, true);
    } else if (m_cursor.row < m_rows - 1) {
        ++m_cursor.row;
    }
    m_pendingWrap = false;
}

void Terminal::reverseIndex() {
    if (m_cursor.row == m_scrollTop) {
        scrollDown(m_scrollTop, m_scrollBottom, 1);
    } else if (m_cursor.row > 0) {
        --m_cursor.row;
    }
    m_pendingWrap = false;
}

void Terminal::resetTabStops(int fromColumn) {
    m_tabStops.resize(static_cast<std::size_t>(m_columns), false);
    for (int col = fromColumn; col < m_columns; ++col) {
        m_tabStops[static_cast<std::size_t>(col)] = col > 0 && col % kTabWidth == 0;
    }
}

void Terminal::tabForward(int count) {
    int col = m_cursor.column;
    for (int i = 0; i < count && col < m_columns - 1; ++i) {
        do {
            ++col;
        } while (col < m_columns - 1 && !m_tabStops[static_cast<std::size_t>(col)]);
    }
    m_cursor.column = col;
    m_pendingWrap = false;
}

void Terminal::tabBackward(int count) {
    int col = m_cursor.column;
    for (int i = 0; i < count && col > 0; ++i) {
        do {
            --col;
        } while (col > 0 && !m_tabStops[static_cast<std::size_t>(col)]);
    }
    m_cursor.column = col;
    m_pendingWrap = false;
}

void Terminal::saveCursor() {
    SavedCursor saved;
    saved.row = m_cursor.row;
    saved.column = m_cursor.column;
    saved.attributes = m_attributes;
    saved.originMode = m_modes.originMode;
    saved.pendingWrap = m_pendingWrap;
    saved.charsets[0] = m_charsets[0];
    saved.charsets[1] = m_charsets[1];
    saved.activeCharset = m_activeCharset;
    m_saved[isAlternateScreen() ? 1 : 0] = saved;
}

void Terminal::restoreCursor() {
    const auto& saved = m_saved[isAlternateScreen() ? 1 : 0];
    if (!saved) {
        m_attributes = {};
        m_modes.originMode = false;
        setCursor(0, 0);
        return;
    }
    m_attributes = saved->attributes;
    m_modes.originMode = saved->originMode;
    m_charsets[0] = saved->charsets[0];
    m_charsets[1] = saved->charsets[1];
    m_activeCharset = saved->activeCharset;
    setCursor(saved->row, saved->column);
    m_pendingWrap = saved->pendingWrap;
}

void Terminal::switchScreen(bool alternate, bool clear) {
    if (alternate == isAlternateScreen()) {
        return;
    }
    clearSelection();
    markFullDamage();
    m_screen = alternate ? &m_alternate : &m_primary;
    if (alternate && clear) {
        eraseLines(0, m_rows);
    }
    // The alternate screen's images do not survive it.
    m_images.erase(std::remove_if(m_images.begin(), m_images.end(), [](const Image& image) { return image.alternate; }),
                   m_images.end());
    m_pendingWrap = false;
}

// ---------------------------------------------------------------------------------------------
// Sixel images

std::vector<const Terminal::Image*> Terminal::visibleImages() const {
    std::vector<const Image*> images;
    const bool alternate = isAlternateScreen();
    for (const Image& image : m_images) {
        if (image.alternate == alternate) {
            images.push_back(&image);
        }
    }
    // Lower z-indices first; equal ones in the order they were placed.
    std::stable_sort(images.begin(), images.end(), [](const Image* a, const Image* b) { return a->z < b->z; });
    return images;
}

void Terminal::placeSixel(const CsiParams& params, std::string_view data) {
    SixelOptions options;
    options.transparentBackground = params.raw(1) == 1; // P2: 1 = leave unset pixels transparent
    options.background = 0xFF000000u | m_palette.background;
    auto bitmap = decodeSixel(data, options);
    if (!bitmap) {
        return;
    }
    const int cellWidth = m_cellPixelWidth > 0 ? m_cellPixelWidth : 10;
    const int cellHeight = m_cellPixelHeight > 0 ? m_cellPixelHeight : 20;
    Image image;
    image.id = m_nextImageId++;
    image.columns = std::min(m_columns, (bitmap->width + cellWidth - 1) / cellWidth);
    image.rows = (bitmap->height + cellHeight - 1) / cellHeight;
    image.sourceWidth = image.pixelWidth = bitmap->width;
    image.sourceHeight = image.pixelHeight = bitmap->height;
    image.bitmap = std::make_shared<const Bitmap>(std::move(*bitmap));
    image.cellWidth = cellWidth;
    image.cellHeight = cellHeight;
    image.alternate = isAlternateScreen();
    if (m_modes.sixelDisplayMode) {
        // DECSDM: the image goes to the top-left corner and the cursor stays where it is.
        image.row = screenStart();
        image.column = 0;
    } else {
        // Sixel scrolling: the image starts at the cursor, which ends up on the line below it
        // (scrolling the screen as needed), in the image's first column.
        if (m_pendingWrap) {
            m_pendingWrap = false;
        }
        image.row = screenStart() + m_cursor.row;
        image.column = std::min(m_cursor.column, std::max(0, m_columns - image.columns));
        for (int i = 0; i < image.rows; ++i) {
            lineFeed();
        }
        m_cursor.column = image.column;
    }
    m_images.push_back(std::move(image));
    pruneImages();
    markFullDamage();
}

void Terminal::pruneImages() {
    constexpr std::size_t kMaxImages = 512;
    constexpr std::size_t kMaxPixels = 64u * 1024 * 1024; // 256 MB of RGBA
    const std::int64_t oldest = historyStart();
    m_images.erase(std::remove_if(m_images.begin(), m_images.end(),
                                  [oldest](const Image& image) { return image.row + image.rows <= oldest; }),
                   m_images.end());
    std::size_t pixels = 0;
    for (const Image& image : m_images) {
        pixels += image.bitmap->pixels.size();
    }
    while (!m_images.empty() && (m_images.size() > kMaxImages || pixels > kMaxPixels)) {
        pixels -= m_images.front().bitmap->pixels.size(); // the oldest goes first
        m_images.erase(m_images.begin());
    }
}

void Terminal::eraseImages(std::int64_t firstRow, std::int64_t lastRow) {
    const bool alternate = isAlternateScreen();
    const auto before = m_images.size();
    m_images.erase(std::remove_if(m_images.begin(), m_images.end(),
                                  [&](const Image& image) {
                                      return image.alternate == alternate && image.row <= lastRow &&
                                             image.row + image.rows - 1 >= firstRow;
                                  }),
                   m_images.end());
    if (m_images.size() != before) {
        markFullDamage();
    }
}

void Terminal::graphicsAttributes(const CsiParams& params) {
    // XTSMGRAPHICS: CSI ? Pi ; Pa ; Pv S. Pi 1 = colour registers, 2 = sixel geometry; Pa 1 =
    // read, 4 = read the maximum. Settings are not changeable: they are reported back unchanged.
    const int item = params.raw(0);
    const int action = params.raw(1);
    if (item == 1) {
        respond(action >= 1 && action <= 4 ? "\x1b[?1;0;256S" : "\x1b[?1;2S");
    } else if (item == 2) {
        const int cellWidth = m_cellPixelWidth > 0 ? m_cellPixelWidth : 10;
        const int cellHeight = m_cellPixelHeight > 0 ? m_cellPixelHeight : 20;
        const bool maximum = action == 4;
        const int width = maximum ? 4096 : std::min(4096, m_columns * cellWidth);
        const int height = maximum ? 4096 : std::min(4096, m_rows * cellHeight);
        respond(action >= 1 && action <= 4 ? "\x1b[?2;0;" + std::to_string(width) + ";" + std::to_string(height) + "S"
                                           : "\x1b[?2;2S");
    } else {
        respond("\x1b[?" + std::to_string(item) + ";1S"); // unknown item
    }
}

// ---------------------------------------------------------------------------------------------
// Kitty graphics protocol

namespace {

constexpr std::size_t kMaxKittyData = 64u * 1024 * 1024; // decoded bytes of one transmission
constexpr std::size_t kKittyQuota = 256u * 1024 * 1024;  // stored image data (ARGB)
constexpr std::size_t kMaxKittyImages = 1024;
constexpr int kMaxKittyCells = 1000; // columns / rows of one placement

bool appendBase64(std::string& out, std::string_view chunk) {
    auto decoded = core::base64Decode(chunk);
    if (!decoded || out.size() + decoded->size() > kMaxKittyData) {
        return false;
    }
    out += *decoded;
    return true;
}

int ceilDiv(std::int64_t value, int divisor) {
    return static_cast<int>((value + divisor - 1) / divisor);
}

} // namespace

void Terminal::apcDispatch(std::string_view data) {
    if (!data.empty() && data.front() == 'G') {
        kittyGraphics(data.substr(1));
    }
}

void Terminal::resetKittyGraphics() {
    m_kittyImages.clear();
    m_kittyPending.reset();
    m_kittyData.clear();
    m_kittyDataBad = false;
}

void Terminal::kittyGraphics(std::string_view data) {
    auto command = parseKittyCommand(data);
    if (!command) {
        return;
    }
    if (m_kittyPending) {
        // A continuation chunk: only its data, m and q count.
        if (!m_kittyDataBad && !appendBase64(m_kittyData, command->payload)) {
            m_kittyDataBad = true;
            m_kittyData.clear();
        }
        if (command->quiet) {
            m_kittyPending->quiet = command->quiet;
        }
        if (command->more) {
            return;
        }
        KittyCommand first = std::move(*m_kittyPending);
        m_kittyPending.reset();
        std::string collected = std::move(m_kittyData);
        m_kittyData.clear();
        if (std::exchange(m_kittyDataBad, false)) {
            kittyRespond(first, first.id, "EINVAL:invalid or too much data");
            return;
        }
        kittyExecute(first, std::move(collected));
        return;
    }
    std::string decoded;
    const bool transmits = command->action == 't' || command->action == 'T' || command->action == 'q';
    const bool good = !transmits || appendBase64(decoded, command->payload);
    if (transmits && command->more) {
        m_kittyPending = std::move(*command);
        m_kittyPending->payload.clear();
        m_kittyData = std::move(decoded);
        m_kittyDataBad = !good;
        return;
    }
    if (!good) {
        kittyRespond(*command, command->id, "EINVAL:invalid or too much data");
        return;
    }
    kittyExecute(*command, std::move(decoded));
}

void Terminal::kittyExecute(const KittyCommand& command, std::string data) {
    switch (command.action) {
    case 't':
    case 'T':
    case 'q': {
        if (command.medium != 'd') {
            // Files and shared memory would let a remote program show local files.
            kittyRespond(command, command.id, "EINVAL:only direct transmission (t=d) is supported");
            return;
        }
        auto decoded = decodeKittyImage(command, std::move(data), m_kittyDecoders);
        if (!decoded.bitmap) {
            kittyRespond(command, command.id, decoded.error);
            return;
        }
        if (command.action == 'q') {
            kittyRespond(command, command.id, ""); // supported; nothing is stored
            return;
        }
        std::uint32_t id = command.id;
        if (id == 0) {
            do {
                id = m_nextKittyId++;
                if (m_nextKittyId == 0) {
                    m_nextKittyId = 0x80000000u;
                }
            } while (m_kittyImages.count(id) != 0);
        }
        // New data for an id replaces the image and its placements.
        m_images.erase(std::remove_if(m_images.begin(), m_images.end(),
                                      [id](const Image& image) { return image.kittyImage == id; }),
                       m_images.end());
        m_kittyImages[id] = {command.number, std::make_shared<const Bitmap>(std::move(*decoded.bitmap)), ++m_kittySerial,
                             command.id == 0 && command.number == 0};
        kittyEnforceQuota();
        std::string error;
        if (command.action == 'T') {
            error = kittyPut(command, id);
        }
        kittyRespond(command, id, error);
        markFullDamage();
        return;
    }
    case 'p': {
        std::uint32_t id = command.id;
        if (id == 0 && command.number != 0) {
            id = kittyNewestWithNumber(command.number);
        }
        if (id == 0 && command.number == 0) {
            return; // nothing to address, nobody to tell
        }
        if (m_kittyImages.count(id) == 0) {
            kittyRespond(command, id, "ENOENT:no such image");
            return;
        }
        kittyRespond(command, id, kittyPut(command, id));
        return;
    }
    case 'd': kittyDelete(command); return;
    case 'f':
    case 'a':
    case 'c': kittyRespond(command, command.id, "EINVAL:animation is not supported"); return;
    default: kittyRespond(command, command.id, "EINVAL:unknown action"); return;
    }
}

std::string Terminal::kittyPut(const KittyCommand& command, std::uint32_t imageId) {
    if (command.unicodePlaceholder) {
        return "EINVAL:Unicode placeholders are not supported";
    }
    const KittyImage& stored = m_kittyImages.at(imageId);
    const Bitmap& bitmap = *stored.bitmap;
    const int cellWidth = m_cellPixelWidth > 0 ? m_cellPixelWidth : 10;
    const int cellHeight = m_cellPixelHeight > 0 ? m_cellPixelHeight : 20;

    Image image;
    image.sourceX = std::min(command.x, bitmap.width);
    image.sourceY = std::min(command.y, bitmap.height);
    image.sourceWidth = command.w > 0 ? std::min(command.w, bitmap.width - image.sourceX) : bitmap.width - image.sourceX;
    image.sourceHeight =
        command.h > 0 ? std::min(command.h, bitmap.height - image.sourceY) : bitmap.height - image.sourceY;
    if (image.sourceWidth <= 0 || image.sourceHeight <= 0) {
        return "EINVAL:empty source rectangle";
    }
    image.offsetX = std::min(command.offsetX, cellWidth - 1);
    image.offsetY = std::min(command.offsetY, cellHeight - 1);
    const int columns = std::min(command.columns, kMaxKittyCells);
    const int rows = std::min(command.rows, kMaxKittyCells);
    const std::int64_t sw = image.sourceWidth;
    const std::int64_t sh = image.sourceHeight;
    if (columns > 0 && rows > 0) { // scaled to fill the cells
        image.columns = columns;
        image.rows = rows;
        image.pixelWidth = columns * cellWidth - image.offsetX;
        image.pixelHeight = rows * cellHeight - image.offsetY;
    } else if (columns > 0) { // the other side keeps the aspect ratio
        image.columns = columns;
        image.pixelWidth = columns * cellWidth - image.offsetX;
        image.pixelHeight = static_cast<int>(std::max<std::int64_t>(1, sh * image.pixelWidth / sw));
        image.rows = std::min(kMaxKittyCells, ceilDiv(image.pixelHeight + image.offsetY, cellHeight));
    } else if (rows > 0) {
        image.rows = rows;
        image.pixelHeight = rows * cellHeight - image.offsetY;
        image.pixelWidth = static_cast<int>(std::max<std::int64_t>(1, sw * image.pixelHeight / sh));
        image.columns = std::min(kMaxKittyCells, ceilDiv(image.pixelWidth + image.offsetX, cellWidth));
    } else {
        image.pixelWidth = image.sourceWidth;
        image.pixelHeight = image.sourceHeight;
        image.columns = std::min(kMaxKittyCells, ceilDiv(sw + image.offsetX, cellWidth));
        image.rows = std::min(kMaxKittyCells, ceilDiv(sh + image.offsetY, cellHeight));
    }
    if (command.placement != 0) { // the same placement id moves the placement
        m_images.erase(std::remove_if(m_images.begin(), m_images.end(),
                                      [&](const Image& placed) {
                                          return placed.kittyImage == imageId &&
                                                 placed.kittyPlacement == command.placement;
                                      }),
                       m_images.end());
    }
    image.id = m_nextImageId++;
    image.bitmap = stored.bitmap;
    image.row = screenStart() + m_cursor.row;
    image.column = m_cursor.column;
    image.cellWidth = cellWidth;
    image.cellHeight = cellHeight;
    image.alternate = isAlternateScreen();
    image.kittyImage = imageId;
    image.kittyPlacement = command.placement;
    image.z = command.z;
    const int placedRows = image.rows;
    const int placedColumns = image.columns;
    m_images.push_back(std::move(image));
    if (!command.keepCursor) {
        // The cursor goes to the image's last row, after its last column.
        m_pendingWrap = false;
        for (int i = 1; i < placedRows; ++i) {
            lineFeed();
        }
        const int column = m_cursor.column + placedColumns;
        if (column >= m_columns) {
            m_cursor.column = 0;
            lineFeed();
        } else {
            m_cursor.column = column;
        }
    }
    pruneImages();
    markFullDamage();
    return {};
}

void Terminal::kittyDelete(const KittyCommand& command) {
    const char what = command.deleteWhat;
    const bool freeData = what >= 'A' && what <= 'Z';
    const char kind = static_cast<char>(freeData ? what - 'A' + 'a' : what);
    const bool alternate = isAlternateScreen();
    const std::int64_t top = screenStart();
    const std::int64_t cellRow = top + command.y - 1; // d=p/q/y: 1-based screen coordinates
    const int cellColumn = command.x - 1;
    const std::uint32_t id = kind == 'n' ? kittyNewestWithNumber(command.number) : command.id;
    auto covers = [](const Image& image, std::int64_t row, int column) {
        return row >= image.row && row < image.row + image.rows && column >= image.column &&
               column < image.column + image.columns;
    };
    auto inRange = [&](std::uint32_t imageId) {
        return imageId >= static_cast<std::uint32_t>(command.x) && imageId <= static_cast<std::uint32_t>(command.y);
    };
    auto matches = [&](const Image& image) {
        if (image.kittyImage == 0) {
            return false; // sixel images are not addressed by kitty commands
        }
        const bool shown = image.alternate == alternate;
        switch (kind) {
        case 'a': return shown;
        case 'i':
        case 'n':
            return image.kittyImage == id && (command.placement == 0 || image.kittyPlacement == command.placement);
        case 'c': return shown && covers(image, top + m_cursor.row, m_cursor.column);
        case 'p': return shown && covers(image, cellRow, cellColumn);
        case 'q': return shown && covers(image, cellRow, cellColumn) && image.z == command.z;
        case 'x': return shown && cellColumn >= image.column && cellColumn < image.column + image.columns;
        case 'y': return shown && cellRow >= image.row && cellRow < image.row + image.rows;
        case 'z': return shown && image.z == command.z;
        case 'r': return inRange(image.kittyImage);
        default: return false; // 'f' (animation frames) and unknown kinds
        }
    };
    const auto before = m_images.size();
    m_images.erase(std::remove_if(m_images.begin(), m_images.end(), matches), m_images.end());
    // Upper case also frees the data of the addressed images that are no longer shown; images
    // without an id cannot be shown again anyway.
    for (auto it = m_kittyImages.begin(); it != m_kittyImages.end();) {
        const std::uint32_t imageId = it->first;
        const bool addressed = ((kind == 'i' || kind == 'n') && imageId == id) || (kind == 'r' && inRange(imageId)) ||
                               (kind != 'i' && kind != 'n' && kind != 'r');
        const bool placed = std::any_of(m_images.begin(), m_images.end(),
                                        [imageId](const Image& image) { return image.kittyImage == imageId; });
        it = ((freeData && addressed) || it->second.anonymous) && !placed ? m_kittyImages.erase(it) : std::next(it);
    }
    if (m_images.size() != before) {
        markFullDamage();
    }
}

std::uint32_t Terminal::kittyNewestWithNumber(std::uint32_t number) const {
    std::uint32_t id = 0;
    std::uint64_t newest = 0;
    for (const auto& [imageId, image] : m_kittyImages) {
        if (image.number == number && image.serial > newest) {
            newest = image.serial;
            id = imageId;
        }
    }
    return id;
}

void Terminal::kittyEnforceQuota() {
    auto bytes = [this] {
        std::size_t total = 0;
        for (const auto& entry : m_kittyImages) {
            total += entry.second.bitmap->pixels.size() * sizeof(std::uint32_t);
        }
        return total;
    };
    auto placed = [this](std::uint32_t id) {
        return std::any_of(m_images.begin(), m_images.end(), [id](const Image& image) { return image.kittyImage == id; });
    };
    // Evict the oldest images, those that are not shown first. The image transmitted last (the
    // newest serial) is never evicted, or a transmission could vanish right away.
    while (m_kittyImages.size() > kMaxKittyImages || bytes() > kKittyQuota) {
        auto victim = m_kittyImages.end();
        bool victimPlaced = true;
        for (auto it = m_kittyImages.begin(); it != m_kittyImages.end(); ++it) {
            if (it->second.serial == m_kittySerial) {
                continue;
            }
            const bool itPlaced = placed(it->first);
            if (victim == m_kittyImages.end() || (victimPlaced && !itPlaced) ||
                (itPlaced == victimPlaced && it->second.serial < victim->second.serial)) {
                victim = it;
                victimPlaced = itPlaced;
            }
        }
        if (victim == m_kittyImages.end()) {
            break; // only the new image is left (it is within the per-image limits)
        }
        const std::uint32_t id = victim->first;
        m_images.erase(std::remove_if(m_images.begin(), m_images.end(),
                                      [id](const Image& image) { return image.kittyImage == id; }),
                       m_images.end());
        m_kittyImages.erase(victim);
    }
}

void Terminal::kittyRespond(const KittyCommand& command, std::uint32_t imageId, std::string_view error) {
    // Only commands that name an image get answers, and q=1 / q=2 silence them.
    if ((command.id == 0 && command.number == 0) || command.quiet >= 2 || (error.empty() && command.quiet == 1)) {
        return;
    }
    std::string reply = "\x1b_Gi=" + std::to_string(imageId != 0 ? imageId : command.id);
    if (command.number != 0) {
        reply += ",I=" + std::to_string(command.number);
    }
    if (command.placement != 0) {
        reply += ",p=" + std::to_string(command.placement);
    }
    reply += ';';
    reply += error.empty() ? std::string_view("OK") : error;
    reply += "\x1b\\";
    respond(reply);
}

void Terminal::respond(std::string_view bytes) {
    if (m_onResponse) {
        m_onResponse(bytes);
    }
}

// ---------------------------------------------------------------------------------------------
// VtHandler

void Terminal::print(char32_t cp) {
    if (cp < 0x80 && m_charsets[m_activeCharset] == Charset::DecSpecialGraphics) {
        cp = decSpecialGraphics(cp);
    }
    if (m_modes.graphemeClusters && cp >= 0x80 && joinCluster(cp)) {
        return;
    }
    int width = core::charWidth(cp);
    if (width == 0) {
        appendCombining(cp);
        return;
    }
    if (width == 2 && m_columns < 2) {
        width = 1;
    }

    if (m_pendingWrap && m_modes.autoWrap) {
        currentLine().wrapped = true;
        m_cursor.column = 0;
        lineFeed();
    }
    m_pendingWrap = false;

    if (width == 2 && m_cursor.column == m_columns - 1) {
        if (m_modes.autoWrap) {
            eraseCells(m_cursor.row, m_cursor.column, m_columns);
            Cell& padding = currentLine().cells[static_cast<std::size_t>(m_columns - 1)];
            padding.attributes.flags = static_cast<std::uint16_t>(padding.attributes.flags | WidePadding);
            currentLine().wrapped = true;
            m_cursor.column = 0;
            lineFeed();
        } else {
            m_cursor.column = m_columns - 2;
        }
    }

    if (m_modes.insertMode) {
        insertCells(width);
    }

    Line& line = currentLine();
    const int col = m_cursor.column;
    touchRows(m_cursor.row, m_cursor.row);
    splitWideCharAt(line, col);
    if (width == 2) {
        splitWideCharAt(line, col + 1);
    }
    line.cells[static_cast<std::size_t>(col)] = Cell{cp, m_attributes, static_cast<std::uint8_t>(width)};
    if (width == 2) {
        line.cells[static_cast<std::size_t>(col + 1)] = Cell{0, m_attributes, 0};
    }
    m_lastPrinted = cp;

    if (col + width >= m_columns) {
        m_cursor.column = m_columns - 1;
        m_pendingWrap = m_modes.autoWrap;
    } else {
        m_cursor.column = col + width;
    }
}

void Terminal::printAscii(std::string_view text) {
    if (m_charsets[m_activeCharset] != Charset::Ascii || m_modes.insertMode || m_columns < 2) {
        VtHandler::printAscii(text); // line drawing / insert mode: the general path
        return;
    }
    int touchedRow = -1;
    for (char c : text) {
        if (m_pendingWrap && m_modes.autoWrap) {
            currentLine().wrapped = true;
            m_cursor.column = 0;
            lineFeed();
        }
        m_pendingWrap = false;
        if (m_cursor.row != touchedRow) {
            touchedRow = m_cursor.row;
            touchRows(touchedRow, touchedRow);
        }
        Line& line = currentLine();
        const int col = m_cursor.column;
        Cell& cell = line.cells[static_cast<std::size_t>(col)];
        if (cell.width != 1) {
            splitWideCharAt(line, col); // overwriting half of a wide character
        }
        cell = Cell{static_cast<char32_t>(static_cast<unsigned char>(c)), m_attributes, 1};
        if (col + 1 >= m_columns) {
            m_cursor.column = m_columns - 1;
            m_pendingWrap = m_modes.autoWrap;
        } else {
            m_cursor.column = col + 1;
        }
    }
    if (!text.empty()) {
        m_lastPrinted = static_cast<unsigned char>(text.back());
    }
}

void Terminal::appendCombining(char32_t mark) {
    // Attach to the most recently printed cell: the cursor cell while a wrap is pending,
    // otherwise the cell left of the cursor.
    int col = m_pendingWrap ? m_cursor.column : m_cursor.column - 1;
    if (col < 0) {
        return;
    }
    Line& line = currentLine();
    if (line.cells[static_cast<std::size_t>(col)].width == 0 && col > 0) {
        --col;
    }
    Cell& cell = line.cells[static_cast<std::size_t>(col)];
    if (cell.width == 0) {
        return;
    }
    std::u32string text(cellText(cell));
    if (text.size() >= kMaxClusterLength) {
        return;
    }
    text.push_back(mark);
    if (const auto id = internCluster(text)) {
        cell.ch = kClusterBase + *id;
        touchRows(m_cursor.row, m_cursor.row);
    }
    // Mode 2027: VARIATION SELECTOR-16 asks for emoji presentation, which is two cells wide.
    if (m_modes.graphemeClusters && mark == 0xFE0F) {
        widenCell(col);
    }
}

void Terminal::widenCell(int col) {
    Line& line = currentLine();
    Cell& cell = line.cells[static_cast<std::size_t>(col)];
    // Only the cell just printed, with room to its right (at the margin it stays narrow).
    if (cell.width != 1 || m_pendingWrap || col + 1 != m_cursor.column || col + 1 >= m_columns) {
        return;
    }
    splitWideCharAt(line, col + 1);
    cell.width = 2;
    line.cells[static_cast<std::size_t>(col + 1)] = Cell{0, cell.attributes, 0};
    if (col + 2 >= m_columns) {
        m_cursor.column = m_columns - 1;
        m_pendingWrap = m_modes.autoWrap;
    } else {
        m_cursor.column = col + 2;
    }
    touchRows(m_cursor.row, m_cursor.row);
}

namespace {

bool isRegionalIndicator(char32_t cp) {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

bool isEmojiModifier(char32_t cp) {
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

// Extended_Pictographic, approximated by the blocks that hold the emoji.
bool isPictographic(char32_t cp) {
    return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2300 && cp <= 0x23FF) ||
           (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0x2190 && cp <= 0x21FF) || cp == 0x00A9 || cp == 0x00AE ||
           cp == 0x203C || cp == 0x2049 || cp == 0x2122 || cp == 0x2139 || cp == 0x24C2 || cp == 0x3030 ||
           cp == 0x303D || cp == 0x3297 || cp == 0x3299 || (cp >= 0x25AA && cp <= 0x25FE) || cp == 0x2934 ||
           cp == 0x2935;
}

} // namespace

bool Terminal::joinCluster(char32_t cp) {
    int col = m_pendingWrap ? m_cursor.column : m_cursor.column - 1;
    if (col < 0) {
        return false;
    }
    Line& line = currentLine();
    if (line.cells[static_cast<std::size_t>(col)].width == 0 && col > 0) {
        --col; // the right half of a wide character
    }
    Cell& cell = line.cells[static_cast<std::size_t>(col)];
    if (cell.width == 0 || cell.ch == 0) {
        return false;
    }
    std::u32string text(cellText(cell));
    if (text.empty() || text.size() >= kMaxClusterLength) {
        return false;
    }
    const char32_t first = text.front();
    const char32_t last = text.back();
    const bool join = (last == 0x200D && isPictographic(cp)) ||                         // ZWJ sequence
                      (isEmojiModifier(cp) && isPictographic(first) && !isEmojiModifier(last)) || // skin tone
                      (isRegionalIndicator(cp) && text.size() == 1 && isRegionalIndicator(first)); // flag
    if (!join) {
        return false;
    }
    text.push_back(cp);
    const auto id = internCluster(text);
    if (!id) {
        return false;
    }
    cell.ch = kClusterBase + *id;
    m_lastPrinted = cp;
    touchRows(m_cursor.row, m_cursor.row);
    widenCell(col); // a flag made of two (narrow) regional indicators is an emoji: two cells
    return true;
}

void Terminal::execute(std::uint8_t control) {
    switch (control) {
    case 0x07: // BEL
        if (m_onBell) {
            m_onBell();
        }
        break;
    case 0x08: // BS
        if (m_cursor.column > 0) {
            --m_cursor.column;
        }
        m_pendingWrap = false;
        break;
    case 0x09: // HT
        tabForward(1);
        break;
    case 0x0A: // LF
    case 0x0B: // VT
    case 0x0C: // FF
        lineFeed();
        if (m_modes.newLineMode) {
            m_cursor.column = 0;
        }
        break;
    case 0x0D: // CR
        m_cursor.column = 0;
        m_pendingWrap = false;
        break;
    case 0x0E: // SO: invoke G1
        m_activeCharset = 1;
        break;
    case 0x0F: // SI: invoke G0
        m_activeCharset = 0;
        break;
    default:
        break;
    }
}

void Terminal::escDispatch(std::string_view intermediates, char final) {
    if (intermediates.empty()) {
        switch (final) {
        case '7': saveCursor(); break;
        case '8': restoreCursor(); break;
        case 'D': lineFeed(); break; // IND
        case 'E':                    // NEL
            m_cursor.column = 0;
            lineFeed();
            break;
        case 'H': m_tabStops[static_cast<std::size_t>(m_cursor.column)] = true; break; // HTS
        case 'M': reverseIndex(); break;                                               // RI
        case 'c': reset(); break;                                                      // RIS
        case '=': m_modes.keypadApplication = true; break;                             // DECKPAM
        case '>': m_modes.keypadApplication = false; break;                            // DECKPNM
        default: break;
        }
        return;
    }
    if (intermediates.size() == 1) {
        const char i = intermediates[0];
        if (i == '(' || i == ')') { // SCS: designate G0 / G1
            m_charsets[i == '(' ? 0 : 1] = final == '0' ? Charset::DecSpecialGraphics : Charset::Ascii;
        } else if (i == '#' && final >= '3' && final <= '6') { // DECDHL top/bottom, DECSWL, DECDWL
            static constexpr LineSize kSizes[] = {LineSize::DoubleHeightTop, LineSize::DoubleHeightBottom,
                                                  LineSize::Normal, LineSize::DoubleWidth};
            currentLine().size = kSizes[final - '3'];
            touchRows(m_cursor.row, m_cursor.row);
        } else if (i == '#' && final == '8') { // DECALN screen alignment test
            m_attributes = {};
            for (auto& line : *m_screen) {
                line = blankLine();
                for (auto& cell : line.cells) {
                    cell.ch = U'E';
                }
            }
            touchRows(0, m_rows - 1);
            m_scrollTop = 0;
            m_scrollBottom = m_rows - 1;
            setCursor(0, 0);
        }
    }
}

void Terminal::setTitle(std::string title) {
    m_title = std::move(title);
    if (m_onTitle) {
        m_onTitle(m_title);
    }
}

void Terminal::oscDispatch(std::string_view payload, bool bellTerminated) {
    const auto semicolon = payload.find(';');
    const auto command = parseNumber(payload.substr(0, semicolon));
    if (!command) {
        return;
    }
    const std::string_view value =
        semicolon == std::string_view::npos ? std::string_view() : payload.substr(semicolon + 1);
    switch (*command) {
    case 0:
    case 2:
        if (semicolon != std::string_view::npos) {
            setTitle(std::string(value));
        }
        break;
    case 4:
    case 10:
    case 11:
    case 12:
    case 104:
    case 110:
    case 111:
    case 112: handleColorOsc(*command, value, bellTerminated); break;
    case 7: { // file://host/path
        constexpr std::string_view kScheme = "file://";
        if (value.substr(0, kScheme.size()) == kScheme) {
            const auto slash = value.find('/', kScheme.size());
            if (slash != std::string_view::npos) {
                m_workingDirectory = percentDecode(value.substr(slash));
            }
        }
        break;
    }
    case 133: { // FinalTerm shell integration: A prompt, B command start, C executing, D done
        if (value.empty() || m_screen == &m_alternate) {
            break;
        }
        const GridPoint here{screenStart() + m_cursor.row, m_cursor.column};
        switch (value.front()) {
        case 'A':
            m_shellMarks.promptStart = here;
            m_shellMarks.commandStart.reset();
            break;
        case 'B': m_shellMarks.commandStart = here; break;
        case 'C': m_shellMarks.commandRunning = true; break;
        case 'D': {
            m_shellMarks.commandRunning = false;
            const auto exitSeparator = value.find(';');
            if (exitSeparator != std::string_view::npos) {
                if (const auto code = parseNumber(value.substr(exitSeparator + 1))) {
                    m_shellMarks.lastExitCode = static_cast<int>(*code);
                }
            }
            break;
        }
        default: break;
        }
        break;
    }
    case 8:
        if (semicolon != std::string_view::npos) {
            handleHyperlinkOsc(value);
        }
        break;
    case 52: handleClipboardOsc(value); break;
    default: break; // OSC 1 (icon name) and others are ignored
    }
}

void Terminal::handleHyperlinkOsc(std::string_view value) {
    // OSC 8 ; params ; URI  -- an empty URI ends the link. Only the "id" parameter matters: cells
    // with the same id and URI belong to one link even when written separately.
    const auto semicolon = value.find(';');
    if (semicolon == std::string_view::npos) {
        return;
    }
    const std::string_view params = value.substr(0, semicolon);
    const std::string_view uri = value.substr(semicolon + 1);
    if (uri.empty() || uri.size() > kMaxHyperlinkLength) {
        m_attributes.hyperlink = 0;
        return;
    }
    std::string_view id;
    for (std::string_view rest = params; !rest.empty();) {
        const auto colon = rest.find(':');
        const std::string_view param = rest.substr(0, colon);
        if (param.substr(0, 3) == "id=") {
            id = param.substr(3);
        }
        rest.remove_prefix(colon == std::string_view::npos ? rest.size() : colon + 1);
    }
    if (id.size() > kMaxHyperlinkIdLength) {
        id = id.substr(0, kMaxHyperlinkIdLength);
    }
    std::string key;
    key.reserve(id.size() + uri.size() + 1);
    key.append(id).push_back('\n');
    key.append(uri);
    if (const auto it = m_hyperlinkIds.find(key); it != m_hyperlinkIds.end()) {
        m_attributes.hyperlink = it->second;
        return;
    }
    if (m_hyperlinks.size() >= kMaxHyperlinks || m_hyperlinkBytes + key.size() > kMaxHyperlinkBytes) {
        m_attributes.hyperlink = 0; // table full: the text stays, the link is dropped
        return;
    }
    m_hyperlinkBytes += key.size();
    m_hyperlinks.push_back(key);
    const auto newId = static_cast<std::uint16_t>(m_hyperlinks.size());
    m_hyperlinkIds.emplace(std::move(key), newId);
    m_attributes.hyperlink = newId;
}

void Terminal::handleClipboardOsc(std::string_view value) {
    // OSC 52 ; Pc ; Pd  -- Pd is base64 text, or "?" to query (never answered).
    const auto semicolon = value.find(';');
    if (semicolon == std::string_view::npos || !m_clipboardWriteAllowed || !m_onClipboard) {
        return;
    }
    const std::string_view targets = value.substr(0, semicolon);
    const std::string_view data = value.substr(semicolon + 1);
    if (data == "?") {
        return;
    }
    const auto text = core::base64Decode(data);
    if (!text) {
        return;
    }
    const bool primary = !targets.empty() && targets.find('c') == std::string_view::npos &&
                         (targets.find('p') != std::string_view::npos || targets.find('s') != std::string_view::npos);
    m_onClipboard(primary ? 'p' : 'c', *text);
}

// ---------------------------------------------------------------------------------------------
// DCS

namespace {

std::string colorSgr(const Color& color, int base) {
    switch (color.kind) {
    case Color::Kind::Default: return {};
    case Color::Kind::Indexed:
        if (base != 58 && color.index < 8) {
            return std::to_string(base - 8 + color.index);
        }
        if (base != 58 && color.index < 16) {
            return std::to_string((base == 38 ? 90 : 100) + color.index - 8);
        }
        return std::to_string(base) + ":5:" + std::to_string(color.index);
    case Color::Kind::Rgb:
        return std::to_string(base) + ":2::" + std::to_string(color.r) + ":" + std::to_string(color.g) + ":" +
               std::to_string(color.b);
    }
    return {};
}

// SGR parameters reproducing `a` (used by DECRQSS), always starting with a reset.
std::string sgrString(const CellAttributes& a) {
    std::string out = "0";
    auto add = [&out](const std::string& part) {
        if (!part.empty()) {
            out += ";" + part;
        }
    };
    if (a.has(Bold)) add("1");
    if (a.has(Dim)) add("2");
    if (a.has(Italic)) add("3");
    switch (a.underlineStyle()) {
    case UnderlineStyle::None: break;
    case UnderlineStyle::Single: add("4"); break;
    default: add("4:" + std::to_string(static_cast<int>(a.underlineStyle()))); break;
    }
    if (a.has(Blink)) add("5");
    if (a.has(Inverse)) add("7");
    if (a.has(Hidden)) add("8");
    if (a.has(Strikethrough)) add("9");
    if (a.has(Overline)) add("53");
    add(colorSgr(a.foreground, 38));
    add(colorSgr(a.background, 48));
    add(colorSgr(a.underlineColor, 58));
    return out;
}

} // namespace

void Terminal::dcsDispatch(const CsiParams& params, char prefix, std::string_view intermediates, char final,
                           std::string_view data) {
    (void)params;
    if (prefix != 0) {
        return;
    }
    if (intermediates == "$" && final == 'q') {
        requestStatusString(data);
    } else if (intermediates == "+" && final == 'q') {
        requestTermcap(data);
    } else if (intermediates.empty() && final == 'q') {
        placeSixel(params, data);
    }
}

void Terminal::requestStatusString(std::string_view request) {
    std::string reply;
    if (request == "m") {
        reply = sgrString(m_attributes) + "m";
    } else if (request == "r") {
        reply = std::to_string(m_scrollTop + 1) + ";" + std::to_string(m_scrollBottom + 1) + "r";
    } else if (request == " q") {
        const int base = m_cursor.shape == CursorShape::Block ? 1 : m_cursor.shape == CursorShape::Underline ? 3 : 5;
        reply = std::to_string(base + (m_cursor.blinking ? 0 : 1)) + " q";
    } else if (request == "\"p") {
        reply = "62;1\"p"; // VT220, 7-bit controls
    } else if (request == "\"q") {
        reply = "0\"q"; // DECSCA: no protected characters
    }
    if (reply.empty()) {
        respond("\x1bP0$r\x1b\\");
    } else {
        respond("\x1bP1$r" + reply + "\x1b\\");
    }
}

void Terminal::requestTermcap(std::string_view request) {
    // XTGETTCAP: names are hex-encoded and separated by ';'. Answer each known capability.
    for (std::string_view rest = request; !rest.empty();) {
        const auto semicolon = rest.find(';');
        const std::string_view hexName = rest.substr(0, semicolon);
        rest.remove_prefix(semicolon == std::string_view::npos ? rest.size() : semicolon + 1);
        const auto name = core::hexDecode(hexName);
        std::optional<std::string> value;
        if (name) {
            if (*name == "TN" || *name == "name") {
                value = "xterm-256color";
            } else if (*name == "Co" || *name == "colors") {
                value = "256";
            } else if (*name == "RGB") {
                value = "8/8/8";
            } else if (*name == "Tc" || *name == "Su" || *name == "bce") {
                value = ""; // boolean capabilities
            } else if (*name == "Ms") { // values are the expanded strings, as tigetstr() returns them
                value = "\x1b]52;%p1%s;%p2%s\x07";
            } else if (*name == "Smulx") {
                value = "\x1b[4:%p1%dm";
            } else if (*name == "Setulc") {
                value = "\x1b[58:2::%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%d%;m";
            }
        }
        if (!value) {
            respond("\x1bP0+r" + std::string(hexName) + "\x1b\\");
        } else if (value->empty()) {
            respond("\x1bP1+r" + std::string(hexName) + "\x1b\\");
        } else {
            respond("\x1bP1+r" + std::string(hexName) + "=" + core::hexEncode(*value) + "\x1b\\");
        }
    }
}

void Terminal::kittyKeyboard(char prefix, const CsiParams& params) {
    auto& stack = m_kittyFlags[isAlternateScreen() ? 1 : 0];
    const auto flags = static_cast<std::uint8_t>(params.raw(0) & 0x1F);
    switch (prefix) {
    case '>': // push
        if (stack.size() >= kMaxKittyStack) {
            stack.erase(stack.begin());
        }
        stack.push_back(flags);
        break;
    case '<': { // pop n
        const auto n = static_cast<std::size_t>(std::max(1, static_cast<int>(params.raw(0))));
        stack.resize(stack.size() > n ? stack.size() - n : 0);
        break;
    }
    case '=': { // set: mode 1 replace, 2 add, 3 remove
        if (stack.empty()) {
            stack.push_back(0);
        }
        const int mode = params.at(1, 1);
        std::uint8_t& top = stack.back();
        top = mode == 2 ? static_cast<std::uint8_t>(top | flags)
              : mode == 3 ? static_cast<std::uint8_t>(top & ~flags)
                          : flags;
        break;
    }
    case '?': respond("\x1b[?" + std::to_string(kittyKeyboardFlags()) + "u"); break;
    default: break;
    }
}

void Terminal::handleColorOsc(int command, std::string_view value, bool bellTerminated) {
    markFullDamage();
    const std::string_view terminator = bellTerminated ? "\a" : "\x1b\\";
    auto nextField = [&value]() {
        const auto semicolon = value.find(';');
        const std::string_view field = value.substr(0, semicolon);
        value.remove_prefix(semicolon == std::string_view::npos ? value.size() : semicolon + 1);
        return field;
    };

    switch (command) {
    case 4: // OSC 4 ; index ; spec [; index ; spec ...]
        while (!value.empty()) {
            const auto index = parseNumber(nextField());
            const std::string_view spec = nextField();
            if (!index || *index < 0 || *index > 255) {
                break;
            }
            auto& slot = m_palette.indexed[static_cast<std::size_t>(*index)];
            if (spec == "?") {
                respond("\x1b]4;" + std::to_string(*index) + ";" + formatColorSpec(slot) + std::string(terminator));
            } else if (const auto rgb = parseColorSpec(spec)) {
                slot = *rgb;
            }
        }
        break;
    case 10: // OSC 10/11/12 ; spec [; spec ...]: each further spec addresses the next colour
    case 11:
    case 12:
        for (int target = command; target <= 12 && !value.empty(); ++target) {
            const std::string_view spec = nextField();
            std::uint32_t& slot = target == 10   ? m_palette.foreground
                                  : target == 11 ? m_palette.background
                                                 : m_palette.cursor;
            if (spec == "?") {
                respond("\x1b]" + std::to_string(target) + ";" + formatColorSpec(slot) + std::string(terminator));
            } else if (const auto rgb = parseColorSpec(spec)) {
                slot = *rgb;
            }
        }
        break;
    case 104:
        if (value.empty()) {
            m_palette.indexed = m_defaultPalette.indexed;
        }
        while (!value.empty()) {
            if (const auto index = parseNumber(nextField()); index && *index >= 0 && *index <= 255) {
                m_palette.indexed[static_cast<std::size_t>(*index)] =
                    m_defaultPalette.indexed[static_cast<std::size_t>(*index)];
            }
        }
        break;
    case 110: m_palette.foreground = m_defaultPalette.foreground; break;
    case 111: m_palette.background = m_defaultPalette.background; break;
    case 112: m_palette.cursor = m_defaultPalette.cursor; break;
    default: break;
    }
}

void Terminal::csiDispatch(const CsiParams& params, char prefix, std::string_view intermediates, char final) {
    if (prefix == '?') {
        dispatchPrivateCsi(params, intermediates, final);
        return;
    }
    if (prefix == '>') {
        if (final == 'c' && params.raw(0) == 0 && intermediates.empty()) {
            respond("\x1b[>1;10;0c"); // secondary DA: VT220-class, firmware 10
        } else if (final == 'q' && params.raw(0) == 0 && intermediates.empty()) {
            respond("\x1bP>|WindTerm-Weifan\x1b\\"); // XTVERSION
        } else if (final == 'm' && intermediates.empty() && params.raw(0) == 4) { // XTMODKEYS
            m_modes.modifyOtherKeys = params.size() > 1 ? std::clamp<int>(params.raw(1), 0, 2) : 0;
        } else if (final == 'u' && intermediates.empty()) {
            kittyKeyboard('>', params);
        }
        return;
    }
    if ((prefix == '<' || prefix == '=') && final == 'u' && intermediates.empty()) {
        kittyKeyboard(prefix, params);
        return;
    }
    if (prefix != 0) {
        return;
    }
    if (intermediates == " " && final == 'q') { // DECSCUSR
        const int style = params.raw(0);
        if (style == 0) {
            m_cursor.shape = m_defaultCursorShape;
            m_cursor.blinking = m_defaultCursorBlink;
        } else {
            m_cursor.shape = style <= 2 ? CursorShape::Block : style <= 4 ? CursorShape::Underline : CursorShape::Bar;
            m_cursor.blinking = style % 2 == 1;
        }
        return;
    }
    if (intermediates == "!" && final == 'p') {
        softReset();
        return;
    }
    if (intermediates == "$" && final == 'p') { // DECRQM (ANSI modes)
        const int mode = params.raw(0);
        respond("\x1b[" + std::to_string(mode) + ";" + std::to_string(modeState(mode)) + "$y");
        return;
    }
    if (!intermediates.empty()) {
        return;
    }

    const int n = params.at(0, 1);
    switch (final) {
    case '@': insertCells(n); break;                                                     // ICH
    case 'A': moveCursorUp(n); break;                                                    // CUU
    case 'B':                                                                            // CUD
    case 'e': moveCursorDown(n); break;                                                  // VPR
    case 'C':                                                                            // CUF
    case 'a': setCursor(m_cursor.row, m_cursor.column + n); break;                       // HPR
    case 'D': setCursor(m_cursor.row, m_cursor.column - n); break;                       // CUB
    case 'E': moveCursorDown(n); m_cursor.column = 0; break;                             // CNL
    case 'F': moveCursorUp(n); m_cursor.column = 0; break;                               // CPL
    case 'G':                                                                            // CHA
    case '`': setCursor(m_cursor.row, n - 1); break;                                     // HPA
    case 'H':                                                                            // CUP
    case 'f': setCursorOriginRelative(params.at(0, 1) - 1, params.at(1, 1) - 1); break; // HVP
    case 'I': tabForward(n); break;                                                      // CHT
    case 'Z': tabBackward(n); break;                                                     // CBT
    case 'J':                                                                            // ED
        switch (params.raw(0)) {
        case 0:
            eraseCells(m_cursor.row, m_cursor.column, m_columns);
            eraseLines(m_cursor.row + 1, m_rows);
            break;
        case 1:
            eraseLines(0, m_cursor.row);
            eraseCells(m_cursor.row, 0, m_cursor.column + 1);
            break;
        case 2: eraseLines(0, m_rows); break;
        case 3: clearScrollback(); break;
        default: break;
        }
        switch (params.raw(0)) { // images in the erased area go too
        case 0: eraseImages(screenStart() + m_cursor.row, screenStart() + m_rows - 1); break;
        case 1: eraseImages(screenStart(), screenStart() + m_cursor.row); break;
        case 2: eraseImages(screenStart(), screenStart() + m_rows - 1); break;
        case 3: eraseImages(historyStart(), screenStart() - 1); break;
        default: break;
        }
        m_pendingWrap = false;
        break;
    case 'K': // EL
        switch (params.raw(0)) {
        case 0: eraseCells(m_cursor.row, m_cursor.column, m_columns); break;
        case 1: eraseCells(m_cursor.row, 0, m_cursor.column + 1); break;
        case 2: eraseCells(m_cursor.row, 0, m_columns); break;
        default: break;
        }
        if (params.raw(0) != 1) {
            currentLine().wrapped = false;
        }
        m_pendingWrap = false;
        break;
    case 'L': // IL
        if (m_cursor.row >= m_scrollTop && m_cursor.row <= m_scrollBottom) {
            scrollDown(m_cursor.row, m_scrollBottom, n);
            setCursor(m_cursor.row, 0);
        }
        break;
    case 'M': // DL
        if (m_cursor.row >= m_scrollTop && m_cursor.row <= m_scrollBottom) {
            scrollUp(m_cursor.row, m_scrollBottom, n, false);
            setCursor(m_cursor.row, 0);
        }
        break;
    case 'P': deleteCells(n); break;                                   // DCH
    case 'S': scrollUp(m_scrollTop, m_scrollBottom, n, true); break;   // SU
    case 'T':                                                          // SD
        if (params.size() <= 1) {
            scrollDown(m_scrollTop, m_scrollBottom, n);
        }
        break;
    case 'X': eraseCells(m_cursor.row, m_cursor.column, m_cursor.column + n); break; // ECH
    case 'b':                                                                        // REP
        if (m_lastPrinted != 0) {
            const int count = std::min(n, m_rows * m_columns);
            for (int i = 0; i < count; ++i) {
                print(m_lastPrinted);
            }
        }
        break;
    case 'c': // DA
        if (params.raw(0) == 0) {
            respond("\x1b[?62;4;22c"); // VT220 with sixel graphics and ANSI colour
        }
        break;
    case 'd': setCursorOriginRelative(n - 1, m_cursor.column); break; // VPA
    case 'g':                                                         // TBC
        if (params.raw(0) == 0) {
            m_tabStops[static_cast<std::size_t>(m_cursor.column)] = false;
        } else if (params.raw(0) == 3) {
            std::fill(m_tabStops.begin(), m_tabStops.end(), false);
        }
        break;
    case 'h':
    case 'l':
        for (std::size_t i = 0; i < params.size(); ++i) {
            setMode(params.raw(i), final == 'h');
        }
        break;
    case 'm': selectGraphicRendition(params); break;
    case 'n': // DSR
        if (params.raw(0) == 5) {
            respond("\x1b[0n");
        } else if (params.raw(0) == 6) {
            const int row = m_cursor.row - (m_modes.originMode ? m_scrollTop : 0);
            respond("\x1b[" + std::to_string(row + 1) + ";" + std::to_string(m_cursor.column + 1) + "R");
        }
        break;
    case 'r': { // DECSTBM
        const int top = params.at(0, 1) - 1;
        const int bottom = std::min(params.at(1, m_rows), m_rows) - 1;
        if (top < bottom) {
            m_scrollTop = top;
            m_scrollBottom = bottom;
            setCursorOriginRelative(0, 0);
        }
        break;
    }
    case 's': saveCursor(); break;    // SCOSC
    case 'u': restoreCursor(); break; // SCORC
    case 't': windowOperation(params); break; // XTWINOPS
    default: break;
    }
}

void Terminal::windowOperation(const CsiParams& params) {
    switch (params.raw(0)) {
    case 14: // text area size in pixels
        if (m_cellPixelWidth > 0 && m_cellPixelHeight > 0) {
            respond("\x1b[4;" + std::to_string(m_rows * m_cellPixelHeight) + ";" +
                    std::to_string(m_columns * m_cellPixelWidth) + "t");
        }
        break;
    case 16: // cell size in pixels
        if (m_cellPixelWidth > 0 && m_cellPixelHeight > 0) {
            respond("\x1b[6;" + std::to_string(m_cellPixelHeight) + ";" + std::to_string(m_cellPixelWidth) + "t");
        }
        break;
    case 18: // text area size in characters
    case 19: // screen size in characters
        respond("\x1b[" + std::to_string(params.raw(0) == 18 ? 8 : 9) + ";" + std::to_string(m_rows) + ";" +
                std::to_string(m_columns) + "t");
        break;
    case 22: // push window title (Ps 0 or 2; icon titles are not tracked)
        if (params.raw(1) != 1) {
            if (m_titleStack.size() >= kMaxTitleStack) {
                m_titleStack.erase(m_titleStack.begin());
            }
            m_titleStack.push_back(m_title);
        }
        break;
    case 23: // pop window title
        if (params.raw(1) != 1 && !m_titleStack.empty()) {
            std::string title = std::move(m_titleStack.back());
            m_titleStack.pop_back();
            setTitle(std::move(title));
        }
        break;
    default: break;
    }
}

int Terminal::modeState(int mode) const {
    switch (mode) {
    case 4: return m_modes.insertMode ? 1 : 2;
    case 20: return m_modes.newLineMode ? 1 : 2;
    default: return 0;
    }
}

int Terminal::privateModeState(int mode) const {
    auto state = [](bool enabled) { return enabled ? 1 : 2; };
    switch (mode) {
    case 1: return state(m_modes.cursorKeysApplication);
    case 5: return state(m_modes.reverseVideo);
    case 6: return state(m_modes.originMode);
    case 7: return state(m_modes.autoWrap);
    case 9: return state(m_modes.mouseTracking == MouseTracking::X10);
    case 12: return state(m_cursor.blinking);
    case 25: return state(m_cursor.visible);
    case 47:
    case 1047:
    case 1049: return state(isAlternateScreen());
    case 66: return state(m_modes.keypadApplication);
    case 1000: return state(m_modes.mouseTracking == MouseTracking::Normal);
    case 1002: return state(m_modes.mouseTracking == MouseTracking::ButtonEvent);
    case 1003: return state(m_modes.mouseTracking == MouseTracking::AnyEvent);
    case 1004: return state(m_modes.focusEvents);
    case 1005: return state(m_modes.mouseEncoding == MouseEncoding::Utf8);
    case 1006: return state(m_modes.mouseEncoding == MouseEncoding::Sgr);
    case 1007: return state(m_modes.alternateScroll);
    case 1015: return state(m_modes.mouseEncoding == MouseEncoding::Urxvt);
    case 2004: return state(m_modes.bracketedPaste);
    case 2026: return state(m_modes.synchronizedOutput);
    case 2027: return state(m_modes.graphemeClusters);
    case 80: return state(m_modes.sixelDisplayMode);
    default: return 0;
    }
}

void Terminal::dispatchPrivateCsi(const CsiParams& params, std::string_view intermediates, char final) {
    if (intermediates == "$" && final == 'p') { // DECRQM (DEC private modes)
        const int mode = params.raw(0);
        respond("\x1b[?" + std::to_string(mode) + ";" + std::to_string(privateModeState(mode)) + "$y");
        return;
    }
    if (!intermediates.empty()) {
        return;
    }
    switch (final) {
    case 'h':
    case 'l':
        for (std::size_t i = 0; i < params.size(); ++i) {
            setPrivateMode(params.raw(i), final == 'h');
        }
        break;
    case 'J': // DECSED, treated as ED (no protected cells yet)
    case 'K': // DECSEL
        csiDispatch(params, 0, {}, final);
        break;
    case 'u': kittyKeyboard('?', params); break; // kitty keyboard flags query
    case 'S': graphicsAttributes(params); break; // XTSMGRAPHICS
    case 'n':
        if (params.raw(0) == 6) { // DECXCPR
            const int row = m_cursor.row - (m_modes.originMode ? m_scrollTop : 0);
            respond("\x1b[?" + std::to_string(row + 1) + ";" + std::to_string(m_cursor.column + 1) + "R");
        }
        break;
    default: break;
    }
}

void Terminal::setMode(int mode, bool enabled) {
    switch (mode) {
    case 4: m_modes.insertMode = enabled; break;
    case 20: m_modes.newLineMode = enabled; break;
    default: break;
    }
}

void Terminal::setPrivateMode(int mode, bool enabled) {
    auto setMouse = [this, enabled](MouseTracking tracking) {
        m_modes.mouseTracking = enabled ? tracking : MouseTracking::None;
    };
    auto setEncoding = [this, enabled](MouseEncoding encoding) {
        if (enabled) {
            m_modes.mouseEncoding = encoding;
        } else if (m_modes.mouseEncoding == encoding) {
            m_modes.mouseEncoding = MouseEncoding::Default;
        }
    };
    switch (mode) {
    case 1: m_modes.cursorKeysApplication = enabled; break;
    case 5:
        m_modes.reverseVideo = enabled;
        markFullDamage();
        break;
    case 6:
        m_modes.originMode = enabled;
        setCursorOriginRelative(0, 0);
        break;
    case 7:
        m_modes.autoWrap = enabled;
        if (!enabled) {
            m_pendingWrap = false;
        }
        break;
    case 9: setMouse(MouseTracking::X10); break;
    case 12: m_cursor.blinking = enabled; break;
    case 25: m_cursor.visible = enabled; break;
    case 66: m_modes.keypadApplication = enabled; break;
    case 1000: setMouse(MouseTracking::Normal); break;
    case 1002: setMouse(MouseTracking::ButtonEvent); break;
    case 1003: setMouse(MouseTracking::AnyEvent); break;
    case 1004: m_modes.focusEvents = enabled; break;
    case 1005: setEncoding(MouseEncoding::Utf8); break;
    case 1006: setEncoding(MouseEncoding::Sgr); break;
    case 1007: m_modes.alternateScroll = enabled; break;
    case 1015: setEncoding(MouseEncoding::Urxvt); break;
    case 2026: m_modes.synchronizedOutput = enabled; break;
    case 2027: m_modes.graphemeClusters = enabled; break;
    case 80: m_modes.sixelDisplayMode = enabled; break;
    case 47:
        switchScreen(enabled, false);
        break;
    case 1047:
        if (!enabled && isAlternateScreen()) {
            eraseLines(0, m_rows);
        }
        switchScreen(enabled, false);
        break;
    case 1048:
        if (enabled) {
            saveCursor();
        } else {
            restoreCursor();
        }
        break;
    case 1049:
        if (enabled) {
            if (!isAlternateScreen()) {
                saveCursor();
                switchScreen(true, true);
            }
        } else if (isAlternateScreen()) {
            switchScreen(false, false);
            restoreCursor();
        }
        break;
    case 2004: m_modes.bracketedPaste = enabled; break;
    default: break;
    }
}

std::size_t Terminal::parseExtendedColor(const CsiParams& params, std::size_t i,
                                         std::optional<Color>& out) const {
    // Count ':'-separated sub-parameters following params[i].
    std::size_t subCount = 0;
    while (params.isSubParam(i + 1 + subCount)) {
        ++subCount;
    }
    if (subCount > 0) {
        const int mode = params.raw(i + 1);
        if (mode == 5 && subCount >= 2) {
            out = Color::indexed(clampByte(params.raw(i + 2)));
        } else if (mode == 2 && subCount >= 5) { // 38:2:<colorspace>:r:g:b
            out = Color::rgb(clampByte(params.raw(i + 3)), clampByte(params.raw(i + 4)),
                             clampByte(params.raw(i + 5)));
        } else if (mode == 2 && subCount == 4) { // 38:2:r:g:b
            out = Color::rgb(clampByte(params.raw(i + 2)), clampByte(params.raw(i + 3)),
                             clampByte(params.raw(i + 4)));
        }
        return i + subCount;
    }

    const int mode = params.raw(i + 1);
    if (mode == 5 && i + 2 < params.size()) {
        out = Color::indexed(clampByte(params.raw(i + 2)));
        return i + 2;
    }
    if (mode == 2 && i + 4 < params.size()) {
        out = Color::rgb(clampByte(params.raw(i + 2)), clampByte(params.raw(i + 3)),
                         clampByte(params.raw(i + 4)));
        return i + 4;
    }
    return params.size(); // malformed: ignore the rest of the sequence
}

void Terminal::selectGraphicRendition(const CsiParams& params) {
    auto& a = m_attributes;
    if (params.empty()) {
        const std::uint16_t link = a.hyperlink;
        a = {};
        a.hyperlink = link;
        return;
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        const int p = params.raw(i);
        std::optional<Color> color;
        switch (p) {
        case 0: {
            const std::uint16_t link = a.hyperlink; // hyperlinks are not SGR state
            a = {};
            a.hyperlink = link;
            break;
        }
        case 1: a.flags |= Bold; break;
        case 2: a.flags |= Dim; break;
        case 3: a.flags |= Italic; break;
        case 4:
            if (params.isSubParam(i + 1)) { // 4:0 none, 4:1..5 underline styles
                const int style = params.raw(i + 1);
                a.setUnderlineStyle(style <= 5 ? static_cast<UnderlineStyle>(style) : UnderlineStyle::Single);
            } else {
                a.setUnderlineStyle(UnderlineStyle::Single);
            }
            break;
        case 5:
        case 6: a.flags |= Blink; break;
        case 7: a.flags |= Inverse; break;
        case 8: a.flags |= Hidden; break;
        case 9: a.flags |= Strikethrough; break;
        case 21: a.setUnderlineStyle(UnderlineStyle::Double); break;
        case 22: a.flags &= ~(Bold | Dim); break;
        case 23: a.flags &= ~Italic; break;
        case 24: a.setUnderlineStyle(UnderlineStyle::None); break;
        case 25: a.flags &= ~Blink; break;
        case 27: a.flags &= ~Inverse; break;
        case 28: a.flags &= ~Hidden; break;
        case 29: a.flags &= ~Strikethrough; break;
        case 38:
            i = parseExtendedColor(params, i, color);
            if (color) {
                a.foreground = *color;
            }
            continue;
        case 39: a.foreground = {}; break;
        case 48:
            i = parseExtendedColor(params, i, color);
            if (color) {
                a.background = *color;
            }
            continue;
        case 49: a.background = {}; break;
        case 53: a.flags |= Overline; break;
        case 55: a.flags &= ~Overline; break;
        case 58: // underline colour
            i = parseExtendedColor(params, i, color);
            if (color) {
                a.underlineColor = *color;
            }
            continue;
        case 59: a.underlineColor = {}; break;
        default:
            if (p >= 30 && p <= 37) {
                a.foreground = Color::indexed(static_cast<std::uint8_t>(p - 30));
            } else if (p >= 40 && p <= 47) {
                a.background = Color::indexed(static_cast<std::uint8_t>(p - 40));
            } else if (p >= 90 && p <= 97) {
                a.foreground = Color::indexed(static_cast<std::uint8_t>(p - 90 + 8));
            } else if (p >= 100 && p <= 107) {
                a.background = Color::indexed(static_cast<std::uint8_t>(p - 100 + 8));
            }
            break;
        }
        while (params.isSubParam(i + 1)) { // skip unsupported sub-parameters
            ++i;
        }
    }
}

} // namespace wt::terminal

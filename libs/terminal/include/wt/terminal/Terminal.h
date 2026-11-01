#pragma once

#include "wt/terminal/Cell.h"
#include "wt/terminal/KittyGraphics.h"
#include "wt/terminal/Selection.h"
#include "wt/terminal/Sixel.h"
#include "wt/terminal/VtParser.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wt::terminal {

enum class CursorShape : std::uint8_t { Block, Underline, Bar };
enum class MouseTracking : std::uint8_t { None, X10, Normal, ButtonEvent, AnyEvent };
enum class MouseEncoding : std::uint8_t { Default, Utf8, Sgr, Urxvt }; // ?1005 / ?1006 / ?1015

struct Cursor {
    int row = 0;
    int column = 0;
    bool visible = true;
    bool blinking = false;
    CursorShape shape = CursorShape::Block;
};

struct TerminalModes {
    bool autoWrap = true;               // DECAWM  (?7)
    bool originMode = false;            // DECOM   (?6)
    bool insertMode = false;            // IRM     (4)
    bool newLineMode = false;           // LNM     (20)
    bool cursorKeysApplication = false; // DECCKM  (?1)
    bool keypadApplication = false;     // DECKPAM / DECNKM (?66)
    bool bracketedPaste = false;        // ?2004
    bool reverseVideo = false;          // DECSCNM (?5)
    bool focusEvents = false;           // ?1004
    bool alternateScroll = true;        // ?1007: wheel sends cursor keys on the alternate screen
    bool synchronizedOutput = false;    // ?2026: the view should hold repaints until reset
    bool graphemeClusters = false;      // ?2027: emoji ZWJ sequences, flags, skin tones are one cell pair
    bool sixelDisplayMode = false;      // ?80 DECSDM: sixel images at the top-left, no scrolling
    int modifyOtherKeys = 0;            // XTMODKEYS (CSI > 4 ; n m): 0, 1 or 2
    MouseTracking mouseTracking = MouseTracking::None;
    MouseEncoding mouseEncoding = MouseEncoding::Default;
};

// Terminal state machine: consumes the byte stream produced by the child process and
// maintains the screen grid, cursor, modes and scrollback. Emulates a useful subset of
// xterm (TERM=xterm-256color). Qt-free; rendering lives in the app layer.
class Terminal final : private VtHandler {
public:
    using ResponseHandler = std::function<void(std::string_view)>;
    using BellHandler = std::function<void()>;
    using TitleHandler = std::function<void(const std::string&)>;
    // OSC 52: `target` is 'c' (clipboard) or 'p' (primary selection); `text` is the decoded data.
    using ClipboardHandler = std::function<void(char target, const std::string& text)>;

    // Rows of the visible screen that changed since the last takeDamage(). `full` means the
    // whole view must be repainted (scrolling, resize, screen switch, colours, selection).
    struct Damage {
        bool full = false;
        std::vector<int> rows; // ascending
    };

    static constexpr std::size_t kDefaultScrollback = 10000;

    Terminal(int rows, int columns, std::size_t scrollbackLimit = kDefaultScrollback);

    void feed(std::string_view bytes);
    void resize(int rows, int columns);
    void reset();

    int rows() const { return m_rows; }
    int columns() const { return m_columns; }

    // Visible screen (primary or alternate), row 0 at the top.
    const Line& line(int row) const { return (*m_screen)[static_cast<std::size_t>(row)]; }
    // History of the primary screen, index 0 is the oldest line. Lines are reflowed to the new
    // width when the column count changes.
    std::size_t scrollbackSize() const { return m_scrollback.size(); }
    const Line& scrollbackLine(std::size_t index) const { return m_scrollback[index]; }
    void clearScrollback();
    std::size_t scrollbackLimit() const { return m_scrollbackLimit; }
    void setScrollbackLimit(std::size_t lines);

    // Absolute line numbering used by selections: scrollbackLine(0) has absolute row
    // historyStart() and screen row 0 has screenStart(). Numbers only grow as lines scroll away.
    std::int64_t historyStart() const { return m_droppedLines; }
    std::int64_t screenStart() const { return m_droppedLines + static_cast<std::int64_t>(m_scrollback.size()); }
    // Line at an absolute row (scrollback or current screen), nullptr if out of range.
    const Line* absoluteLine(std::int64_t row) const;

    // Text of a cell: the code point, or the whole grapheme cluster for combined characters.
    std::u32string_view cellText(const Cell& cell) const;
    // URI of an OSC 8 hyperlink id (CellAttributes::hyperlink); empty for 0 / unknown ids.
    std::string_view hyperlinkUri(std::uint16_t id) const;

    // Text helpers (UTF-8, trailing blanks trimmed), mainly for tests and copy.
    std::string lineText(int row) const;
    std::string screenText() const;
    std::string lineToText(const Line& line) const;

    // Selection, in absolute coordinates. It is cleared when the selected content changes.
    void startSelection(GridPoint point, SelectionMode mode);
    void extendSelection(GridPoint point);
    void clearSelection();
    bool hasSelection() const { return m_selection.active; }
    std::optional<SelectionRange> selectionRange() const;
    // Selected text as UTF-8; wrapped lines are joined, trailing blanks trimmed.
    std::string selectedText() const;

    const Cursor& cursor() const { return m_cursor; }
    const TerminalModes& modes() const { return m_modes; }
    // Active kitty keyboard protocol flags (CSI > flags u), 0 when the protocol is off.
    unsigned kittyKeyboardFlags() const;
    bool isAlternateScreen() const { return m_screen == &m_alternate; }
    const std::string& title() const { return m_title; }
    // Last directory reported through OSC 7 (file://host/path), percent-decoded; empty if none.
    const std::string& workingDirectory() const { return m_workingDirectory; }

    // Shell integration marks (OSC 133 A/B/C/D, as emitted by iTerm2/VS Code/WezTerm shell
    // integration scripts), in absolute coordinates.
    struct ShellMarks {
        std::optional<GridPoint> promptStart;  // A
        std::optional<GridPoint> commandStart; // B: where the user's input begins
        bool commandRunning = false;           // between C and D
        std::optional<int> lastExitCode;       // D;<code>
    };
    const ShellMarks& shellMarks() const { return m_shellMarks; }
    // Text from `start` to the end of its logical line (following autowrap), trailing blanks
    // trimmed: what the user typed after a prompt.
    std::string textFrom(GridPoint start) const;

    // Images on the screen: sixel images (DCS q) and kitty graphics placements, anchored to
    // absolute rows so they scroll with the text. Images on the alternate screen go away with
    // it; erasing the display, resizing to another width and RIS remove them, and they are
    // dropped once they scrolled out of the history.
    struct Image {
        std::uint64_t id = 0; // unique per placement
        std::shared_ptr<const Bitmap> bitmap;
        std::int64_t row = 0; // absolute row of the top edge
        int column = 0;
        int columns = 0; // cells covered
        int rows = 0;
        int cellWidth = 0;  // cell size in pixels when it was placed (for scaling on zoom)
        int cellHeight = 0;
        bool alternate = false;
        // The part of the bitmap shown and its size on the screen (pixels at cellWidth x
        // cellHeight), offset inside the first cell.
        int sourceX = 0;
        int sourceY = 0;
        int sourceWidth = 0;
        int sourceHeight = 0;
        int pixelWidth = 0;
        int pixelHeight = 0;
        int offsetX = 0;
        int offsetY = 0;
        // Kitty graphics placements (kittyImage is 0 for sixel images).
        std::uint32_t kittyImage = 0;
        std::uint32_t kittyPlacement = 0;
        std::int32_t z = 0; // below zero the image is drawn under the text
    };
    // Images of the screen that is shown (primary or alternate), in drawing order.
    std::vector<const Image*> visibleImages() const;
    std::size_t imageCount() const { return m_images.size(); }
    // Kitty graphics: images transmitted and kept for placements (not necessarily shown).
    std::size_t kittyImageCount() const { return m_kittyImages.size(); }
    // PNG decoding for kitty graphics comes from the GUI layer, keeping the model dependency-free.
    void setPngDecoder(std::function<std::optional<Bitmap>(std::string_view png)> decoder) {
        m_kittyDecoders.png = std::move(decoder);
    }

    // Colours (OSC 4/10/11/12 may change them at runtime; RIS and OSC 104/110-112 restore the
    // defaults set here).
    const ColorPalette& palette() const { return m_palette; }
    void setDefaultPalette(const ColorPalette& palette);
    // Cursor style used after reset and for DECSCUSR 0 (the user's preference).
    void setDefaultCursorStyle(CursorShape shape, bool blinking);
    // Pixel size of one cell, used for XTWINOPS size reports (0 = unknown).
    void setCellPixelSize(int width, int height);
    const CellAttributes& currentAttributes() const { return m_attributes; }
    // Incremented whenever visible state may have changed; lets views skip redundant repaints.
    std::uint64_t revision() const { return m_revision; }
    // Returns and clears the accumulated damage.
    Damage takeDamage();

    // Bytes the terminal must send back to the application (DA, DSR/CPR, ...).
    void setResponseHandler(ResponseHandler handler) { m_onResponse = std::move(handler); }
    void setBellHandler(BellHandler handler) { m_onBell = std::move(handler); }
    void setTitleHandler(TitleHandler handler) { m_onTitle = std::move(handler); }
    void setClipboardHandler(ClipboardHandler handler) { m_onClipboard = std::move(handler); }
    // OSC 52 writes are ignored unless explicitly allowed (a remote host could otherwise replace
    // the clipboard contents). Clipboard reads are never answered.
    void setClipboardWriteAllowed(bool allowed) { m_clipboardWriteAllowed = allowed; }
    bool clipboardWriteAllowed() const { return m_clipboardWriteAllowed; }

private:
    enum class Charset : std::uint8_t { Ascii, DecSpecialGraphics };

    struct SavedCursor {
        int row = 0;
        int column = 0;
        CellAttributes attributes;
        bool originMode = false;
        bool pendingWrap = false;
        Charset charsets[2] = {Charset::Ascii, Charset::Ascii};
        int activeCharset = 0;
    };

    // VtHandler
    void print(char32_t cp) override;
    void printAscii(std::string_view text) override;
    void execute(std::uint8_t control) override;
    void csiDispatch(const CsiParams& params, char prefix, std::string_view intermediates,
                     char final) override;
    void escDispatch(std::string_view intermediates, char final) override;
    void oscDispatch(std::string_view payload, bool bellTerminated) override;
    void dcsDispatch(const CsiParams& params, char prefix, std::string_view intermediates, char final,
                     std::string_view data) override;
    void apcDispatch(std::string_view data) override;

    void requestStatusString(std::string_view request); // DECRQSS
    void requestTermcap(std::string_view request);       // XTGETTCAP
    void kittyKeyboard(char prefix, const CsiParams& params);
    void handleHyperlinkOsc(std::string_view value);
    void handleClipboardOsc(std::string_view value);

    void dispatchPrivateCsi(const CsiParams& params, std::string_view intermediates, char final);
    void windowOperation(const CsiParams& params);
    void setMode(int mode, bool enabled);
    void setPrivateMode(int mode, bool enabled);
    // DECRQM values: 0 not recognised, 1 set, 2 reset, 3 permanently set, 4 permanently reset.
    int modeState(int mode) const;
    int privateModeState(int mode) const;
    void handleColorOsc(int command, std::string_view value, bool bellTerminated);
    void selectGraphicRendition(const CsiParams& params);
    std::size_t parseExtendedColor(const CsiParams& params, std::size_t i, std::optional<Color>& out) const;

    Cell blankCell() const;
    Line blankLine() const;
    Line& currentLine() { return (*m_screen)[static_cast<std::size_t>(m_cursor.row)]; }
    void splitWideCharAt(Line& line, int column);
    void eraseCells(int row, int from, int to);
    void eraseLines(int from, int to);
    void scrollUp(int top, int bottom, int count, bool toScrollback);
    void scrollDown(int top, int bottom, int count);
    void insertCells(int count);
    void deleteCells(int count);
    void pushScrollback(Line&& line);
    // Rewraps the primary screen + history to `columns`. `tracked` points (absolute rows) are
    // mapped through the rewrap, like the cursor.
    void reflowPrimary(int rows, int columns, int& cursorRow, int& cursorColumn, std::vector<GridPoint*> tracked);
    void appendCombining(char32_t mark);
    // Mode 2027: adds `cp` to the previous grapheme cluster when it continues it; true if it did.
    bool joinCluster(char32_t cp);
    void placeSixel(const CsiParams& params, std::string_view data);
    // Drops images outside the history / on the given screen rows.
    void pruneImages();
    void eraseImages(std::int64_t firstRow, std::int64_t lastRow);
    void graphicsAttributes(const CsiParams& params); // XTSMGRAPHICS
    // Kitty graphics protocol (APC G ...).
    struct KittyImage {
        std::uint32_t number = 0;
        std::shared_ptr<const Bitmap> bitmap;
        std::uint64_t serial = 0; // transmission order, for evicting the oldest
        bool anonymous = false;   // sent without i / I: freed once it is not shown
    };
    void kittyGraphics(std::string_view data);
    void kittyExecute(const KittyCommand& command, std::string data);
    // Places an image at the cursor; returns an error ("ECODE:message") or "".
    std::string kittyPut(const KittyCommand& command, std::uint32_t imageId);
    void kittyDelete(const KittyCommand& command);
    void kittyRespond(const KittyCommand& command, std::uint32_t imageId, std::string_view error);
    void kittyEnforceQuota();
    std::uint32_t kittyNewestWithNumber(std::uint32_t number) const;
    void resetKittyGraphics();
    // Mode 2027: makes the just-printed narrow cell at `col` two cells wide (emoji presentation).
    void widenCell(int col);
    std::optional<std::uint32_t> internCluster(const std::u32string& text);

    // Selection helpers
    void touchRows(int firstRow, int lastRow); // screen rows whose content changed
    void markDirty(int firstRow, int lastRow);
    void markFullDamage() { m_fullDamage = true; }
    bool isWordCell(const Line& line, int column) const;
    GridPoint wordStart(GridPoint point) const;
    GridPoint wordEnd(GridPoint point) const;
    GridPoint logicalLineStart(std::int64_t row) const;
    GridPoint logicalLineEnd(std::int64_t row) const;

    void lineFeed();
    void reverseIndex();
    void tabForward(int count);
    void tabBackward(int count);
    void setCursor(int row, int column);
    void setCursorOriginRelative(int row, int column);
    void moveCursorUp(int count);
    void moveCursorDown(int count);
    void saveCursor();
    void restoreCursor();
    void switchScreen(bool alternate, bool clear);
    void resetTabStops(int fromColumn = 0);
    void respond(std::string_view bytes);
    void softReset();
    void setTitle(std::string title);

    int m_rows;
    int m_columns;
    std::size_t m_scrollbackLimit;

    std::vector<Line> m_primary;
    std::vector<Line> m_alternate;
    std::vector<Line>* m_screen = &m_primary;
    std::deque<Line> m_scrollback;
    std::int64_t m_droppedLines = 0; // lines discarded from the front of the scrollback
    Line m_spareLine;                // most recently discarded line, recycled when scrolling

    Cursor m_cursor;
    bool m_pendingWrap = false;
    CellAttributes m_attributes;
    int m_scrollTop = 0;
    int m_scrollBottom = 0;
    std::vector<bool> m_tabStops;
    TerminalModes m_modes;
    std::optional<SavedCursor> m_saved[2]; // [0] primary, [1] alternate
    Charset m_charsets[2] = {Charset::Ascii, Charset::Ascii};
    int m_activeCharset = 0;
    char32_t m_lastPrinted = 0;
    std::string m_title;
    std::vector<std::string> m_titleStack; // XTWINOPS 22/23
    std::string m_workingDirectory;
    ShellMarks m_shellMarks;
    std::vector<Image> m_images;
    std::uint64_t m_nextImageId = 1;
    std::unordered_map<std::uint32_t, KittyImage> m_kittyImages;
    std::optional<KittyCommand> m_kittyPending; // first chunk of a chunked transmission
    std::string m_kittyData;                    // its decoded data so far
    bool m_kittyDataBad = false;                // invalid base64 or too much data
    std::uint64_t m_kittySerial = 0;
    std::uint32_t m_nextKittyId = 0x80000000u; // ids the terminal assigns
    KittyDecoders m_kittyDecoders;
    std::uint64_t m_revision = 0;

    CursorShape m_defaultCursorShape = CursorShape::Block;
    bool m_defaultCursorBlink = false;
    ColorPalette m_defaultPalette = ColorPalette::xterm();
    ColorPalette m_palette = m_defaultPalette;
    int m_cellPixelWidth = 0;
    int m_cellPixelHeight = 0;

    std::vector<std::u32string> m_clusters;
    std::unordered_map<std::u32string, std::uint32_t> m_clusterIds;
    std::vector<std::string> m_hyperlinks; // index = id - 1
    std::unordered_map<std::string, std::uint16_t> m_hyperlinkIds;
    std::size_t m_hyperlinkBytes = 0;

    std::vector<std::uint8_t> m_kittyFlags[2]; // flag stacks for the primary/alternate screen
    std::vector<bool> m_dirtyRows;
    bool m_fullDamage = true;
    bool m_clipboardWriteAllowed = false;

    struct SelectionState {
        bool active = false;
        SelectionMode mode = SelectionMode::Character;
        GridPoint anchor;
        GridPoint extent;
    } m_selection;

    ResponseHandler m_onResponse;
    BellHandler m_onBell;
    TitleHandler m_onTitle;
    ClipboardHandler m_onClipboard;

    VtParser m_parser{*this};
};

} // namespace wt::terminal

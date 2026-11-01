#include "WtTest.h"

#include "wt/terminal/KeyEncoder.h"
#include "wt/terminal/MouseEncoder.h"
#include "wt/terminal/Terminal.h"

#include <string>

using namespace wt::terminal;

#define ESC "\x1b"
#define CSI "\x1b["

namespace {

const Cell& cellAt(const Terminal& t, int row, int col) {
    return t.line(row).cells[static_cast<std::size_t>(col)];
}

} // namespace

WT_TEST(terminal_prints_and_moves_cursor) {
    Terminal t(5, 10);
    t.feed("hello");
    WT_CHECK_EQ(t.lineText(0), std::string("hello"));
    WT_CHECK_EQ(t.cursor().row, 0);
    WT_CHECK_EQ(t.cursor().column, 5);
    t.feed("\r\nworld");
    WT_CHECK_EQ(t.lineText(1), std::string("world"));
    WT_CHECK_EQ(t.cursor().row, 1);
}

WT_TEST(terminal_autowrap_uses_pending_wrap) {
    Terminal t(3, 5);
    t.feed("abcde");
    WT_CHECK_EQ(t.cursor().row, 0);
    WT_CHECK_EQ(t.cursor().column, 4); // stays on last column until the next printable
    t.feed("f");
    WT_CHECK_EQ(t.lineText(0), std::string("abcde"));
    WT_CHECK(t.line(0).wrapped);
    WT_CHECK_EQ(t.lineText(1), std::string("f"));

    Terminal noWrap(3, 5);
    noWrap.feed(CSI "?7labcdefg");
    WT_CHECK_EQ(noWrap.lineText(0), std::string("abcdg"));
    WT_CHECK_EQ(noWrap.cursor().row, 0);
}

WT_TEST(terminal_cursor_positioning) {
    Terminal t(10, 20);
    t.feed(CSI "5;7HX");
    WT_CHECK_EQ(cellAt(t, 4, 6).ch, U'X');
    t.feed(CSI "2A" CSI "3D");
    WT_CHECK_EQ(t.cursor().row, 2);
    WT_CHECK_EQ(t.cursor().column, 4);
    t.feed(CSI "100;100H");
    WT_CHECK_EQ(t.cursor().row, 9);
    WT_CHECK_EQ(t.cursor().column, 19);
    t.feed(CSI "H");
    WT_CHECK_EQ(t.cursor().row, 0);
    WT_CHECK_EQ(t.cursor().column, 0);
    t.feed(CSI "4G" CSI "3d");
    WT_CHECK_EQ(t.cursor().row, 2);
    WT_CHECK_EQ(t.cursor().column, 3);
}

WT_TEST(terminal_erase_in_display_and_line) {
    Terminal t(3, 5);
    t.feed("11111\r\n22222\r\n33333");
    t.feed(CSI "2;3H" CSI "K");
    WT_CHECK_EQ(t.lineText(1), std::string("22"));
    t.feed(CSI "1K");
    WT_CHECK_EQ(t.lineText(1), std::string(""));
    t.feed(CSI "J");
    WT_CHECK_EQ(t.lineText(2), std::string(""));
    WT_CHECK_EQ(t.lineText(0), std::string("11111"));
    t.feed(CSI "2J");
    WT_CHECK_EQ(t.screenText(), std::string(""));
}

WT_TEST(terminal_erase_uses_background_color) {
    Terminal t(2, 4);
    t.feed(CSI "44m" CSI "2J");
    WT_CHECK(cellAt(t, 1, 3).attributes.background == Color::indexed(4));
}

WT_TEST(terminal_scrolls_into_scrollback) {
    Terminal t(3, 10, 100);
    t.feed("l1\r\nl2\r\nl3\r\nl4\r\nl5");
    WT_CHECK_EQ(t.lineText(0), std::string("l3"));
    WT_CHECK_EQ(t.lineText(2), std::string("l5"));
    WT_REQUIRE(t.scrollbackSize() == 2);
    WT_CHECK_EQ(t.lineToText(t.scrollbackLine(0)), std::string("l1"));
    WT_CHECK_EQ(t.lineToText(t.scrollbackLine(1)), std::string("l2"));
    t.feed(CSI "3J");
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(0));
}

WT_TEST(terminal_scrollback_limit) {
    Terminal t(2, 10, 3);
    for (int i = 0; i < 10; ++i) {
        t.feed("line" + std::to_string(i) + "\r\n");
    }
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(3));
    WT_CHECK_EQ(t.lineToText(t.scrollbackLine(0)), std::string("line6"));
}

WT_TEST(terminal_scroll_region) {
    Terminal t(5, 10);
    t.feed("a\r\nb\r\nc\r\nd\r\ne");
    t.feed(CSI "2;4r"); // rows 2-4
    WT_CHECK_EQ(t.cursor().row, 0);
    t.feed(CSI "4;1H\nX");
    WT_CHECK_EQ(t.lineText(0), std::string("a"));
    WT_CHECK_EQ(t.lineText(1), std::string("c"));
    WT_CHECK_EQ(t.lineText(2), std::string("d"));
    WT_CHECK_EQ(t.lineText(3), std::string("X"));
    WT_CHECK_EQ(t.lineText(4), std::string("e"));
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(0)); // region scrolls never reach scrollback
    t.feed(CSI "2;1H" ESC "M");                      // reverse index at top margin
    WT_CHECK_EQ(t.lineText(1), std::string(""));
    WT_CHECK_EQ(t.lineText(2), std::string("c"));
    WT_CHECK_EQ(t.lineText(4), std::string("e"));
}

WT_TEST(terminal_insert_delete_lines_and_chars) {
    Terminal t(4, 6);
    t.feed("aaaa\r\nbbbb\r\ncccc\r\ndddd");
    t.feed(CSI "2;1H" CSI "L");
    WT_CHECK_EQ(t.lineText(1), std::string(""));
    WT_CHECK_EQ(t.lineText(2), std::string("bbbb"));
    WT_CHECK_EQ(t.lineText(3), std::string("cccc"));
    t.feed(CSI "2M");
    WT_CHECK_EQ(t.lineText(1), std::string("cccc"));
    WT_CHECK_EQ(t.lineText(2), std::string(""));

    Terminal c(1, 8);
    c.feed("abcdef" CSI "3G" CSI "2@");
    WT_CHECK_EQ(c.lineText(0), std::string("ab  cdef"));
    c.feed(CSI "3P");
    WT_CHECK_EQ(c.lineText(0), std::string("abdef"));
    c.feed(CSI "1G" CSI "2X");
    WT_CHECK_EQ(c.lineText(0), std::string("  def"));
}

WT_TEST(terminal_insert_mode) {
    Terminal t(1, 10);
    t.feed("abc" CSI "1G" CSI "4hXY" CSI "4lZ");
    WT_CHECK_EQ(t.lineText(0), std::string("XYZbc"));
}

WT_TEST(terminal_sgr_attributes_and_colors) {
    Terminal t(1, 20);
    t.feed(CSI "1;3;4;31;42mA");
    const auto& a = cellAt(t, 0, 0).attributes;
    WT_CHECK(a.has(Bold) && a.has(Italic) && a.has(Underline));
    WT_CHECK(a.foreground == Color::indexed(1));
    WT_CHECK(a.background == Color::indexed(2));

    t.feed(CSI "22;24;38;5;208;48;2;10;20;30mB");
    const auto& b = cellAt(t, 0, 1).attributes;
    WT_CHECK(!b.has(Bold) && !b.has(Underline) && b.has(Italic));
    WT_CHECK(b.foreground == Color::indexed(208));
    WT_CHECK(b.background == Color::rgb(10, 20, 30));

    t.feed(CSI "38:2::1:2:3;48:5:17;95;4:0mC");
    const auto& c = cellAt(t, 0, 2).attributes;
    WT_CHECK(c.foreground == Color::indexed(13)); // 95 applied after 38:2 -> bright magenta
    WT_CHECK(c.background == Color::indexed(17));
    WT_CHECK(!c.has(Underline));

    t.feed(CSI "38:2:1:2:3mD" CSI "0mE");
    WT_CHECK(cellAt(t, 0, 3).attributes.foreground == Color::rgb(1, 2, 3));
    WT_CHECK(cellAt(t, 0, 4).attributes == CellAttributes{});

    t.feed(CSI "7;9;2mF" CSI "27;29mG");
    WT_CHECK(cellAt(t, 0, 5).attributes.has(Inverse));
    WT_CHECK(cellAt(t, 0, 5).attributes.has(Strikethrough));
    WT_CHECK(!cellAt(t, 0, 6).attributes.has(Inverse));
    WT_CHECK(cellAt(t, 0, 6).attributes.has(Dim));
}

WT_TEST(terminal_wide_characters) {
    Terminal t(2, 5);
    t.feed("a中b");
    WT_CHECK_EQ(cellAt(t, 0, 1).width, 2);
    WT_CHECK_EQ(cellAt(t, 0, 2).width, 0);
    WT_CHECK_EQ(cellAt(t, 0, 3).ch, U'b');
    WT_CHECK_EQ(t.lineText(0), std::string("a中b"));
    WT_CHECK_EQ(t.cursor().column, 4);
    t.feed("文"); // does not fit in the last column: wraps
    WT_CHECK_EQ(t.lineText(1), std::string("文"));
    WT_CHECK(t.line(0).wrapped);

    // Overwriting half of a wide character blanks the other half.
    Terminal o(1, 4);
    o.feed("中" CSI "2GX");
    WT_CHECK_EQ(cellAt(o, 0, 0).ch, U' ');
    WT_CHECK_EQ(cellAt(o, 0, 0).width, 1);
    WT_CHECK_EQ(cellAt(o, 0, 1).ch, U'X');
}

WT_TEST(terminal_tabs) {
    Terminal t(1, 30);
    t.feed("\tx");
    WT_CHECK_EQ(cellAt(t, 0, 8).ch, U'x');
    t.feed(CSI "3g" CSI "5G" ESC "H" CSI "1G\ty");
    WT_CHECK_EQ(cellAt(t, 0, 4).ch, U'y');
    t.feed(CSI "30G" CSI "Z");
    WT_CHECK_EQ(t.cursor().column, 4);
}

WT_TEST(terminal_save_restore_cursor) {
    Terminal t(5, 10);
    t.feed(CSI "3;4H" CSI "31m" ESC "7" CSI "H" CSI "0m" ESC "8X");
    WT_CHECK_EQ(cellAt(t, 2, 3).ch, U'X');
    WT_CHECK(cellAt(t, 2, 3).attributes.foreground == Color::indexed(1));
}

WT_TEST(terminal_alternate_screen_1049) {
    Terminal t(3, 10);
    t.feed("shell$ ");
    t.feed(CSI "?1049h");
    WT_CHECK(t.isAlternateScreen());
    WT_CHECK_EQ(t.lineText(0), std::string(""));
    t.feed(CSI "Hvim screen");
    WT_CHECK_EQ(t.lineText(0), std::string("vim screen"));
    t.feed(CSI "?1049l");
    WT_CHECK(!t.isAlternateScreen());
    WT_CHECK_EQ(t.lineText(0), std::string("shell$"));
    WT_CHECK_EQ(t.cursor().column, 7);
}

WT_TEST(terminal_alternate_screen_has_no_scrollback) {
    Terminal t(2, 10);
    t.feed(CSI "?1049h" "a\r\nb\r\nc\r\nd");
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(0));
}

WT_TEST(terminal_modes) {
    Terminal t(2, 10);
    t.feed(CSI "?1h" CSI "?25l" CSI "?2004h" CSI "?1000h" CSI "?1006h" ESC "=");
    WT_CHECK(t.modes().cursorKeysApplication);
    WT_CHECK(!t.cursor().visible);
    WT_CHECK(t.modes().bracketedPaste);
    WT_CHECK(t.modes().mouseTracking == MouseTracking::Normal);
    WT_CHECK(t.modes().mouseEncoding == MouseEncoding::Sgr);
    WT_CHECK(t.modes().keypadApplication);
    t.feed(CSI "?1l" CSI "?25h" CSI "?2004l" CSI "?1000l");
    WT_CHECK(!t.modes().cursorKeysApplication);
    WT_CHECK(t.cursor().visible);
    WT_CHECK(!t.modes().bracketedPaste);
    WT_CHECK(t.modes().mouseTracking == MouseTracking::None);
    t.feed(CSI "5 q");
    WT_CHECK(t.cursor().shape == CursorShape::Bar);
    WT_CHECK(t.cursor().blinking);
}

WT_TEST(terminal_origin_mode) {
    Terminal t(10, 10);
    t.feed(CSI "3;6r" CSI "?6h");
    WT_CHECK_EQ(t.cursor().row, 2);
    t.feed(CSI "2;1HX");
    WT_CHECK_EQ(cellAt(t, 3, 0).ch, U'X');
    t.feed(CSI "99;1H");
    WT_CHECK_EQ(t.cursor().row, 5); // clamped to the bottom margin
}

WT_TEST(terminal_device_responses) {
    Terminal t(24, 80);
    std::string responses;
    t.setResponseHandler([&responses](std::string_view r) { responses += r; });
    t.feed(CSI "5;10H" CSI "6n");
    WT_CHECK_EQ(responses, std::string(CSI "5;10R"));
    responses.clear();
    t.feed(CSI "c" CSI ">c" CSI "5n" CSI "18t");
    WT_CHECK_EQ(responses, std::string(CSI "?62;4;22c" CSI ">1;10;0c" CSI "0n" CSI "8;24;80t")); // 4: sixel
}

WT_TEST(terminal_title_and_bell) {
    Terminal t(2, 10);
    std::string title;
    int bells = 0;
    t.setTitleHandler([&title](const std::string& s) { title = s; });
    t.setBellHandler([&bells] { ++bells; });
    t.feed(ESC "]0;my title\a" "\a" ESC "]2;second" ESC "\\");
    WT_CHECK_EQ(title, std::string("second"));
    WT_CHECK_EQ(t.title(), std::string("second"));
    WT_CHECK_EQ(bells, 1); // the BEL terminating the OSC is not a bell
}

WT_TEST(terminal_dec_line_drawing) {
    Terminal t(1, 10);
    t.feed(ESC "(0" "lqk" ESC "(B" "q");
    WT_CHECK_EQ(t.lineText(0), std::string("┌─┐q"));
    t.feed("\x0e"); // SO without G1 designation stays ASCII
    t.feed("q");
    WT_CHECK_EQ(cellAt(t, 0, 4).ch, U'q');
}

WT_TEST(terminal_repeat_and_newline_mode) {
    Terminal t(3, 10);
    t.feed("x" CSI "3b");
    WT_CHECK_EQ(t.lineText(0), std::string("xxxx"));
    t.feed(CSI "20h" "\nA");
    WT_CHECK_EQ(t.lineText(1), std::string("A"));
}

WT_TEST(terminal_resize_keeps_cursor_line_visible) {
    Terminal t(5, 10);
    t.feed("1\r\n2\r\n3\r\n4\r\n5");
    t.resize(3, 10);
    WT_CHECK_EQ(t.rows(), 3);
    WT_CHECK_EQ(t.lineText(2), std::string("5"));
    WT_CHECK_EQ(t.cursor().row, 2);
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(2));
    t.resize(5, 4);
    WT_CHECK_EQ(t.lineText(0), std::string("1")); // pulled back from scrollback
    WT_CHECK_EQ(t.cursor().row, 4);
    WT_CHECK_EQ(t.line(0).cells.size(), std::size_t(4));
    t.feed(CSI "1;4H" "Z");
    WT_CHECK_EQ(cellAt(t, 0, 3).ch, U'Z');
}

WT_TEST(terminal_resize_rewraps_wide_char_instead_of_splitting_it) {
    Terminal t(1, 4);
    t.feed("ab中");
    t.resize(1, 3);
    WT_CHECK_EQ(t.lineText(0), std::string("中")); // the cursor line stays visible
    WT_REQUIRE(t.scrollbackSize() == 1);
    WT_CHECK_EQ(t.lineToText(t.scrollbackLine(0)), std::string("ab"));
    WT_CHECK(t.scrollbackLine(0).wrapped);
}

WT_TEST(terminal_full_reset) {
    Terminal t(2, 10);
    t.feed(CSI "31m" CSI "?25l" "junk" ESC "c");
    WT_CHECK_EQ(t.screenText(), std::string(""));
    WT_CHECK(t.cursor().visible);
    WT_CHECK(t.currentAttributes() == CellAttributes{});
}

WT_TEST(terminal_palette) {
    WT_CHECK_EQ(paletteColor(1), 0xCD0000u);
    WT_CHECK_EQ(paletteColor(16), 0x000000u);
    WT_CHECK_EQ(paletteColor(196), 0xFF0000u);
    WT_CHECK_EQ(paletteColor(231), 0xFFFFFFu);
    WT_CHECK_EQ(paletteColor(232), 0x080808u);
    WT_CHECK_EQ(paletteColor(255), 0xEEEEEEu);
}

WT_TEST(terminal_combining_marks_form_clusters) {
    Terminal t(2, 10);
    t.feed("e\xCC\x81x"); // e + COMBINING ACUTE ACCENT, x
    WT_CHECK_EQ(t.lineText(0), std::string("e\xCC\x81x"));
    WT_CHECK(cellAt(t, 0, 0).isCluster());
    WT_CHECK(t.cellText(cellAt(t, 0, 0)) == std::u32string_view(U"é"));
    WT_CHECK_EQ(cellAt(t, 0, 1).ch, U'x');
    WT_CHECK_EQ(t.cursor().column, 2);

    // Identical clusters share one entry; marks attach to the leading half of wide chars.
    t.feed("\r\n\xE4\xB8\xAD\xEF\xB8\x8F" "e\xCC\x81");
    WT_CHECK(cellAt(t, 1, 0).isCluster());
    WT_CHECK_EQ(cellAt(t, 1, 0).width, std::uint8_t(2));
    WT_CHECK(t.cellText(cellAt(t, 1, 0)) == std::u32string_view(U"中️"));
    WT_CHECK_EQ(cellAt(t, 1, 2).ch, cellAt(t, 0, 0).ch);
}

WT_TEST(terminal_combining_mark_with_pending_wrap_and_at_line_start) {
    Terminal t(2, 3);
    t.feed("abc\xCC\x88"); // mark lands on the last column while the wrap is pending
    WT_CHECK_EQ(t.lineText(0), std::string("abc\xCC\x88"));
    WT_CHECK_EQ(t.cursor().row, 0);
    t.feed("\r\n\xCC\x88z"); // nothing to attach to at column 0: dropped
    WT_CHECK_EQ(t.lineText(1), std::string("z"));
    // Absurdly long mark sequences are capped instead of growing without bound.
    std::string zalgo = "\r\nq";
    for (int i = 0; i < 100; ++i) {
        zalgo += "\xCC\x81";
    }
    t.feed(zalgo);
    WT_CHECK(t.cellText(cellAt(t, 1, 0)).size() <= 16);
}

WT_TEST(terminal_osc_colors_set_query_and_reset) {
    Terminal t(2, 10);
    std::string response;
    t.setResponseHandler([&](std::string_view bytes) { response += bytes; });
    t.feed(ESC "]4;1;rgb:12/34/56" ESC "\\");
    WT_CHECK_EQ(t.palette().indexed[1], 0x123456u);
    t.feed(ESC "]4;1;?\a");
    WT_CHECK_EQ(response, std::string(ESC "]4;1;rgb:1212/3434/5656\a"));
    response.clear();
    t.feed(ESC "]11;?" ESC "\\");
    WT_CHECK_EQ(response, std::string(ESC "]11;rgb:1e1e/1e1e/1e1e" ESC "\\"));
    t.feed(ESC "]10;#fff;#000080\a"); // second spec addresses OSC 11
    WT_CHECK_EQ(t.palette().foreground, 0xF0F0F0u); // #RGB digits are the high nibbles
    WT_CHECK_EQ(t.palette().background, 0x000080u);
    t.feed(ESC "]12;rgb:f/0/0\a");
    WT_CHECK_EQ(t.palette().cursor, 0xFF0000u);
    t.feed(ESC "]4;2;#a0b0c0;3;bogus\a");
    WT_CHECK_EQ(t.palette().indexed[2], 0xA0B0C0u);
    WT_CHECK_EQ(t.palette().indexed[3], paletteColor(3));
    t.feed(ESC "]104;2\a");
    WT_CHECK_EQ(t.palette().indexed[2], paletteColor(2));
    WT_CHECK_EQ(t.palette().indexed[1], 0x123456u);
    t.feed(ESC "]104\a" ESC "]110\a" ESC "]111\a" ESC "]112\a");
    WT_CHECK(t.palette() == ColorPalette::xterm());

    ColorPalette theme = ColorPalette::xterm();
    theme.background = 0x002B36;
    t.setDefaultPalette(theme);
    t.feed(ESC "]11;#ffffff\a" ESC "c");
    WT_CHECK_EQ(t.palette().background, 0x002B36u); // RIS restores the configured theme
}

WT_TEST(terminal_osc7_working_directory) {
    Terminal t(2, 10);
    t.feed(ESC "]7;file://host/Users/me/My%20Dir\a");
    WT_CHECK_EQ(t.workingDirectory(), std::string("/Users/me/My Dir"));
    t.feed(ESC "]7;http://nope/x\a");
    WT_CHECK_EQ(t.workingDirectory(), std::string("/Users/me/My Dir"));
}

WT_TEST(terminal_decrqm_reports_modes) {
    Terminal t(2, 10);
    std::string response;
    t.setResponseHandler([&](std::string_view bytes) { response += bytes; });
    t.feed(CSI "?2004$p");
    WT_CHECK_EQ(response, std::string(CSI "?2004;2$y"));
    response.clear();
    t.feed(CSI "?2004h" CSI "?2004$p" CSI "?1049$p" CSI "?7$p" CSI "?9999$p");
    WT_CHECK_EQ(response, std::string(CSI "?2004;1$y" CSI "?1049;2$y" CSI "?7;1$y" CSI "?9999;0$y"));
    response.clear();
    t.feed(CSI "4h" CSI "4$p" CSI "20$p" CSI "2$p");
    WT_CHECK_EQ(response, std::string(CSI "4;1$y" CSI "20;2$y" CSI "2;0$y"));
}

WT_TEST(terminal_mouse_and_misc_private_modes) {
    Terminal t(2, 10);
    WT_CHECK(t.modes().alternateScroll);
    t.feed(CSI "?1002h" CSI "?1015h" CSI "?1006h");
    WT_CHECK(t.modes().mouseTracking == MouseTracking::ButtonEvent);
    WT_CHECK(t.modes().mouseEncoding == MouseEncoding::Sgr);
    t.feed(CSI "?1015l"); // resetting a mode that is not active keeps the current encoding
    WT_CHECK(t.modes().mouseEncoding == MouseEncoding::Sgr);
    t.feed(CSI "?1006l");
    WT_CHECK(t.modes().mouseEncoding == MouseEncoding::Default);
    t.feed(CSI "?1005h" CSI "?1007l" CSI "?2026h");
    WT_CHECK(t.modes().mouseEncoding == MouseEncoding::Utf8);
    WT_CHECK(!t.modes().alternateScroll);
    WT_CHECK(t.modes().synchronizedOutput);
    t.feed(CSI "?2026l");
    WT_CHECK(!t.modes().synchronizedOutput);
}

WT_TEST(terminal_title_stack_and_window_reports) {
    Terminal t(3, 12);
    std::string response;
    t.setResponseHandler([&](std::string_view bytes) { response += bytes; });
    t.feed(ESC "]2;shell\a" CSI "22;0t" ESC "]2;vim\a");
    WT_CHECK_EQ(t.title(), std::string("vim"));
    t.feed(CSI "23;0t");
    WT_CHECK_EQ(t.title(), std::string("shell"));
    t.feed(CSI "23;0t"); // empty stack: no change
    WT_CHECK_EQ(t.title(), std::string("shell"));

    t.feed(CSI "14t");
    WT_CHECK_EQ(response, std::string()); // pixel size unknown
    t.setCellPixelSize(8, 16);
    t.feed(CSI "14t" CSI "16t" CSI "18t" CSI "19t");
    WT_CHECK_EQ(response, std::string(CSI "4;48;96t" CSI "6;16;8t" CSI "8;3;12t" CSI "9;3;12t"));
    response.clear();
    t.feed(CSI ">q");
    WT_CHECK_EQ(response, std::string("\x1bP>|WindTerm-Weifan\x1b\\"));
}

WT_TEST(terminal_absolute_rows_survive_scrolling) {
    Terminal t(3, 10, 4);
    WT_CHECK_EQ(t.historyStart(), std::int64_t(0));
    t.feed("a\r\nb\r\nc\r\nd"); // "a" scrolls into the history
    WT_CHECK_EQ(t.screenStart(), std::int64_t(1));
    WT_CHECK_EQ(t.lineToText(*t.absoluteLine(0)), std::string("a"));
    WT_CHECK_EQ(t.lineToText(*t.absoluteLine(3)), std::string("d"));
    WT_CHECK(t.absoluteLine(4) == nullptr);
    t.feed("\r\ne\r\nf\r\ng\r\nh"); // history limit 4 drops "a"
    WT_CHECK_EQ(t.historyStart(), std::int64_t(1));
    WT_CHECK(t.absoluteLine(0) == nullptr);
    WT_CHECK_EQ(t.lineToText(*t.absoluteLine(1)), std::string("b"));
    t.feed(CSI "3J");
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(0));
    WT_CHECK_EQ(t.historyStart(), t.screenStart());
    WT_CHECK_EQ(t.lineToText(*t.absoluteLine(t.screenStart())), std::string("f"));
}

WT_TEST(key_encoder_cursor_keys) {
    KeyboardModes normal;
    KeyboardModes app;
    app.cursorKeysApplication = true;
    WT_CHECK_EQ(encodeKey(Key::Up, NoModifier, normal), std::string(CSI "A"));
    WT_CHECK_EQ(encodeKey(Key::Up, NoModifier, app), std::string(ESC "OA"));
    WT_CHECK_EQ(encodeKey(Key::Left, ControlModifier, app), std::string(CSI "1;5D"));
    WT_CHECK_EQ(encodeKey(Key::Home, ShiftModifier, normal), std::string(CSI "1;2H"));
    WT_CHECK_EQ(encodeKey(Key::End, NoModifier, normal), std::string(CSI "F"));
}

WT_TEST(key_encoder_function_and_editing_keys) {
    KeyboardModes m;
    WT_CHECK_EQ(encodeKey(Key::F1, NoModifier, m), std::string(ESC "OP"));
    WT_CHECK_EQ(encodeKey(Key::F1, ShiftModifier, m), std::string(CSI "1;2P"));
    WT_CHECK_EQ(encodeKey(Key::F5, NoModifier, m), std::string(CSI "15~"));
    WT_CHECK_EQ(encodeKey(Key::F12, AltModifier, m), std::string(CSI "24;3~"));
    WT_CHECK_EQ(encodeKey(Key::Delete, NoModifier, m), std::string(CSI "3~"));
    WT_CHECK_EQ(encodeKey(Key::PageDown, ControlModifier, m), std::string(CSI "6;5~"));
    WT_CHECK_EQ(encodeKey(Key::Backspace, NoModifier, m), std::string("\x7f"));
    WT_CHECK_EQ(encodeKey(Key::Backspace, AltModifier, m), std::string(ESC "\x7f"));
    WT_CHECK_EQ(encodeKey(Key::Tab, ShiftModifier, m), std::string(CSI "Z"));
    WT_CHECK_EQ(encodeKey(Key::Enter, NoModifier, m), std::string("\r"));
    KeyboardModes lnm;
    lnm.newLineMode = true;
    WT_CHECK_EQ(encodeKey(Key::Enter, NoModifier, lnm), std::string("\r\n"));
}

WT_TEST(key_encoder_characters) {
    WT_CHECK_EQ(encodeChar(U'c', ControlModifier), std::string("\x03"));
    WT_CHECK_EQ(encodeChar(U'C', ControlModifier), std::string("\x03"));
    WT_CHECK_EQ(encodeChar(U'[', ControlModifier), std::string("\x1b"));
    WT_CHECK_EQ(encodeChar(U' ', ControlModifier), std::string(1, '\0'));
    WT_CHECK_EQ(encodeChar(U'?', ControlModifier), std::string("\x7f"));
    WT_CHECK_EQ(encodeChar(U'x', AltModifier), std::string(ESC "x"));
    WT_CHECK_EQ(encodeChar(U'中', NoModifier), std::string("\xE4\xB8\xAD"));
}

WT_TEST(key_encoder_keypad) {
    KeyboardModes numeric;
    KeyboardModes app;
    app.keypadApplication = true;
    WT_CHECK_EQ(encodeKey(Key::Keypad5, NoModifier, numeric), std::string("5"));
    WT_CHECK_EQ(encodeKey(Key::KeypadPlus, NoModifier, numeric), std::string("+"));
    WT_CHECK_EQ(encodeKey(Key::KeypadEnter, NoModifier, numeric), std::string("\r"));
    WT_CHECK_EQ(encodeKey(Key::Keypad5, NoModifier, app), std::string(ESC "Ou"));
    WT_CHECK_EQ(encodeKey(Key::Keypad0, NoModifier, app), std::string(ESC "Op"));
    WT_CHECK_EQ(encodeKey(Key::KeypadEnter, NoModifier, app), std::string(ESC "OM"));
    WT_CHECK_EQ(encodeKey(Key::KeypadDivide, NoModifier, app), std::string(ESC "Oo"));
    WT_CHECK_EQ(encodeKey(Key::KeypadEqual, NoModifier, app), std::string(ESC "OX"));
    WT_CHECK_EQ(encodeKey(Key::KeypadMinus, AltModifier, numeric), std::string(ESC "-"));
}

WT_TEST(key_encoder_modifier_combinations) {
    KeyboardModes m;
    WT_CHECK_EQ(encodeKey(Key::Up, ShiftModifier | AltModifier | ControlModifier, m), std::string(CSI "1;8A"));
    WT_CHECK_EQ(encodeKey(Key::Delete, ShiftModifier, m), std::string(CSI "3;2~"));
    WT_CHECK_EQ(encodeKey(Key::F3, ControlModifier, m), std::string(CSI "1;5R"));
    WT_CHECK_EQ(encodeKey(Key::Escape, AltModifier, m), std::string(ESC ESC));
    WT_CHECK_EQ(encodeKey(Key::Backspace, ControlModifier, m), std::string("\x08"));
    WT_CHECK_EQ(encodeChar(U'2', ControlModifier), std::string(1, '\0'));
    WT_CHECK_EQ(encodeChar(U'3', ControlModifier), std::string(ESC));
    WT_CHECK_EQ(encodeChar(U'/', ControlModifier), std::string("\x1f"));
    WT_CHECK_EQ(encodeChar(U'c', ControlModifier | AltModifier), std::string(ESC "\x03"));
}

WT_TEST(mouse_encoder_tracking_modes) {
    MouseEvent press{MouseAction::Press, MouseButton::Left, NoModifier, 0, 0};
    MouseEvent release{MouseAction::Release, MouseButton::Left, NoModifier, 0, 0};
    MouseEvent drag{MouseAction::Motion, MouseButton::Left, NoModifier, 1, 2};
    MouseEvent hover{MouseAction::Motion, MouseButton::None, NoModifier, 1, 2};
    const auto def = MouseEncoding::Default;

    WT_CHECK_EQ(encodeMouse(press, MouseTracking::None, def), std::string());
    WT_CHECK_EQ(encodeMouse(press, MouseTracking::X10, def), std::string(CSI "M !!"));
    WT_CHECK_EQ(encodeMouse(release, MouseTracking::X10, def), std::string());
    WT_CHECK_EQ(encodeMouse(release, MouseTracking::Normal, def), std::string(CSI "M#!!"));
    WT_CHECK_EQ(encodeMouse(drag, MouseTracking::Normal, def), std::string());
    WT_CHECK_EQ(encodeMouse(drag, MouseTracking::ButtonEvent, def), std::string(CSI "M@#\""));
    WT_CHECK_EQ(encodeMouse(hover, MouseTracking::ButtonEvent, def), std::string());
    WT_CHECK_EQ(encodeMouse(hover, MouseTracking::AnyEvent, def), std::string(CSI "MC#\""));
}

WT_TEST(mouse_encoder_buttons_modifiers_and_wheel) {
    const auto sgr = MouseEncoding::Sgr;
    const auto normal = MouseTracking::Normal;
    WT_CHECK_EQ(encodeMouse({MouseAction::Press, MouseButton::Right, NoModifier, 4, 9}, normal, sgr),
                std::string(CSI "<2;10;5M"));
    WT_CHECK_EQ(encodeMouse({MouseAction::Release, MouseButton::Right, NoModifier, 4, 9}, normal, sgr),
                std::string(CSI "<2;10;5m"));
    WT_CHECK_EQ(encodeMouse({MouseAction::Press, MouseButton::Middle, ShiftModifier | ControlModifier, 0, 0},
                            normal, sgr),
                std::string(CSI "<21;1;1M"));
    WT_CHECK_EQ(encodeMouse({MouseAction::Press, MouseButton::Left, AltModifier, 0, 0}, normal, sgr),
                std::string(CSI "<8;1;1M"));
    WT_CHECK_EQ(encodeMouse({MouseAction::Press, MouseButton::WheelUp, NoModifier, 2, 3}, normal, sgr),
                std::string(CSI "<64;4;3M"));
    WT_CHECK_EQ(encodeMouse({MouseAction::Press, MouseButton::WheelDown, NoModifier, 2, 3}, normal, sgr),
                std::string(CSI "<65;4;3M"));
    WT_CHECK_EQ(encodeMouse({MouseAction::Release, MouseButton::WheelDown, NoModifier, 2, 3}, normal, sgr),
                std::string());
    WT_CHECK_EQ(encodeMouse({MouseAction::Press, MouseButton::WheelUp, NoModifier, 0, 0}, MouseTracking::X10, sgr),
                std::string());
    // X10 never reports modifiers.
    WT_CHECK_EQ(encodeMouse({MouseAction::Press, MouseButton::Left, ControlModifier, 0, 0}, MouseTracking::X10,
                            MouseEncoding::Default),
                std::string(CSI "M !!"));
}

WT_TEST(mouse_encoder_coordinate_encodings) {
    const MouseEvent far{MouseAction::Press, MouseButton::Left, NoModifier, 299, 499};
    WT_CHECK_EQ(encodeMouse(far, MouseTracking::Normal, MouseEncoding::Default), std::string());
    WT_CHECK_EQ(encodeMouse(far, MouseTracking::Normal, MouseEncoding::Sgr), std::string(CSI "<0;500;300M"));
    WT_CHECK_EQ(encodeMouse(far, MouseTracking::Normal, MouseEncoding::Urxvt), std::string(CSI "32;500;300M"));
    // UTF-8 (?1005): 500 + 32 = U+0214, 300 + 32 = U+014C.
    WT_CHECK_EQ(encodeMouse(far, MouseTracking::Normal, MouseEncoding::Utf8),
                std::string(CSI "M \xC8\x94\xC5\x8C"));
    const MouseEvent edge{MouseAction::Press, MouseButton::Left, NoModifier, 0, 222};
    WT_CHECK_EQ(encodeMouse(edge, MouseTracking::Normal, MouseEncoding::Default), std::string(CSI "M \xff!"));
}

WT_TEST(key_encoder_paste) {
    WT_CHECK_EQ(encodePaste("a\nb\r\nc", false), std::string("a\rb\rc"));
    WT_CHECK_EQ(encodePaste("x", true), std::string(CSI "200~x" CSI "201~"));
    WT_CHECK_EQ(encodePaste("evil" CSI "201~rm", true), std::string(CSI "200~evilrm" CSI "201~"));
}

WT_TEST(terminal_survives_random_input_and_resizes) {
    // Deterministic stress test: random bytes mixed with escape-sequence fragments and resizes
    // must never crash or break invariants (run under ASan/UBSan via the `asan` preset).
    static const char* const kFragments[] = {
        CSI, ESC, ESC "]0;", "\a", CSI "?1049h", CSI "?1049l", CSI "2;5r", CSI "r", CSI "38;2;",
        CSI "4@", CSI "3P", CSI "2L", CSI "2M", CSI "10;10H", CSI "?6h", CSI "?7l", CSI "?7h",
        "中", "\xF0\x9F\x98", "\r\n", "\t", ESC "(0", ESC "M", CSI "5b", CSI "4h", CSI "4l",
    };
    std::uint32_t seed = 12345;
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    Terminal t(10, 20, 50);
    for (int iteration = 0; iteration < 20000; ++iteration) {
        std::string chunk;
        const int pieces = static_cast<int>(next() % 8) + 1;
        for (int i = 0; i < pieces; ++i) {
            if (next() % 3 == 0) {
                chunk += kFragments[next() % (sizeof(kFragments) / sizeof(kFragments[0]))];
            } else {
                chunk.push_back(static_cast<char>(next() & 0xFF));
            }
        }
        t.feed(chunk);
        if (iteration % 500 == 0) {
            t.resize(static_cast<int>(next() % 30) + 1, static_cast<int>(next() % 60) + 1);
        }
        WT_REQUIRE(t.cursor().row >= 0 && t.cursor().row < t.rows());
        WT_REQUIRE(t.cursor().column >= 0 && t.cursor().column < t.columns());
    }
    for (int row = 0; row < t.rows(); ++row) {
        const auto& cells = t.line(row).cells;
        WT_REQUIRE(static_cast<int>(cells.size()) == t.columns());
        for (std::size_t c = 0; c < cells.size(); ++c) {
            if (cells[c].width == 2) {
                WT_REQUIRE(c + 1 < cells.size() && cells[c + 1].width == 0); // wide pairs stay intact
            }
        }
    }
    WT_CHECK(t.scrollbackSize() <= 50);
}

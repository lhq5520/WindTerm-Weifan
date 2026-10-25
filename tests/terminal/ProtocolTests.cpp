// Phase 2 protocol coverage: DCS (DECRQSS, XTGETTCAP), OSC 8/52, underline styles, kitty keyboard,
// modifyOtherKeys, line sizes, damage tracking, wide-character padding and search.

#include "WtTest.h"

#include "wt/terminal/KeyEncoder.h"
#include "wt/terminal/Search.h"
#include "wt/terminal/Terminal.h"

#include <string>
#include <vector>

using namespace wt::terminal;

#define ESC "\x1b"
#define CSI "\x1b["
#define ST "\x1b\\"

namespace {

struct Recorder {
    std::string responses;
    void attach(Terminal& t) {
        t.setResponseHandler([this](std::string_view bytes) { responses.append(bytes); });
    }
};

const Cell& cellAt(const Terminal& t, int row, int col) {
    return t.line(row).cells[static_cast<std::size_t>(col)];
}

KeyboardModes modesOf(const Terminal& t) {
    KeyboardModes modes;
    modes.kittyFlags = t.kittyKeyboardFlags();
    modes.modifyOtherKeys = t.modes().modifyOtherKeys;
    return modes;
}

} // namespace

WT_TEST(dcs_decrqss_reports_sgr_margins_and_cursor) {
    Terminal t(10, 20);
    Recorder r;
    r.attach(t);
    t.feed(CSI "1;4:3;38;5;200;48;2;1;2;3m");
    t.feed("\x1bP$qm" ST);
    WT_CHECK_EQ(r.responses, std::string("\x1bP1$r0;1;4:3;38:5:200;48:2::1:2:3m" ST));
    r.responses.clear();
    t.feed(CSI "3;7r" "\x1bP$qr" ST);
    WT_CHECK_EQ(r.responses, std::string("\x1bP1$r3;7r" ST));
    r.responses.clear();
    t.feed(CSI "6 q" "\x1bP$q q" ST);
    WT_CHECK_EQ(r.responses, std::string("\x1bP1$r6 q" ST));
    r.responses.clear();
    t.feed("\x1bP$qxyz" ST);
    WT_CHECK_EQ(r.responses, std::string("\x1bP0$r" ST));
}

WT_TEST(dcs_xtgettcap_answers_known_capabilities) {
    Terminal t(5, 10);
    Recorder r;
    r.attach(t);
    // "TN" = 544e, "Co" = 436f, "zz" = 7a7a
    t.feed("\x1bP+q544e;436f;7a7a" ST);
    WT_CHECK_EQ(r.responses, std::string("\x1bP1+r544e=787465726d2d323536636f6c6f72" ST "\x1bP1+r436f=323536" ST
                                         "\x1bP0+r7a7a" ST));
}

WT_TEST(dcs_strings_are_bounded_split_and_cancellable) {
    Terminal t(5, 10);
    Recorder r;
    r.attach(t);
    // Split across feeds, terminated by C1 ST encoded in UTF-8.
    t.feed("\x1bP$");
    t.feed("q");
    t.feed("m\xC2\x9C");
    WT_CHECK_EQ(r.responses, std::string("\x1bP1$r0m" ST));
    r.responses.clear();
    t.feed("\x1bP$qm\x18" "after"); // CAN aborts: no reply, text printed
    WT_CHECK(r.responses.empty());
    WT_CHECK_EQ(t.lineText(0), std::string("after"));
    t.feed("\x1bP$q" + std::string(70 * 1024, 'm') + ST); // over the limit: dropped
    WT_CHECK(r.responses.empty());
    t.feed("\x1bPzzz" ST "ok"); // unknown DCS swallowed
    WT_CHECK_EQ(t.lineText(0), std::string("afterok"));
}

WT_TEST(osc8_hyperlinks_mark_cells) {
    Terminal t(5, 40);
    t.feed("a" "\x1b]8;;https://example.com/x\a" "link" "\x1b]8;;\a" "b");
    const auto id = cellAt(t, 0, 1).attributes.hyperlink;
    WT_CHECK(id != 0);
    WT_CHECK_EQ(cellAt(t, 0, 4).attributes.hyperlink, id);
    WT_CHECK_EQ(cellAt(t, 0, 0).attributes.hyperlink, std::uint16_t(0));
    WT_CHECK_EQ(cellAt(t, 0, 5).attributes.hyperlink, std::uint16_t(0));
    WT_CHECK_EQ(std::string(t.hyperlinkUri(id)), std::string("https://example.com/x"));
    WT_CHECK(t.hyperlinkUri(0).empty());
    // SGR reset does not end a link; the same id+URI reuses the id.
    t.feed("\x1b]8;id=7;https://e.org" ST "x" CSI "0my" "\x1b]8;;" ST "\x1b]8;id=7;https://e.org" ST "z");
    const auto other = cellAt(t, 0, 6).attributes.hyperlink;
    WT_CHECK(other != id && other != 0);
    WT_CHECK_EQ(cellAt(t, 0, 7).attributes.hyperlink, other);
    WT_CHECK_EQ(cellAt(t, 0, 8).attributes.hyperlink, other);
}

WT_TEST(osc52_clipboard_requires_permission) {
    Terminal t(5, 20);
    std::vector<std::pair<char, std::string>> writes;
    t.setClipboardHandler([&](char target, const std::string& text) { writes.emplace_back(target, text); });
    t.feed("\x1b]52;c;aGVsbG8=\a");
    WT_CHECK(writes.empty());
    t.setClipboardWriteAllowed(true);
    t.feed("\x1b]52;c;aGVsbG8=\a");
    t.feed("\x1b]52;p;d29ybGQ=" ST);
    t.feed("\x1b]52;c;?\a");    // query: never answered
    t.feed("\x1b]52;c;!!!\a");  // invalid base64
    WT_REQUIRE(writes.size() == 2u);
    WT_CHECK(writes[0] == std::make_pair('c', std::string("hello")));
    WT_CHECK(writes[1] == std::make_pair('p', std::string("world")));
}

WT_TEST(sgr_underline_styles_colour_and_overline) {
    Terminal t(5, 20);
    t.feed(CSI "4:3m" "a" CSI "21m" "b" CSI "4m" "c" CSI "24m" "d");
    WT_CHECK(cellAt(t, 0, 0).attributes.underlineStyle() == UnderlineStyle::Curly);
    WT_CHECK(cellAt(t, 0, 1).attributes.underlineStyle() == UnderlineStyle::Double);
    WT_CHECK(cellAt(t, 0, 2).attributes.underlineStyle() == UnderlineStyle::Single);
    WT_CHECK(cellAt(t, 0, 3).attributes.underlineStyle() == UnderlineStyle::None);
    WT_CHECK(cellAt(t, 0, 0).attributes.has(Underline));
    t.feed(CSI "4:5;58:2::10:20:30;53m" "e" CSI "59;55;4:0m" "f");
    const auto& e = cellAt(t, 0, 4).attributes;
    WT_CHECK(e.underlineStyle() == UnderlineStyle::Dashed);
    WT_CHECK(e.underlineColor == Color::rgb(10, 20, 30));
    WT_CHECK(e.has(Overline));
    const auto& f = cellAt(t, 0, 5).attributes;
    WT_CHECK(f.underlineColor.isDefault() && !f.has(Overline) && !f.has(Underline));
    t.feed(CSI "58;5;9m" "g");
    WT_CHECK(cellAt(t, 0, 6).attributes.underlineColor == Color::indexed(9));
}

WT_TEST(kitty_keyboard_flags_stack_per_screen) {
    Terminal t(5, 20);
    Recorder r;
    r.attach(t);
    t.feed(CSI "?u");
    WT_CHECK_EQ(r.responses, std::string(CSI "?0u"));
    t.feed(CSI ">1u");
    WT_CHECK_EQ(t.kittyKeyboardFlags(), 1u);
    t.feed(CSI ">9u");
    WT_CHECK_EQ(t.kittyKeyboardFlags(), 9u);
    t.feed(CSI "?1049h");
    WT_CHECK_EQ(t.kittyKeyboardFlags(), 0u); // separate stack on the alternate screen
    t.feed(CSI "=3;1u");
    WT_CHECK_EQ(t.kittyKeyboardFlags(), 3u);
    t.feed(CSI "=1;3u");
    WT_CHECK_EQ(t.kittyKeyboardFlags(), 2u);
    t.feed(CSI "?1049l");
    WT_CHECK_EQ(t.kittyKeyboardFlags(), 9u);
    t.feed(CSI "<u");
    WT_CHECK_EQ(t.kittyKeyboardFlags(), 1u);
    t.feed(CSI "<5u");
    WT_CHECK_EQ(t.kittyKeyboardFlags(), 0u);
    t.feed(CSI ">1u" ESC "c");
    WT_CHECK_EQ(t.kittyKeyboardFlags(), 0u); // RIS
    // CSI u without a prefix is still SCORC.
    t.feed(CSI "3;3H" CSI "s" CSI "H" CSI "u");
    WT_CHECK_EQ(t.cursor().row, 2);
}

WT_TEST(kitty_keyboard_encoding) {
    KeyboardModes legacy;
    KeyboardModes kitty;
    kitty.kittyFlags = KittyDisambiguate;
    KeyboardModes all;
    all.kittyFlags = KittyDisambiguate | KittyReportAllKeys;
    WT_CHECK_EQ(encodeKey(Key::Escape, NoModifier, legacy), std::string(ESC));
    WT_CHECK_EQ(encodeKey(Key::Escape, NoModifier, kitty), std::string(CSI "27u"));
    WT_CHECK_EQ(encodeKey(Key::Enter, NoModifier, kitty), std::string("\r"));
    WT_CHECK_EQ(encodeKey(Key::Enter, ShiftModifier, kitty), std::string(CSI "13;2u"));
    WT_CHECK_EQ(encodeKey(Key::Tab, ControlModifier, kitty), std::string(CSI "9;5u"));
    WT_CHECK_EQ(encodeKey(Key::Backspace, AltModifier, kitty), std::string(CSI "127;3u"));
    WT_CHECK_EQ(encodeKey(Key::Enter, NoModifier, all), std::string(CSI "13u"));
    WT_CHECK_EQ(encodeKey(Key::Up, ControlModifier, kitty), std::string(CSI "1;5A")); // unchanged
    WT_CHECK_EQ(encodeCharKey(U'a', ControlModifier, kitty), std::string(CSI "97;5u"));
    WT_CHECK_EQ(encodeCharKey(U'A', ControlModifier | ShiftModifier, kitty), std::string(CSI "97;6u"));
    WT_CHECK_EQ(encodeCharKey(U'a', NoModifier, kitty), std::string("a"));
    WT_CHECK_EQ(encodeCharKey(U'A', ShiftModifier, kitty), std::string("A"));
    WT_CHECK_EQ(encodeCharKey(U'a', NoModifier, all), std::string(CSI "97u"));
    WT_CHECK_EQ(encodeCharKey(U'a', ControlModifier, legacy), std::string("\x01"));
    WT_CHECK_EQ(encodeCharKey(U'x', AltModifier, legacy), std::string(ESC "x"));
}

WT_TEST(modify_other_keys_levels) {
    Terminal t(5, 20);
    t.feed(CSI ">4;1m");
    KeyboardModes level1 = modesOf(t);
    WT_CHECK_EQ(level1.modifyOtherKeys, 1);
    WT_CHECK_EQ(encodeCharKey(U'a', ControlModifier, level1), std::string("\x01"));
    WT_CHECK_EQ(encodeCharKey(U',', ControlModifier, level1), std::string(CSI "27;5;44~"));
    WT_CHECK_EQ(encodeCharKey(U'A', ControlModifier | ShiftModifier, level1), std::string(CSI "27;6;65~"));
    WT_CHECK_EQ(encodeCharKey(U'x', AltModifier, level1), std::string(ESC "x"));
    t.feed(CSI ">4;2m");
    KeyboardModes level2 = modesOf(t);
    WT_CHECK_EQ(encodeCharKey(U'a', ControlModifier, level2), std::string(CSI "27;5;97~"));
    WT_CHECK_EQ(encodeCharKey(U'x', AltModifier, level2), std::string(CSI "27;3;120~"));
    WT_CHECK_EQ(encodeCharKey(U'x', NoModifier, level2), std::string("x"));
    t.feed(CSI ">4m");
    WT_CHECK_EQ(t.modes().modifyOtherKeys, 0);
}

WT_TEST(dec_line_sizes) {
    Terminal t(5, 20);
    t.feed(ESC "#6" "wide\r\n" ESC "#3" "top\r\n" ESC "#4" "bottom\r\n" ESC "#5");
    WT_CHECK(t.line(0).size == LineSize::DoubleWidth);
    WT_CHECK(t.line(1).size == LineSize::DoubleHeightTop);
    WT_CHECK(t.line(2).size == LineSize::DoubleHeightBottom);
    WT_CHECK(t.line(3).size == LineSize::Normal);
    t.feed(CSI "2J");
    WT_CHECK(t.line(0).size == LineSize::Normal);
}

WT_TEST(damage_tracks_changed_rows) {
    Terminal t(6, 20);
    WT_CHECK(t.takeDamage().full); // initial paint
    WT_CHECK(!t.takeDamage().full);
    t.feed(CSI "3;1Hhello");
    auto damage = t.takeDamage();
    WT_CHECK(!damage.full);
    WT_CHECK(damage.rows == std::vector<int>{2});
    t.feed(CSI "5;1H" CSI "K");
    WT_CHECK(t.takeDamage().rows == std::vector<int>{4});
    t.feed(CSI "6;1H\n"); // scroll: every row moved
    WT_CHECK_EQ(t.takeDamage().rows.size(), std::size_t(6));
    t.feed(CSI "H"); // cursor motion alone damages nothing
    WT_CHECK(t.takeDamage().rows.empty());
    t.resize(4, 10);
    WT_CHECK(t.takeDamage().full);
    t.feed(CSI "?5h");
    WT_CHECK(t.takeDamage().full);
    t.feed("\x1b]11;#000000\a");
    WT_CHECK(t.takeDamage().full);
    t.startSelection({t.screenStart(), 0}, SelectionMode::Character);
    WT_CHECK(t.takeDamage().full);
}

WT_TEST(wide_padding_is_not_text) {
    Terminal t(4, 5);
    t.feed("abcd中x");
    WT_CHECK(t.line(0).wrapped);
    WT_CHECK(cellAt(t, 0, 4).attributes.has(WidePadding));
    WT_CHECK_EQ(t.lineText(0), std::string("abcd"));
    t.startSelection({t.screenStart(), 0}, SelectionMode::Character);
    t.extendSelection({t.screenStart() + 1, 2});
    WT_CHECK_EQ(t.selectedText(), std::string("abcd中x"));
    t.resize(4, 10); // reflow joins without the padding blank
    WT_CHECK_EQ(t.lineText(0), std::string("abcd中x"));
    WT_CHECK(!t.line(0).wrapped);
}

WT_TEST(selection_follows_reflow) {
    Terminal t(6, 20);
    t.feed("first line\r\nhello wonderful world\r\nlast");
    const std::int64_t row = t.screenStart() + 1;
    t.startSelection({row, 6}, SelectionMode::Character);
    t.extendSelection({row, 14}); // "wonderful"
    WT_CHECK_EQ(t.selectedText(), std::string("wonderful"));
    t.resize(6, 8);
    WT_REQUIRE(t.hasSelection());
    WT_CHECK_EQ(t.selectedText(), std::string("wonderful"));
    t.resize(6, 30);
    WT_REQUIRE(t.hasSelection());
    WT_CHECK_EQ(t.selectedText(), std::string("wonderful"));
    // Word selection keeps its mode.
    t.startSelection({t.screenStart() + 1, 1}, SelectionMode::Word);
    t.resize(6, 4);
    WT_CHECK_EQ(t.selectedText(), std::string("hello"));
    // Block selections are dropped.
    t.startSelection({t.screenStart(), 0}, SelectionMode::Block);
    t.resize(6, 12);
    WT_CHECK(!t.hasSelection());
}

WT_TEST(search_literal_regex_and_wrapped_lines) {
    Terminal t(4, 10, 100);
    t.feed("Alpha beta\r\ngamma ALPHA\r\nxxxxxxxxalpha\r\nend");
    // "alpha" on the third logical line wraps after "xxxxxxxxal".
    auto matches = searchTerminal(t, literalMatcher("alpha", false));
    WT_REQUIRE(matches.size() == 3u);
    WT_CHECK_EQ(matches[0].start.column, 0);
    WT_CHECK_EQ(matches[1].start.column, 6);
    WT_CHECK_EQ(matches[2].start.column, 8);
    WT_CHECK_EQ(matches[2].end.row, matches[2].start.row + 1);
    WT_CHECK_EQ(matches[2].end.column, 2);
    WT_CHECK(matches[0].start.row < t.screenStart()); // found in the scrollback
    WT_CHECK_EQ(searchTerminal(t, literalMatcher("ALPHA", true)).size(), std::size_t(1));

    std::string error;
    auto regex = regexMatcher("g[a-z]+a", true, &error);
    WT_REQUIRE(regex.has_value());
    matches = searchTerminal(t, *regex);
    WT_REQUIRE(matches.size() == 1u);
    WT_CHECK_EQ(matches[0].end.column - matches[0].start.column, 4);
    WT_CHECK(!regexMatcher("(", true, &error));
    WT_CHECK(!error.empty());

    SearchOptions whole;
    whole.wholeWord = true;
    WT_CHECK_EQ(searchTerminal(t, literalMatcher("alpha", false), whole).size(), std::size_t(2));
    SearchOptions capped;
    capped.maxMatches = 2;
    WT_CHECK_EQ(searchTerminal(t, literalMatcher("a", false), capped).size(), std::size_t(2));
}

WT_TEST(search_unicode_case_folding_and_wide_chars) {
    Terminal t(3, 20);
    t.feed("Привет 中文 ÉCOLE");
    auto matches = searchTerminal(t, literalMatcher("привет", false));
    WT_CHECK_EQ(matches.size(), std::size_t(1));
    matches = searchTerminal(t, literalMatcher("中文", true));
    WT_REQUIRE(matches.size() == 1u);
    WT_CHECK_EQ(matches[0].start.column, 7);
    WT_CHECK_EQ(matches[0].end.column, 10); // both halves of the second wide character
    WT_CHECK_EQ(searchTerminal(t, literalMatcher("école", false)).size(), std::size_t(1));
}

WT_TEST(search_next_match_wraps_around) {
    std::vector<SearchMatch> matches = {{{1, 0}, {1, 2}}, {{3, 4}, {3, 5}}, {{7, 0}, {7, 1}}};
    WT_CHECK_EQ(nextMatch(matches, {0, 0}, false).value_or(99), std::size_t(0));
    WT_CHECK_EQ(nextMatch(matches, {1, 0}, false).value_or(99), std::size_t(1));
    WT_CHECK_EQ(nextMatch(matches, {9, 0}, false).value_or(99), std::size_t(0));
    WT_CHECK_EQ(nextMatch(matches, {3, 4}, true).value_or(99), std::size_t(0));
    WT_CHECK_EQ(nextMatch(matches, {0, 0}, true).value_or(99), std::size_t(2));
    WT_CHECK(!nextMatch({}, {0, 0}, false));
}

// ---- Regressions found in review -------------------------------------------------------------

WT_TEST(osc8_ids_are_bounded) {
    Terminal t(5, 40);
    // A huge id is truncated, so thousands of distinct links cannot exhaust memory.
    for (int i = 0; i < 200; ++i) {
        t.feed("\x1b]8;id=" + std::string(60000, 'x') + std::to_string(i) + ";http://a\x1b\\" "L\x1b]8;;\x1b\\");
    }
    // Ids beyond the 250-byte limit collapse into one link.
    const auto id = cellAt(t, 0, 0).attributes.hyperlink;
    WT_CHECK(id != 0);
    t.feed("\r\x1b]8;id=" + std::string(300, 'y') + ";http://b\x1b\\" "M" "\x1b]8;id=" + std::string(400, 'y') +
           ";http://b\x1b\\" "N\x1b]8;;\x1b\\");
    WT_CHECK_EQ(cellAt(t, 0, 0).attributes.hyperlink, cellAt(t, 0, 1).attributes.hyperlink);
}

WT_TEST(bracketed_paste_cannot_be_terminated_by_fragments) {
    const std::string pasted = encodePaste("\x1b[20\x1b[201~1~; rm -rf ~\n", true);
    const std::string body = pasted.substr(6, pasted.size() - 12); // without the markers
    WT_CHECK_EQ(pasted.substr(0, 6), std::string("\x1b[200~"));
    WT_CHECK_EQ(pasted.substr(pasted.size() - 6), std::string("\x1b[201~"));
    WT_CHECK(body.find('\x1b') == std::string::npos);
    WT_CHECK_EQ(encodePaste("a\x1b" "b", false), std::string("a\x1b" "b")); // unbracketed: as typed
}

WT_TEST(pending_wrap_survives_resize) {
    Terminal rowsOnly(3, 10);
    rowsOnly.feed("0123456789");
    rowsOnly.resize(5, 10);
    rowsOnly.feed("A");
    WT_CHECK_EQ(rowsOnly.lineText(0), std::string("0123456789"));
    WT_CHECK_EQ(rowsOnly.lineText(1), std::string("A"));

    Terminal wider(3, 10);
    wider.feed("0123456789");
    wider.resize(3, 20);
    wider.feed("A");
    WT_CHECK_EQ(wider.lineText(0), std::string("0123456789A"));

    Terminal narrower(3, 10);
    narrower.feed("0123456789");
    narrower.resize(4, 5);
    narrower.feed("A");
    WT_CHECK_EQ(narrower.lineText(0) + narrower.lineText(1) + "|" + narrower.lineText(2),
                std::string("0123456789|A"));
}

WT_TEST(xtgettcap_string_capabilities_are_expanded) {
    Terminal t(5, 10);
    Recorder r;
    r.attach(t);
    t.feed("\x1bP+q4d73" ST); // "Ms"
    // "\x1b]52;%p1%s;%p2%s\x07" hex-encoded
    const std::string expected = "\x1bP1+r4d73=1b5d35323b25703125733b257032257307" ST;
    WT_CHECK_EQ(r.responses, expected);
}

WT_TEST(reflow_maps_points_on_wide_padding_to_the_next_character) {
    Terminal t(4, 5);
    t.feed("abcd中x"); // "abcd" + padding, then 中 wraps
    const std::int64_t row = t.screenStart();
    t.startSelection({row, 4}, SelectionMode::Character); // on the padding cell
    t.extendSelection({row + 1, 1});                      // second half of 中
    WT_CHECK_EQ(t.selectedText(), std::string("中"));
    t.resize(4, 3); // "abc" "d" "中x"
    WT_REQUIRE(t.hasSelection());
    WT_CHECK_EQ(t.selectedText(), std::string("中"));
}

WT_TEST(kitty_f3_avoids_cursor_report_clash) {
    KeyboardModes kitty;
    kitty.kittyFlags = KittyDisambiguate;
    WT_CHECK_EQ(encodeKey(Key::F3, NoModifier, kitty), std::string(CSI "13~"));
    WT_CHECK_EQ(encodeKey(Key::F3, ShiftModifier, kitty), std::string(CSI "13;2~"));
    WT_CHECK_EQ(encodeKey(Key::F3, NoModifier, KeyboardModes{}), std::string(ESC "OR"));
}

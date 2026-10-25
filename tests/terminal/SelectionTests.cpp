#include "WtTest.h"

#include "wt/terminal/Terminal.h"

#include <string>

using namespace wt::terminal;

#define CSI "\x1b["

namespace {

GridPoint screenPoint(const Terminal& t, int row, int column) {
    return {t.screenStart() + row, column};
}

std::string select(Terminal& t, GridPoint from, GridPoint to, SelectionMode mode = SelectionMode::Character) {
    t.startSelection(from, mode);
    t.extendSelection(to);
    return t.selectedText();
}

} // namespace

WT_TEST(selection_character_range_and_reverse_drag) {
    Terminal t(3, 20);
    t.feed("hello world\r\nsecond line   \r\nthird");
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 6), screenPoint(t, 0, 10)), std::string("world"));
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 10), screenPoint(t, 0, 6)), std::string("world"));
    // Across lines: trailing blanks trimmed, newline between unwrapped lines.
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 6), screenPoint(t, 1, 19)), std::string("world\nsecond line"));
    WT_CHECK_EQ(select(t, screenPoint(t, 2, 3), screenPoint(t, 1, 7)), std::string("line\nthir"));
    auto range = t.selectionRange();
    WT_REQUIRE(range.has_value());
    WT_CHECK(range->contains(t.screenStart() + 1, 7));
    WT_CHECK(!range->contains(t.screenStart() + 1, 6));
    WT_CHECK(range->contains(t.screenStart() + 2, 3));
    WT_CHECK(!range->contains(t.screenStart() + 2, 4));
    t.clearSelection();
    WT_CHECK(!t.hasSelection());
    WT_CHECK_EQ(t.selectedText(), std::string());
}

WT_TEST(selection_joins_wrapped_lines) {
    Terminal t(3, 5);
    t.feed("abcdefgh\r\nxy");
    WT_CHECK(t.line(0).wrapped);
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 0), screenPoint(t, 2, 4)), std::string("abcdefgh\nxy"));
    WT_CHECK_EQ(select(t, screenPoint(t, 1, 0), screenPoint(t, 1, 0), SelectionMode::Line), std::string("abcdefgh"));
}

WT_TEST(selection_word_and_line_modes) {
    Terminal t(3, 30);
    t.feed("cd /usr/local/bin && ls (x)\r\nnext");
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 8), screenPoint(t, 0, 8), SelectionMode::Word),
                std::string("/usr/local/bin"));
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 0), screenPoint(t, 0, 4), SelectionMode::Word),
                std::string("cd /usr/local/bin"));
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 24), screenPoint(t, 0, 24), SelectionMode::Word), std::string("("));
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 25), screenPoint(t, 0, 25), SelectionMode::Word), std::string("x"));
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 3), screenPoint(t, 1, 1), SelectionMode::Line),
                std::string("cd /usr/local/bin && ls (x)\nnext"));
}

WT_TEST(selection_word_spans_wrapped_line) {
    Terminal t(3, 4);
    t.feed("ab cdefg h");
    WT_CHECK_EQ(select(t, screenPoint(t, 1, 1), screenPoint(t, 1, 1), SelectionMode::Word), std::string("cdefg"));
}

WT_TEST(selection_wide_characters_and_clusters) {
    Terminal t(2, 10);
    t.feed("a中文e\xCC\x81z");
    // Starting on the trailing half of 中 still includes the whole character.
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 2), screenPoint(t, 0, 3)), std::string("中文"));
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 0), screenPoint(t, 0, 1)), std::string("a中"));
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 5), screenPoint(t, 0, 6)), std::string("e\xCC\x81z"));
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 3), screenPoint(t, 0, 3), SelectionMode::Word),
                std::string("a中文e\xCC\x81z"));
}

WT_TEST(selection_block_mode) {
    Terminal t(3, 10);
    t.feed("abcdef\r\nghijkl\r\nmn");
    WT_CHECK_EQ(select(t, screenPoint(t, 0, 4), screenPoint(t, 2, 1), SelectionMode::Block),
                std::string("bcde\nhijk\nn"));
    auto range = t.selectionRange();
    WT_REQUIRE(range.has_value());
    WT_CHECK(range->block);
    WT_CHECK(range->contains(t.screenStart() + 1, 1));
    WT_CHECK(!range->contains(t.screenStart() + 1, 5));
}

WT_TEST(selection_follows_text_into_scrollback) {
    Terminal t(2, 10);
    t.feed("keep\r\n");
    t.startSelection(screenPoint(t, 0, 0), SelectionMode::Line);
    t.feed("1\r\n2\r\n3"); // "keep" scrolls into the history
    WT_CHECK(t.hasSelection());
    WT_CHECK_EQ(t.selectedText(), std::string("keep"));
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(2));
}

WT_TEST(selection_cleared_when_content_changes) {
    Terminal t(3, 10);
    t.feed("one\r\ntwo\r\nthree");
    t.startSelection(screenPoint(t, 1, 0), SelectionMode::Line);
    t.feed(CSI "3;1Hx"); // another row: kept
    WT_CHECK(t.hasSelection());
    t.feed(CSI "2;2Hx"); // the selected row: cleared
    WT_CHECK(!t.hasSelection());

    t.startSelection(screenPoint(t, 0, 0), SelectionMode::Line);
    t.feed(CSI "2J");
    WT_CHECK(!t.hasSelection());

    t.startSelection(screenPoint(t, 0, 0), SelectionMode::Line);
    t.feed(CSI "?1049h");
    WT_CHECK(!t.hasSelection());
    t.feed(CSI "?1049l");

    t.startSelection(screenPoint(t, 0, 0), SelectionMode::Line);
    t.resize(4, 10);
    WT_CHECK(!t.hasSelection());

    // Scrolling inside a region that does not feed the history moves text under the selection.
    t.feed(CSI "2;3r");
    t.startSelection(screenPoint(t, 3, 0), SelectionMode::Line); // outside the region
    t.feed(CSI "3;1H\n");
    WT_CHECK(t.hasSelection());
    t.startSelection(screenPoint(t, 1, 0), SelectionMode::Line);
    t.feed(CSI "3;1H\n");
    WT_CHECK(!t.hasSelection());
}

WT_TEST(selection_cleared_when_history_drops_it) {
    Terminal t(2, 10, 2);
    t.feed("x\r\n");
    t.startSelection(screenPoint(t, 0, 0), SelectionMode::Line);
    t.feed("1\r\n2\r\n");
    WT_CHECK(t.hasSelection());
    t.feed("3\r\n4\r\n");
    WT_CHECK(!t.hasSelection());
}

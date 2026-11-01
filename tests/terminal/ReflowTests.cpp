#include "WtTest.h"

#include "wt/terminal/Terminal.h"

#include <string>
#include <vector>

using namespace wt::terminal;

#define CSI "\x1b["

namespace {

// History followed by the screen, one entry per physical line.
std::vector<std::string> allLines(const Terminal& t) {
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < t.scrollbackSize(); ++i) {
        lines.push_back(t.lineToText(t.scrollbackLine(i)));
    }
    for (int row = 0; row < t.rows(); ++row) {
        lines.push_back(t.lineText(row));
    }
    return lines;
}

using Lines = std::vector<std::string>;

} // namespace

WT_TEST(reflow_narrower_rewraps_long_lines) {
    Terminal t(4, 10);
    t.feed("0123456789abc\r\nxy");
    t.resize(4, 5);
    WT_CHECK(allLines(t) == (Lines{"01234", "56789", "abc", "xy"}));
    WT_CHECK(t.line(0).wrapped && t.line(1).wrapped && !t.line(2).wrapped);
    WT_CHECK_EQ(t.cursor().row, 3);
    WT_CHECK_EQ(t.cursor().column, 2);
    for (int row = 0; row < t.rows(); ++row) {
        WT_CHECK_EQ(t.line(row).cells.size(), std::size_t(5));
    }
}

WT_TEST(reflow_wider_joins_wrapped_lines) {
    Terminal t(4, 5);
    t.feed("0123456789abc\r\nxy");
    WT_CHECK(allLines(t) == (Lines{"01234", "56789", "abc", "xy"}));
    t.resize(4, 20);
    WT_CHECK(allLines(t) == (Lines{"0123456789abc", "xy", "", ""}));
    WT_CHECK(!t.line(0).wrapped);
    WT_CHECK_EQ(t.cursor().row, 1);
    WT_CHECK_EQ(t.cursor().column, 2);
    t.feed("Z");
    WT_CHECK_EQ(t.lineText(1), std::string("xyZ"));
}

WT_TEST(reflow_moves_the_decsc_cursor_with_its_text) {
    Terminal t(4, 10);
    // Save the cursor after "0123456" (column 7 of the first line), then go elsewhere.
    t.feed("0123456789abc\r\nxy" CSI "1;8H" "\x1b" "7" CSI "4;1H");
    // "01234" (now history) / "56789" / "abc" / "xy" / "": the saved spot is row 0, column 2.
    t.resize(4, 5);
    t.feed("\x1b" "8" "#");
    WT_CHECK_EQ(t.lineText(0), std::string("56#89"));
    // Save again (after '#', on the '8'); growing back joins the lines and the cursor follows.
    t.feed("\x1b" "7" CSI "4;1H");
    t.resize(4, 20);
    t.feed("\x1b" "8" "!");
    WT_CHECK_EQ(t.lineText(0), std::string("0123456#!9abc"));
    // Fewer rows push lines into the scrollback; the saved row moves up with them.
    Terminal s(4, 10);
    s.feed("a\r\nb\r\nc\r\nd" CSI "3;1H" "\x1b" "7" CSI "4;2H");
    s.resize(2, 10); // "a" and "b" go to the scrollback
    s.feed("\x1b" "8" "X");
    WT_CHECK_EQ(s.lineText(0), std::string("X"));
}

WT_TEST(reflow_round_trip_preserves_text_and_attributes) {
    Terminal t(3, 8);
    t.feed(CSI "31mredred" CSI "0m-plain-text");
    t.resize(3, 3);
    t.resize(3, 8);
    WT_CHECK(allLines(t) == (Lines{"redred-p", "lain-tex", "t"}));
    WT_CHECK(t.line(0).cells[0].attributes.foreground == Color::indexed(1));
    WT_CHECK(t.line(0).cells[6].attributes.foreground.isDefault());
}

WT_TEST(reflow_moves_overflow_into_scrollback_and_back) {
    Terminal t(3, 6);
    t.feed("aaaaaa" "bbbbbb\r\n" "c");
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(0));
    t.resize(3, 3); // aaa aaa bbb bbb c -> 5 lines, 2 go to the history
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(2));
    WT_CHECK(allLines(t) == (Lines{"aaa", "aaa", "bbb", "bbb", "c"}));
    WT_CHECK_EQ(t.cursor().row, 2);
    t.resize(3, 12);
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(0));
    WT_CHECK(allLines(t) == (Lines{"aaaaaabbbbbb", "c", ""}));
}

WT_TEST(reflow_keeps_wide_characters_whole) {
    Terminal t(2, 6);
    t.feed("ab中文");
    t.resize(2, 3);
    WT_CHECK(allLines(t) == (Lines{"ab", "中", "文"}));
    for (std::size_t i = 0; i < t.scrollbackSize(); ++i) {
        const auto& cells = t.scrollbackLine(i).cells;
        WT_CHECK_EQ(cells.size(), std::size_t(3));
    }
    WT_CHECK_EQ(t.line(1).cells[0].width, std::uint8_t(2));
    WT_CHECK_EQ(t.line(1).cells[1].width, std::uint8_t(0));
    t.resize(2, 1); // too narrow for wide characters: they degrade to one column
    for (int row = 0; row < t.rows(); ++row) {
        WT_CHECK_EQ(t.line(row).cells.size(), std::size_t(1));
    }
}

WT_TEST(reflow_keeps_cursor_on_blank_prompt_line) {
    Terminal t(5, 10);
    t.feed("$ ls\r\nfile\r\n$ ");
    t.resize(5, 4);
    WT_CHECK(allLines(t) == (Lines{"$ ls", "file", "$", "", ""}));
    WT_CHECK_EQ(t.cursor().row, 2);
    WT_CHECK_EQ(t.cursor().column, 2);
}

WT_TEST(reflow_primary_while_alternate_screen_is_active) {
    Terminal t(3, 8);
    t.feed("12345678ab\r\nprompt");
    t.feed(CSI "?1049h" CSI "H" "full-screen");
    t.resize(3, 4);
    WT_CHECK(t.isAlternateScreen());
    WT_CHECK_EQ(t.lineText(0), std::string("full")); // alternate screen is truncated, not reflowed
    t.feed(CSI "?1049l");
    WT_CHECK(allLines(t) == (Lines{"1234", "5678", "ab", "prom", "pt"}));
    WT_CHECK_EQ(t.cursor().row, 2); // restored onto the reflowed prompt line
    WT_CHECK_EQ(t.cursor().column, 2);
}

WT_TEST(reflow_respects_scrollback_limit) {
    Terminal t(2, 10, 3);
    t.feed("0123456789" "0123456789" "0123456789" "x");
    t.resize(2, 2);
    WT_CHECK_EQ(t.scrollbackSize(), std::size_t(3));
    WT_CHECK_EQ(t.lineText(t.rows() - 1), std::string("x"));
}

WT_TEST(reflow_and_selection_survive_random_input) {
    static const char* const kFragments[] = {
        "\x1b[", "\x1b[?1049h", "\x1b[?1049l", "\x1b[2;4r", "\x1b[r", "\r\n", "\t", "中文", "e\xCC\x81",
        "\xCC\x81", "\x1b]4;1;?\a", "\x1b]11;#123\a", "\x1b[3J", "\x1b[2J", "\x1b[5S", "\x1b[3T",
        "\x1b[?2026h", "\x1b[22t", "\x1b[23t", "abcdefghijklmnopqrstuvwxyz", "\x1b[4@", "\x1b[2P",
        "\x1bP$qm\x1b\\", "\x1bP+q544e\x1b\\", "\x1b]8;;http://x\a", "\x1b]8;;\a", "\x1b[>1u", "\x1b[<u",
        "\x1b#6", "\x1b[4:3;58;5;1m", "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad", "\x1bP",
    };
    std::uint32_t seed = 987654;
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    Terminal t(8, 16, 40);
    for (int iteration = 0; iteration < 6000; ++iteration) {
        std::string chunk;
        const int pieces = static_cast<int>(next() % 6) + 1;
        for (int i = 0; i < pieces; ++i) {
            if (next() % 2 == 0) {
                chunk += kFragments[next() % (sizeof(kFragments) / sizeof(kFragments[0]))];
            } else {
                chunk.push_back(static_cast<char>(next() & 0xFF));
            }
        }
        t.feed(chunk);
        switch (next() % 16) {
        case 0: t.resize(static_cast<int>(next() % 20) + 1, static_cast<int>(next() % 40) + 1); break;
        case 1:
            t.startSelection({t.historyStart() + static_cast<std::int64_t>(next() % 60), static_cast<int>(next() % 50)},
                             static_cast<SelectionMode>(next() % 4));
            break;
        case 2: t.extendSelection({t.screenStart() + static_cast<int>(next() % 10), static_cast<int>(next() % 50)}); break;
        case 3: (void)t.selectedText(); break;
        default: break;
        }
        WT_REQUIRE(t.cursor().row >= 0 && t.cursor().row < t.rows());
        WT_REQUIRE(t.cursor().column >= 0 && t.cursor().column < t.columns());
    }
    auto checkLine = [&](const Line& line) {
        WT_REQUIRE(static_cast<int>(line.cells.size()) == t.columns());
        for (std::size_t c = 0; c < line.cells.size(); ++c) {
            if (line.cells[c].width == 2) {
                WT_REQUIRE(c + 1 < line.cells.size() && line.cells[c + 1].width == 0);
            }
        }
    };
    for (int row = 0; row < t.rows(); ++row) {
        checkLine(t.line(row));
    }
    t.resize(t.rows(), t.columns() + 3); // forces a reflow so every history line is re-normalised
    for (std::size_t i = 0; i < t.scrollbackSize(); ++i) {
        checkLine(t.scrollbackLine(i));
    }
    WT_CHECK(t.scrollbackSize() <= 40);
}

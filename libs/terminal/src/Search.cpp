#include "wt/terminal/Search.h"

#include "wt/core/Utf8.h"
#include "wt/terminal/Terminal.h"

#include <algorithm>
#include <memory>
#include <regex>

namespace wt::terminal {
namespace {

// Simple lower-casing that never changes the UTF-8 length of a code point, so byte offsets in the
// folded text equal those in the original.
char32_t foldCase(char32_t cp) {
    if (cp >= U'A' && cp <= U'Z') {
        return cp + 32;
    }
    if (cp < 0xC0) {
        return cp;
    }
    if (cp <= 0xDE && cp != 0xD7) { // Latin-1 letters
        return cp + 32;
    }
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) { // Greek
        return cp + 32;
    }
    if (cp >= 0x410 && cp <= 0x42F) { // Cyrillic
        return cp + 32;
    }
    if (cp >= 0x400 && cp <= 0x40F) {
        return cp + 80;
    }
    return cp;
}

std::string foldUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    core::Utf8Decoder decoder;
    decoder.decode(text, [&out](char32_t cp) { core::appendUtf8(out, foldCase(cp)); });
    return out;
}

bool isWordByte(unsigned char c) {
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// One logical line flattened to UTF-8 with the cell each code point came from.
struct FlatLine {
    std::string text;
    std::vector<std::size_t> offsets; // byte offset where each cell's text starts
    std::vector<GridPoint> cells;

    GridPoint cellAt(std::size_t byte) const {
        const auto it = std::upper_bound(offsets.begin(), offsets.end(), byte);
        return cells[static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, it - offsets.begin() - 1))];
    }
};

} // namespace

SearchMatcher literalMatcher(std::string needle, bool caseSensitive) {
    if (!caseSensitive) {
        needle = foldUtf8(needle);
    }
    return [needle = std::move(needle), caseSensitive](std::string_view text,
                                                        const std::function<bool(std::size_t, std::size_t)>& emit) {
        if (needle.empty()) {
            return;
        }
        std::string folded;
        if (!caseSensitive) {
            folded = foldUtf8(text);
            text = folded;
        }
        for (std::size_t pos = text.find(needle); pos != std::string_view::npos;
             pos = text.find(needle, pos + needle.size())) {
            if (!emit(pos, pos + needle.size())) {
                return;
            }
        }
    };
}

std::optional<SearchMatcher> regexMatcher(std::string_view pattern, bool caseSensitive, std::string* error,
                                          std::size_t maxLineBytes) {
    if (pattern.empty()) {
        if (error) {
            *error = "empty pattern";
        }
        return std::nullopt;
    }
    auto flags = std::regex::ECMAScript | std::regex::optimize;
    if (!caseSensitive) {
        flags |= std::regex::icase;
    }
    std::shared_ptr<std::regex> regex;
    try {
        regex = std::make_shared<std::regex>(std::string(pattern), flags);
    } catch (const std::regex_error& e) {
        if (error) {
            *error = e.what();
        }
        return std::nullopt;
    }
    return SearchMatcher([regex, maxLineBytes](std::string_view text,
                                               const std::function<bool(std::size_t, std::size_t)>& emit) {
        if (text.size() > maxLineBytes) {
            return;
        }
        try {
            for (auto it = std::cregex_iterator(text.data(), text.data() + text.size(), *regex);
                 it != std::cregex_iterator(); ++it) {
                const auto begin = static_cast<std::size_t>(it->position(0));
                const auto length = static_cast<std::size_t>(it->length(0));
                if (length == 0) {
                    continue; // empty matches cannot be highlighted
                }
                if (!emit(begin, begin + length)) {
                    return;
                }
            }
        } catch (const std::regex_error&) {
            // complexity limits: treat as no match
        }
    });
}

std::vector<SearchMatch> searchTerminal(const Terminal& terminal, const SearchMatcher& matcher,
                                        const SearchOptions& options) {
    std::vector<SearchMatch> matches;
    if (!matcher) {
        return matches;
    }
    const std::int64_t first = terminal.isAlternateScreen() ? terminal.screenStart() : terminal.historyStart();
    const std::int64_t last = terminal.screenStart() + terminal.rows() - 1;

    FlatLine flat;
    auto flush = [&]() {
        if (flat.text.empty()) {
            return true;
        }
        bool more = true;
        matcher(flat.text, [&](std::size_t begin, std::size_t end) {
            if (options.wholeWord) {
                const auto* bytes = reinterpret_cast<const unsigned char*>(flat.text.data());
                if ((begin > 0 && isWordByte(bytes[begin - 1]) && isWordByte(bytes[begin])) ||
                    (end < flat.text.size() && isWordByte(bytes[end]) && isWordByte(bytes[end - 1]))) {
                    return true;
                }
            }
            SearchMatch match{flat.cellAt(begin), flat.cellAt(end - 1)};
            // Include both halves of a wide character at the end.
            if (const Line* line = terminal.absoluteLine(match.end.row);
                line && match.end.column + 1 < static_cast<int>(line->cells.size()) &&
                line->cells[static_cast<std::size_t>(match.end.column)].width == 2) {
                ++match.end.column;
            }
            matches.push_back(match);
            more = matches.size() < options.maxMatches;
            return more;
        });
        flat = {};
        return more;
    };

    for (std::int64_t row = first; row <= last; ++row) {
        const Line* line = terminal.absoluteLine(row);
        if (!line) {
            continue;
        }
        for (std::size_t col = 0; col < line->cells.size(); ++col) {
            const Cell& cell = line->cells[col];
            if (cell.width == 0 || cell.attributes.has(WidePadding)) {
                continue;
            }
            flat.offsets.push_back(flat.text.size());
            flat.cells.push_back({row, static_cast<int>(col)});
            for (char32_t cp : terminal.cellText(cell)) {
                core::appendUtf8(flat.text, cp == 0 ? U' ' : cp);
            }
        }
        if (!line->wrapped || row == last) {
            // Trailing blanks of a logical line are not searchable text.
            const std::size_t keep = flat.text.find_last_not_of(' ') + 1;
            flat.text.resize(keep == std::string::npos ? 0 : keep);
            if (!flush()) {
                break;
            }
        }
    }
    return matches;
}

std::optional<std::size_t> nextMatch(const std::vector<SearchMatch>& matches, GridPoint from, bool backward) {
    if (matches.empty()) {
        return std::nullopt;
    }
    if (!backward) {
        for (std::size_t i = 0; i < matches.size(); ++i) {
            if (from < matches[i].start) {
                return i;
            }
        }
        return 0;
    }
    for (std::size_t i = matches.size(); i-- > 0;) {
        if (matches[i].start < from) {
            return i;
        }
    }
    return matches.size() - 1;
}

} // namespace wt::terminal

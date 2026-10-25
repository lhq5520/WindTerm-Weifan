#pragma once

#include "wt/terminal/Selection.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wt::terminal {

class Terminal;

// Inclusive cell range of a match in absolute coordinates (see Terminal::historyStart()).
struct SearchMatch {
    GridPoint start;
    GridPoint end;
    friend bool operator==(const SearchMatch&, const SearchMatch&) = default;
};

struct SearchOptions {
    bool wholeWord = false;
    std::size_t maxMatches = 10000;
};

// A search engine: reports [begin, end) byte ranges of non-overlapping matches in a UTF-8 text,
// in ascending order. The emit callback returns false to stop early.
using SearchMatcher =
    std::function<void(std::string_view text, const std::function<bool(std::size_t, std::size_t)>& emit)>;

// Plain-text matcher. Case-insensitive matching folds ASCII, Latin-1, Greek and Cyrillic letters.
SearchMatcher literalMatcher(std::string needle, bool caseSensitive);

// std::regex (ECMAScript) matcher, for headless use and tests; front ends may plug in a faster
// engine. Logical lines longer than `maxLineBytes` are skipped to bound the recursion depth of
// std::regex implementations. Returns nullopt (and sets `error`) for an invalid pattern.
std::optional<SearchMatcher> regexMatcher(std::string_view pattern, bool caseSensitive, std::string* error = nullptr,
                                          std::size_t maxLineBytes = 16 * 1024);

// Searches the scrollback and the visible screen (only the screen while the alternate screen is
// active). Wrapped lines are joined, so matches may span line breaks. Results are in order.
std::vector<SearchMatch> searchTerminal(const Terminal& terminal, const SearchMatcher& matcher,
                                        const SearchOptions& options = {});

// Picks the match to jump to: the first one after (or, backwards, before) `from`, wrapping around.
std::optional<std::size_t> nextMatch(const std::vector<SearchMatch>& matches, GridPoint from, bool backward);

} // namespace wt::terminal

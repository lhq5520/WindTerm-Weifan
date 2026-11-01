#pragma once

#include "wt/core/Json.h"
#include "wt/core/Status.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wt::session {

// Commands the user ran, for completion. Each entry remembers where it was used (a context such
// as a session id, so suggestions prefer commands from the same host). Newest first.
class CommandHistory {
public:
    static constexpr int kVersion = 1;
    static constexpr std::size_t kDefaultLimit = 2000;

    // Records a command (moving an existing one to the front). Ignores empty, multi-line and
    // very long commands and those starting with a space (the shells' "do not remember" convention).
    bool add(std::string_view context, std::string_view command, std::int64_t timestamp = 0);
    // Commands starting with `prefix` (longer than it), same-context ones first, newest first.
    std::vector<std::string> complete(std::string_view context, std::string_view prefix, std::size_t limit = 8) const;
    bool remove(std::string_view command);
    void clear() { m_entries.clear(); }
    std::size_t size() const { return m_entries.size(); }
    void setLimit(std::size_t limit);

    core::Json toJson() const;
    static CommandHistory fromJson(const core::Json& json);
    core::Status load(std::string_view path); // a missing file yields an empty history
    core::Status save(std::string_view path) const;

private:
    struct Entry {
        std::string context;
        std::string command;
        std::int64_t lastUsed = 0;
        int uses = 0;
    };
    std::vector<Entry> m_entries;
    std::size_t m_limit = kDefaultLimit;
};

// Follows what the user types on the current command line, from the bytes sent to the session.
// Cursor keys, history recall and other escape sequences make the line unknowable from the input
// alone; reliable() then turns false until the next Enter.
class InputLineTracker {
public:
    struct Submitted {
        std::string text;
        bool reliable = true;
    };
    // Feeds user input; returns the lines submitted with Enter (CR) in it.
    std::vector<Submitted> feed(std::string_view input);
    const std::string& line() const { return m_line; }
    bool reliable() const { return m_reliable; }
    bool empty() const { return m_line.empty(); }
    void reset();

private:
    void eraseLastCharacter();
    void eraseLastWord();

    std::string m_line;
    bool m_reliable = true;
    int m_escape = 0; // inside an escape sequence: 1 after ESC, 2 inside CSI/SS3
};

} // namespace wt::session

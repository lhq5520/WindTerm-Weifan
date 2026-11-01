#include "wt/session/CommandHistory.h"

#include "wt/platform/Files.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace wt::session {
namespace {

constexpr std::size_t kMaxCommandLength = 4096;

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

bool CommandHistory::add(std::string_view context, std::string_view command, std::int64_t timestamp) {
    // Trailing blanks are noise; a leading blank means "keep this out of the history".
    while (!command.empty() && (command.back() == ' ' || command.back() == '\t')) {
        command.remove_suffix(1);
    }
    if (command.empty() || command.front() == ' ' || command.size() > kMaxCommandLength ||
        command.find_first_of("\r\n") != std::string_view::npos) {
        return false;
    }
    const std::int64_t when = timestamp ? timestamp : nowSeconds();
    Entry entry{std::string(context), std::string(command), when, 1};
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) {
        return e.command == command && e.context == context;
    });
    if (it != m_entries.end()) {
        entry.uses = it->uses + 1;
        m_entries.erase(it);
    }
    m_entries.insert(m_entries.begin(), std::move(entry));
    if (m_entries.size() > m_limit) {
        m_entries.resize(m_limit);
    }
    return true;
}

std::vector<std::string> CommandHistory::complete(std::string_view context, std::string_view prefix,
                                                  std::size_t limit) const {
    std::vector<std::string> result;
    auto collect = [&](bool sameContext) {
        for (const Entry& entry : m_entries) {
            if (result.size() >= limit) {
                return;
            }
            if ((entry.context == context) != sameContext || entry.command.size() <= prefix.size() ||
                entry.command.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            if (std::find(result.begin(), result.end(), entry.command) == result.end()) {
                result.push_back(entry.command);
            }
        }
    };
    collect(true);
    collect(false);
    return result;
}

bool CommandHistory::remove(std::string_view command) {
    const auto before = m_entries.size();
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&](const Entry& e) { return e.command == command; }),
                    m_entries.end());
    return m_entries.size() != before;
}

void CommandHistory::setLimit(std::size_t limit) {
    m_limit = std::max<std::size_t>(1, limit);
    if (m_entries.size() > m_limit) {
        m_entries.resize(m_limit);
    }
}

core::Json CommandHistory::toJson() const {
    core::Json json;
    json["version"] = kVersion;
    core::Json list = core::Json::array();
    for (const Entry& entry : m_entries) {
        core::Json item;
        item["context"] = entry.context;
        item["command"] = entry.command;
        item["lastUsed"] = entry.lastUsed;
        item["uses"] = entry.uses;
        list.push(std::move(item));
    }
    json["commands"] = std::move(list);
    return json;
}

CommandHistory CommandHistory::fromJson(const core::Json& json) {
    CommandHistory history;
    for (const core::Json& item : json["commands"].items()) {
        Entry entry{item["context"].toString(), item["command"].toString(), item["lastUsed"].toInt(0),
                    static_cast<int>(std::clamp<std::int64_t>(item["uses"].toInt(1), 1, 1000000))};
        if (!entry.command.empty() && history.m_entries.size() < history.m_limit) {
            history.m_entries.push_back(std::move(entry));
        }
    }
    // Keep the newest-first invariant even for hand-edited files.
    std::stable_sort(history.m_entries.begin(), history.m_entries.end(),
                     [](const Entry& a, const Entry& b) { return a.lastUsed > b.lastUsed; });
    return history;
}

core::Status CommandHistory::load(std::string_view path) {
    std::error_code ec;
    if (!std::filesystem::exists(platform::pathFromUtf8(path), ec)) {
        m_entries.clear();
        return core::Status::ok();
    }
    std::string error;
    const auto text = platform::readFile(path, &error);
    if (!text) {
        return core::Status::error(error);
    }
    const auto json = core::Json::parse(*text, &error);
    if (!json) {
        return core::Status::error(std::string(path) + ": " + error);
    }
    const std::size_t limit = m_limit;
    *this = fromJson(*json);
    setLimit(limit);
    return core::Status::ok();
}

core::Status CommandHistory::save(std::string_view path) const {
    return platform::writeFileAtomic(path, toJson().dump(1) + "\n", true);
}

// ---------------------------------------------------------------------------------------------
// InputLineTracker

void InputLineTracker::reset() {
    m_line.clear();
    m_reliable = true;
    m_escape = 0;
}

void InputLineTracker::eraseLastCharacter() {
    if (m_line.empty()) {
        return;
    }
    // Back over UTF-8 continuation bytes to the start of the last character.
    std::size_t end = m_line.size() - 1;
    while (end > 0 && (static_cast<unsigned char>(m_line[end]) & 0xC0) == 0x80) {
        --end;
    }
    m_line.erase(end);
}

void InputLineTracker::eraseLastWord() {
    while (!m_line.empty() && m_line.back() == ' ') {
        m_line.pop_back();
    }
    while (!m_line.empty() && m_line.back() != ' ') {
        m_line.pop_back();
    }
}

std::vector<InputLineTracker::Submitted> InputLineTracker::feed(std::string_view input) {
    std::vector<Submitted> submitted;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (m_escape == 1) {
            // ESC [ / ESC O start a sequence; anything else is Alt+key (also unknowable).
            m_escape = (c == '[' || c == 'O') ? 2 : 0;
            m_reliable = false;
            continue;
        }
        if (m_escape == 2) {
            if (c >= 0x40 && c <= 0x7E) {
                m_escape = 0;
                // Bracketed paste markers (ESC [ 200~ / 201~) do not move the cursor.
            }
            continue;
        }
        switch (c) {
        case 0x1B:
            // A paste is bracketed by ESC [200~ ... ESC [201~; its content is plain text.
            if (input.substr(i, 6) == "\x1b[200~" || input.substr(i, 6) == "\x1b[201~") {
                i += 5;
            } else {
                m_escape = 1;
                m_reliable = false; // Escape / Alt+key / cursor keys: the shell may edit the line
            }
            break;
        case '\r':
        case '\n':
            submitted.push_back({m_line, m_reliable});
            reset();
            break;
        case 0x7F:
        case 0x08: eraseLastCharacter(); break;
        case 0x15: m_line.clear(); break; // Ctrl+U
        case 0x17: eraseLastWord(); break; // Ctrl+W
        case 0x03:                         // Ctrl+C abandons the line
        case 0x04: reset(); break;         // Ctrl+D
        case '\t':
            m_reliable = false; // the shell completes something we cannot see
            break;
        default:
            if (c < 0x20) {
                m_reliable = false; // Ctrl+A/E/R/... edit or recall in ways we cannot follow
            } else {
                m_line += static_cast<char>(c);
            }
            break;
        }
    }
    if (m_escape == 1) {
        // A lone ESC at the end of a key press is the Escape key, not the start of a sequence:
        // it must not swallow the next key (Enter).
        m_escape = 0;
    }
    return submitted;
}

} // namespace wt::session

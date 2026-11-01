#pragma once

#include "wt/core/Status.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wt::session {

// Turns terminal output into plain text lines: escape sequences are removed and carriage returns,
// backspaces, tabs and the in-line editing sequences shells use when redrawing a command line
// (erase in line, cursor left/right, insert/delete characters) are applied to the current line.
class PlainTextAssembler {
public:
    // Feeds output bytes; completed lines (without the newline) are appended to `lines`.
    void feed(std::string_view bytes, std::vector<std::string>& lines);
    // The line being written (no newline yet), e.g. a prompt.
    std::string currentLine() const;
    bool hasCurrentLine() const { return !m_cells.empty(); }
    // Ends the current line (as if a newline arrived) and returns it.
    std::string takeCurrentLine();
    // Counts completed lines (lets callers notice that a new line started).
    std::uint64_t lineNumber() const { return m_lineNumber; }

private:
    enum class State : std::uint8_t { Ground, Escape, EscapeIntermediate, Csi, String, StringEscape };
    void put(std::string character, int width);
    void csi(char final);
    int param(std::size_t index, int fallback) const;
    std::string lineText() const;

    State m_state = State::Ground;
    std::string m_utf8;      // pending bytes of a multi-byte character
    std::size_t m_utf8Need = 0;
    std::string m_params;    // CSI parameter bytes
    bool m_stringBel = true; // OSC strings may end with BEL
    std::vector<std::string> m_cells; // one entry per column; "" for the right half of wide characters
    std::size_t m_cursor = 0;
    std::uint64_t m_lineNumber = 0;
};

enum class LogFormat { Text, Raw };

// Writes a session's output to a file: plain text (optionally with a timestamp per line) or the
// raw byte stream (replayable with `cat`). Files are created private (0600 on POSIX) and appended
// to. Not thread-safe.
class SessionLogger {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    SessionLogger();
    ~SessionLogger();
    SessionLogger(const SessionLogger&) = delete;
    SessionLogger& operator=(const SessionLogger&) = delete;

    core::Status open(const std::string& path, LogFormat format, bool timestamps);
    void close();
    bool isOpen() const;
    const std::string& path() const { return m_path; }
    LogFormat format() const { return m_format; }
    std::uint64_t bytesWritten() const { return m_written; }

    // Output from the session.
    void write(std::string_view bytes);
    // A line of our own (connection notices), written in text format as "[...]" lines.
    void note(std::string_view text);
    void setClock(Clock clock) { m_clock = std::move(clock); }

private:
    void writeLine(const std::string& line);
    void writeBytes(std::string_view bytes);
    std::string timestamp() const;

    struct File;
    std::unique_ptr<File> m_file;
    std::string m_path;
    LogFormat m_format = LogFormat::Text;
    bool m_timestamps = false;
    PlainTextAssembler m_text;
    Clock m_clock;
    std::uint64_t m_written = 0;
};

// Expands a log file name pattern: %Y %m %d %H %M %S (date/time), %N (session name), %h (host),
// %% (a percent sign). Characters that cannot appear in file names are replaced by '_'.
std::string expandLogFileName(std::string_view pattern, const std::string& sessionName, const std::string& host,
                              const std::tm& time);

} // namespace wt::session

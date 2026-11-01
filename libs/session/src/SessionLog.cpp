#include "wt/session/SessionLog.h"

#include "wt/core/CharWidth.h"
#include "wt/core/Utf8.h"
#include "wt/platform/Files.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace wt::session {

// ---------------------------------------------------------------------------------------------
// PlainTextAssembler

int PlainTextAssembler::param(std::size_t index, int fallback) const {
    std::size_t current = 0;
    int value = -1;
    for (const char c : m_params) {
        if (c == ';') {
            if (current == index) {
                return value < 0 ? fallback : value;
            }
            ++current;
            value = -1;
        } else if (c >= '0' && c <= '9') {
            value = std::min(100000, (value < 0 ? 0 : value) * 10 + (c - '0'));
        }
    }
    return current == index && value >= 0 ? value : fallback;
}

void PlainTextAssembler::put(std::string character, int width) {
    if (width <= 0) {
        // Combining marks join the previous character.
        if (m_cursor > 0 && m_cursor <= m_cells.size()) {
            std::size_t i = m_cursor - 1;
            while (i > 0 && m_cells[i].empty()) {
                --i;
            }
            m_cells[i] += character;
        }
        return;
    }
    while (m_cells.size() < m_cursor) {
        m_cells.emplace_back(" ");
    }
    if (m_cursor < m_cells.size()) {
        m_cells[m_cursor] = std::move(character);
    } else {
        m_cells.push_back(std::move(character));
    }
    ++m_cursor;
    if (width == 2) {
        if (m_cursor < m_cells.size()) {
            m_cells[m_cursor].clear();
        } else {
            m_cells.emplace_back();
        }
        ++m_cursor;
    }
}

void PlainTextAssembler::csi(char final) {
    if (!m_params.empty() && (m_params[0] == '?' || m_params[0] == '>' || m_params[0] == '<' || m_params[0] == '=')) {
        return; // private sequences do not change the text
    }
    const auto count = static_cast<std::size_t>(std::max(1, param(0, 1)));
    switch (final) {
    case 'K':
        switch (param(0, 0)) {
        case 0:
            if (m_cursor < m_cells.size()) {
                m_cells.resize(m_cursor);
            }
            break;
        case 1:
            for (std::size_t i = 0; i <= m_cursor && i < m_cells.size(); ++i) {
                m_cells[i] = " ";
            }
            break;
        default: m_cells.clear(); break;
        }
        break;
    case 'C': m_cursor += count; break;
    case 'D': m_cursor = count > m_cursor ? 0 : m_cursor - count; break;
    case 'G': m_cursor = count - 1; break;
    case 'P':
        if (m_cursor < m_cells.size()) {
            m_cells.erase(m_cells.begin() + static_cast<std::ptrdiff_t>(m_cursor),
                          m_cells.begin() + static_cast<std::ptrdiff_t>(std::min(m_cells.size(), m_cursor + count)));
        }
        break;
    case '@':
        if (m_cursor < m_cells.size()) {
            m_cells.insert(m_cells.begin() + static_cast<std::ptrdiff_t>(m_cursor), count, std::string(" "));
        }
        break;
    case 'X':
        for (std::size_t i = m_cursor; i < m_cursor + count && i < m_cells.size(); ++i) {
            m_cells[i] = " ";
        }
        break;
    default: break; // colours, cursor rows, screen clears: nothing for a text log
    }
}

std::string PlainTextAssembler::lineText() const {
    std::string text;
    for (const std::string& cell : m_cells) {
        text += cell;
    }
    text.erase(text.find_last_not_of(' ') + 1);
    return text;
}

std::string PlainTextAssembler::currentLine() const {
    return lineText();
}

std::string PlainTextAssembler::takeCurrentLine() {
    std::string text = lineText();
    m_cells.clear();
    m_cursor = 0;
    ++m_lineNumber;
    return text;
}

void PlainTextAssembler::feed(std::string_view bytes, std::vector<std::string>& lines) {
    constexpr std::size_t kMaxLine = 64 * 1024;
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        switch (m_state) {
        case State::Ground:
            if (m_utf8Need > 0) {
                if ((c & 0xC0) == 0x80) {
                    m_utf8 += ch;
                    if (--m_utf8Need == 0) {
                        const std::u32string decoded = core::fromUtf8(m_utf8);
                        const int width = decoded.empty() ? 1 : core::charWidth(decoded.front());
                        put(std::move(m_utf8), width);
                        m_utf8.clear();
                    }
                    break;
                }
                m_utf8.clear(); // broken sequence: drop it and look at this byte afresh
                m_utf8Need = 0;
            }
            if (c == 0x1B) {
                m_state = State::Escape;
            } else if (c == '\n') {
                lines.push_back(takeCurrentLine());
            } else if (c == '\r') {
                m_cursor = 0;
            } else if (c == '\b') {
                if (m_cursor > 0) {
                    --m_cursor;
                }
                while (m_cursor > 0 && m_cursor < m_cells.size() && m_cells[m_cursor].empty()) {
                    --m_cursor; // over the right half of a wide character
                }
            } else if (c == '\t') {
                const std::size_t next = (m_cursor / 8 + 1) * 8;
                while (m_cursor < next) {
                    put(" ", 1);
                }
            } else if (c < 0x20 || c == 0x7F) {
                // other control characters leave no text
            } else if (c < 0x80) {
                put(std::string(1, ch), 1);
            } else if (c >= 0xC2 && c <= 0xF4) {
                m_utf8.assign(1, ch);
                m_utf8Need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            }
            break;
        case State::Escape:
            if (c == '[') {
                m_state = State::Csi;
                m_params.clear();
            } else if (c == ']') {
                m_state = State::String;
                m_stringBel = true;
            } else if (c == 'P' || c == 'X' || c == '^' || c == '_') {
                m_state = State::String;
                m_stringBel = false;
            } else if (c >= 0x20 && c <= 0x2F) {
                m_state = State::EscapeIntermediate;
            } else {
                m_state = State::Ground; // two-byte sequence such as ESC 7 or ESC =
            }
            break;
        case State::EscapeIntermediate:
            if (c < 0x20 || c >= 0x30) {
                m_state = State::Ground; // final byte (ESC ( B) or garbage
            }
            break;
        case State::Csi:
            if (c >= 0x40 && c <= 0x7E) {
                csi(ch);
                m_state = State::Ground;
            } else if (c >= 0x20 && c <= 0x3F) {
                if (m_params.size() < 64) {
                    m_params += ch;
                }
            } else if (c == 0x1B) {
                m_state = State::Escape;
            } else if (c >= 0x20) {
                m_state = State::Ground;
            }
            break;
        case State::String:
            if (c == 0x07 && m_stringBel) {
                m_state = State::Ground;
            } else if (c == 0x1B) {
                m_state = State::StringEscape;
            }
            break;
        case State::StringEscape: m_state = c == '\\' ? State::Ground : State::String; break;
        }
        if (m_cells.size() > kMaxLine) {
            lines.push_back(takeCurrentLine());
        }
    }
}

// ---------------------------------------------------------------------------------------------
// SessionLogger

struct SessionLogger::File {
    std::FILE* handle = nullptr;
    ~File() {
        if (handle) {
            std::fclose(handle);
        }
    }
};

SessionLogger::SessionLogger() = default;

SessionLogger::~SessionLogger() {
    close();
}

bool SessionLogger::isOpen() const {
    return m_file && m_file->handle;
}

core::Status SessionLogger::open(const std::string& path, LogFormat format, bool timestamps) {
    close();
    const std::filesystem::path fsPath = platform::pathFromUtf8(path);
    std::error_code ec;
    if (fsPath.has_parent_path()) {
        std::filesystem::create_directories(fsPath.parent_path(), ec);
    }
#ifdef _WIN32
    std::FILE* handle = _wfopen(fsPath.c_str(), L"ab");
#else
    // Logs can contain anything that crossed the terminal: keep them private.
    std::FILE* handle = nullptr;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        handle = ::fdopen(fd, "ab");
        if (!handle) {
            ::close(fd);
        }
    }
#endif
    if (!handle) {
        return core::Status::error("cannot open " + path + ": " + std::strerror(errno), errno);
    }
    m_file = std::make_unique<File>();
    m_file->handle = handle;
    m_path = path;
    m_format = format;
    m_timestamps = timestamps;
    m_text = {};
    m_written = 0;
    return core::Status::ok();
}

void SessionLogger::close() {
    if (!isOpen()) {
        m_file.reset();
        return;
    }
    if (m_format == LogFormat::Text && m_text.hasCurrentLine()) {
        writeLine(m_text.takeCurrentLine());
    }
    std::fflush(m_file->handle);
    m_file.reset();
}

std::string SessionLogger::timestamp() const {
    const auto now = m_clock ? m_clock() : std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char text[96]; // ints of any size fit
    std::snprintf(text, sizeof(text), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, static_cast<int>(millis));
    return text;
}

void SessionLogger::writeBytes(std::string_view bytes) {
    if (!isOpen() || bytes.empty()) {
        return;
    }
    m_written += std::fwrite(bytes.data(), 1, bytes.size(), m_file->handle);
}

void SessionLogger::writeLine(const std::string& line) {
    std::string out = m_timestamps ? timestamp() : std::string();
    out += line;
    out += '\n';
    writeBytes(out);
}

void SessionLogger::write(std::string_view bytes) {
    if (!isOpen()) {
        return;
    }
    if (m_format == LogFormat::Raw) {
        writeBytes(bytes);
    } else {
        std::vector<std::string> lines;
        m_text.feed(bytes, lines);
        for (const std::string& line : lines) {
            writeLine(line);
        }
    }
    std::fflush(m_file->handle);
}

void SessionLogger::note(std::string_view text) {
    if (!isOpen()) {
        return;
    }
    if (m_format == LogFormat::Raw) {
        writeBytes("\r\n" + std::string(text) + "\r\n");
    } else {
        if (m_text.hasCurrentLine()) {
            writeLine(m_text.takeCurrentLine());
        }
        writeLine(std::string(text));
    }
    std::fflush(m_file->handle);
}

std::string expandLogFileName(std::string_view pattern, const std::string& sessionName, const std::string& host,
                              const std::tm& time) {
    auto sanitized = [](const std::string& text) {
        std::string out;
        for (const char c : text) {
            const auto byte = static_cast<unsigned char>(c);
            out += (byte < 0x20 || std::strchr("/\\:*?\"<>|", c)) ? '_' : c;
        }
        return out.empty() ? std::string("session") : out;
    };
    auto two = [](int value) {
        char text[8];
        std::snprintf(text, sizeof(text), "%02d", value);
        return std::string(text);
    };
    std::string out;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%' || i + 1 >= pattern.size()) {
            out += pattern[i];
            continue;
        }
        const char code = pattern[++i];
        switch (code) {
        case 'Y': out += std::to_string(time.tm_year + 1900); break;
        case 'm': out += two(time.tm_mon + 1); break;
        case 'd': out += two(time.tm_mday); break;
        case 'H': out += two(time.tm_hour); break;
        case 'M': out += two(time.tm_min); break;
        case 'S': out += two(time.tm_sec); break;
        case 'N': out += sanitized(sessionName); break;
        case 'h': out += sanitized(host); break;
        case '%': out += '%'; break;
        default:
            out += '%';
            out += code;
            break;
        }
    }
    return out;
}

} // namespace wt::session

#pragma once

#include <string>
#include <utility>

namespace wt::core {

// Lightweight success/error result used across the Qt-free layers.
class Status {
public:
    Status() = default;

    static Status ok() { return {}; }
    static Status error(std::string message, int systemError = 0) {
        Status s;
        s.m_ok = false;
        s.m_message = std::move(message);
        s.m_systemError = systemError;
        return s;
    }

    bool isOk() const { return m_ok; }
    explicit operator bool() const { return m_ok; }

    const std::string& message() const { return m_message; }
    // errno (POSIX) / GetLastError (Windows) captured at the failure site, 0 if not applicable.
    int systemError() const { return m_systemError; }

private:
    bool m_ok = true;
    std::string m_message;
    int m_systemError = 0;
};

} // namespace wt::core

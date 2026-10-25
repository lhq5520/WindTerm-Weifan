#pragma once

#include <libssh2.h>

#include <mutex>

namespace wt::ssh {

// libssh2_init() must run once before any other libssh2 call (it initialises the crypto backend).
inline void ensureLibssh2() {
    static std::once_flag once;
    std::call_once(once, [] { libssh2_init(0); });
}

// Session without a socket, used for known_hosts parsing.
struct DetachedSession {
    DetachedSession() {
        ensureLibssh2();
        session = libssh2_session_init();
    }
    ~DetachedSession() {
        if (session) {
            libssh2_session_free(session);
        }
    }
    DetachedSession(const DetachedSession&) = delete;
    DetachedSession& operator=(const DetachedSession&) = delete;
    LIBSSH2_SESSION* session = nullptr;
};

} // namespace wt::ssh

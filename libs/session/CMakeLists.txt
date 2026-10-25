# wt_session: Qt-free application model (themes, key bindings, settings, session profiles,
# reconnect policy) and its JSON persistence.

add_library(wt_session STATIC
    include/wt/session/Backoff.h
    include/wt/session/Keymap.h
    include/wt/session/SessionProfile.h
    include/wt/session/Settings.h
    include/wt/session/Theme.h
    src/Backoff.cpp
    src/Keymap.cpp
    src/SessionProfile.cpp
    src/Settings.cpp
    src/Theme.cpp)
add_library(wt::session ALIAS wt_session)

target_include_directories(wt_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wt_session PUBLIC wt::core wt::terminal wt::platform)
wt_configure_target(wt_session)

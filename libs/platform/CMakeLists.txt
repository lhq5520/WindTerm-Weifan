# wt_platform: Qt-free OS abstractions (pseudo-terminals, process spawning, files).

add_library(wt_platform STATIC include/wt/platform/Files.h include/wt/platform/Pty.h src/Files.cpp)
add_library(wt::platform ALIAS wt_platform)

if(WIN32)
    target_sources(wt_platform PRIVATE src/win/ConPty.cpp)
else()
    target_sources(wt_platform PRIVATE src/posix/PosixPty.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(wt_platform PRIVATE Threads::Threads)
endif()

target_include_directories(wt_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wt_platform PUBLIC wt::core)
wt_configure_target(wt_platform)

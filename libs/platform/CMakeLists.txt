# wt_platform: Qt-free OS abstractions (pseudo-terminals, process spawning, serial ports, files).

add_library(wt_platform STATIC
    include/wt/platform/Files.h
    include/wt/platform/Keychain.h
    include/wt/platform/Pty.h
    include/wt/platform/SerialPort.h
    src/Files.cpp
    src/Keychain.cpp
    src/SerialPort.cpp)
add_library(wt::platform ALIAS wt_platform)

if(APPLE)
    target_link_libraries(wt_platform PUBLIC "-framework Security" "-framework CoreFoundation")
endif()
if(WIN32)
    target_sources(wt_platform PRIVATE src/win/ConPty.cpp src/win/WinSerialPort.cpp)
    target_link_libraries(wt_platform PUBLIC advapi32)
else()
    target_sources(wt_platform PRIVATE src/posix/PosixPty.cpp src/posix/PosixSerialPort.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(wt_platform PRIVATE src/posix/LinuxCustomBaud.cpp)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(wt_platform PRIVATE Threads::Threads)
endif()

target_include_directories(wt_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wt_platform PUBLIC wt::core)
wt_configure_target(wt_platform)

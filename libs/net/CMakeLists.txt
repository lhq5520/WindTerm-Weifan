# wt_net: Qt-free networking helpers (TCP connect with timeout/cancel, SOCKS5, HTTP CONNECT,
# the Telnet protocol).

add_library(wt_net STATIC
    include/wt/net/Proxy.h
    include/wt/net/Socket.h
    include/wt/net/Telnet.h
    src/Proxy.cpp
    src/Socket.cpp
    src/Telnet.cpp)
add_library(wt::net ALIAS wt_net)

target_include_directories(wt_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wt_net PUBLIC wt::core)
if(WIN32)
    target_link_libraries(wt_net PUBLIC ws2_32)
endif()
wt_configure_target(wt_net)

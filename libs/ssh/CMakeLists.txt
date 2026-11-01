# wt_ssh: Qt-free SSH client layer on top of libssh2 (host keys, authentication, shells, SFTP,
# jump hosts and proxies, port/agent/X11 forwarding).

add_library(wt_ssh STATIC
    include/wt/ssh/KnownHosts.h
    include/wt/ssh/Sftp.h
    include/wt/ssh/SshConnection.h
    include/wt/ssh/SshShell.h
    include/wt/ssh/SshTypes.h
    include/wt/ssh/X11.h
    src/Agent.cpp
    src/Agent.h
    src/Forwarding.cpp
    src/Forwarding.h
    src/KnownHosts.cpp
    src/Libssh2Init.h
    src/Sftp.cpp
    src/SshConnection.cpp
    src/SshShell.cpp
    src/X11.cpp)
add_library(wt::ssh ALIAS wt_ssh)

target_include_directories(wt_ssh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wt_ssh PUBLIC wt::core wt::net wt::platform PRIVATE WtLibssh2::libssh2)
find_package(Threads REQUIRED)
target_link_libraries(wt_ssh PRIVATE Threads::Threads)
wt_configure_target(wt_ssh)

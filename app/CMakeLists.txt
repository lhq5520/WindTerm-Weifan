# Qt6 GUI front end. Everything Qt-specific lives here; the libraries below it are Qt-free.

set(CMAKE_AUTOMOC ON)

# Shared by the executable and the GUI tests.
add_library(wt_app STATIC
    src/AppContext.cpp
    src/AppContext.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/PtyBackend.cpp
    src/PtyBackend.h
    src/SearchBar.cpp
    src/SearchBar.h
    src/SerialBackend.cpp
    src/SerialBackend.h
    src/SessionDialogs.cpp
    src/SessionDialogs.h
    src/SettingsDialog.cpp
    src/SettingsDialog.h
    src/SessionBackend.h
    src/TelnetBackend.cpp
    src/TelnetBackend.h
    src/TerminalSession.cpp
    src/TerminalPane.cpp
    src/TerminalPane.h
    src/TerminalSession.h
    src/TerminalTab.cpp
    src/TerminalTab.h
    src/TerminalWidget.cpp
    src/TerminalWidget.h)
if(WT_HAVE_SSH)
    target_sources(wt_app PRIVATE
        src/ssh/GuiInteraction.cpp
        src/ssh/GuiInteraction.h
        src/ssh/SftpPanel.cpp
        src/ssh/SftpPanel.h
        src/ssh/SshBackend.cpp
        src/ssh/SshBackend.h
        src/ssh/SshPrompter.cpp
        src/ssh/SshPrompter.h)
    target_link_libraries(wt_app PUBLIC wt::ssh)
    target_compile_definitions(wt_app PUBLIC WT_HAVE_SSH=1)
endif()
if(WT_HAVE_VAULT)
    target_link_libraries(wt_app PUBLIC wt::security)
    target_compile_definitions(wt_app PUBLIC WT_HAVE_VAULT=1)
endif()
target_include_directories(wt_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(wt_app PUBLIC Qt6::Widgets wt::terminal wt::platform wt::net wt::session)
target_compile_definitions(wt_app PUBLIC QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
wt_configure_target(wt_app)

set(WT_APP_RESOURCES resources/app.qrc)
if(APPLE)
    set(WT_MACOS_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/windterm-weifan.icns)
    set_source_files_properties(${WT_MACOS_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    list(APPEND WT_APP_RESOURCES ${WT_MACOS_ICON})
elseif(WIN32)
    set(WT_WINDOWS_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/windterm-weifan.ico)
    configure_file(resources/windows/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
    list(APPEND WT_APP_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
endif()

set(CMAKE_AUTORCC ON)
qt_add_executable(windterm-weifan MACOSX_BUNDLE WIN32 src/main.cpp ${WT_APP_RESOURCES})
target_link_libraries(windterm-weifan PRIVATE wt_app)
target_compile_definitions(windterm-weifan PRIVATE WT_VERSION="${PROJECT_VERSION}")
wt_configure_target(windterm-weifan)

set_target_properties(windterm-weifan PROPERTIES
    OUTPUT_NAME "WindTerm-Weifan"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "WindTerm-Weifan"
    MACOSX_BUNDLE_EXECUTABLE_NAME "WindTerm-Weifan"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.weifan.windterm"
    MACOSX_BUNDLE_ICON_FILE "windterm-weifan.icns"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

include(GNUInstallDirs)
install(TARGETS windterm-weifan
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(UNIX AND NOT APPLE)
    install(FILES resources/linux/windterm-weifan.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    foreach(size 16 32 48 64 128 256 512)
        install(FILES resources/icons/windterm-weifan-${size}.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
                RENAME windterm-weifan.png)
    endforeach()
endif()

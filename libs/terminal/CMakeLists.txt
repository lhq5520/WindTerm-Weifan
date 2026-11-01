# wt_terminal: Qt-free terminal emulation (VT parser, screen model, key encoding).

add_library(wt_terminal STATIC
    include/wt/terminal/Cell.h
    include/wt/terminal/KeyEncoder.h
    include/wt/terminal/KittyGraphics.h
    include/wt/terminal/MouseEncoder.h
    include/wt/terminal/Search.h
    include/wt/terminal/Selection.h
    include/wt/terminal/Sixel.h
    include/wt/terminal/Terminal.h
    include/wt/terminal/VtParser.h
    src/KeyEncoder.cpp
    src/KittyGraphics.cpp
    src/MouseEncoder.cpp
    src/Palette.cpp
    src/Search.cpp
    src/Sixel.cpp
    src/Terminal.cpp
    src/VtParser.cpp)
add_library(wt::terminal ALIAS wt_terminal)

target_include_directories(wt_terminal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wt_terminal PUBLIC wt::core)
# zlib-compressed kitty graphics data (o=z); without zlib such images are refused.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(wt_terminal PRIVATE ZLIB::ZLIB)
    target_compile_definitions(wt_terminal PRIVATE WT_HAVE_ZLIB)
endif()
wt_configure_target(wt_terminal)

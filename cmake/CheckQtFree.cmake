# Script mode: cmake -DWT_LIBS_DIR=<dir> -P CheckQtFree.cmake
# Fails if any source under the Qt-free libraries includes Qt headers or uses Qt macros.

if(NOT WT_LIBS_DIR)
    message(FATAL_ERROR "WT_LIBS_DIR is not set")
endif()

file(GLOB_RECURSE sources
    "${WT_LIBS_DIR}/*.h" "${WT_LIBS_DIR}/*.hpp" "${WT_LIBS_DIR}/*.cpp" "${WT_LIBS_DIR}/*.cmake"
    "${WT_LIBS_DIR}/*/CMakeLists.txt")

set(violations "")
foreach(file IN LISTS sources)
    file(STRINGS "${file}" lines REGEX "#[ \t]*include[ \t]*[<\"]Q|Q_OBJECT|Qt6::|find_package\\(Qt")
    foreach(line IN LISTS lines)
        string(APPEND violations "  ${file}: ${line}\n")
    endforeach()
endforeach()

list(LENGTH sources count)
if(violations)
    message(FATAL_ERROR "Qt dependency found in Qt-free libraries:\n${violations}")
endif()
message(STATUS "Qt-free check passed (${count} files scanned)")

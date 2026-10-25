# Locates libssh2 and provides the imported target WtLibssh2::libssh2.
#
# Tries, in order: an upstream/vcpkg CMake package (libssh2::libssh2 or Libssh2::libssh2),
# pkg-config (Homebrew, Linux distributions) and a plain header/library search.

if(TARGET WtLibssh2::libssh2)
    set(WtLibssh2_FOUND TRUE)
    return()
endif()

find_package(libssh2 CONFIG QUIET)
if(NOT libssh2_FOUND)
    find_package(Libssh2 CONFIG QUIET)
endif()
foreach(candidate libssh2::libssh2 Libssh2::libssh2 libssh2::libssh2_shared libssh2::libssh2_static
                  Libssh2::libssh2_shared Libssh2::libssh2_static)
    if(TARGET ${candidate})
        add_library(WtLibssh2::libssh2 INTERFACE IMPORTED)
        target_link_libraries(WtLibssh2::libssh2 INTERFACE ${candidate})
        set(WtLibssh2_FOUND TRUE)
        set(WtLibssh2_VERSION "${libssh2_VERSION}${Libssh2_VERSION}")
        message(STATUS "libssh2: CMake package (${candidate})")
        return()
    endif()
endforeach()

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(WT_LIBSSH2 QUIET IMPORTED_TARGET libssh2>=1.10)
    if(WT_LIBSSH2_FOUND)
        add_library(WtLibssh2::libssh2 INTERFACE IMPORTED)
        target_link_libraries(WtLibssh2::libssh2 INTERFACE PkgConfig::WT_LIBSSH2)
        set(WtLibssh2_FOUND TRUE)
        set(WtLibssh2_VERSION ${WT_LIBSSH2_VERSION})
        message(STATUS "libssh2: pkg-config ${WT_LIBSSH2_VERSION}")
        return()
    endif()
endif()

find_path(WT_LIBSSH2_INCLUDE_DIR libssh2.h)
find_library(WT_LIBSSH2_LIBRARY NAMES ssh2 libssh2)
if(WT_LIBSSH2_INCLUDE_DIR AND WT_LIBSSH2_LIBRARY)
    add_library(WtLibssh2::libssh2 UNKNOWN IMPORTED)
    set_target_properties(WtLibssh2::libssh2 PROPERTIES
        IMPORTED_LOCATION "${WT_LIBSSH2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${WT_LIBSSH2_INCLUDE_DIR}")
    set(WtLibssh2_FOUND TRUE)
    message(STATUS "libssh2: ${WT_LIBSSH2_LIBRARY}")
    return()
endif()

set(WtLibssh2_FOUND FALSE)
if(WtLibssh2_FIND_REQUIRED)
    message(FATAL_ERROR "libssh2 (>= 1.10) not found. Install it (brew install libssh2 / "
                        "apt install libssh2-1-dev / vcpkg install libssh2) or set WT_ENABLE_SSH=OFF.")
endif()

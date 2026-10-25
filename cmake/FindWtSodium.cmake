# Locates libsodium and provides the imported target WtSodium::sodium (vcpkg's unofficial-sodium
# package, pkg-config, or a plain library search).

if(TARGET WtSodium::sodium)
    set(WtSodium_FOUND TRUE)
    return()
endif()

find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
    add_library(WtSodium::sodium INTERFACE IMPORTED)
    target_link_libraries(WtSodium::sodium INTERFACE unofficial-sodium::sodium)
    set(WtSodium_FOUND TRUE)
    return()
endif()

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(WT_SODIUM QUIET IMPORTED_TARGET libsodium>=1.0.18)
    if(WT_SODIUM_FOUND)
        add_library(WtSodium::sodium INTERFACE IMPORTED)
        target_link_libraries(WtSodium::sodium INTERFACE PkgConfig::WT_SODIUM)
        set(WtSodium_FOUND TRUE)
        return()
    endif()
endif()

find_path(WT_SODIUM_INCLUDE_DIR sodium.h)
find_library(WT_SODIUM_LIBRARY NAMES sodium libsodium)
if(WT_SODIUM_INCLUDE_DIR AND WT_SODIUM_LIBRARY)
    add_library(WtSodium::sodium UNKNOWN IMPORTED)
    set_target_properties(WtSodium::sodium PROPERTIES
        IMPORTED_LOCATION "${WT_SODIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${WT_SODIUM_INCLUDE_DIR}")
    set(WtSodium_FOUND TRUE)
    return()
endif()

set(WtSodium_FOUND FALSE)

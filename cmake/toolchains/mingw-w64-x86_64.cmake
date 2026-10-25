# Cross-compiles for 64-bit Windows with mingw-w64 (used to compile-check the Windows code paths
# from Linux, e.g. `cmake --preset windows-mingw-cross`). Host libraries (pkg-config, OpenSSL,
# libssh2, Qt) must not leak into the build.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(WT_MINGW_PREFIX x86_64-w64-mingw32)

# Debian/Ubuntu ship "-posix" variants (std::thread support); Homebrew/MSYS2 only the plain names.
find_program(WT_MINGW_CC NAMES ${WT_MINGW_PREFIX}-gcc-posix ${WT_MINGW_PREFIX}-gcc REQUIRED)
find_program(WT_MINGW_CXX NAMES ${WT_MINGW_PREFIX}-g++-posix ${WT_MINGW_PREFIX}-g++ REQUIRED)
find_program(WT_MINGW_RC NAMES ${WT_MINGW_PREFIX}-windres)
set(CMAKE_C_COMPILER ${WT_MINGW_CC})
set(CMAKE_CXX_COMPILER ${WT_MINGW_CXX})
if(WT_MINGW_RC)
    set(CMAKE_RC_COMPILER ${WT_MINGW_RC})
endif()

# Search only the cross toolchain's sysroot (override WT_MINGW_SYSROOT for other installations).
if(NOT WT_MINGW_SYSROOT)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-sysroot OUTPUT_VARIABLE WT_MINGW_SYSROOT
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT WT_MINGW_SYSROOT)
        find_program(_wt_mingw_gxx ${CMAKE_CXX_COMPILER})
        get_filename_component(_wt_mingw_bin "${_wt_mingw_gxx}" REALPATH)
        get_filename_component(_wt_mingw_bin "${_wt_mingw_bin}" DIRECTORY)
        set(WT_MINGW_SYSROOT "${_wt_mingw_bin}/../${WT_MINGW_PREFIX}")
    endif()
endif()
set(CMAKE_FIND_ROOT_PATH "${WT_MINGW_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON) # never pick up the host's .pc files

# Static runtime so the test executables run without extra DLLs (e.g. under Wine).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")

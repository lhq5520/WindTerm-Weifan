cmake_minimum_required(VERSION 3.21)

project(WindTermWeifan
    VERSION 0.1.0
    DESCRIPTION "Cross-platform terminal emulator (WindTerm-inspired rewrite)"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use e.g. `cmake --preset dev`.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WT_BUILD_APP "Build the Qt6 GUI application" ON)
option(WT_BUILD_TESTS "Build unit and integration tests" ON)
option(WT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(WT_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(WT_ENABLE_SSH AUTO CACHE STRING "SSH/SFTP support via libssh2: ON, OFF or AUTO (if found)")
set_property(CACHE WT_ENABLE_SSH PROPERTY STRINGS AUTO ON OFF)
set(WT_ENABLE_VAULT AUTO CACHE STRING "Encrypted credential vault: ON, OFF or AUTO (if a crypto backend is found)")
set_property(CACHE WT_ENABLE_VAULT PROPERTY STRINGS AUTO ON OFF)
set(WT_CRYPTO_BACKEND AUTO CACHE STRING "Vault crypto: openssl (>= 3.2), sodium, or AUTO (OpenSSL first)")
set_property(CACHE WT_CRYPTO_BACKEND PROPERTY STRINGS AUTO openssl sodium)

include(WtCompileOptions)

# The legacy WindTerm sources under src/ (Qt5-era Pty, libssh 0.9.5 fork, Onigmo, widgets)
# are kept untouched for reference and are intentionally NOT part of this build.
# See docs/ARCHITECTURE.md for the migration plan.

add_subdirectory(libs/core)
add_subdirectory(libs/platform)
add_subdirectory(libs/terminal)
add_subdirectory(libs/net)
add_subdirectory(libs/session)

set(WT_HAVE_VAULT OFF)
set(WT_CRYPTO_BACKEND_RESOLVED "")
if(NOT WT_ENABLE_VAULT STREQUAL "OFF")
    string(TOLOWER "${WT_CRYPTO_BACKEND}" _wt_backend)
    if(_wt_backend STREQUAL "auto" OR _wt_backend STREQUAL "openssl")
        find_package(OpenSSL 3.2 COMPONENTS Crypto)
        if(OpenSSL_FOUND)
            set(WT_CRYPTO_BACKEND_RESOLVED "openssl")
        endif()
    endif()
    if(NOT WT_CRYPTO_BACKEND_RESOLVED AND (_wt_backend STREQUAL "auto" OR _wt_backend STREQUAL "sodium"))
        find_package(WtSodium)
        if(WtSodium_FOUND)
            set(WT_CRYPTO_BACKEND_RESOLVED "sodium")
        endif()
    endif()
    if(WT_CRYPTO_BACKEND_RESOLVED)
        set(WT_HAVE_VAULT ON)
        add_subdirectory(libs/security)
    elseif(WT_ENABLE_VAULT STREQUAL "ON")
        message(FATAL_ERROR "The credential vault needs OpenSSL >= 3.2 or libsodium (WT_CRYPTO_BACKEND=${WT_CRYPTO_BACKEND}).")
    endif()
endif()

set(WT_HAVE_SSH OFF)
if(NOT WT_ENABLE_SSH STREQUAL "OFF")
    if(WT_ENABLE_SSH STREQUAL "ON")
        find_package(WtLibssh2 REQUIRED)
    else()
        find_package(WtLibssh2)
    endif()
    if(WtLibssh2_FOUND)
        set(WT_HAVE_SSH ON)
        add_subdirectory(libs/ssh)
    endif()
endif()

if(WT_BUILD_APP)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
    if(UNIX AND NOT APPLE)
        find_package(Qt6 6.5 REQUIRED COMPONENTS DBus) # Secret Service credential store
    endif()
    add_subdirectory(app)
endif()

if(WT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(WT_BUILD_APP)
    include(WtPackaging)
endif()

message(STATUS "WindTerm-Weifan ${PROJECT_VERSION}: app=${WT_BUILD_APP} tests=${WT_BUILD_TESTS} ssh=${WT_HAVE_SSH} vault=${WT_HAVE_VAULT}(${WT_CRYPTO_BACKEND_RESOLVED}) "
               "sanitizers=${WT_ENABLE_SANITIZERS} system=${CMAKE_SYSTEM_NAME} "
               "arch=${CMAKE_SYSTEM_PROCESSOR} osx_arch=${CMAKE_OSX_ARCHITECTURES}")

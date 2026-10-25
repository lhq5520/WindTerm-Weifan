# wt_core: Qt-free foundation types (status, UTF-8, character width, base64, JSON).

add_library(wt_core STATIC
    include/wt/core/Base64.h
    include/wt/core/CharWidth.h
    include/wt/core/Json.h
    include/wt/core/Status.h
    include/wt/core/Utf8.h
    src/Base64.cpp
    src/CharWidth.cpp
    src/CharWidthTables.inc
    src/Json.cpp
    src/Utf8.cpp)
add_library(wt::core ALIAS wt_core)

target_include_directories(wt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wt_core PUBLIC cxx_std_20)
wt_configure_target(wt_core)

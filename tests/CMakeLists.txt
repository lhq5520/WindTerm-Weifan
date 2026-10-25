add_library(wt_test_support STATIC support/WtTestMain.cpp support/WtTest.h support/TestServers.h
                                    support/SshdFixture.h)
target_include_directories(wt_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_features(wt_test_support PUBLIC cxx_std_20)
wt_configure_target(wt_test_support)

function(wt_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;LABELS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE wt_test_support ${ARG_LIBS})
    wt_configure_target(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT 60)
endfunction()

wt_add_test(wt_core_tests SOURCES core/CoreTests.cpp core/JsonTests.cpp LIBS wt::core LABELS "unit;core")
wt_add_test(wt_terminal_tests
    SOURCES terminal/VtParserTests.cpp terminal/TerminalTests.cpp terminal/SelectionTests.cpp
            terminal/ReflowTests.cpp terminal/ProtocolTests.cpp
    LIBS wt::terminal
    LABELS "unit;terminal")

wt_add_test(wt_net_tests SOURCES net/NetTests.cpp LIBS wt::net LABELS "integration;net")
wt_add_test(wt_session_tests SOURCES session/SessionTests.cpp LIBS wt::session LABELS "unit;session")

if(WT_HAVE_VAULT)
    wt_add_test(wt_security_tests SOURCES security/SecurityTests.cpp LIBS wt::security LABELS "unit;security")
    target_compile_definitions(wt_security_tests PRIVATE WT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
endif()

if(WT_HAVE_SSH AND NOT WIN32)
    wt_add_test(wt_ssh_tests SOURCES ssh/SshTests.cpp LIBS wt::ssh LABELS "integration;ssh")
    set_tests_properties(wt_ssh_tests PROPERTIES TIMEOUT 180)
endif()

if(WIN32)
    wt_add_test(wt_pty_tests SOURCES platform/ConPtyTests.cpp LIBS wt::platform LABELS "integration;platform")
else()
    wt_add_test(wt_pty_tests SOURCES platform/PtyTests.cpp LIBS wt::platform LABELS "integration;platform")
endif()

# Architecture guard: the core/platform/terminal layers must stay free of Qt.
add_test(NAME wt_qt_free_libs
    COMMAND ${CMAKE_COMMAND} -DWT_LIBS_DIR=${PROJECT_SOURCE_DIR}/libs -P ${PROJECT_SOURCE_DIR}/cmake/CheckQtFree.cmake)
set_tests_properties(wt_qt_free_libs PROPERTIES LABELS "lint")

if(WT_BUILD_APP)
    add_subdirectory(app)
endif()

# Not a test: throughput numbers for the parser + model (run manually, best in a release build).
add_executable(wt_terminal_bench ${PROJECT_SOURCE_DIR}/tools/bench/TerminalBench.cpp)
target_link_libraries(wt_terminal_bench PRIVATE wt::terminal)
wt_configure_target(wt_terminal_bench)

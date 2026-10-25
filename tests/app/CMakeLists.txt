find_package(Qt6 REQUIRED COMPONENTS Test)

set(CMAKE_AUTOMOC ON)

function(wt_add_gui_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE wt_app Qt6::Test)
    wt_configure_target(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS "integration;gui"
        TIMEOUT 180
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

wt_add_gui_test(wt_app_tests AppTests.cpp)
wt_add_gui_test(wt_workspace_tests WorkspaceTests.cpp)

if(WT_HAVE_SSH AND NOT WIN32)
    wt_add_gui_test(wt_ssh_app_tests SshAppTests.cpp)
    target_include_directories(wt_ssh_app_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    set_tests_properties(wt_ssh_app_tests PROPERTIES LABELS "integration;gui;ssh" TIMEOUT 300)
endif()

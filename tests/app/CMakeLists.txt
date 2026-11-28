# The app's suites: core/ (model, mrkdwn, the fake workspace, the Slack
# service, the updater, audio, crashes), then one directory per app library
# and screen group, each only when that part of the app is built.
#
# One test executable, msga_app_tests: they all link (most of) the same stack,
# up to the whole shell, and one link of it beats sixteen. Each test file is a
# suite (test_shell.cpp -> test_shell, support/test.h) that its directory adds
# with target_sources and runs as its own ctest test(s), each in its own
# process with its own environment (msga_add_test, ../CMakeLists.txt):
#
#     msga_app_tests test_shell             # what ctest test shell_tests runs
#     msga_app_tests test_messages image    # one case group (messages_image)
#
# Labelled by directory: `ctest -L claude` runs every Claude Code suite.
add_executable(msga_app_tests main.cpp)
target_link_libraries(msga_app_tests PRIVATE msga_test)
target_compile_definitions(msga_app_tests PRIVATE
    MSGA_SOURCE_DIR="${MSGA_ROOT}"
    MSGA_TEST_ASSETS="${MSGA_TEST_ASSETS}")

add_subdirectory(core)
foreach(part claude llm spell legacy)
    if(TARGET msga_app_${part})
        add_subdirectory(${part})
    endif()
endforeach()
foreach(group messages settings shell)
    if(TARGET msga_screens_${group})
        add_subdirectory(screens/${group})
    endif()
endforeach()

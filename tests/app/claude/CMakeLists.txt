# msga_app_claude: the Claude Code workspace, against fake ~/.claude trees.
# Suites of msga_app_tests (../CMakeLists.txt), ctest test claude_<suite> each.
target_link_libraries(msga_app_tests PRIVATE msga_app_claude)
# Headless plat, a pinned zone, a throwaway HOME/XDG per suite: the tests
# never see the user's ~/.claude (each case builds its own fake one).
foreach(s terminal launcher roster approvals transcript roles outputs worktrees catalog backend links)
    target_sources(msga_app_tests PRIVATE test_${s}.cpp)
    set(_home "${CMAKE_CURRENT_BINARY_DIR}/test-home-${s}")
    msga_add_test(claude_${s} COMMAND msga_app_tests test_${s} TZ HOME ${_home}
        ENV "CLAUDE_CONFIG_DIR=${_home}/.claude" LABELS claude)
endforeach()

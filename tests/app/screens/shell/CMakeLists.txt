# msga_screens_shell: whole shells on the test fixture (../../../assets).
# Suites of msga_app_tests (../../CMakeLists.txt), one ctest test each.
target_link_libraries(msga_app_tests PRIVATE msga_screens_shell msga_app_fake msga_llm_testkit)

# Headless plat, a pinned zone, and a throwaway HOME/XDG so the tests never
# touch the user's settings or font index cache.
set(_home "${CMAKE_CURRENT_BINARY_DIR}/test-home")
foreach(s shell shortcuts pages shell_ui agents agent_ui links_ui)
    target_sources(msga_app_tests PRIVATE test_${s}.cpp)
endforeach()
msga_add_test(shell_tests COMMAND msga_app_tests test_shell TZ HOME ${_home} LABELS shell)
msga_add_test(shortcut_tests COMMAND msga_app_tests test_shortcuts TZ HOME ${_home} LABELS shell)
msga_add_test(pages_tests COMMAND msga_app_tests test_pages TZ HOME ${_home} LABELS shell)
msga_add_test(shell_ui_tests COMMAND msga_app_tests test_shell_ui TZ HOME ${_home} LABELS shell)
msga_add_test(agent_tests COMMAND msga_app_tests test_agents TZ HOME ${_home} LABELS shell)
msga_add_test(agent_ui_tests COMMAND msga_app_tests test_agent_ui TZ HOME ${_home} LABELS shell)
msga_add_test(links_ui_tests COMMAND msga_app_tests test_links_ui TZ HOME ${_home} LABELS shell)

# sidebar_model.h's free functions: a bare model::Store, no plat/widget tree.
target_sources(msga_app_tests PRIVATE test_sidebar_model.cpp)
msga_add_test(sidebar_model_tests COMMAND msga_app_tests test_sidebar_model LABELS shell)

# shell_notify_model.h's free functions: same, a bare model::Store.
target_sources(msga_app_tests PRIVATE test_shell_notify_model.cpp)
msga_add_test(shell_notify_model_tests COMMAND msga_app_tests test_shell_notify_model LABELS shell)

# buildVoiceContext: a bare model::Store, no screens::Context/ui:: needed.
target_sources(msga_app_tests PRIVATE test_voice_strip.cpp)
msga_add_test(voice_strip_tests COMMAND msga_app_tests test_voice_strip LABELS shell)

# typingStateOf: same, a bare model::Store.
target_sources(msga_app_tests PRIVATE test_typing_indicator.cpp)
msga_add_test(typing_indicator_tests COMMAND msga_app_tests test_typing_indicator LABELS shell)

# Its own throwaway HOME: it writes a launcher entry and a socket there.
target_sources(msga_app_tests PRIVATE test_identity.cpp)
msga_add_test(identity_tests COMMAND msga_app_tests test_identity
    HOME ${CMAKE_CURRENT_BINARY_DIR}/test-home-identity LABELS shell)

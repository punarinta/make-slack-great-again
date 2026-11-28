# msga_app_spell: the word rules and the OS's (or Hunspell's) checker. A
# suite of msga_app_tests (../CMakeLists.txt).
target_sources(msga_app_tests PRIVATE test_spell.cpp)
target_link_libraries(msga_app_tests PRIVATE msga_app_spell)
# Headless plat and a throwaway HOME/XDG: added words never reach the
# user's own list.
msga_add_test(spell_tests COMMAND msga_app_tests test_spell
    HOME ${CMAKE_CURRENT_BINARY_DIR}/test-home LABELS spell)

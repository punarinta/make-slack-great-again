# The Settings dialog, driven inside a whole shell on the test fixture
# (../../../assets), headless. A suite of msga_app_tests (../../CMakeLists.txt).
target_sources(msga_app_tests PRIVATE test_settings.cpp)
target_link_libraries(msga_app_tests PRIVATE msga_screens_shell msga_app_fake msga_llm_testkit)
msga_add_test(settings_tests COMMAND msga_app_tests test_settings TZ
    HOME ${CMAKE_CURRENT_BINARY_DIR}/test-home LABELS settings)

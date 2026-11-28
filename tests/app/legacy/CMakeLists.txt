# msga_app_legacy: the upgrade from earlier versions' data. A suite of
# msga_app_tests (../CMakeLists.txt). Its own throwaway HOME: it writes the
# old settings and cache there.
target_sources(msga_app_tests PRIVATE test_legacy.cpp)
target_link_libraries(msga_app_tests PRIVATE msga_app_legacy)
msga_add_test(legacy_tests COMMAND msga_app_tests test_legacy
    HOME ${CMAKE_CURRENT_BINARY_DIR}/test-home LABELS legacy)

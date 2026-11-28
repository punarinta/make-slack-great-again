# msga_app_llm: the provider wires and the Service, against the fake AI
# server (msga_llm_testkit, ../../CMakeLists.txt). Suites of msga_app_tests
# (../CMakeLists.txt), ctest test llm_<suite> each.
target_link_libraries(msga_app_tests PRIVATE msga_llm_testkit)
foreach(s llm voice)
    target_sources(msga_app_tests PRIVATE test_${s}.cpp)
    msga_add_test(llm_${s} COMMAND msga_app_tests test_${s} LABELS llm)
endforeach()

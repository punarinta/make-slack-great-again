# msga_net. Offline: against a local Python server (server.py: HTTP, chunked,
# redirects with cookies, keep-alive, WebSocket echo). The live test against
# slack.com only runs with MSGA_NET_LIVE=1 in the environment.
add_executable(msga_net_tests test_net.cpp main.cpp)
target_link_libraries(msga_net_tests PRIVATE msga_net msga_test)
target_compile_definitions(msga_net_tests PRIVATE
    NET_TEST_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/server.py")
msga_add_test(msga_net_tests COMMAND msga_net_tests)
msga_size_map(msga_net_tests)

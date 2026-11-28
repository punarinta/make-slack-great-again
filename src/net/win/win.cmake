# WinHTTP transport (transport.h): HTTP and WebSockets on the OS's own stack,
# Schannel TLS and the system proxy settings — nothing bundled.
target_sources(msga_net PRIVATE ${CMAKE_CURRENT_LIST_DIR}/winhttp.cpp)
target_compile_definitions(msga_net PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(msga_net PUBLIC winhttp ws2_32)

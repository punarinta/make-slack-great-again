# msga_net — HTTP requests and WebSockets (see net.h). The common code owns
# threads, redirects and delivery; the transport is per OS (transport.h):
# WinHTTP on Windows, NSURLSession on macOS (both use the OS's TLS), and our
# own HTTP/1.1 + WebSocket over mbedTLS everywhere else (Linux).
add_library(msga_net STATIC
    client.cpp
    websocket.cpp
    url.cpp
)
target_include_directories(msga_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(msga_net PUBLIC msga_base plat msga_flags)

if(MSGA_PORT)
    include(port/port.cmake)
elseif(WIN32)
    include(win/win.cmake)
elseif(APPLE)
    include(mac/mac.cmake)
else()
    include(posix/posix.cmake)
endif()

# What net costs an app: an HTTPS request + a WebSocket, nothing else.
add_executable(msga_net_size_probe probe/size_probe.cpp)
target_link_libraries(msga_net_size_probe PRIVATE msga_net)
msga_size_map(msga_net_size_probe)

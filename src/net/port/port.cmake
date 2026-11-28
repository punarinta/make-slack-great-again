# The port transport: HTTP and WebSockets through the C port contract
# (port/port_net.h), so the port brings its own sockets, DNS and TLS.
# Included from net/CMakeLists.txt when MSGA_PORT is on.
target_sources(msga_net PRIVATE port/transport_port.cpp)
target_link_libraries(msga_net PUBLIC msga_port)

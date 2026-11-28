# The reference port (see ref_core.c): the whole contract in C11 over POSIX,
# headless. Built as plain C, pedantic, to show a port needs nothing else.
add_library(msga_port_impl STATIC
    ref_core.c
    ref_sys.c
    ref_net.c
    ref_app.c
    ref_audio.c
)
target_link_libraries(msga_port_impl PUBLIC msga_port_contract)
set_target_properties(msga_port_impl PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(msga_port_impl PRIVATE -pedantic -Wall -Wextra
        -ffunction-sections -fdata-sections)
endif()
find_package(Threads REQUIRED)
target_link_libraries(msga_port_impl PUBLIC Threads::Threads)

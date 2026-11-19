# The port contract (include/port/*.h, docs/port-contract.md): what msga's
# core needs from an OS, as plain C. Added from the root CMakeLists.txt when
# MSGA_PLATFORM=port. MSGA_PORT_IMPL names the directory of the port's C
# files, a CMake fragment that defines the static library msga_port_impl;
# the default is the reference port in ref/ (POSIX, headless, for the test
# suites).
enable_language(C)

add_library(msga_port_contract INTERFACE)
target_include_directories(msga_port_contract INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(MSGA_PORT_IMPL "${CMAKE_CURRENT_SOURCE_DIR}/ref" CACHE PATH
    "Directory with the port's C implementation (defines msga_port_impl)")
add_subdirectory(${MSGA_PORT_IMPL} port_impl)

# What the core links: the headers and the port's code.
add_library(msga_port INTERFACE)
target_link_libraries(msga_port INTERFACE msga_port_contract msga_port_impl)

# The headers must stay plain C11: compiled as such, pedantic, warnings as
# errors, on every port build.
add_library(msga_port_header_check OBJECT header_check.c)
target_link_libraries(msga_port_header_check PRIVATE msga_port_contract)
set_target_properties(msga_port_header_check PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(msga_port_header_check PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()
# Built with the port, so a header change that breaks C fails the build.
add_dependencies(msga_port_impl msga_port_header_check)

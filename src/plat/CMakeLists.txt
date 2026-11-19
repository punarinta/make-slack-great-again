# plat — msga's thin platform layer (see include/plat/plat.h and
# docs/platform-layer-plan.md). It also builds standalone:
#   cmake -S src/plat -B build-plat -G Ninja && cmake --build build-plat && ctest --test-dir build-plat
# Windows (cross, from Linux; the toolchain path is relative to src/plat/):
#   cmake -S src/plat -B build-plat-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake_minimum_required(VERSION 3.21)
project(plat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The product builds without exceptions/RTTI would be the size win; keep the
# library clean under both so that choice stays open.
option(PLAT_NO_EXCEPTIONS "Build plat with -fno-exceptions -fno-rtti" OFF)

# PLAT_PORT: the one backend is the port adapter (src/port/), over the C
# port contract (src/port/include/port, docs/port-contract.md): no native
# backend, no OS headers. The embedding project supplies the msga_port
# target (the contract and the port's C code).
option(PLAT_PORT "Build plat on the C port contract instead of the native backends" OFF)

add_library(plat STATIC
    src/core/common.cpp
    src/core/dir_watch.cpp
    src/core/transfer.cpp
    src/core/input.cpp
    src/core/image_util.cpp
    src/core/strings.cpp
    src/core/wire.cpp
    src/core/drop.cpp
)
target_include_directories(plat PUBLIC include PRIVATE src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Sections so an embedder's --gc-sections / -dead_strip can drop the parts
    # of plat it never calls.
    target_compile_options(plat PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
        -ffunction-sections -fdata-sections)
    if(PLAT_NO_EXCEPTIONS)
        # Not for C: the generated Wayland protocol .c files reject -fno-rtti.
        target_compile_options(plat PRIVATE
            $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fno-exceptions -fno-rtti>)
        # Nothing unwinds without exceptions: no .eh_frame in size builds
        # (it was ~75 KB of the msga binary).
        target_compile_options(plat PRIVATE
            $<$<CONFIG:MinSizeRel>:-fno-asynchronous-unwind-tables -fno-unwind-tables>)
    endif()
endif()

# Test-only code: TestHooks (injected input, screen readback, fake-desktop
# probes, and the protocols/libraries they need) and the headless backend
# (PLAT_BACKEND=headless). On for plat's own tests; an embedding project turns
# it on only for its test builds, so release binaries carry none of it.
option(PLAT_TEST_HOOKS "Compile plat's TestHooks and headless backend (test builds only)" ON)
if(PLAT_TEST_HOOKS)
    target_compile_definitions(plat PRIVATE PLAT_TEST_HOOKS)
    if(NOT PLAT_PORT) # a port's test hooks are the port's (port_test.h)
        target_sources(plat PRIVATE src/headless/headless.cpp)
    endif()
endif()

if(PLAT_PORT)
    # The loop, timers and threads are the port's: no LoopCore here.
    target_sources(plat PRIVATE src/port/port_app.cpp src/port/port_audio.cpp)
    target_compile_definitions(plat PUBLIC PLAT_PORT MSGA_PORT)
    target_link_libraries(plat PUBLIC msga_port)
else()
    target_sources(plat PRIVATE src/core/loop_core.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(plat PUBLIC Threads::Threads)
endif()

# The string, UTF-8 and hash primitives plat shares with the app's base
# library (src/prim; an embedding project may have added it already).
if(NOT TARGET prim)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../prim ${CMAKE_CURRENT_BINARY_DIR}/prim)
endif()
target_link_libraries(plat PUBLIC prim)

if(UNIX AND NOT PLAT_PORT)
    target_sources(plat PRIVATE src/posix/posix_loop.cpp)
endif()
if(UNIX AND NOT APPLE AND NOT PLAT_PORT)
    # Child processes and file helpers for the desktop tools, XDG dirs and audio.
    target_sources(plat PRIVATE src/freedesktop/process.cpp src/freedesktop/files.cpp)
endif()

# Many of OpenBSD's shared libraries carry no DT_SONAME, so the linker
# records whatever path it was handed as the dependency, and pkg_check_modules
# hands it absolute ones (a cross build's sysroot path would end up in the
# binary). Turns a pkg-config target's libraries into -L/-l:<file> pairs
# there: found by name, a library without a soname is recorded as its file
# name (libxcb-cursor.so.0.0), as OpenBSD's own linker does.
function(plat_link_by_name target)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
        return()
    endif()
    get_target_property(_libs ${target} INTERFACE_LINK_LIBRARIES)
    set(_out)
    foreach(_lib IN LISTS _libs)
        if(_lib MATCHES "^(.*)/(lib[^/]+\\.so(\\.[0-9]+)*)$")
            list(APPEND _out "-L${CMAKE_MATCH_1}" "-l:${CMAKE_MATCH_2}")
        else()
            list(APPEND _out "${_lib}")
        endif()
    endforeach()
    set_target_properties(${target} PROPERTIES INTERFACE_LINK_LIBRARIES "${_out}")
endfunction()

# Each backend owns its own fragment, which adds sources and dependencies to
# the `plat` target and defines PLAT_HAS_<BACKEND> when it builds.
# src/freedesktop: what X11 and Wayland share on any freedesktop.org desktop
# (Linux and the BSDs): xkb keymaps, the single-instance socket, XDG dirs,
# cursor themes, child processes, the D-Bus services. Nothing in it is
# Linux-only beyond a few #ifs (the peer-credential call, memfd).
if(PLAT_PORT)
    # The port adapter above is the only backend.
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    option(PLAT_WAYLAND "Build the Wayland backend" ON)
    option(PLAT_X11 "Build the X11 backend" ON)
    if(PLAT_WAYLAND OR PLAT_X11)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(PLAT_XKB REQUIRED IMPORTED_TARGET xkbcommon)
        target_sources(plat PRIVATE src/freedesktop/xkb_keyboard.cpp src/freedesktop/instance.cpp
            src/freedesktop/xdg_dirs.cpp src/freedesktop/cursor_names.cpp src/freedesktop/posix_app.cpp)
        # D-Bus desktop services (tray, notifications, badge, portal) shared by
        # both backends; src/freedesktop/services.cmake replaces the stub.
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/freedesktop/services.cmake")
            include(src/freedesktop/services.cmake)
        else()
            target_sources(plat PRIVATE src/freedesktop/services_stub.cpp)
        endif()
        plat_link_by_name(PkgConfig::PLAT_XKB)
        target_link_libraries(plat PUBLIC PkgConfig::PLAT_XKB)
        set(PLAT_LINUX_INPUT ON)
    endif()
    if(PLAT_WAYLAND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/wayland/wayland.cmake")
        include(src/wayland/wayland.cmake)
    endif()
    if(PLAT_X11 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/x11/x11.cmake")
        include(src/x11/x11.cmake)
    endif()
elseif(WIN32)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/win32/win32.cmake")
        include(src/win32/win32.cmake)
    endif()
elseif(APPLE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/cocoa/cocoa.cmake")
        include(src/cocoa/cocoa.cmake)
    endif()
endif()

if(NOT PLAT_PORT) # the port's audio: src/port/port_audio.cpp
    include(src/audio/audio.cmake)
else()
    target_sources(plat PRIVATE src/audio/audio_common.cpp)
endif()

# ── Tests ───────────────────────────────────────────────────────────────────
# Tests and selftest. Off when plat is embedded in another project
# (the app build sets it OFF so its ctest isn't full of never-built plat binaries).
option(PLAT_BUILD_TESTS "Build plat's tests and selftest" ON)
if(NOT PLAT_BUILD_TESTS)
    return()
endif()
enable_testing()
# The tests live in the repository's tests/plat (one level above src/).
set(PLAT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/plat ${CMAKE_CURRENT_BINARY_DIR}/tests)

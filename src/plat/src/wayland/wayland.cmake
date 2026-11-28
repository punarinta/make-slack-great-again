# Wayland backend. Links libwayland-client/-cursor and xkbcommon directly (the
# static musl build has no dlopen, so no libdecor and no runtime loading), and
# generates protocol glue with wayland-scanner at build time.
#
# System protocol XMLs come from wayland-protocols (pkg-config pkgdatadir, or
# -DPLAT_WAYLAND_PROTOCOLS_DIR=… for cross/static toolchains). The wlroots
# test-injection protocols are not packaged anywhere, so they are vendored in
# src/wayland/protocols/ (MIT, see the <copyright> in each file); they and
# wl_testhooks.cpp are only built with PLAT_TEST_HOOKS.

pkg_check_modules(PLAT_WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-cursor)
# A from-source libwayland's .pc carries Cflags: -pthread (distro ones don't),
# which would reach every target linking plat and break the test trees' shared
# PCH under Clang. The headers don't need it; Threads::Threads links pthreads.
get_target_property(_plat_wl_opts PkgConfig::PLAT_WAYLAND INTERFACE_COMPILE_OPTIONS)
if(_plat_wl_opts)
    list(REMOVE_ITEM _plat_wl_opts -pthread)
    set_target_properties(PkgConfig::PLAT_WAYLAND PROPERTIES INTERFACE_COMPILE_OPTIONS "${_plat_wl_opts}")
endif()

if(NOT PLAT_WAYLAND_PROTOCOLS_DIR)
    pkg_get_variable(PLAT_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
endif()
if(NOT PLAT_WAYLAND_PROTOCOLS_DIR OR NOT EXISTS "${PLAT_WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
    message(FATAL_ERROR "plat: wayland-protocols not found (set PLAT_WAYLAND_PROTOCOLS_DIR)")
endif()

pkg_get_variable(PLAT_WAYLAND_SCANNER_PC wayland-scanner wayland_scanner)
find_program(PLAT_WAYLAND_SCANNER NAMES wayland-scanner HINTS "${PLAT_WAYLAND_SCANNER_PC}")
if(NOT PLAT_WAYLAND_SCANNER)
    message(FATAL_ERROR "plat: wayland-scanner not found")
endif()

set(_wp "${PLAT_WAYLAND_PROTOCOLS_DIR}")
set(_vendored "${CMAKE_CURRENT_SOURCE_DIR}/src/wayland/protocols")
# name=xml — `name` is the stem of the generated <name>-client-protocol.h/.c.
set(PLAT_WAYLAND_PROTOCOLS
    "xdg-shell=${_wp}/stable/xdg-shell/xdg-shell.xml"
    "viewporter=${_wp}/stable/viewporter/viewporter.xml"
    "fractional-scale-v1=${_wp}/staging/fractional-scale/fractional-scale-v1.xml"
    "cursor-shape-v1=${_wp}/staging/cursor-shape/cursor-shape-v1.xml"
    # cursor-shape references zwp_tablet_tool_v2, so its interface table must link.
    "tablet-unstable-v2=${_wp}/unstable/tablet/tablet-unstable-v2.xml"
    "xdg-activation-v1=${_wp}/staging/xdg-activation/xdg-activation-v1.xml"
    "xdg-decoration-unstable-v1=${_wp}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
    "text-input-unstable-v3=${_wp}/unstable/text-input/text-input-unstable-v3.xml"
    "primary-selection-unstable-v1=${_wp}/unstable/primary-selection/primary-selection-unstable-v1.xml"
    "pointer-gestures-unstable-v1=${_wp}/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml"
    # Monitor layout (logical positions/sizes) and portal parent handles.
    "xdg-output-unstable-v1=${_wp}/unstable/xdg-output/xdg-output-unstable-v1.xml"
    "xdg-foreign-unstable-v1=${_wp}/unstable/xdg-foreign/xdg-foreign-unstable-v1.xml"
    "xdg-foreign-unstable-v2=${_wp}/unstable/xdg-foreign/xdg-foreign-unstable-v2.xml"
)
if(PLAT_TEST_HOOKS)
    list(APPEND PLAT_WAYLAND_PROTOCOLS
        "virtual-keyboard-unstable-v1=${_vendored}/virtual-keyboard-unstable-v1.xml"
        "wlr-virtual-pointer-unstable-v1=${_vendored}/wlr-virtual-pointer-unstable-v1.xml"
        "wlr-screencopy-unstable-v1=${_vendored}/wlr-screencopy-unstable-v1.xml"
    )
endif()

set(_gen "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
file(MAKE_DIRECTORY "${_gen}")
set(_genSources)
foreach(entry IN LISTS PLAT_WAYLAND_PROTOCOLS)
    string(FIND "${entry}" "=" eq)
    string(SUBSTRING "${entry}" 0 ${eq} name)
    math(EXPR eq "${eq} + 1")
    string(SUBSTRING "${entry}" ${eq} -1 xml)
    add_custom_command(
        OUTPUT "${_gen}/${name}-client-protocol.h" "${_gen}/${name}-protocol.c"
        COMMAND "${PLAT_WAYLAND_SCANNER}" client-header "${xml}" "${_gen}/${name}-client-protocol.h"
        COMMAND "${PLAT_WAYLAND_SCANNER}" private-code "${xml}" "${_gen}/${name}-protocol.c"
        DEPENDS "${xml}"
        COMMENT "wayland-scanner ${name}"
        VERBATIM
    )
    list(APPEND _genSources "${_gen}/${name}-client-protocol.h" "${_gen}/${name}-protocol.c")
endforeach()

target_sources(plat PRIVATE
    ${_genSources}
    src/wayland/wl_app.cpp
    src/wayland/wl_window.cpp
    src/wayland/wl_output.cpp
    src/wayland/wl_seat.cpp
    src/wayland/wl_data.cpp
)
if(PLAT_TEST_HOOKS)
    target_sources(plat PRIVATE src/wayland/wl_testhooks.cpp)
endif()
target_include_directories(plat PRIVATE "${_gen}")
plat_link_by_name(PkgConfig::PLAT_WAYLAND)
target_link_libraries(plat PUBLIC PkgConfig::PLAT_WAYLAND)
target_compile_definitions(plat PUBLIC PLAT_HAS_WAYLAND)

# X11 backend. Pure XCB linked directly (no Xlib, no dlopen) so the static
# musl build works the same as a desktop one.
# xcb-xtest only feeds the TestHooks input injectors (PLAT_TEST_HOOKS).
set(_x11Modules xcb xcb-xkb xcb-shm xcb-randr xcb-cursor xcb-xfixes xcb-xinput xkbcommon-x11)
if(PLAT_TEST_HOOKS)
    list(APPEND _x11Modules xcb-xtest)
endif()
pkg_check_modules(PLAT_X11_DEPS REQUIRED IMPORTED_TARGET ${_x11Modules})
target_sources(plat PRIVATE
    src/x11/x11_app.cpp
    src/x11/x11_window.cpp
    src/x11/x11_selection.cpp
    src/x11/x11_dnd.cpp
    src/x11/x11_xinput.cpp
    src/x11/x11_monitors.cpp
)
plat_link_by_name(PkgConfig::PLAT_X11_DEPS)
target_link_libraries(plat PUBLIC PkgConfig::PLAT_X11_DEPS)
target_compile_definitions(plat PUBLIC PLAT_HAS_X11)

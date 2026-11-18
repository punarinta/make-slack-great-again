# D-Bus desktop services for the X11/Wayland backends (tray, notifications, badge,
# file dialogs, reveal, network/sleep/lock events, portal settings,
# portal colour scheme). libdbus-1 is linked directly (the static musl build
# has no dlopen); without it the build falls back to the everything-
# unavailable stub.
pkg_check_modules(PLAT_DBUS IMPORTED_TARGET dbus-1)
if(PLAT_DBUS_FOUND)
    target_sources(plat PRIVATE
        src/freedesktop/dbus_conn.cpp
        src/freedesktop/file_dialog.cpp
        src/freedesktop/notifications.cpp
        src/freedesktop/services_dbus.cpp
        src/freedesktop/sni_tray.cpp
        src/freedesktop/system_events.cpp
    )
    plat_link_by_name(PkgConfig::PLAT_DBUS)
    target_link_libraries(plat PUBLIC PkgConfig::PLAT_DBUS)
    # Portal colour-scheme check driven by scripts/plat-selftest-linux-services.sh
    # against the fake desktop (the selftest cannot flip the setting itself).
    add_executable(plat_services_probe scripts/fakes/services_probe.cpp)
    target_link_libraries(plat_services_probe PRIVATE plat)
else()
    message(STATUS "plat: libdbus-1 not found, D-Bus desktop services disabled")
    target_sources(plat PRIVATE src/freedesktop/services_stub.cpp)
endif()

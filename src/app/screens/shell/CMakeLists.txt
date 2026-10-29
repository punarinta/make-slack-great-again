# msga_screens_shell — window chrome, rail, sidebar, header, composer, thread
# panel wiring, tray/notifications — and the msga executable.
add_library(msga_screens_shell STATIC
    accounts.cpp
    activity_page.cpp
    avatars.cpp
    canvas_page.cpp
    canvas_viewer.cpp
    channel_dialogs.cpp
    composer.cpp
    composer_popups.cpp
    context_menus.cpp
    fuzzy_match.cpp
    gif_search.cpp
    header.cpp
    huddle_banner.cpp
    nav_chrome.cpp
    nav_history.cpp
    desktop_entry.cpp
    profile_card.cpp
    recent_folders.cpp
    session_dialogs.cpp
    session_status_dialog.cpp
    shell_dialogs.cpp
    signin_dialogs.cpp
    message_search.cpp
    quick_switcher.cpp
    shortcuts.cpp
    shell.cpp
    shell_agents.cpp
    shell_links.cpp
    shell_notify.cpp
    shell_text.cpp
    sidebar.cpp
    sidebar_footer.cpp
    teammate_page.cpp
    overview_page.cpp
    saved_page.cpp
    scheduled_page.cpp
    threads_page.cpp
    standin.cpp
    status_dialog.cpp
    swipe_nav.cpp
    typing_indicator.cpp
    voice_strip.cpp
)
target_include_directories(msga_screens_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
if(MSGA_SELF_UPDATE)
    target_sources(msga_screens_shell PRIVATE update_bar.cpp)
endif()
# Linux: the logo the launcher entry names (desktop_entry.cpp), as the old
# app's :/icon.png — gfx/icon_256.png as a byte array.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_filename_component(_png "${CMAKE_CURRENT_SOURCE_DIR}/../../../../gfx/icon_256.png" ABSOLUTE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_png}")
    file(READ "${_png}" _hex HEX)
    file(SIZE "${_png}" _size)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/msga_icon_png.cpp"
        "// Generated from gfx/icon_256.png (screens/shell/CMakeLists.txt).\n"
        "extern const unsigned char kMsgaIconPng[] = {${_bytes}};\n"
        "extern const unsigned kMsgaIconPngSize = ${_size};\n")
    target_sources(msga_screens_shell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/msga_icon_png.cpp")
endif()
target_link_libraries(msga_screens_shell PUBLIC msga_screens_common msga_app_core msga_app_slack msga_app_claude msga_app_llm msga_app_spell msga_ui msga_screens_settings msga_flags)
if(TARGET msga_screens_messages)
    target_link_libraries(msga_screens_shell PUBLIC msga_screens_messages)
    target_compile_definitions(msga_screens_shell PUBLIC MSGA_HAVE_MESSAGES=1)
endif()

# The app itself: <build>/msga. With -DMSGA_DEMO=ON it also carries the fake
# backend: `msga --demo <dir>` runs the demo workspace.
add_executable(msga main.cpp)
target_link_libraries(msga PRIVATE msga_screens_shell)
if(TARGET msga_app_update)
    target_link_libraries(msga PRIVATE msga_app_update)
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../../version.cmake)
target_compile_definitions(msga PRIVATE MSGA_VERSION=${MSGA_VERSION}) # the updater compares it
# Linux: msga's own std::terminate handler in place of libstdc++'s verbose one,
# which links the C++ demangler into the binary just to print a type name
# (app/crash/terminate_handler.cpp).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(msga PRIVATE ../../crash/terminate_handler.cpp)
endif()
# Leak-hunting diagnostics (app/diag/mem_stats.h): SIGUSR1 prints the memory
# numbers (and runs a LeakSanitizer pass under ASan), MSGA_MEMSTATS=<s> logs
# them every <s> seconds. Debug and demo builds only, never the release.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR MSGA_DEMO)
    target_compile_definitions(msga PRIVATE MSGA_DEV_DIAG)
endif()
if(MSGA_DEMO)
    # msga's scripted walkthrough (--demo-tour) and the leak soak (--soak):
    # demo builds only.
    target_sources(msga PRIVATE
        demo/tour.cpp demo/tour_script.cpp demo/soak.cpp demo/claude_demo.cpp)
    target_link_libraries(msga PRIVATE msga_app_fake)
    target_compile_definitions(msga PRIVATE MSGA_DEMO)
    # Demo mode's own checks (the tour parser, ../../../../demo's tour.json and
    # fixture.json): with the demo, never in the test suite under tests/.
    if(MSGA_BUILD_TESTS)
        add_executable(demo_tests demo/demo_tests.cpp demo/tour_script.cpp)
        target_link_libraries(demo_tests PRIVATE msga_app_fake msga_test)
        target_compile_definitions(demo_tests PRIVATE
            MSGA_DEMO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../../demo")
        add_test(NAME demo_tests COMMAND demo_tests)
        set_tests_properties(demo_tests PROPERTIES ENVIRONMENT "PLAT_BACKEND=headless;TZ=Europe/Stockholm")
    endif()
endif()
set_target_properties(msga PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
msga_size_map(msga)

# The packaging identities (app/identity.h): the bundle and executable
# resources every release has used, so the OS and the Homebrew cask see one app.
get_filename_component(_res "${CMAKE_CURRENT_SOURCE_DIR}/../../resources" ABSOLUTE)
get_filename_component(_gfx "${CMAKE_CURRENT_SOURCE_DIR}/../../../../gfx" ABSOLUTE)
if(APPLE)
    # <build>/msga.app, bundle id com.nisdos.msga, msga:// in Info.plist
    # (notifications and the URL scheme need a bundle). The plist and the icon
    # are installed and the bundle ad-hoc signed after every build: CMake
    # writes MACOSX_BUNDLE_INFO_PLIST only at configure time, and an unsigned
    # bundle gets a per-build identity that UNUserNotificationCenter refuses.
    set_target_properties(msga PROPERTIES MACOSX_BUNDLE ON)
    set(MACOSX_BUNDLE_EXECUTABLE_NAME      "msga")
    set(MACOSX_BUNDLE_BUNDLE_NAME          "msga")
    set(MACOSX_BUNDLE_DISPLAY_NAME         "msga")
    set(MACOSX_BUNDLE_INFO_STRING          "msga")
    set(MACOSX_BUNDLE_ICON_FILE            "AppIcon")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER       "com.nisdos.msga")
    set(MACOSX_BUNDLE_BUNDLE_VERSION       "${MSGA_VERSION}")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${MSGA_VERSION}")
    set(MACOSX_BUNDLE_LONG_VERSION_STRING  "${MSGA_VERSION}")
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(MSGA_MACOS_MIN_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(MSGA_MACOS_MIN_VERSION "14.0")
    endif()
    set(MSGA_INFO_PLIST "${CMAKE_BINARY_DIR}/msga_Info.plist")
    configure_file("${_res}/Info.plist.in" "${MSGA_INFO_PLIST}")
    set_target_properties(msga PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${MSGA_INFO_PLIST}")
    # The icon: Xcode 26's actool compiles the Icon Composer .icon (Tahoe's
    # Liquid Glass + a legacy AppIcon.icns); any other toolchain copies the
    # static gfx/mac/icon.icns. Probed by running the real command once.
    execute_process(COMMAND xcrun --find actool
        OUTPUT_VARIABLE _actool OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(_icon_step
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_gfx}/mac/icon.icns"
                "$<TARGET_BUNDLE_CONTENT_DIR:msga>/Resources/AppIcon.icns")
    if(EXISTS "${_actool}")
        execute_process(
            COMMAND "${_actool}" "${_gfx}/mac/AppIcon.icon" --app-icon AppIcon
                    --compile "${CMAKE_BINARY_DIR}/actool_probe" --platform macosx
                    --minimum-deployment-target "${MSGA_MACOS_MIN_VERSION}"
                    --output-partial-info-plist "${CMAKE_BINARY_DIR}/actool_probe.plist"
            RESULT_VARIABLE _probe OUTPUT_QUIET ERROR_QUIET)
        file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/actool_probe" "${CMAKE_BINARY_DIR}/actool_probe.plist")
        if(_probe EQUAL 0)
            set(_icon_step
                COMMAND "${_actool}" "${_gfx}/mac/AppIcon.icon" --app-icon AppIcon
                        --compile "$<TARGET_BUNDLE_CONTENT_DIR:msga>/Resources"
                        --platform macosx --minimum-deployment-target "${MSGA_MACOS_MIN_VERSION}"
                        --output-partial-info-plist "${CMAKE_BINARY_DIR}/actool_partial.plist")
        endif()
    endif()
    add_custom_command(TARGET msga POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MSGA_INFO_PLIST}" "$<TARGET_BUNDLE_CONTENT_DIR:msga>/Info.plist"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:msga>/Resources"
        ${_icon_step}
        COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:msga>"
        VERBATIM
        COMMENT "Installing Info.plist, app icon and ad-hoc signing msga.app")
elseif(WIN32)
    # A GUI-subsystem exe (-mwindows): no console window opens behind msga.
    # MinGW's WinMainCRTStartup still calls main().
    set_target_properties(msga PROPERTIES WIN32_EXECUTABLE ON)
    # msga.exe's icon (IDI_ICON1, which plat loads) and version resource.
    set(MSGA_ICON_PATH "${_gfx}/win/icon.ico")
    configure_file("${_res}/msga.rc.in" "${CMAKE_BINARY_DIR}/msga.rc" @ONLY)
    target_sources(msga PRIVATE "${CMAKE_BINARY_DIR}/msga.rc")
endif()

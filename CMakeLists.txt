cmake_minimum_required(VERSION 3.21)
project(msga LANGUAGES CXX)

include("${CMAKE_SOURCE_DIR}/credentials.cmake" OPTIONAL)
include("${CMAKE_SOURCE_DIR}/version.cmake")
foreach(_var MSGA_CLIENT_ID MSGA_CLIENT_SECRET MSGA_XAPP MSGA_TEAMS_CLIENT_ID
        MSGA_GOOGLE_CLIENT_ID MSGA_GOOGLE_CLIENT_SECRET)
    if(NOT DEFINED ${_var})
        set(${_var} "")
    endif()
endforeach()
string(TIMESTAMP MSGA_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(
    "${CMAKE_SOURCE_DIR}/src/app_credentials.h.in"
    "${CMAKE_BINARY_DIR}/app_credentials.h"
    @ONLY
)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MSGA_CLAZY "Add 'clazy' target for Qt-aware static analysis" OFF)
option(MSGA_ASAN "Build with AddressSanitizer + LeakSanitizer" OFF)

# 6.5 is a hard floor: QtWebSockets only gained QWebSocket::errorOccurred there
# (QAbstractSocket got it much earlier — the two are unrelated modules), and the
# realtime sockets rely on it. Without the version here the build fails much
# later with three unexplained "not a member of QWebSocket" errors.
set(MSGA_QT_MIN 6.5)
set(MSGA_QT_COMPONENTS Core Gui Widgets Network WebSockets Svg Concurrent Test)

# The scripts pass -DCMAKE_PREFIX_PATH, but honour $QT_PREFIX here too so a bare
# `cmake -B build -S .` picks the same Qt as `QT_PREFIX=... ./scripts/build.sh`.
if(NOT CMAKE_PREFIX_PATH AND DEFINED ENV{QT_PREFIX})
    set(CMAKE_PREFIX_PATH "$ENV{QT_PREFIX}")
endif()

# Look it up quietly first: CMake's own failure for a missing component is
# "package configuration file ... set Qt6_FOUND to FALSE", which tells a first-
# time builder nothing. Diagnose the two real causes instead — a Qt below the
# floor, and a Qt missing WebSockets (neither qt6-base-dev nor the online
# installer's default selection includes it).
find_package(Qt6 ${MSGA_QT_MIN} QUIET COMPONENTS ${MSGA_QT_COMPONENTS})
if(NOT Qt6_FOUND)
    set(_msga_qt_hint "\
Install Qt ${MSGA_QT_MIN}+ with the WebSockets module:
  Debian/Ubuntu: sudo apt install qt6-base-dev qt6-websockets-dev qt6-svg-dev
  macOS:         brew install qt
  Any distro:    https://www.qt.io/download-qt-installer \
— tick 'Qt WebSockets', it is NOT selected by default
Then point the build at it: QT_PREFIX=$ENV{HOME}/Qt/<version>/gcc_64 ./scripts/build.sh
(scripts/configure-linux.sh checks all of this before you build.)")

    # Retry without the version constraint to learn what is actually installed.
    find_package(Qt6 QUIET COMPONENTS Core)
    if(Qt6_FOUND AND Qt6_VERSION VERSION_LESS ${MSGA_QT_MIN})
        message(FATAL_ERROR
            "Qt ${Qt6_VERSION} (${Qt6_DIR}) is too old — msga needs Qt ${MSGA_QT_MIN}+ for "
            "QWebSocket::errorOccurred.\n${_msga_qt_hint}")
    endif()

    set(_msga_qt_missing "")
    foreach(_comp IN LISTS MSGA_QT_COMPONENTS)
        find_package(Qt6${_comp} QUIET)
        if(NOT Qt6${_comp}_FOUND)
            list(APPEND _msga_qt_missing ${_comp})
        endif()
    endforeach()
    if(_msga_qt_missing)
        list(JOIN _msga_qt_missing ", " _msga_qt_missing)
        message(FATAL_ERROR
            "Qt 6 is installed but these modules are missing: ${_msga_qt_missing}.\n${_msga_qt_hint}")
    endif()

    message(FATAL_ERROR "Qt ${MSGA_QT_MIN}+ not found.\n${_msga_qt_hint}")
endif()
message(STATUS "Qt ${Qt6_VERSION} (${Qt6_DIR})")
find_package(Qt6 OPTIONAL_COMPONENTS LinguistTools)

include(FetchContent)

FetchContent_Declare(
    microsoft_gsl
    GIT_REPOSITORY https://github.com/microsoft/GSL.git
    GIT_TAG        v4.0.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(microsoft_gsl)

# lib_rpl / lib_base are consumed as raw sources (their headers are compiled
# directly into the msga target), NOT as subprojects. Point SOURCE_SUBDIR at a
# directory with no CMakeLists.txt so FetchContent_MakeAvailable() only
# populates the sources and never calls add_subdirectory() on their own build
# trees. This replaces the deprecated single-argument FetchContent_Populate().
#
# Pinned to exact commits (NOT a moving branch) so a build is reproducible and
# a compromised/rewritten upstream master can't silently inject code into our
# binary. To intentionally upgrade: bump the SHA below to a reviewed commit.
# (GIT_SHALLOW is off for a pinned SHA — a shallow fetch of an arbitrary commit
# is not reliably supported across Git hosts.)
FetchContent_Declare(
    lib_rpl
    GIT_REPOSITORY https://github.com/desktop-app/lib_rpl.git
    GIT_TAG        c57cccffb01d85570decd7fccb88419c9a682e63 # master @ 2026-07-20
    SOURCE_SUBDIR  _msga_no_configure
)
FetchContent_MakeAvailable(lib_rpl)

FetchContent_Declare(
    lib_base
    GIT_REPOSITORY https://github.com/desktop-app/lib_base.git
    GIT_TAG        82d182a275e197fd717fecc86193d9d91f4fc5b5 # master @ 2026-07-20
    SOURCE_SUBDIR  _msga_no_configure
)
FetchContent_MakeAvailable(lib_base)

add_executable(msga MACOSX_BUNDLE WIN32
    src/main.cpp
    src/base_stubs.cpp

    # App
    src/app/crash_handler.cpp
    src/app/single_instance.cpp

    # Network (transport mechanics, service-agnostic)
    src/network/http_queue.cpp

    # Auth (neutral strategy interface + factory; per-service flows live below the seam)
    src/auth/token_store.cpp
    src/auth/auth_strategy.h # header-only QObject — listed so AUTOMOC picks it up
    src/auth/auth_strategy_factory.cpp

    # LLM service (Settings → AI assistance)
    src/llm/llm_provider.h # header-only QObject — listed so AUTOMOC picks it up
    src/llm/llm_token_store.cpp
    src/llm/oauth_loopback.cpp
    src/llm/llm_provider_base.cpp
    src/llm/anthropic_provider.cpp
    src/llm/openai_provider.cpp
    src/llm/llm_service.cpp
    src/llm/discussion_summary.cpp

    # Backend port + factory (neutral)
    src/backend/backend_factory.cpp

    # Slack backend adapter (below the Backend seam)
    src/backend/slack/public_backend.cpp
    src/backend/slack/json_mappers.cpp
    src/backend/slack/socket_mode_realtime.cpp
    src/backend/slack/session_realtime.cpp
    src/backend/slack/slack_events.cpp
    src/backend/slack/web_api_client.cpp
    src/backend/slack/shared_realtime.cpp
    src/backend/slack/slack_auth.cpp
    src/backend/slack/oauth_flow.cpp
    src/backend/slack/session_import/browser_login.cpp
    src/backend/slack/session_import/token_deriver.cpp
    src/backend/slack/session_import/local_importer.cpp
    src/backend/slack/session_import/session_migrator.cpp

    # Microsoft Teams backend adapter (below the Backend seam)
    src/backend/teams/teams_backend.cpp
    src/backend/teams/graph_client.cpp
    src/backend/teams/json_mappers.cpp
    src/backend/teams/teams_auth.cpp
    src/backend/teams/oauth_flow.cpp

    # Email (IMAP/SMTP) backend adapter (below the Backend seam)
    src/backend/imap/mime_parser.cpp
    src/backend/imap/imap_protocol.cpp
    src/backend/imap/imap_client.cpp
    src/backend/imap/imap_mappers.cpp
    src/backend/imap/imap_bucketing.cpp
    src/backend/imap/imap_auth.cpp
    src/backend/imap/imap_auth_strategy.cpp
    src/backend/imap/imap_providers.cpp
    src/backend/imap/imap_server_detect.cpp
    src/backend/imap/imap_html.cpp
    src/backend/imap/imap_bimi.cpp
    src/backend/imap/imap_favicon.cpp
    src/backend/imap/imap_compose.cpp
    src/backend/imap/imap_smtp.cpp
    src/backend/imap/imap_backend.cpp

    # Text
    src/text/mrkdwn_parser.cpp

    # Utilities
    src/util/background_tasks.cpp
    src/util/desktop_integration.cpp
    # Listed so AUTOMOC processes the Q_OBJECT (the impl TUs are platform-suffixed
    # — desktop_notifier_{linux,win,mac} — so the basename doesn't match a source).
    src/util/desktop_notifier.h
    src/util/emoji.cpp
    src/util/emoji_catalog.cpp
    src/util/emoji_data.cpp
    src/util/emoji_pixmap.cpp
    src/util/mailto_link.cpp
    src/util/relative_time.cpp
    src/util/sound_player.cpp
    src/util/time_format.cpp

    # Cache
    src/cache/workspace_cache.cpp
    src/cache/cache_evictor.cpp

    # Session / domain store
    src/session/session.cpp

    # UI
    src/ui/theme.cpp
    src/ui/theme_manager.cpp
    src/ui/file_dialog_utils.cpp
    src/ui/virtual_list/virtual_list_widget.cpp
    src/ui/image_cache.cpp
    src/ui/main_window.cpp
    src/ui/title_bar/title_bar.cpp
    src/ui/loading_indicator/loading_indicator.cpp
    src/ui/message_list/message_list.cpp
    src/ui/message_list/message_list_paint.cpp
    src/ui/message_list/message_render.cpp
    src/ui/message_list/file_chip_widget.cpp
    src/ui/image_viewer/image_viewer.cpp
    src/ui/table_viewer/table_viewer.cpp
    src/ui/conv_list/conv_list_widget.cpp
    src/ui/conv_footer/conv_footer_widget.cpp
    src/ui/composer/formatting_toolbar.cpp
    src/ui/composer/attachment_strip.cpp
    src/ui/composer/edit_mode_banner.cpp
    src/ui/composer/composer_widget.cpp
    src/ui/emoji_picker/emoji_picker_popup.cpp
    src/ui/composer/mention_completer.cpp
    src/ui/typing_indicator/typing_indicator.cpp
    src/ui/mention_popup/mention_popup.cpp
    src/ui/context_menu/context_menu.cpp
    src/ui/popup_tooltip/popup_tooltip.cpp
    src/ui/user_profile_card/user_profile_card.cpp
    src/ui/workspace_switcher/workspace_switcher.cpp
    src/ui/settings/settings_dialog.cpp
    src/ui/settings/theme_preview_card.cpp
    src/ui/search/search_widget.cpp
    src/ui/thread_panel/thread_panel.cpp
    src/ui/threads_page/threads_page.cpp
    src/ui/saved_page/saved_messages_page.cpp
    src/ui/canvas_page/canvas_page.cpp
    src/ui/canvas_page/canvas_diff.cpp
    src/ui/canvas_page/canvas_emoji.cpp
    src/ui/conv_tabs/conv_tabs_widget.cpp
    src/ui/welcome_tips/welcome_widget.cpp
    src/ui/app_dialog/app_dialog.cpp
    src/ui/delete_message_dialog/delete_message_dialog.cpp
    src/ui/reminder_dialog/reminder_dialog.cpp
    src/ui/summary_dialog/summary_dialog.cpp
    src/ui/summary_dialog/summarize_job.cpp
    src/ui/conv_selector/conv_selector_widget.cpp
    src/ui/forward_dialog/forward_dialog.cpp
    src/ui/styled_line_edit/styled_line_edit.cpp
    src/ui/styled_button/styled_button.cpp
    src/ui/icon_button/icon_button.cpp
    src/ui/dropdown/dropdown.cpp
    src/ui/create_channel_dialog/create_channel_dialog.cpp
    src/ui/imap_add_account/imap_add_account_dialog.cpp
    src/ui/session_import_dialog/session_import_dialog.cpp
    src/ui/profile_dialog/profile_dialog.cpp
    src/ui/status_dialog/status_dialog.cpp
    src/ui/browse_channels_dialog/browse_channels_dialog.cpp
    src/ui/browse_channels_dialog/browse_list_view.cpp
    src/ui/update_checker/update_checker.cpp
    src/ui/update_bar/update_bar.cpp
    src/ui/huddle_banner/huddle_banner.cpp
    src/ui/parallel_usage_banner/parallel_usage_banner.cpp

    # Qt resources
    resources/resources.qrc
)

target_precompile_headers(msga PRIVATE src/pch.h)

target_include_directories(msga PRIVATE
    src
    ${CMAKE_BINARY_DIR}
)

# Third-party sources we vendor via FetchContent. Mark SYSTEM so the compiler
# doesn't emit (and we don't try to fix) warnings from headers we don't own —
# e.g. lib_rpl's rpl/variable.h trips C++20's deprecated implicit [=] capture
# of 'this'. Keeps our own -Wdeprecated output clean and actionable.
target_include_directories(msga SYSTEM PRIVATE
    ${lib_rpl_SOURCE_DIR}
    ${lib_base_SOURCE_DIR}
)

target_link_libraries(msga PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    Qt6::Svg
    Qt6::Concurrent
    Microsoft.GSL::GSL
)

# macOS app-bundle metadata. CMake's default Info.plist gives the bundle no
# identifier and — critically — no CFBundleURLTypes, so macOS would not know
# msga.app handles the msga:// scheme that the OAuth redirect comes back on
# (the browser then reports "unknown protocol" and sign-in can't complete).
# Supply our own template (resources/Info.plist.in) that declares the scheme.
#
# CMake writes MACOSX_BUNDLE_INFO_PLIST into the .app only at CONFIGURE time —
# there is no build rule for it. So a plain `cmake --build` after the bundle is
# recreated (which is every scripts/build.sh run: it reuses the configured dir
# and only relinks) leaves the app with NO Info.plist, hence no
# CFBundleIdentifier. That silently breaks two things: codesign falls back to an
# unstable per-binary ad-hoc identifier (msga-<hash>, different each build), and
# UNUserNotificationCenter then refuses to register the app — no notification
# banners, no entry under System Settings ▸ Notifications. To make it robust we
# generate the plist to a stable build-tree path and re-install + ad-hoc sign it
# on EVERY build via POST_BUILD, so the identity is always com.nisdos.msga.
#
# Everything here must keep matching the plist the release build writes over it
# (bundle id, CFBundleIconFile, LSMinimumSystemVersion): two bundles claiming
# msga:// under DIFFERENT ids means LaunchServices can route an OAuth redirect to
# a stale dev build instead of the installed app.
if(APPLE)
    set(MACOSX_BUNDLE_EXECUTABLE_NAME      "msga")
    set(MACOSX_BUNDLE_BUNDLE_NAME          "msga")
    set(MACOSX_BUNDLE_DISPLAY_NAME         "msga")
    set(MACOSX_BUNDLE_INFO_STRING          "msga")
    # App icon: gfx/mac/icon.icns, generated from gfx/icon.svg. Installed as
    # Resources/AppIcon.icns — the name the release build's own icns uses, so a
    # release bundle ends up with one icon file, not two with one unreferenced.
    set(MACOSX_BUNDLE_ICON_FILE            "AppIcon")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER       "com.nisdos.msga")
    set(MACOSX_BUNDLE_BUNDLE_VERSION       "${MSGA_VERSION}")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${MSGA_VERSION}")
    set(MACOSX_BUNDLE_LONG_VERSION_STRING  "${MSGA_VERSION}")
    # Follow the deployment target when one is passed (the release build sets
    # it); otherwise state the version releases are built against.
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(MSGA_MACOS_MIN_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(MSGA_MACOS_MIN_VERSION "14.0")
    endif()

    # Fully substitute the template now (the template uses ${VAR} placeholders,
    # so no @ONLY) into a stable location outside the bundle.
    set(MSGA_INFO_PLIST "${CMAKE_BINARY_DIR}/msga_Info.plist")
    configure_file("${CMAKE_SOURCE_DIR}/resources/Info.plist.in" "${MSGA_INFO_PLIST}")
    set_target_properties(msga PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${MSGA_INFO_PLIST}")

    # App icon. macOS 26 (Tahoe) wants an Icon Composer ".icon", which actool
    # compiles into an asset catalog (Assets.car) carrying the Liquid Glass
    # rendition; the same run also emits a legacy AppIcon.icns for older macOS,
    # so one source covers every version. That input needs Xcode 26's actool: a
    # Command Line Tools install ships none at all, and an older Xcode's actool
    # rejects ".icon". Anything else falls back to copying the static
    # gfx/mac/icon.icns. Failing soft is the point — the icon step runs
    # POST_BUILD, so a wrong guess here would break every build, not the icon.
    #
    # Locate first (xcrun follows the selected developer dir; a full Xcode that
    # xcode-select doesn't point at is the backup), then prove the binary can
    # actually compile our .icon.
    execute_process(COMMAND xcrun --find actool
        OUTPUT_VARIABLE MSGA_ACTOOL_PATH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT EXISTS "${MSGA_ACTOOL_PATH}")
        # Own result variable: find_program is a silent no-op while a normal
        # variable of its result name exists — and execute_process above defines
        # MSGA_ACTOOL_PATH even when xcrun fails, as the empty string.
        find_program(MSGA_ACTOOL_PROGRAM actool
            HINTS /Applications/Xcode.app/Contents/Developer/usr/bin)
        set(MSGA_ACTOOL_PATH "${MSGA_ACTOOL_PROGRAM}")
    endif()

    set(MSGA_ACTOOL "")
    if(EXISTS "${MSGA_ACTOOL_PATH}")
        # Capability probe rather than a version comparison: run the exact
        # command the build will run, into a throwaway directory. An actool that
        # doesn't understand ".icon" exits non-zero here instead of at build time.
        execute_process(
            COMMAND "${MSGA_ACTOOL_PATH}" "${CMAKE_SOURCE_DIR}/gfx/mac/AppIcon.icon"
                    --app-icon "${MACOSX_BUNDLE_ICON_FILE}"
                    --compile "${CMAKE_BINARY_DIR}/actool_probe"
                    --platform macosx
                    --minimum-deployment-target "${MSGA_MACOS_MIN_VERSION}"
                    --output-partial-info-plist "${CMAKE_BINARY_DIR}/actool_probe.plist"
            RESULT_VARIABLE MSGA_ACTOOL_PROBE OUTPUT_QUIET ERROR_QUIET)
        file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/actool_probe"
                            "${CMAKE_BINARY_DIR}/actool_probe.plist")
        if(MSGA_ACTOOL_PROBE EQUAL 0)
            set(MSGA_ACTOOL "${MSGA_ACTOOL_PATH}")
        else()
            message(STATUS "msga: ${MSGA_ACTOOL_PATH} cannot compile .icon "
                           "(needs Xcode 26+) — rc=${MSGA_ACTOOL_PROBE}")
        endif()
    endif()
    if(MSGA_ACTOOL)
        message(STATUS "msga: app icon via Icon Composer (.icon) → ${MSGA_ACTOOL}")
        set(MSGA_ICON_STEP
            COMMAND "${MSGA_ACTOOL}" "${CMAKE_SOURCE_DIR}/gfx/mac/AppIcon.icon"
                    --app-icon "${MACOSX_BUNDLE_ICON_FILE}"
                    --compile "$<TARGET_BUNDLE_CONTENT_DIR:msga>/Resources"
                    --platform macosx
                    --minimum-deployment-target "${MSGA_MACOS_MIN_VERSION}"
                    --output-partial-info-plist "${CMAKE_BINARY_DIR}/actool_partial.plist")
    else()
        message(STATUS "msga: no Icon Composer-capable actool — app icon via static icon.icns")
        set(MSGA_ICON_STEP
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/gfx/mac/icon.icns"
                    "$<TARGET_BUNDLE_CONTENT_DIR:msga>/Resources/${MACOSX_BUNDLE_ICON_FILE}.icns")
    endif()

    # Guarantee the bundle has its Info.plist + app icon and a stable signature
    # after every build — including a bare `cmake --build` that never re-runs
    # configure. (The icon uses the same every-build install as the plist: a
    # MACOSX_PACKAGE_LOCATION resource would not be re-copied into a bundle
    # that was recreated without its sources changing.)
    add_custom_command(TARGET msga POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${MSGA_INFO_PLIST}" "$<TARGET_BUNDLE_CONTENT_DIR:msga>/Info.plist"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:msga>/Resources"
        ${MSGA_ICON_STEP}
        COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:msga>"
        VERBATIM
        COMMENT "Installing Info.plist, app icon and ad-hoc signing msga.app")
endif()

# Crash reports (src/app/crash_handler.cpp): -rdynamic keeps function names
# visible to backtrace_symbols on Linux; dbghelp is the Windows symbol engine.
# A fully static binary (release-linux-static.sh passes -static) has no
# dynamic symbol table, so -rdynamic is pointless there — frames resolve
# offline via addr2line instead.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_EXE_LINKER_FLAGS MATCHES "(^| )-static( |$)")
    set_property(TARGET msga PROPERTY ENABLE_EXPORTS ON)
endif()

# Wayland platform plugin import (static builds).
#
# On a Wayland desktop, a binary with only the xcb platform plugin runs through
# XWayland, where a fractionally-scaled output (125%/150%) is rendered at an
# integer scale and the compositor upscales the whole window — everything looks
# blurry. The Wayland client plugin makes msga a native Wayland client so the
# scaling logic in src/main.cpp takes effect and text stays crisp.
#
# For a STATIC Qt, a plugin is only usable if it is Q_IMPORT_PLUGIN'd into the
# binary. Qt's automatic import covers plugins of the modules we link: linking
# Qt6::Gui pulls in the xcb platform plugin (it lives in qtbase). The Wayland
# platform plugin and its shell-integration/decoration plugins are registered by
# the separate Qt6::WaylandClient module, which we don't otherwise link — so they
# must be found and imported explicitly. The platform plugin alone cannot create
# windows: it needs a shell integration (xdg-shell on modern compositors), so we
# import the whole set that exists. No-op on a dynamic build (plugins load from
# disk at runtime) and when qtwayland isn't present (OPTIONAL_COMPONENTS).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 QUIET OPTIONAL_COMPONENTS WaylandClient)
    if(Qt6WaylandClient_FOUND)
        set(_msga_wayland_plugins "")
        foreach(_p
                QWaylandIntegrationPlugin           # the "wayland" platform plugin
                QWaylandXdgShellIntegrationPlugin   # shell integration (modern compositors)
                QWaylandWlShellIntegrationPlugin    # shell integration (legacy wl_shell)
                QWaylandBradientDecorationPlugin)   # client-side window decorations
            if(TARGET Qt6::${_p})
                list(APPEND _msga_wayland_plugins Qt6::${_p})
            endif()
        endforeach()
        if(_msga_wayland_plugins)
            qt6_import_plugins(msga INCLUDE ${_msga_wayland_plugins})
            message(STATUS "msga: importing Wayland plugins: ${_msga_wayland_plugins}")
        endif()
    endif()
endif()
if(WIN32)
    target_compile_definitions(msga PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(msga PRIVATE dbghelp)

    # Windows application icon + version metadata. windres compiles the .rc into
    # the PE resource section so Explorer/taskbar/alt-tab show the app icon and
    # the shell/browser show the app name (FileDescription). The .ico path is
    # baked in via configure_file (forward slashes — windres treats a backslash
    # as an escape). The icon is resource id 1 (lowest), which is what the shell
    # picks as the executable icon. The .rc also carries the VERSIONINFO block;
    # release-windows.sh strips winpthread's competing VERSION resource from the
    # static archive first, so windres' .rsrc merge has no duplicate (see
    # msga.rc.in). @MSGA_VERSION@ comes from version.cmake (included above).
    set(MSGA_ICON_PATH "${CMAKE_SOURCE_DIR}/gfx/win/icon.ico")
    configure_file(
        "${CMAKE_SOURCE_DIR}/resources/msga.rc.in"
        "${CMAKE_BINARY_DIR}/msga.rc"
        @ONLY
    )
    target_sources(msga PRIVATE "${CMAKE_BINARY_DIR}/msga.rc")
endif()

# Notification sound backend (src/util/sound_player*): platform-native audio,
# no Qt Multimedia, so the static Linux binary keeps zero shared-lib deps.
if(APPLE)
    enable_language(OBJCXX)
    target_sources(msga PRIVATE src/util/sound_player_mac.mm)
    set_source_files_properties(src/util/sound_player_mac.mm
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(msga PRIVATE "-framework AppKit" "-framework Foundation")
elseif(WIN32)
    target_sources(msga PRIVATE src/util/sound_player_win.cpp)
    target_link_libraries(msga PRIVATE winmm)
else()
    target_sources(msga PRIVATE src/util/sound_player_linux.cpp)
endif()

# Secret storage (src/util/secret_store*): credentials (tokens, API keys, the
# personal Slack app secret, IMAP passwords) live in the OS keychain where one
# exists, never in the plaintext QSettings/plist. macOS uses the Security
# framework (generic-password items); other platforms fall back to QSettings
# (documented limitation — see src/util/secret_store.h). secret_store.cpp holds
# the platform-neutral migration helper and is always compiled.
target_sources(msga PRIVATE src/util/secret_store.cpp)
if(APPLE)
    target_sources(msga PRIVATE src/util/secret_store_mac.mm)
    set_source_files_properties(src/util/secret_store_mac.mm
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(msga PRIVATE "-framework Security" "-framework CoreFoundation")
else()
    target_sources(msga PRIVATE src/util/secret_store_qsettings.cpp)
endif()

# Desktop notifications with per-message avatars (src/util/desktop_notifier*):
# each OS needs a different API to put the sender avatar into the notification —
# Qt's QSystemTrayIcon::showMessage can't on any platform. One backend per TU,
# shared header; each degrades to the tray fallback when its API is unavailable.
#   • Linux  — org.freedesktop.Notifications `image-data` hint via QtDBus
#   • Windows — WinRT toast `appLogoOverride` (raw COM) + AUMID/shortcut/scheme
#   • macOS  — UNUserNotificationCenter banner + attachment (UserNotifications);
#             the same TU also sets the Dock tile badge (mac_app_badge.h)
if(APPLE)
    target_sources(msga PRIVATE src/util/desktop_notifier_mac.mm)
    set_source_files_properties(src/util/desktop_notifier_mac.mm
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(msga PRIVATE "-framework UserNotifications")
elseif(WIN32)
    target_sources(msga PRIVATE src/util/desktop_notifier_win.cpp)
    # runtimeobject: WinRT (RoInitialize/RoGetActivationFactory/HSTRING).
    # shell32/ole32: Start-Menu shortcut (IShellLink) + COM. uuid: the CLSID_*
    # / FOLDERID_* GUID symbols. propsys: IPropertyStore plumbing.
    target_link_libraries(msga PRIVATE runtimeobject propsys shell32 ole32 uuid)
else()
    target_sources(msga PRIVATE src/util/desktop_notifier_linux.cpp)
    find_package(Qt6 QUIET OPTIONAL_COMPONENTS DBus)
    if(Qt6DBus_FOUND)
        target_link_libraries(msga PRIVATE Qt6::DBus)
        target_compile_definitions(msga PRIVATE MSGA_HAS_DBUS)
    endif()
endif()

# Optional one-click Slack session import (Settings → System → "Import Slack
# session"): reads and decrypts the locally-installed Slack desktop app's cookie
# store. Needs OpenSSL (AES/PBKDF2) and Qt6::Sql (the Chromium cookie SQLite);
# the keyring lookup additionally uses Qt6::DBus when present. Linux only for now.
# Without the deps the importer compiles to a graceful stub and the UI offers the
# guided manual-paste fallback — so the build is green everywhere regardless.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(OpenSSL QUIET COMPONENTS Crypto)
    find_package(Qt6 QUIET OPTIONAL_COMPONENTS Sql)
    if(OpenSSL_FOUND AND Qt6Sql_FOUND)
        target_link_libraries(msga PRIVATE OpenSSL::Crypto Qt6::Sql)
        target_compile_definitions(msga PRIVATE MSGA_SLACK_SESSION_IMPORT)
        message(STATUS "Slack session import: enabled (OpenSSL + Qt6::Sql)")
    else()
        message(STATUS "Slack session import: disabled — install libssl-dev + Qt6 Sql "
                       "to enable one-click import (guided manual paste still works)")
    endif()
endif()

# AddressSanitizer + LeakSanitizer (MSGA_ASAN=ON). Fast (~2x) runtime detection
# of heap errors (use-after-free, overflows) and, via the bundled LeakSanitizer,
# memory leaks reported on exit. -fno-omit-frame-pointer keeps traces readable;
# pair with a Debug build (-g) so frames resolve to file:line. Use scripts/build.sh
# --asan, which builds into build-asan/ to keep instrumented objects separate.
if(MSGA_ASAN)
    target_compile_options(msga PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(msga PRIVATE -fsanitize=address)
endif()

# Main-thread hang watchdog (src/app/crash_handler.cpp + the heartbeat in
# src/main.cpp). DEV-ONLY: compiled into the Debug builds produced by
# scripts/build.sh and scripts/run-asan.sh, and never into the shipped packages
# (every release-*.sh configures -DCMAKE_BUILD_TYPE=Release). It dumps the stuck
# main thread's backtrace when the GUI event loop stalls — a debugging aid, not
# something to ship. Linux-only: it relies on POSIX per-process timers
# (timer_create) targeted at a thread via Linux's SIGEV_THREAD_ID, which macOS
# does not provide.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (CMAKE_BUILD_TYPE STREQUAL "Debug" OR MSGA_ASAN))
    target_compile_definitions(msga PRIVATE MSGA_HANG_WATCHDOG)
    # SIGEV_THREAD_ID + the sigev_notify_thread_id member are exposed only under
    # _GNU_SOURCE, which crash_handler.cpp #defines before any include. The
    # force-included PCH (-include cmake_pch.hxx) would pull in <signal.h> ahead
    # of that define and defeat it, so skip the PCH for just this one file —
    # keeping _GNU_SOURCE scoped to this TU instead of the whole target.
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/app/crash_handler.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    # timer_create lives in librt on glibc < 2.34; in libc on newer glibc and on
    # musl. Link librt only where it still exists as a separate library.
    find_library(MSGA_RT_LIB rt)
    if(MSGA_RT_LIB)
        target_link_libraries(msga PRIVATE ${MSGA_RT_LIB})
    endif()
endif()

if(Qt6LinguistTools_FOUND)
    qt_add_translations(msga
        TS_FILES translations/msga_en.ts translations/msga_ja.ts
        RESOURCE_PREFIX "/translations"
    )
endif()

option(MSGA_BUILD_TESTS "Build unit tests" OFF)
if(MSGA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MSGA_CLAZY)
    find_program(CLAZY_STANDALONE NAMES clazy-standalone REQUIRED)
    file(GLOB_RECURSE _msga_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
    add_custom_target(clazy
        COMMAND ${CLAZY_STANDALONE}
            -checks=level1
            -p ${CMAKE_BINARY_DIR}
            ${_msga_sources}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clazy (level1)..."
        VERBATIM
    )
endif()

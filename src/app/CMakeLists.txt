# msga_app_core — the data side of the app: model (Store, Backend) and
# mrkdwn → rich text. No UI code here: the screens and the msga executable
# link this library.
add_library(msga_app_core STATIC
    model/store.cpp
    model/store_slot.cpp
    model/image_size.cpp
    model/backend.cpp
    model/backend_proxy.cpp
    model/jobs.cpp
    model/timers.cpp
    cache/workspace_cache.cpp
    auth/workspaces.cpp
    identity.cpp
    mrkdwn/mrkdwn.cpp
    mrkdwn/markdown.cpp
    mrkdwn/link_labels.cpp
    mrkdwn/emoji.cpp
    i18n/languages.cpp
    i18n/languages_generated.cpp
    crash/crash_handler.cpp
    diag/mem_stats.cpp
    media/sounds.cpp
    media/audio_errors.cpp
    media/audio_player.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/sfx_notify.cpp
)
# The notification chime (sfx/notify.wav) compiled in as a byte array.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/sfx_notify.cpp
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/../../sfx/notify.wav
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/sfx_notify.cpp -DNAMESPACE=sounds::data
            -DNAME=kNotifyWav -P ${CMAKE_CURRENT_SOURCE_DIR}/../tools/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../sfx/notify.wav ${CMAKE_CURRENT_SOURCE_DIR}/../tools/embed.cmake
    VERBATIM)
# Crash reports (crash/crash_handler.cpp) carry the version and build time.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../version.cmake)
string(TIMESTAMP MSGA_CRASH_BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S" UTC)
set_property(SOURCE crash/crash_handler.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    MSGA_VERSION=${MSGA_VERSION} "MSGA_BUILD_TIMESTAMP=\"${MSGA_CRASH_BUILD_TIMESTAMP}\"")
if(WIN32)
    target_link_libraries(msga_app_core PUBLIC dbghelp) # the crash handler's symbol engine
endif()
# Main-thread hang watchdog (crash/crash_handler.cpp + the heartbeat in
# screens/shell/main.cpp). DEV-ONLY: Debug builds (scripts/build.sh --debug,
# the test trees), never the release (MinSizeRel). Linux-only: it relies on
# POSIX per-process timers aimed at a thread via Linux's SIGEV_THREAD_ID.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(SOURCE crash/crash_handler.cpp APPEND PROPERTY COMPILE_DEFINITIONS MSGA_HANG_WATCHDOG)
    # timer_create lives in librt on glibc < 2.34; in libc on newer glibc and
    # on musl. Link librt only where it still exists as a separate library.
    find_library(MSGA_RT_LIB rt)
    if(MSGA_RT_LIB)
        target_link_libraries(msga_app_core PUBLIC ${MSGA_RT_LIB})
    endif()
endif()
# Both "app/model/…" and "model/…" resolve (screens use the short form).
target_include_directories(msga_app_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(msga_app_core PUBLIC msga_base plat msga_flags PRIVATE msga_stb)

# msga_app_slack — the Slack service: sign-in (session cookie via a driven
# browser or a paste, OAuth app keys), the Web API, the workspace backend.
add_library(msga_app_slack STATIC
    slack/credentials.cpp
    slack/web_api.cpp
    slack/session.cpp
    slack/browser_login.cpp
    slack/oauth.cpp
    slack/slack_backend.cpp
    slack/slack_backend_core.cpp
    slack/slack_actions.cpp
    slack/slack_activity.cpp
    slack/slack_realtime.cpp
    slack/slack_json.cpp
    slack/socket_mode.cpp
    slack/rtm_presence.cpp
)
target_link_libraries(msga_app_slack PUBLIC msga_app_core msga_net msga_flags)

# msga_app_update — the in-app updater (msga.app manifests, the download, the
# binary swap).
if(MSGA_SELF_UPDATE)
    add_library(msga_app_update STATIC update/updater.cpp)
    target_link_libraries(msga_app_update PUBLIC msga_app_core msga_net msga_flags)
endif()

# msga_app_claude — the Claude Code workspace (claude/CMakeLists.txt).
add_subdirectory(claude)

# msga_app_llm — AI providers and what is asked of them (llm/CMakeLists.txt).
add_subdirectory(llm)

# msga_app_spell — composer spell checking (spell/CMakeLists.txt).
add_subdirectory(spell)

# "Import from local Slack": the real importer on Linux (our own SQLite reader
# + AES-CBC/PBKDF2 over net/base, Secret Service over libdbus), the stub
# elsewhere. libdbus is already found by plat; reuse it for the v11 key.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(msga_app_slack PRIVATE
        slack/local_import_linux.cpp
        slack/sqlite_reader.cpp)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(APP_DBUS IMPORTED_TARGET dbus-1)
    endif()
    if(APP_DBUS_FOUND)
        target_compile_definitions(msga_app_slack PRIVATE MSGA_HAS_DBUS=1)
        target_link_libraries(msga_app_slack PRIVATE PkgConfig::APP_DBUS)
    endif()
else()
    target_sources(msga_app_slack PRIVATE slack/local_import_none.cpp)
endif()
# The build's own Slack app keys (credentials.cmake, never committed; see
# credentials.cmake.example). Only this file sees them.
set_source_files_properties(slack/credentials.cpp PROPERTIES COMPILE_DEFINITIONS
    "MSGA_CLIENT_ID=\"${MSGA_CLIENT_ID}\";MSGA_CLIENT_SECRET=\"${MSGA_CLIENT_SECRET}\";MSGA_XAPP=\"${MSGA_XAPP}\"")

# Translations: i18n/<code>.po -> i18n/languages_generated.cpp (checked in, so
# normal builds need no Python). After changing UI strings or a .po:
#   cmake --build <dir> --target translations      (== tools/i18n.py update)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_target(translations
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../tools/i18n.py update
        COMMENT "Updating the .po files and the compiled-in translation tables"
        VERBATIM)
endif()

# msga_app_fake — the fake backend on the demo fixture. Opt-in (see
# MSGA_DEMO / MSGA_BUILD_TESTS in ../../CMakeLists.txt): the demo and the tests
# link it, a plain msga never contains it.
if(MSGA_DEMO OR MSGA_BUILD_TESTS)
    add_library(msga_app_fake STATIC
        fake/fixture.cpp
        fake/fake_backend.cpp
    )
    target_link_libraries(msga_app_fake PUBLIC msga_app_core)
endif()

# ── Screens and the msga executable ────────────────────────────────────────
# Each screen group owns its CMakeLists: messages → lib msga_screens_messages,
# settings → lib msga_screens_settings (the Settings dialog),
# shell → lib msga_screens_shell + the msga executable (main.cpp).
# MSGA_WITH_MESSAGES=OFF builds the shell with its stand-in message list
# (while the messages screens are being written, or to measure the shell alone).
option(MSGA_WITH_MESSAGES "Build and embed the messages screens" ON)
foreach(group common messages settings shell)
    if(group STREQUAL "messages" AND NOT MSGA_WITH_MESSAGES)
        continue()
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/screens/${group}/CMakeLists.txt")
        add_subdirectory(screens/${group})
    endif()
endforeach()
# ────────────────────────────────────────────────────────────────────────────

# The upgrade from earlier versions' data (legacy/legacy.h): delete this block and the
# directory to drop it.
option(MSGA_LEGACY_IMPORT "Import the settings and caches of earlier versions" ON)
if(MSGA_LEGACY_IMPORT)
    add_subdirectory(legacy)
endif()

# A size probe: an executable that pulls in every public entry point of
# msga_base + msga_app_core, so the linker map shows what the libraries cost
# once dead code is stripped (tools/size_report.py <exe>.map). It drives the
# Store through the fake backend, so it exists only where that is built.
if(TARGET msga_app_fake)
    add_executable(msga_core_size_probe probe/size_probe.cpp)
    target_link_libraries(msga_core_size_probe PRIVATE msga_app_fake)
    msga_size_map(msga_core_size_probe)
endif()

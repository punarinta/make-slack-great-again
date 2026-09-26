option(MSGA_COVERAGE "Build tests with coverage instrumentation" OFF)

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

if(MSGA_COVERAGE)
    # Directory-scoped: applies to every test target defined below without
    # touching each add_msga_test helper. Needs both compile + link flags.
    # Placed AFTER Catch2 is fetched so the vendored library is NOT instrumented
    # — its .gcno/.gcda files otherwise produce gcov errors that abort gcovr.
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

set(TEST_INCLUDES
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
)

# Vendored third-party sources — included SYSTEM so their header warnings
# (e.g. lib_rpl's deprecated C++20 implicit [=] capture) stay out of our build
# output. Mirrors the SYSTEM treatment in the top-level CMakeLists.txt.
set(TEST_SYSTEM_INCLUDES
    ${lib_rpl_SOURCE_DIR}
    ${lib_base_SOURCE_DIR}
)

# ── Shared app sources: compiled once, linked into every test ──────────────
# Every app source a test exercises is compiled ONCE into msga_test_support and
# linked into the test executable, instead of being recompiled per test (this
# used to be ~60% of the test build's CPU time: theme/mrkdwn/time_format ~25×,
# session.cpp 11×, the SecretStore pair into all ~80 tests). It is a STATIC
# library, so the linker pulls only the objects some test references. A new app
# source a test needs goes in this list, not in the test's own source list.
#
# Compiled with the test flags, NOT the app's: none of the MSGA_BACKEND_* /
# MSGA_DEMO / MSGA_HANG_WATCHDOG defines (the msga target sets those on
# itself), so no object is ever shared with the app build. Sources that need a
# per-test define or system library stay in their test's own source list:
# crash_handler.cpp (MSGA_HANG_WATCHDOG), the demo backend (private repo only)
# and the sound player (platform backend + its system frameworks).
#
# SecretStore backend for tests: always the QSettings fallback, never the macOS
# keychain, so test runs stay hermetic — no keychain access prompts, no items
# left behind on the developer's login keychain. secret_store_mac.mm is
# deliberately unused here.
#
# Inline audio player: Media::AudioPlayer's platform engine TU is picked the
# same way the top-level CMakeLists does.
if(APPLE)
    set(_audio_engine ${CMAKE_SOURCE_DIR}/src/media/audio_engine_mac.mm)
    set_source_files_properties(${_audio_engine} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
elseif(WIN32)
    set(_audio_engine ${CMAKE_SOURCE_DIR}/src/media/audio_engine_win.cpp)
else()
    set(_audio_engine ${CMAKE_SOURCE_DIR}/src/media/audio_engine_linux.cpp)
endif()

set(MSGA_TEST_SUPPORT_SRCS
    ${CMAKE_SOURCE_DIR}/src/auth/auth_strategy_factory.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/token_store.cpp

    ${CMAKE_SOURCE_DIR}/src/backend/backend_factory.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/backend_registry.cpp

    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/cc_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/cc_outputs.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/cc_attach.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/cc_pty.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/cc_vt.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/cc_launcher.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/cc_roles.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/cc_roster.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/cc_transcript.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/claude_code_auth.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/claude_code/claude_code_backend.cpp

    ${CMAKE_SOURCE_DIR}/src/backend/fake_backend/fake_backend.cpp

    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_auth.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_bimi.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_bucketing.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_client.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_compose.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_favicon.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_html.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_providers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_smtp.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/mime_parser.cpp

    ${CMAKE_SOURCE_DIR}/src/backend/slack/json_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/oauth_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/public_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/rtm_presence.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/session_import/browser_login.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/session_import/local_importer.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/session_import/token_deriver.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/session_realtime.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/shared_realtime.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/slack_auth.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/slack_events.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/slack_module.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/socket_mode_realtime.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/web_api_client.cpp

    ${CMAKE_SOURCE_DIR}/src/backend/teams/graph_client.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/json_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/oauth_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/teams_auth.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/teams_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/teams_module.cpp

    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp

    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp

    ${CMAKE_SOURCE_DIR}/src/llm/audio_transcriber.cpp
    ${CMAKE_SOURCE_DIR}/src/llm/discussion_summary.cpp
    ${CMAKE_SOURCE_DIR}/src/llm/llm_provider.cpp
    ${CMAKE_SOURCE_DIR}/src/llm/llm_service.cpp
    ${CMAKE_SOURCE_DIR}/src/llm/llm_token_store.cpp
    ${CMAKE_SOURCE_DIR}/src/llm/llm_wire.cpp
    ${CMAKE_SOURCE_DIR}/src/llm/oauth_loopback.cpp

    ${CMAKE_SOURCE_DIR}/src/media/audio_player.cpp

    ${CMAKE_SOURCE_DIR}/src/network/gif_search.cpp
    ${CMAKE_SOURCE_DIR}/src/network/http_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/network/oauth_pkce.cpp

    ${CMAKE_SOURCE_DIR}/src/session/session.cpp

    ${CMAKE_SOURCE_DIR}/src/text/link_labels.cpp
    ${CMAKE_SOURCE_DIR}/src/text/markdown_compose.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp

    ${CMAKE_SOURCE_DIR}/src/ui/app_dialog/app_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/browse_channels_dialog/browse_channels_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/browse_channels_dialog/browse_list_view.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/canvas_page/canvas_diff.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/canvas_page/canvas_display.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/canvas_page/canvas_emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/canvas_page/canvas_page.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/canvas_page/canvas_viewer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/attachment_strip.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/composer_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/edit_mode_banner.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/formatting_toolbar.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/mention_completer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/undo_send_pill.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/context_menu/context_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/conv_list/conv_list_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/conv_list/named_conversation.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/conv_selector/conv_selector_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/delete_message_dialog/delete_message_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/emoji_picker/emoji_picker_popup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/file_dialog_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/forward_dialog/forward_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/gif_picker/gif_picker_popup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/icon_button/icon_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/icon_picker_dialog/icon_picker_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/image_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/image_viewer/image_viewer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/loading_indicator/loading_indicator.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/members_popup/members_popup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/mention_popup/mention_popup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/file_chip_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/message_list.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/message_list_paint.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/message_render.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/move_to_thread_dialog/move_to_thread_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/popup_tooltip/popup_tooltip.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/quick_switcher/quick_switcher_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/quick_switcher/workspace_tab_strip.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/reminder_dialog/reminder_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/search/search_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/session_import_dialog/session_import_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/shortcuts.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_button/styled_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_line_edit/styled_line_edit.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/summary_dialog/summarize_job.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/summary_dialog/summary_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/table_viewer/table_viewer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_custom.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/thread_panel/thread_export_job.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/thread_panel/thread_panel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/transcript_dialog/transcript_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/tray_icon_dialog/tray_icon_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/user_profile_card/user_profile_card.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/virtual_list/virtual_list_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/workspace_icon_dialog/workspace_icon_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/workspace_switcher/workspace_bubble.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/workspace_switcher/workspace_switcher.cpp

    ${CMAKE_SOURCE_DIR}/src/util/avatar_glyphs.cpp
    ${CMAKE_SOURCE_DIR}/src/util/background_tasks.cpp
    ${CMAKE_SOURCE_DIR}/src/util/custom_tray_icon.cpp
    ${CMAKE_SOURCE_DIR}/src/util/custom_workspace_icon.cpp
    ${CMAKE_SOURCE_DIR}/src/util/desktop_integration.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji_data.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji_pixmap.cpp
    ${CMAKE_SOURCE_DIR}/src/util/fuzzy_match.cpp
    ${CMAKE_SOURCE_DIR}/src/util/mailto_link.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/util/secret_store.cpp
    ${CMAKE_SOURCE_DIR}/src/util/secret_store_qsettings.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${_audio_engine}
)

add_library(msga_test_support STATIC ${MSGA_TEST_SUPPORT_SRCS})
target_include_directories(msga_test_support PRIVATE ${TEST_INCLUDES})
target_include_directories(msga_test_support SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDES})
# PRIVATE on a static library = link-only for the tests: they get these on their
# link line but keep their own compile flags (QT_*_LIB defines, include dirs).
target_link_libraries(msga_test_support PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Svg
    Qt6::Concurrent Microsoft.GSL::GSL)
if(APPLE)
    target_link_libraries(msga_test_support PRIVATE "-framework AVFoundation")
elseif(WIN32)
    target_link_libraries(msga_test_support PRIVATE mfplay ole32)
else()
    target_link_libraries(msga_test_support PRIVATE msga_miniaudio)
endif()

# No AUTOMOC here: it folds every moc output into ONE mocs_compilation.cpp
# object, and a test touching any QObject would pull that object — and through
# it every QObject class in the library plus their dependencies. Moc each
# header into its own translation unit instead, so linking stays per-object.
# Same header set AUTOMOC would scan (each source's sibling header), plus the
# header-only QObject bases; headers without Q_OBJECT yield an empty TU (-nn
# silences moc's "no relevant classes" note for them).
set_target_properties(msga_test_support PROPERTIES AUTOMOC OFF)
set(_test_support_moc_headers
    ${CMAKE_SOURCE_DIR}/src/auth/auth_strategy.h
    ${CMAKE_SOURCE_DIR}/src/media/audio_engine.h
)
foreach(_src IN LISTS MSGA_TEST_SUPPORT_SRCS)
    string(REGEX REPLACE "\\.(cpp|mm)$" ".h" _hdr "${_src}")
    if(NOT _hdr STREQUAL _src AND EXISTS "${_hdr}")
        list(APPEND _test_support_moc_headers "${_hdr}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _test_support_moc_headers)
qt_wrap_cpp(_test_support_mocs ${_test_support_moc_headers}
    TARGET msga_test_support OPTIONS -nn)
target_sources(msga_test_support PRIVATE ${_test_support_mocs})

# The app's precompiled header (same header, so no source can come to depend
# on an include the app build doesn't also give it).
target_precompile_headers(msga_test_support PRIVATE ${CMAKE_SOURCE_DIR}/src/pch.h)

# Qt resources (icons, sounds, emoji data) for the tests that load them: an
# OBJECT library so rcc output is compiled once, and its static registration
# object is always linked (a static library would drop it as unreferenced).
add_library(msga_test_resources OBJECT ${CMAKE_SOURCE_DIR}/resources/resources.qrc)
target_link_libraries(msga_test_resources PRIVATE Qt6::Core)
# Same rcc compression as the app (MSGA_RCC_OPTIONS, top-level CMakeLists.txt).
set_property(TARGET msga_test_resources APPEND PROPERTY AUTORCC_OPTIONS ${MSGA_RCC_OPTIONS})

# ── One test executable ─────────────────────────────────────────────────────
# Every test file is linked into ONE executable, msga_tests, instead of one
# executable per file: the per-file links used to cost ~80 CPU-s and ~1.8 GB
# of Debug binaries, mostly the same msga_test_support objects linked 77 times.
#
# Each test file is a suite named after it, and each ctest entry still runs one
# suite in its own process (`msga_tests test_session`), so process-global setup
# (QApplication, QSettings::setPath, QStandardPaths test mode, env) stays per
# file. A file that needs such setup declares it with MSGA_TEST_MAIN in place
# of main() — see test_main.h, which also has the rules for adding a test file.
add_executable(msga_tests msga_tests_main.cpp)
set_target_properties(msga_tests PROPERTIES AUTOMOC ON)
target_include_directories(msga_tests PRIVATE ${TEST_INCLUDES})
target_include_directories(msga_tests SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDES})
target_link_libraries(msga_tests PRIVATE
    msga_test_support
    msga_test_resources
    Catch2::Catch2
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Svg
    Qt6::Concurrent Qt6::Test
    Microsoft.GSL::GSL)
# Its own precompiled header, not the app's src/pch.h: see test_pch.h.
target_precompile_headers(msga_tests PRIVATE test_pch.h)

# add_msga_test(<suite> [extra sources...]): adds <suite>.cpp (plus any extra
# sources only this suite needs) to msga_tests and registers a ctest entry that
# runs just that file's test cases. The suite name must be the file name: the
# suite selects its test cases by Catch2's filename tag ([#<suite>]).
function(add_msga_test suite)
    target_sources(msga_tests PRIVATE ${suite}.cpp ${ARGN})
    add_test(NAME ${suite} COMMAND msga_tests ${suite})
endfunction()

# Widget test helper: like add_msga_test, run under QApplication (the file's
# MSGA_TEST_MAIN) with the offscreen QPA.
# ctest runs these headless (offscreen QPA): no windows pop over the developer's
# desktop mid-run, and results stop depending on the live compositor (Wayland
# hides parentless popups, focus/expose timing varies per WM — the same reason
# test_user_profile_card already forced offscreen internally). To watch a test's
# UI while debugging, run `msga_tests <suite>` directly — the property applies
# only under ctest.
function(add_msga_widget_test suite)
    add_msga_test(${suite} ${ARGN})
    set_tests_properties(${suite} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_msga_test(test_emoji)

add_msga_test(test_fuzzy_match)

add_msga_test(test_nav_history)

add_msga_test(test_content_view)

add_msga_test(test_background_tasks)

add_msga_test(test_desktop_integration)

# Static-source guard: scheme registration must stay outside the WinRT guard.
# Parses the real Windows source (runs on any host), so it needs its path.
add_msga_test(test_win_uri_scheme)
set_source_files_properties(test_win_uri_scheme.cpp PROPERTIES
    COMPILE_DEFINITIONS MSGA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_msga_test(test_mrkdwn)

# Composer text → mrkdwn + rich_text (CommonMark rewrites, list blocks).
add_msga_test(test_markdown_compose)

# Shortened link labels ("host/…/…") — detection, display expansion, copy text.
add_msga_test(test_link_labels)

# Slack deep-link parsing (header-only).
add_msga_test(test_slack_links)

# GIPHY wire format behind the composer's GIF picker.
# Custom main: the key lives in SecretStore, so QSettings has to be redirected
# at a temp dir before any test touches it.
add_msga_test(test_gif_search)

add_msga_test(test_json_mappers)

# IMAP MIME / RFC 5322 parser (raw message → text/attachments).
add_msga_test(test_imap_mime)

# IMAP protocol layer: response framing + light parsers.
add_msga_test(test_imap_protocol)

# IMAP ENVELOPE/FLAGS/THREAD S-expression mappers.
add_msga_test(test_imap_mappers)

# Model-D bucketing engine: thread → DM/MPDM/list-channel grouping.
add_msga_test(test_imap_bucketing)

# HTML email → TextWithEntities.
add_msga_test(test_imap_html)

# Outgoing MIME message builder.
add_msga_test(test_imap_compose)

# IMAP provider auto-detection (domain → server settings).
add_msga_test(test_imap_providers)

# Favicon fallback resolver — pure parts (<link rel=icon> scan, parent-domain
# hop, image sniffing).
add_msga_test(test_imap_favicon)

# Microsoft Teams JSON mappers (Graph JSON → domain).
add_msga_test(test_teams_mappers)

# Pure huddle-notification policy (header-only helper in domain.h).
add_msga_test(test_huddle_notify)

# Pure notification-freshness policy (header-only helper in domain.h).
add_msga_test(test_notify_age)

# Notification click-target token codec (encode/decode round-trip, thread root).
add_msga_test(test_notif_token)

add_msga_test(test_workspace_cache)

add_msga_test(test_cache_evictor)

add_msga_test(test_token_store)

add_msga_test(test_secret_store)

add_msga_test(test_backend_factory)

add_msga_test(test_auth_strategy)

# Claude Code backend: transcript + roster parsing, and the backend end to end
# against a fake ~/.claude (CLAUDE_CONFIG_DIR).
add_msga_test(test_claude_code)
if(UNIX)
    # A stand-in for Claude Code's terminal UI (`claude attach`), for typing
    # into live background sessions.
    add_executable(cc_fake_attach cc_fake_attach.cpp)
    add_dependencies(msga_tests cc_fake_attach)
    set_source_files_properties(test_claude_code.cpp PROPERTIES
        COMPILE_DEFINITIONS CC_FAKE_ATTACH="$<TARGET_FILE:cc_fake_attach>")
endif()

add_msga_test(test_time_format)

add_msga_test(test_discussion_summary)

# Pure wire codecs for both chat-completion formats — no network.
add_msga_test(test_llm_wire)

# Provider registry + HTTP round trips against FakeHttpServer (own main: needs a
# QCoreApplication for the event loop and a redirected QSettings path).
add_msga_test(test_llm_service)

add_msga_test(test_fake_backend)

# --demo fixture loader, tour script parser and demo backend. Only where the
# fixture exists (the private repo): the public mirror ships no demo/ directory
# and must build without any demo-mode code.
if(EXISTS "${CMAKE_SOURCE_DIR}/demo/fixture.json")
    add_msga_test(test_demo_fixture
        ${CMAKE_SOURCE_DIR}/src/backend/demo/demo_fixture.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/demo/demo_backend.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/demo/demo_tour_script.cpp
        ${CMAKE_SOURCE_DIR}/src/backend/demo/demo_services.cpp
    )
    set_source_files_properties(test_demo_fixture.cpp PROPERTIES
        COMPILE_DEFINITIONS MSGA_DEMO_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/demo")
endif()

add_msga_test(test_session)

add_msga_widget_test(test_message_render)

add_msga_widget_test(test_theme)
# Same suite, pre-mode config seeded: exercises ThemeManager's one-time
# charcoal → dark-mode migration, which only runs in the constructor.
add_test(NAME test_theme_migration COMMAND msga_tests test_theme "[migration]")
set_tests_properties(test_theme_migration PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MSGA_TEST_SEED_THEME=charcoal")

add_msga_widget_test(test_canvas_diff)

add_msga_widget_test(test_canvas_page)

# canvas_emoji reuses MsgRender::resolveEmojiRich.
add_msga_widget_test(test_canvas_emoji)

add_msga_widget_test(test_image_cache)

add_msga_widget_test(test_workspace_switcher)

add_msga_widget_test(test_workspace_icon)

add_msga_widget_test(test_tray_icon)

add_msga_widget_test(test_user_profile_card)

add_msga_widget_test(test_context_menu)

add_msga_widget_test(test_popup_tooltip)

add_msga_widget_test(test_app_dialog)

add_msga_widget_test(test_summary_dialog)

# Disabled: the "XXX is typing…" indicator feature is not in use right now.
# Re-enabling needs src/ui/typing_indicator/typing_indicator.cpp in
# MSGA_TEST_SUPPORT_SRCS.
# add_msga_widget_test(test_typing_indicator)

add_msga_widget_test(test_search_widget)

add_msga_test(test_token_refresh)

add_msga_test(test_socket_mode_realtime)

add_msga_test(test_rtm_presence)

add_msga_test(test_enterprise_grid)

add_msga_test(test_slack_members)

add_msga_test(test_bot_buttons)

add_msga_test(test_send_retry)

# Session-token (xoxc/xoxd) sign-in: blob round-trip + Cookie-header emission.
add_msga_test(test_session_auth)

# Session-token derivation: boot-page scrape (incl. Slack's 403-with-payload),
# auth.test validation, per-candidate failure reasons.
add_msga_test(test_token_deriver)

# Browser sign-in: the pure parsers (localConfig_v2, host harvesting, /json/version).
add_msga_test(test_browser_login)

# SessionImportDialog render/smoke harness (screenshot + card-height regression).
add_msga_widget_test(test_session_import_dialog)

add_msga_widget_test(test_browse_channels_dialog)

add_msga_widget_test(test_members_popup)

add_msga_widget_test(test_shortcuts)

add_msga_widget_test(test_move_to_thread_dialog)

add_msga_widget_test(test_quick_switcher)

add_msga_widget_test(test_composer)

add_msga_widget_test(test_conv_list_widget)

# ThreadPanel owns a MessageListWidget and a ComposerWidget.
add_msga_widget_test(test_thread_panel)

# Forwarding shares the thread composer's message-rendering dependencies (it
# used to be compiled into test_thread_panel's executable).
add_msga_widget_test(test_forward_dialog)

add_msga_widget_test(test_message_list)

# Sound player: shared logic + the platform-native backend for this OS.
if(APPLE)
    set(_sound_backend ${CMAKE_SOURCE_DIR}/src/util/sound_player_mac.mm)
elseif(WIN32)
    set(_sound_backend ${CMAKE_SOURCE_DIR}/src/util/sound_player_win.cpp)
else()
    set(_sound_backend ${CMAKE_SOURCE_DIR}/src/util/sound_player_linux.cpp)
endif()
add_msga_test(test_sound_player
    ${CMAKE_SOURCE_DIR}/src/util/sound_player.cpp
    ${_sound_backend}
)
# Platform-native audio backends need their system frameworks (mirrors the
# msga target in the top-level CMakeLists): NSSound lives in AppKit on macOS,
# PlaySound in winmm on Windows.
if(APPLE)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/util/sound_player_mac.mm
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(msga_tests PRIVATE "-framework AppKit" "-framework Foundation")
elseif(WIN32)
    target_link_libraries(msga_tests PRIVATE winmm)
endif()

# Hang-watchdog startup-window regression (logs/2026-06-30_09-55-13.log). The
# watchdog itself is Linux + dev-build only (POSIX per-process timers targeted
# at the main thread); mirror the top-level CMakeLists gating so the test
# exercises the real signal path where it exists and trivially passes elsewhere.
add_msga_test(test_hang_watchdog
    ${CMAKE_SOURCE_DIR}/src/app/crash_handler.cpp
)
# crash_handler.cpp #defines _GNU_SOURCE before its first include; the PCH
# would pull in <signal.h> ahead of it (same exemption as the app target).
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/app/crash_handler.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (CMAKE_BUILD_TYPE STREQUAL "Debug" OR MSGA_ASAN))
    # Scoped to the two files that read it, as the app scopes it to its target.
    set_source_files_properties(test_hang_watchdog.cpp ${CMAKE_SOURCE_DIR}/src/app/crash_handler.cpp
        PROPERTIES COMPILE_DEFINITIONS MSGA_HANG_WATCHDOG)
    find_library(MSGA_RT_LIB_TEST rt)
    if(MSGA_RT_LIB_TEST)
        target_link_libraries(msga_tests PRIVATE ${MSGA_RT_LIB_TEST})
    endif()
endif()

# Inline audio player: AudioPlayer state machine (scripted engine) + the real
# platform engine's decode path (Linux: miniaudio on a generated WAV, never
# played — no sound server needed under ctest).
add_msga_test(test_audio_player)

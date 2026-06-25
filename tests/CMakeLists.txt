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

function(add_msga_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${TEST_INCLUDES})
    target_include_directories(${target} SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDES})
    target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain Qt6::Core Microsoft.GSL::GSL)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

# Like add_msga_test but the source file provides its own main() (needed when
# a QCoreApplication must be constructed before Catch2 runs, e.g. QStandardPaths).
function(add_msga_qt_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${TEST_INCLUDES})
    target_include_directories(${target} SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDES})
    target_link_libraries(${target} PRIVATE Catch2::Catch2 Qt6::Core Microsoft.GSL::GSL)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

# Widget test helper: needs QApplication + Qt6::Widgets.
function(add_msga_widget_test target)
    add_executable(${target} ${ARGN})
    set_target_properties(${target} PROPERTIES AUTOMOC ON)
    target_include_directories(${target} PRIVATE ${TEST_INCLUDES})
    target_include_directories(${target} SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDES})
    target_link_libraries(${target} PRIVATE
        Catch2::Catch2
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Svg
        Microsoft.GSL::GSL)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_msga_test(test_emoji
    test_emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji.cpp
)

add_msga_test(test_nav_history
    test_nav_history.cpp
)

add_msga_test(test_background_tasks
    test_background_tasks.cpp
    ${CMAKE_SOURCE_DIR}/src/util/background_tasks.cpp
)
# BackgroundTasks is a Q_OBJECT — needs moc.
set_target_properties(test_background_tasks PROPERTIES AUTOMOC ON)

add_msga_test(test_desktop_integration
    test_desktop_integration.cpp
    ${CMAKE_SOURCE_DIR}/src/util/desktop_integration.cpp
)

# Static-source guard: scheme registration must stay outside the WinRT guard.
# Parses the real Windows source (runs on any host), so it needs its path.
add_msga_test(test_win_uri_scheme
    test_win_uri_scheme.cpp
)
target_compile_definitions(test_win_uri_scheme PRIVATE
    MSGA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_msga_test(test_mrkdwn
    test_mrkdwn.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
)

add_msga_test(test_json_mappers
    test_json_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/json_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
)

# IMAP MIME / RFC 5322 parser (raw message → text/attachments; Qt6::Core only).
add_msga_test(test_imap_mime
    test_imap_mime.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/mime_parser.cpp
)

# IMAP protocol layer: response framing + light parsers (Qt6::Core only).
add_msga_test(test_imap_protocol
    test_imap_protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_protocol.cpp
)

# IMAP ENVELOPE/FLAGS/THREAD S-expression mappers (Qt6::Core only).
add_msga_test(test_imap_mappers
    test_imap_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/mime_parser.cpp
)

# Model-D bucketing engine: thread → DM/MPDM/list-channel grouping (Qt6::Core).
add_msga_test(test_imap_bucketing
    test_imap_bucketing.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_bucketing.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/mime_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_providers.cpp
)

# HTML email → TextWithEntities (Qt6::Core only).
add_msga_test(test_imap_html
    test_imap_html.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_html.cpp
)

# Outgoing MIME message builder (Qt6::Core only).
add_msga_test(test_imap_compose
    test_imap_compose.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_compose.cpp
)

# IMAP provider auto-detection (domain → server settings; Qt6::Core only).
add_msga_test(test_imap_providers
    test_imap_providers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_providers.cpp
)

# Microsoft Teams JSON mappers (Graph JSON → domain; needs only Qt6::Core).
add_msga_test(test_teams_mappers
    test_teams_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/json_mappers.cpp
)

# Pure huddle-notification policy (header-only helper in domain.h).
add_msga_test(test_huddle_notify
    test_huddle_notify.cpp
)
target_link_libraries(test_huddle_notify PRIVATE Qt6::Gui)

# Source set for the Slack backend below the seam — shared by the backend /
# network / factory tests so each lists it once.
set(SLACK_BACKEND_SRCS
    ${CMAKE_SOURCE_DIR}/src/network/http_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/web_api_client.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/public_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/json_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/socket_mode_realtime.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/shared_realtime.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/slack_auth.cpp
)

# Microsoft Teams backend below the seam (excludes oauth_flow.cpp — added per-test
# like the Slack set). http_queue.cpp comes from SLACK_BACKEND_SRCS where co-listed.
set(TEAMS_BACKEND_SRCS
    ${CMAKE_SOURCE_DIR}/src/backend/teams/teams_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/graph_client.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/json_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/teams_auth.cpp
)

# Email (IMAP) backend below the seam — used by the factory test.
set(IMAP_BACKEND_SRCS
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_client.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_bucketing.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_auth.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_providers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_html.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_bimi.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_compose.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_smtp.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/mime_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/llm/oauth_loopback.cpp # OAuth (XOAUTH2) token refresh
)

add_msga_qt_test(test_workspace_cache
    test_workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
)

add_msga_test(test_cache_evictor
    test_cache_evictor.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
)

add_msga_qt_test(test_token_store
    test_token_store.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/token_store.cpp
)

add_msga_qt_test(test_backend_factory
    test_backend_factory.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/backend_factory.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/token_store.cpp
    ${SLACK_BACKEND_SRCS}
    ${TEAMS_BACKEND_SRCS}
    ${IMAP_BACKEND_SRCS}
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
)
target_link_libraries(test_backend_factory PRIVATE
    Qt6::Gui Qt6::Network Qt6::WebSockets Qt6::Concurrent)
set_target_properties(test_backend_factory PROPERTIES AUTOMOC ON)

add_msga_qt_test(test_auth_strategy
    test_auth_strategy.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/auth_strategy.h # header-only QObject base — moc it
    ${CMAKE_SOURCE_DIR}/src/auth/auth_strategy_factory.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/token_store.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/oauth_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/slack_auth.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/oauth_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/teams/teams_auth.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_auth_strategy.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/imap/imap_auth.cpp
)
target_link_libraries(test_auth_strategy PRIVATE Qt6::Gui Qt6::Network)
set_target_properties(test_auth_strategy PROPERTIES AUTOMOC ON)

add_msga_qt_test(test_time_format
    test_time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
)

add_msga_test(test_fake_backend
    test_fake_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/fake_backend/fake_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
)

add_msga_qt_test(test_session
    test_session.cpp
    ${CMAKE_SOURCE_DIR}/src/session/session.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
)
# session.cpp probes image dimensions for optimistic upload previews (QImageReader)
target_link_libraries(test_session PRIVATE Qt6::Gui)

add_msga_widget_test(test_message_render
    test_message_render.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/message_render.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/session/session.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
)

add_msga_widget_test(test_theme
    test_theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
)

target_link_libraries(test_theme PRIVATE Qt6::Test)

add_msga_widget_test(test_canvas_diff
    test_canvas_diff.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/canvas_page/canvas_diff.cpp
)

add_msga_widget_test(test_canvas_page
    test_canvas_page.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/canvas_page/canvas_page.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/canvas_page/canvas_diff.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/canvas_page/canvas_emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/fake_backend/fake_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/message_render.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/popup_tooltip/popup_tooltip.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/context_menu/context_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/app_dialog/app_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/icon_button/icon_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_button/styled_button.cpp
    ${CMAKE_SOURCE_DIR}/src/util/mailto_link.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/session/session.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
)

# canvas_emoji reuses MsgRender::resolveEmojiRich, hence the message_render
# link set (same transitive deps as test_message_render).
add_msga_widget_test(test_canvas_emoji
    test_canvas_emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/canvas_page/canvas_emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/message_render.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/session/session.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
)

add_msga_widget_test(test_image_cache
    test_image_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/image_cache.cpp
)

add_msga_widget_test(test_workspace_switcher
    test_workspace_switcher.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/workspace_switcher/workspace_switcher.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/popup_tooltip/popup_tooltip.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/image_cache.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

add_msga_widget_test(test_user_profile_card
    test_user_profile_card.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/user_profile_card/user_profile_card.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
target_link_libraries(test_user_profile_card PRIVATE Qt6::Test)

add_msga_widget_test(test_context_menu
    test_context_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/context_menu/context_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
target_link_libraries(test_context_menu PRIVATE Qt6::Test)

# Disabled: the "XXX is typing…" indicator feature is not in use right now.
# add_msga_widget_test(test_typing_indicator
#     test_typing_indicator.cpp
#     ${CMAKE_SOURCE_DIR}/src/ui/typing_indicator/typing_indicator.cpp
#     ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
#     ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
# )
# target_link_libraries(test_typing_indicator PRIVATE Qt6::Test)

add_msga_widget_test(test_search_widget
    test_search_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/search/search_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/popup_tooltip/popup_tooltip.cpp
    ${CMAKE_SOURCE_DIR}/src/session/session.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
target_link_libraries(test_search_widget PRIVATE Qt6::Test)

add_msga_qt_test(test_token_refresh
    test_token_refresh.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/token_store.cpp
    ${SLACK_BACKEND_SRCS}
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
)
target_link_libraries(test_token_refresh PRIVATE Qt6::Network Qt6::WebSockets)
set_target_properties(test_token_refresh PROPERTIES AUTOMOC ON)

add_msga_qt_test(test_socket_mode_realtime
    test_socket_mode_realtime.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/socket_mode_realtime.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/slack/json_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
)
target_link_libraries(test_socket_mode_realtime PRIVATE Qt6::Network Qt6::WebSockets)
set_target_properties(test_socket_mode_realtime PROPERTIES AUTOMOC ON)

add_msga_qt_test(test_send_retry
    test_send_retry.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/token_store.cpp
    ${SLACK_BACKEND_SRCS}
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
)
target_link_libraries(test_send_retry PRIVATE Qt6::Network Qt6::WebSockets)
set_target_properties(test_send_retry PROPERTIES AUTOMOC ON)

add_msga_widget_test(test_browse_channels_dialog
    test_browse_channels_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/browse_channels_dialog/browse_channels_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/browse_channels_dialog/browse_list_view.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/app_dialog/app_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/icon_button/icon_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/virtual_list/virtual_list_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_button/styled_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_line_edit/styled_line_edit.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/image_cache.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

add_msga_widget_test(test_composer
    test_composer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/composer_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/attachment_strip.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/formatting_toolbar.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/edit_mode_banner.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/emoji_picker/emoji_picker_popup.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/virtual_list/virtual_list_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/mention_completer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/image_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/mention_popup/mention_popup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/popup_tooltip/popup_tooltip.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_button/styled_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_line_edit/styled_line_edit.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/icon_button/icon_button.cpp
    ${CMAKE_SOURCE_DIR}/src/session/session.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)


add_msga_widget_test(test_conv_list_widget
    test_conv_list_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/conv_list/conv_list_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/message_render.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/file_chip_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/context_menu/context_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/popup_tooltip/popup_tooltip.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/emoji_picker/emoji_picker_popup.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/user_profile_card/user_profile_card.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/image_viewer/image_viewer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/delete_message_dialog/delete_message_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/app_dialog/app_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/icon_button/icon_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_button/styled_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_line_edit/styled_line_edit.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/virtual_list/virtual_list_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/loading_indicator/loading_indicator.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/image_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/session/session.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/background_tasks.cpp
    ${CMAKE_SOURCE_DIR}/src/util/mailto_link.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

add_msga_widget_test(test_message_list
    test_message_list.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/message_list.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/message_list_paint.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/message_render.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/message_list/file_chip_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/context_menu/context_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/popup_tooltip/popup_tooltip.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/emoji_picker/emoji_picker_popup.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/user_profile_card/user_profile_card.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/image_viewer/image_viewer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/delete_message_dialog/delete_message_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/app_dialog/app_dialog.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/icon_button/icon_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_button/styled_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/styled_line_edit/styled_line_edit.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/virtual_list/virtual_list_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/loading_indicator/loading_indicator.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/image_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/session/session.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/cache_evictor.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/util/background_tasks.cpp
    ${CMAKE_SOURCE_DIR}/src/util/mailto_link.cpp
    ${CMAKE_SOURCE_DIR}/src/util/emoji.cpp
    ${CMAKE_SOURCE_DIR}/src/util/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/util/relative_time.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)


# Sound player: shared logic + the platform-native backend for this OS.
if(APPLE)
    set(_sound_backend ${CMAKE_SOURCE_DIR}/src/util/sound_player_mac.mm)
elseif(WIN32)
    set(_sound_backend ${CMAKE_SOURCE_DIR}/src/util/sound_player_win.cpp)
else()
    set(_sound_backend ${CMAKE_SOURCE_DIR}/src/util/sound_player_linux.cpp)
endif()
add_msga_qt_test(test_sound_player
    test_sound_player.cpp
    ${CMAKE_SOURCE_DIR}/src/util/sound_player.cpp
    ${_sound_backend}
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
# Platform-native audio backends need their system frameworks (mirrors the
# msga target in the top-level CMakeLists): NSSound lives in AppKit on macOS,
# PlaySound in winmm on Windows.
if(APPLE)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/util/sound_player_mac.mm
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(test_sound_player PRIVATE "-framework AppKit" "-framework Foundation")
elseif(WIN32)
    target_link_libraries(test_sound_player PRIVATE winmm)
endif()

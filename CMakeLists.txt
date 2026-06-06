cmake_minimum_required(VERSION 3.21)
project(msga LANGUAGES CXX)

include("${CMAKE_SOURCE_DIR}/credentials.cmake" OPTIONAL)
foreach(_var MSGA_CLIENT_ID MSGA_CLIENT_SECRET MSGA_XAPP)
    if(NOT DEFINED ${_var})
        set(${_var} "")
    endif()
endforeach()
configure_file(
    "${CMAKE_SOURCE_DIR}/src/app_credentials.h.in"
    "${CMAKE_BINARY_DIR}/app_credentials.h"
    @ONLY
)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets Svg)
find_package(Qt6 OPTIONAL_COMPONENTS LinguistTools)

include(FetchContent)

FetchContent_Declare(
    microsoft_gsl
    GIT_REPOSITORY https://github.com/microsoft/GSL.git
    GIT_TAG        v4.0.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(microsoft_gsl)

FetchContent_Declare(
    lib_rpl
    GIT_REPOSITORY https://github.com/desktop-app/lib_rpl.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(lib_rpl)
if(NOT lib_rpl_POPULATED)
    FetchContent_Populate(lib_rpl)
endif()

FetchContent_Declare(
    lib_base
    GIT_REPOSITORY https://github.com/desktop-app/lib_base.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(lib_base)
if(NOT lib_base_POPULATED)
    FetchContent_Populate(lib_base)
endif()

add_executable(msga MACOSX_BUNDLE
    src/main.cpp
    src/base_stubs.cpp

    # App
    src/app/single_instance.cpp

    # Network
    src/network/web_api_client.cpp
    src/backend/public_backend/socket_mode_realtime.cpp

    # Auth
    src/auth/token_store.cpp
    src/auth/oauth_flow.cpp

    # Backend port
    src/backend/public_backend/public_backend.cpp
    src/backend/public_backend/json_mappers.cpp

    # Text
    src/text/mrkdwn_parser.cpp

    # Cache
    src/cache/workspace_cache.cpp

    # Session / domain store
    src/session/session.cpp

    # UI
    src/ui/virtual_list/virtual_list_widget.cpp
    src/ui/image_cache.cpp
    src/ui/main_window.cpp
    src/ui/title_bar/title_bar.cpp
    src/ui/message_list/message_list.cpp
    src/ui/message_list/message_list_paint.cpp
    src/ui/message_list/message_render.cpp
    src/ui/conv_list/conv_list_widget.cpp
    src/ui/composer/formatting_toolbar.cpp
    src/ui/composer/attachment_strip.cpp
    src/ui/composer/edit_mode_banner.cpp
    src/ui/composer/composer_widget.cpp
    src/ui/emoji_picker/emoji_picker_popup.cpp
    src/ui/composer/mention_completer.cpp
    src/ui/mention_popup/mention_popup.cpp
    src/ui/context_menu/context_menu.cpp
    src/ui/popup_tooltip/popup_tooltip.cpp
    src/ui/workspace_switcher/workspace_switcher.cpp
    src/ui/settings/settings_dialog.cpp
    src/ui/search/search_widget.cpp
    src/ui/thread_panel/thread_panel.cpp
    src/ui/app_dialog/app_dialog.cpp
    src/ui/delete_message_dialog/delete_message_dialog.cpp
    src/ui/conv_selector/conv_selector_widget.cpp
    src/ui/forward_dialog/forward_dialog.cpp

    # Qt resources
    resources/resources.qrc
)

target_precompile_headers(msga PRIVATE src/pch.h)

target_include_directories(msga PRIVATE
    src
    ${CMAKE_BINARY_DIR}
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
    Microsoft.GSL::GSL
)

if(Qt6LinguistTools_FOUND)
    qt_add_translations(msga
        TS_FILES translations/msga_en.ts
        RESOURCE_PREFIX "/translations"
    )
endif()

option(MSGA_BUILD_TESTS "Build unit tests" OFF)
if(MSGA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

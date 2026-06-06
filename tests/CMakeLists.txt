include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

set(TEST_INCLUDES
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
    ${lib_rpl_SOURCE_DIR}
    ${lib_base_SOURCE_DIR}
)

function(add_msga_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${TEST_INCLUDES})
    target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain Qt6::Core Microsoft.GSL::GSL)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

# Like add_msga_test but the source file provides its own main() (needed when
# a QCoreApplication must be constructed before Catch2 runs, e.g. QStandardPaths).
function(add_msga_qt_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${TEST_INCLUDES})
    target_link_libraries(${target} PRIVATE Catch2::Catch2 Qt6::Core Microsoft.GSL::GSL)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

# Widget test helper: needs QApplication + Qt6::Widgets.
function(add_msga_widget_test target)
    add_executable(${target} ${ARGN})
    set_target_properties(${target} PROPERTIES AUTOMOC ON)
    target_include_directories(${target} PRIVATE ${TEST_INCLUDES})
    target_link_libraries(${target} PRIVATE
        Catch2::Catch2
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Svg
        Microsoft.GSL::GSL)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_msga_test(test_mrkdwn
    test_mrkdwn.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
)

add_msga_test(test_json_mappers
    test_json_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/backend/public_backend/json_mappers.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
)

add_msga_qt_test(test_workspace_cache
    test_workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
)

add_msga_qt_test(test_token_store
    test_token_store.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/token_store.cpp
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
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
)

add_msga_widget_test(test_composer
    test_composer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/composer_widget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/emoji_picker/emoji_picker_popup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/composer/mention_completer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/mention_popup/mention_popup.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/popup_tooltip/popup_tooltip.cpp
    ${CMAKE_SOURCE_DIR}/src/session/session.cpp
    ${CMAKE_SOURCE_DIR}/src/cache/workspace_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/text/mrkdwn_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/base_stubs.cpp
    ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)

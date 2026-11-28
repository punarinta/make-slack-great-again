# msga_screens_settings — the Settings dialog (an in-window modal) and the
# settings file it edits (shell::Settings, ../shell/settings.cpp: the shell
# owns saving it, and building it here keeps the dialog from linking the shell).
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../../version.cmake)
string(TIMESTAMP MSGA_BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S" UTC)
add_library(msga_screens_settings STATIC
    settings_dialog.cpp
    settings_pages.cpp
    ../shell/settings.cpp)
target_include_directories(msga_screens_settings PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
target_link_libraries(msga_screens_settings PUBLIC msga_screens_common msga_app_core msga_app_llm msga_app_spell msga_ui msga_flags)
if(TARGET msga_app_update)
    target_link_libraries(msga_screens_settings PUBLIC msga_app_update)
endif()
target_compile_definitions(msga_screens_settings PRIVATE
    MSGA_VERSION=${MSGA_VERSION} MSGA_BUILD_TIMESTAMP="${MSGA_BUILD_TIMESTAMP}")
# The build's GIPHY key (credentials.cmake, never committed): only the
# settings file sees it (Settings::buildGiphyKey).
set_source_files_properties(../shell/settings.cpp PROPERTIES COMPILE_DEFINITIONS
    "MSGA_GIPHY_KEY=\"${MSGA_GIPHY_KEY}\"")
if(TARGET msga_screens_messages)
    target_link_libraries(msga_screens_settings PUBLIC msga_screens_messages)
    target_compile_definitions(msga_screens_settings PRIVATE MSGA_HAVE_MESSAGES=1)
endif()

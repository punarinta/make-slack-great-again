# msga_screens_messages — the message list, its rows, the thread panel and the
# shared ImageCache (see the headers here).
add_library(msga_screens_messages STATIC
    image_cache.cpp
    rich.cpp
    table_view.cpp
    rows.cpp
    audio_card.cpp
    message_list.cpp
    message_list_model.cpp
    thread_panel.cpp
    thread_export.cpp
    emoji_picker.cpp
    message_dialogs.cpp
    summary.cpp
)
# The scroll torture test (msga --debug-scroll): demo workspace only.
if(MSGA_DEMO)
    target_sources(msga_screens_messages PRIVATE debug_scroll.cpp)
endif()
# context.h includes "model/…" relative to app/, so app/ is on the path too.
target_include_directories(msga_screens_messages PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(msga_screens_messages PUBLIC msga_screens_common msga_app_core msga_app_llm msga_ui msga_flags)

# MessageList + ThreadPanel on demo/fixture.json, without the shell.
if(MSGA_DEMO)
    add_executable(messages_demo demo/messages_demo.cpp)
    target_link_libraries(messages_demo PRIVATE msga_screens_messages msga_app_fake)
    target_compile_definitions(messages_demo PRIVATE
        MSGA_DEMO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../../demo")
    msga_size_map(messages_demo)
endif()

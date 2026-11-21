# msga_screens_common — what every screen group shares beyond Context:
# the file/folder choosers (OS dialog, else the in-app ui::FileBrowser) and
# the disk cache of downloaded pictures (remote_images.h), the loading
# ring, the avatar placeholder letter, the icon button, the message rules
# (system lines, grouping, authors), the custom theme's JSON and the people
# search key.
add_library(msga_screens_common STATIC
    avatar_initial.cpp
    canvas_doc.cpp
    custom_theme.cpp
    tag_badge.cpp
    user_search.cpp
    downloads.cpp
    file_badge.cpp
    file_dialogs.cpp
    icon_button.cpp
    loading_indicator.cpp
    message_rules.cpp
    message_text.cpp
    remote_images.cpp
)
target_include_directories(msga_screens_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(msga_screens_common PUBLIC msga_app_core msga_ui msga_net msga_flags)

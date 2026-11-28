# ui: everything runs on plat's headless backend with input injected through
# its TestHooks, so no display is needed.
add_executable(ui_tests
    test_layout.cpp
    test_input.cpp
    test_vlist.cpp
    test_textedit.cpp
    test_paint.cpp
    test_menu.cpp
    test_dialog.cpp
    test_image.cpp
    main.cpp
)
# One ctest test per case group ("hover: ..."), named exactly.
set(ui_test_names layout visibility label hit hover focus shortcut popup menu scroll vlist textedit
    html damage theme controls image)
if(MSGA_FREEDESKTOP OR MSGA_PORT)
    target_sources(ui_tests PRIVATE test_filebrowser.cpp)
    list(APPEND ui_test_names filebrowser)
endif()
target_link_libraries(ui_tests PRIVATE msga_ui msga_test)
foreach(t ${ui_test_names})
    msga_add_test(ui_${t} COMMAND ui_tests ${t})
endforeach()

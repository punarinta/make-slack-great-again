# plat's tests and selftest, added by plat's own standalone build
# (src/plat/CMakeLists.txt, PLAT_BUILD_TESTS=ON). They include plat's private
# headers (PLAT_DIR/src).
add_executable(plat_unit_tests unit_tests.cpp)
target_link_libraries(plat_unit_tests PRIVATE plat)
target_include_directories(plat_unit_tests PRIVATE ${PLAT_DIR}/src)
add_test(NAME plat_unit_tests COMMAND plat_unit_tests)

if(PLAT_LINUX_INPUT)
    add_executable(plat_xkb_tests xkb_tests.cpp)
    target_link_libraries(plat_xkb_tests PRIVATE plat)
    target_include_directories(plat_xkb_tests PRIVATE ${PLAT_DIR}/src)
    add_test(NAME plat_xkb_tests COMMAND plat_xkb_tests)
endif()

# Exercises the real backend end to end (window, present + readback, input
# through the OS, clipboard, timers, cross-thread post). Needs a display:
# src/plat/scripts/plat-selftest-*.sh provide one per backend (Xvfb,
# headless cage, Wine).
add_executable(plat_selftest selftest.cpp)
target_link_libraries(plat_selftest PRIVATE plat)
# The same end-to-end cases against the headless reference backend.
add_test(NAME plat_selftest_headless COMMAND plat_selftest)
set_tests_properties(plat_selftest_headless PROPERTIES ENVIRONMENT "PLAT_BACKEND=headless")

if(MINGW)
    target_link_options(plat_selftest PRIVATE -static)
    target_link_options(plat_unit_tests PRIVATE -static)
endif()

if(WIN32 AND PLAT_TEST_HOOKS)
    # Win32-only end-to-end checks (Custom frame, modal loops, DPI, IME-free
    # text paths, drops); like plat_selftest it needs a desktop, so it is not
    # a ctest test — run it with plat-selftest-wine.sh or on Windows. It drives
    # the backend through its test hooks, so it needs PLAT_TEST_HOOKS.
    add_executable(plat_win32_tests win32_tests.cpp)
    target_link_libraries(plat_win32_tests PRIVATE plat)
    target_include_directories(plat_win32_tests PRIVATE ${PLAT_DIR}/src)
    target_compile_definitions(plat_win32_tests PRIVATE PLAT_TEST_HOOKS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    if(MINGW)
        target_link_options(plat_win32_tests PRIVATE -static)
    endif()
elseif(APPLE)
    # Cocoa-only data-transfer checks on a private pasteboard; like
    # plat_win32_tests it needs a login session (the pasteboard server), so it
    # is not a ctest test — run it directly on a Mac.
    enable_language(OBJCXX)
    add_executable(plat_cocoa_tests cocoa_tests.mm)
    set_source_files_properties(cocoa_tests.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(plat_cocoa_tests PRIVATE plat)
    target_include_directories(plat_cocoa_tests PRIVATE ${PLAT_DIR}/src)
endif()

# msga_app_core / msga_app_slack / msga_app_update, the fake workspace on the
# test fixture (../../assets), and the Slack Web API against a fake server
# (../../support/fake_slack.py). Suites of msga_app_tests (../CMakeLists.txt),
# ctest test core_<suite> each.
set(core_suites mrkdwn markdown_compose emoji store jobs fixture languages slack slack_write
    slack_read slack_cache slack_realtime slack_features slack_links)
if(TARGET msga_app_update)
    list(APPEND core_suites update)
    target_link_libraries(msga_app_tests PRIVATE msga_app_update)
endif()
# The importer is Linux-native, and left out of the Flatpak (src/app/CMakeLists.txt).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT MSGA_PORT AND NOT MSGA_FLATPAK)
    list(APPEND core_suites local_import)
endif()
target_sources(msga_app_tests PRIVATE ../../support/fake_slack_server.cpp)
target_link_libraries(msga_app_tests PRIVATE msga_app_fake msga_app_slack)
target_compile_definitions(msga_app_tests PRIVATE
    FAKE_SLACK_SCRIPT="${MSGA_ROOT}/tests/support/fake_slack.py")
# The fake backend's timers run on plat's headless loop; times pin a zone.
foreach(s ${core_suites})
    target_sources(msga_app_tests PRIVATE test_${s}.cpp)
    msga_add_test(core_${s} COMMAND msga_app_tests test_${s} TZ LABELS core)
endforeach()

if(UNIX AND NOT MSGA_PORT)
    # plat audio through fake helper programs (Linux) / the OS player
    # without playing anything (macOS). Never the speakers or the mic. A
    # port build has the port's audio, not these backends.
    target_sources(msga_app_tests PRIVATE test_audio.cpp)
    msga_add_test(audio_tests COMMAND msga_app_tests test_audio LABELS core)
endif()
if(UNIX)
    # Crash handlers and the watchdog are process-wide: the suite installs
    # them in its own BASE_TEST_SUITE_MAIN.
    target_sources(msga_app_tests PRIVATE test_crash.cpp)
    msga_add_test(crash_tests COMMAND msga_app_tests test_crash LABELS core)
endif()

if(Python3_FOUND)
    # Every tr() string is in the .po files and the table is regenerated.
    add_test(NAME i18n_up_to_date
        COMMAND Python3::Interpreter ${MSGA_ROOT}/src/tools/i18n.py check)
endif()

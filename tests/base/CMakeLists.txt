# msga_base: UTF-8, JSON, files, time, i18n, the keychain, the settings of earlier versions, threads.
add_executable(msga_base_tests
    test_utf8.cpp
    test_str.cpp
    test_mime.cpp
    test_json.cpp
    test_file.cpp
    test_time.cpp
    test_i18n.cpp
    test_secret.cpp
    test_old_settings.cpp
    test_crypto.cpp
    test_process.cpp
    test_observers.cpp
    test_thread.cpp
    main.cpp
)
target_link_libraries(msga_base_tests PRIVATE msga_base msga_test)
if(APPLE)
    # enable_language is per directory: src/base's doesn't reach here.
    enable_language(OBJCXX)
    target_sources(msga_base_tests PRIVATE test_old_settings_mac.mm)
    set_source_files_properties(test_old_settings_mac.mm PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti")
endif()
# Time formatting tests pin a zone so "local time" is reproducible anywhere.
msga_add_test(msga_base_tests COMMAND msga_base_tests TZ)

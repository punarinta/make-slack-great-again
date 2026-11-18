# msga_base — UTF-8, JSON, files, logging, time, translations, threads (see README).
add_library(msga_base STATIC
    utf8.cpp
    json.cpp
    file.cpp
    log.cpp
    time.cpp
    locale_names.cpp
    i18n.cpp
    str.cpp
    mime.cpp
    crypto.cpp
    process.cpp
    observers.cpp
    old_settings.cpp
    thread.cpp
)
# The keychain (secret.h): macOS has one; elsewhere the settings store of
# earlier versions holds the credentials, as it always did (old_settings.h: an INI file on
# Linux, the registry on Windows).
if(MSGA_PORT)
    # A port (src/port): files, children and secrets through its C contract;
    # file.cpp and process.cpp keep their OS-neutral halves.
    target_sources(msga_base PRIVATE file_port.cpp process_port.cpp secret_port.cpp)
    target_link_libraries(msga_base PUBLIC msga_port)
elseif(APPLE)
    # Objective-C++ only by file type (it uses the C API); OBJCXX sources do
    # not get msga_flags' C++-only -fno-exceptions/-fno-rtti, hence repeated.
    enable_language(OBJCXX)
    target_sources(msga_base PRIVATE secret_mac.mm old_settings_mac.mm cfstr.cpp)
    set_source_files_properties(secret_mac.mm old_settings_mac.mm PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti")
    target_link_libraries(msga_base PUBLIC "-framework Security" "-framework CoreFoundation")
elseif(WIN32)
    target_sources(msga_base PRIVATE secret_settings.cpp old_settings_win.cpp winstr.cpp)
else()
    target_sources(msga_base PRIVATE secret_settings.cpp)
endif()
target_include_directories(msga_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(msga_base PUBLIC msga_flags prim)
# json.cpp stays -Os: measured on an 8.5 MB Slack-shaped array, -O2 parses at
# 1010 MB/s vs 633 MB/s but doubles the object (27.5 KB vs 13.6 KB). A 1 MB
# payload is ~1.6 ms at -Os; that is not worth 14 KB.
if(MSGA_PORT)
    # Threads are the port's (base/thread.cpp).
elseif(WIN32)
    target_link_libraries(msga_base PUBLIC kernel32 advapi32 bcrypt)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(msga_base PUBLIC Threads::Threads)
endif()

# msga_app_spell — composer spell checking (spell.h): the word rules, the
# checker, and the OS's own checker where there is one — NSSpellChecker
# (AppKit) on macOS, ISpellChecker (spellcheck.h, COM) on Windows. Linux has
# no system service a static binary can use (Enchant and Sonnet dlopen their
# providers), so it builds the vendored Hunspell (third_party/hunspell/README:
# trimmed to ~140 KB of the binary) and reads the distribution's dictionaries
# at runtime. Nothing of it is bundled: no dictionaries, no Unicode tables.
add_library(msga_app_spell STATIC
    spell_words.cpp
    spell_checker.cpp
    spell_names.cpp
)
target_include_directories(msga_app_spell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(msga_app_spell PUBLIC msga_app_core msga_text plat msga_flags)

# A port build takes the Hunspell branch on any OS (backend_linux.cpp's
# MSGA_PORT parts: the app's own dictionaries, msga's Unicode tables).
if(APPLE AND NOT MSGA_PORT)
    enable_language(OBJCXX)
    target_sources(msga_app_spell PRIVATE backend_mac.mm)
    # OBJCXX sources do not get msga_flags' C++-only flags.
    set_source_files_properties(backend_mac.mm PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti")
    target_link_libraries(msga_app_spell PRIVATE "-framework AppKit")
elseif(WIN32 AND NOT MSGA_PORT)
    target_sources(msga_app_spell PRIVATE backend_win.cpp)
    target_link_libraries(msga_app_spell PRIVATE ole32)
else()
    # Only this branch compiles Hunspell; size-optimised because spell() takes about a
    # microsecond either way, hidden visibility so nothing exports (and so
    # keeps) its API, the morphology functions msga never calls included.
    set(_hs ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/hunspell)
    add_library(msga_hunspell STATIC
        ${_hs}/affentry.cxx
        ${_hs}/affixmgr.cxx
        ${_hs}/csutil.cxx
        ${_hs}/filemgr.cxx
        ${_hs}/hashmgr.cxx
        ${_hs}/hunspell.cxx
        ${_hs}/hunzip.cxx
        ${_hs}/phonet.cxx
        ${_hs}/replist.cxx
        ${_hs}/suggestmgr.cxx)
    target_include_directories(msga_hunspell SYSTEM PUBLIC ${_hs})
    target_compile_definitions(msga_hunspell
        PUBLIC HUNSPELL_STATIC
        PRIVATE HUNSPELL_HOST_UNICODE HUNSPELL_NO_XML HUNSPELL_NO_IOSTREAM)
    target_compile_options(msga_hunspell PRIVATE -w $<$<NOT:$<CONFIG:Debug>>:${MSGA_SIZE_OPT}>
        -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections
        # RTTI stays, here only (a PEOS build too: libc++abi has the type_info
        # classes): AffixMgr builds its trees with dynamic_cast.
        -fno-exceptions
        $<$<CONFIG:MinSizeRel>:-fno-asynchronous-unwind-tables -fno-unwind-tables>)

    target_sources(msga_app_spell PRIVATE backend_linux.cpp)
    target_link_libraries(msga_app_spell PRIVATE msga_hunspell)
endif()

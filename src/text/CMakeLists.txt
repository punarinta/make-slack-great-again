# text — font discovery + fallback, shaping, glyph cache, rich layout (see text.h).
#
# Own font index (no fontconfig), HarfBuzz shaping, FreeType raster. Only core
# APIs are used (no hb-ft, no hb-glib/icu, no FreeType modules beyond
# sfnt/truetype/cff/autofit/smooth): Linux dev builds link the distribution's
# libraries through pkg-config; macOS, Windows, the static Linux release and
# the BSDs build trimmed copies (font_libs.cmake). On the BSDs the packaged
# HarfBuzz drags glib, graphite2 and gettext into every link (and into a
# cross build's sysroot); the source build needs nothing but a compiler.
# A port build too: the core reaches the OS only through the port contract,
# never a system library.
if(APPLE OR WIN32 OR MSGA_STATIC OR CMAKE_SYSTEM_NAME MATCHES "BSD" OR MSGA_PORT)
    set(_bundled_default ON)
else()
    set(_bundled_default OFF)
endif()
option(MSGA_BUNDLED_FONT_LIBS "Build FreeType and HarfBuzz from source (font_libs.cmake)" ${_bundled_default})
if(MSGA_BUNDLED_FONT_LIBS)
    include(font_libs.cmake)
    set(MSGA_FT_TARGET msga_freetype)
    set(MSGA_HB_TARGET msga_harfbuzz)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MSGA_FT REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(MSGA_HB REQUIRED IMPORTED_TARGET GLOBAL harfbuzz) # GLOBAL: tests/text links it too
    set(MSGA_FT_TARGET PkgConfig::MSGA_FT)
    set(MSGA_HB_TARGET PkgConfig::MSGA_HB)
endif()
set(MSGA_HB_TARGET ${MSGA_HB_TARGET} PARENT_SCOPE) # text_tests calls HarfBuzz itself

add_library(msga_text STATIC
    colr.cpp
    font_index.cpp
    fonts.cpp
    glyph_cache.cpp
    layout.cpp
    unicode.cpp
)
target_include_directories(msga_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(msga_text PUBLIC msga_gfx msga_flags
    PRIVATE msga_base ${MSGA_FT_TARGET} ${MSGA_HB_TARGET})
# The layoutBuilds()/layoutTextOwned() test counters: test and demo builds only.
if(PLAT_TEST_HOOKS)
    target_compile_definitions(msga_text PRIVATE MSGA_TEXT_COUNTERS)
endif()

# ── Gallery ─────────────────────────────────────────────────────────────────
# A dev binary; the tests are in tests/text.
add_executable(text_gallery gallery/gallery.cpp)
target_link_libraries(text_gallery PRIVATE msga_text plat)
if(COMMAND msga_size_map)
    msga_size_map(text_gallery)
endif()

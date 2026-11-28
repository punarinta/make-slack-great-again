# gfx — CPU painter, image decoding, icons (see gfx.h).

# stb_image: third-party code gets its own flags (no -Wall noise), always
# size-optimised, no unwind tables (nothing unwinds through C).
add_library(msga_stb STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb/stb_image.c)
target_include_directories(msga_stb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
target_compile_options(msga_stb PRIVATE ${MSGA_SIZE_OPT} -ffunction-sections -fdata-sections -fvisibility=hidden -w
    -fno-asynchronous-unwind-tables -fno-unwind-tables)

add_library(msga_gfx STATIC
    core.cpp
    blend.cpp
    shapes.cpp
    cover.cpp
    raster.cpp
    image.cpp
    icons.cpp
    icons_generated.cpp
    svg.cpp
)
# renderSvg() is gfx's own renderer on every OS (decided 2026-09-30): the
# Direct2D/NSImage paths were measured and dropped — same pixels everywhere,
# and macOS CoreSVG drew display="none"/hidden elements. 25 KB buys that.
target_include_directories(msga_gfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(msga_gfx PUBLIC msga_flags PRIVATE msga_stb msga_base)
# blend.cpp holds the per-pixel loops (span blending, coverage rows, image
# sampling, the scanline rasterizer): every painted pixel goes through them
# each frame, so it is the one file where -O2 pays for its size (~8 KB more,
# translucent fills ~4x faster). Everything else stays at the size level.
set_source_files_properties(blend.cpp PROPERTIES COMPILE_OPTIONS "-O2")

option(MSGA_GFX_WEBP "Decode WebP (still and animated) through the system libwebp" OFF)
if(MSGA_GFX_WEBP)
    find_package(PkgConfig REQUIRED)
    # GIPHY GIFs can come back as animated WebP, not GIF
    # both needed to actually play one, not just show its first frame:
    # - libwebp: still frames (WebPDecodeRGBA)
    # - libwebpdemux: animated frames (WebPAnimDecoder)
    # libwebp, not the decode-only libwebpdecoder: libwebpdemux depends on
    # libwebp itself, so libwebpdecoder would load a second copy of the
    # decoder (a static link only pulls in the decoder's objects).
    pkg_check_modules(MSGA_WEBP REQUIRED IMPORTED_TARGET libwebp libwebpdemux)
    # PUBLIC: gfx_tests' own #ifdef MSGA_GFX_WEBP branches (gfx_tests.cpp)
    # need to see this too, or they silently test the OFF behaviour even
    # when webp is actually linked in -- caught live building this.
    target_compile_definitions(msga_gfx PUBLIC MSGA_GFX_WEBP)
    target_link_libraries(msga_gfx PRIVATE PkgConfig::MSGA_WEBP)
endif()

# Regenerate the icon tables from ../../gfx/ui/*.svg plus the tray plane and the colour logo (checked in, so normal
# builds need no Python): cmake --build <dir> --target icons
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_target(icons
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../tools/icons.py
                ${CMAKE_CURRENT_SOURCE_DIR}/../../gfx/ui ${CMAKE_CURRENT_SOURCE_DIR}
                Tray=${CMAKE_CURRENT_SOURCE_DIR}/../../gfx/icon_tray.svg
                Logo=color:${CMAKE_CURRENT_SOURCE_DIR}/../../gfx/icon.svg
        COMMENT "Generating gfx icon tables"
        VERBATIM)
endif()

# ── Gallery ─────────────────────────────────────────────────────────────────
# A dev binary over the test media (tests/assets) and the repository's art.
# The tests are in tests/gfx.
add_executable(gfx_gallery gallery/gallery.cpp)
target_link_libraries(gfx_gallery PRIVATE msga_gfx plat)
target_compile_definitions(gfx_gallery PRIVATE
    MSGA_TEST_ASSETS="${MSGA_TEST_ASSETS}"
    GFX_REPO_ART="${CMAKE_CURRENT_SOURCE_DIR}/../../gfx")
if(COMMAND msga_size_map)
    msga_size_map(gfx_gallery)
endif()


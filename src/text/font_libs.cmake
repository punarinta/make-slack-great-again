# FreeType and HarfBuzz built here (MSGA_BUNDLED_FONT_LIBS), from pinned
# release tarballs like mbedTLS (net/posix/posix.cmake): what the macOS and
# Windows builds always use (neither system has them; no Homebrew or MSYS2
# libraries in a release), and the static Linux release. Linux dev builds
# take the distribution's through pkg-config.
#
# Trimmed to what text/ reaches; --gc-sections drops the rest of each:
#   FreeType  TrueType/OpenType + CFF, the auto-hinter, the AA renderer
#             (font_libs/msga_ftmodule.h, msga_ftoption.h). PNG glyphs (colour
#             emoji) decode through msga's stb_image (font_libs/pngshim_stb.c),
#             so no libpng or zlib.
#   HarfBuzz  the single-file build (src/harfbuzz.cc) without what text/
#             never calls (drawing, colour, math, name and style APIs, buffer
#             debugging; Apple's AAT tables off Apple, where fonts are
#             OpenType). Shaping, variations, metrics, CFF and legacy kern stay.
#
# Offline builds: -DFETCHCONTENT_SOURCE_DIR_MSGA_FREETYPE=<unpacked
# freetype-2.14.3>, likewise MSGA_HARFBUZZ for harfbuzz-12.2.0.
include(FetchContent)
FetchContent_Declare(msga_freetype
    URL https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz
    URL_HASH SHA256=36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake-here)
FetchContent_Declare(msga_harfbuzz
    URL https://github.com/harfbuzz/harfbuzz/releases/download/12.2.0/harfbuzz-12.2.0.tar.xz
    URL_HASH SHA256=ecb603aa426a8b24665718667bda64a84c1504db7454ee4cadbd362eea64e545
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake-here)
FetchContent_MakeAvailable(msga_freetype msga_harfbuzz)
set(_ft ${msga_freetype_SOURCE_DIR})
set(_hb ${msga_harfbuzz_SOURCE_DIR})
set(_cfg ${CMAKE_CURRENT_SOURCE_DIR}/font_libs)

# Third-party code: its own flags, no warnings, size-optimised, no unwind
# tables (nothing unwinds through it).
set(_third_party_flags ${MSGA_SIZE_OPT} -w -ffunction-sections -fdata-sections
    -fvisibility=hidden -fno-asynchronous-unwind-tables -fno-unwind-tables)

# ── FreeType ──────────────────────────────────────────────────────────────────
# The sfnt module file by file (upstream's sfnt.c #includes pngshim.c, which
# pngshim_stb.c replaces); the other modules through their one-file builds.
set(_sfnt sfdriver sfobjs sfwoff sfwoff2 ttbdf ttcmap ttcolr ttcpal ttsvg ttgpos
    ttkern ttload ttmtx ttpost ttsbit woff2tags)
list(TRANSFORM _sfnt PREPEND ${_ft}/src/sfnt/)
list(TRANSFORM _sfnt APPEND .c)
add_library(msga_freetype STATIC
    ${_ft}/src/base/ftsystem.c
    ${_ft}/src/base/ftinit.c
    ${_ft}/src/base/ftdebug.c
    ${_ft}/src/base/ftbase.c
    ${_ft}/src/base/ftbbox.c
    ${_ft}/src/base/ftbitmap.c
    ${_ft}/src/base/ftglyph.c
    ${_ft}/src/base/ftmm.c
    ${_ft}/src/base/ftsynth.c
    ${_ft}/src/truetype/truetype.c
    ${_ft}/src/cff/cff.c
    ${_ft}/src/psaux/psaux.c
    ${_ft}/src/psnames/psnames.c
    ${_ft}/src/pshinter/pshinter.c
    ${_ft}/src/autofit/autofit.c
    ${_ft}/src/smooth/smooth.c
    ${_sfnt}
    ${_cfg}/pngshim_stb.c)
target_include_directories(msga_freetype SYSTEM PUBLIC ${_cfg} ${_ft}/include)
target_include_directories(msga_freetype PRIVATE ${_ft}/src/sfnt ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
target_compile_definitions(msga_freetype
    PUBLIC FT_CONFIG_OPTIONS_H="msga_ftoption.h" FT_CONFIG_MODULES_H="msga_ftmodule.h"
    PRIVATE FT2_BUILD_LIBRARY)
target_compile_options(msga_freetype PRIVATE ${_third_party_flags})
target_link_libraries(msga_freetype PRIVATE msga_stb)

# ── HarfBuzz ──────────────────────────────────────────────────────────────────
set(_hb_off HB_NO_BUFFER_MESSAGE HB_NO_BUFFER_SERIALIZE HB_NO_BUFFER_VERIFY
    HB_NO_COLOR HB_NO_DRAW HB_NO_PAINT HB_NO_MATH HB_NO_META HB_NO_NAME HB_NO_STYLE
    HB_NO_VERTICAL HB_NO_HINTING HB_NO_FACE_COLLECT_UNICODES HB_NO_LAYOUT_COLLECT_GLYPHS
    HB_NO_LAYOUT_FEATURE_PARAMS HB_NO_LAYOUT_UNUSED HB_NO_OT_FONT_GLYPH_NAMES HB_NO_GETENV
    HB_NO_SETLOCALE HB_NO_OPEN HB_NO_MMAP HB_NO_ERRNO HB_NO_ATEXIT HB_DISABLE_DEPRECATED
    HB_NO_CUBIC_GLYF HB_NO_VAR_COMPOSITES HB_NO_VAR_HVF)
if(APPLE)
    # Apple's size-dependent tracking requires HarfBuzz's STAT support.
    list(REMOVE_ITEM _hb_off HB_NO_STYLE)
endif()
if(NOT APPLE)
    # Apple Color Emoji's ZWJ sequences are AAT (morx); fonts elsewhere are OpenType.
    list(APPEND _hb_off HB_NO_AAT)
endif()
add_library(msga_harfbuzz STATIC ${_hb}/src/harfbuzz.cc)
target_include_directories(msga_harfbuzz SYSTEM PUBLIC ${_hb}/src)
target_compile_definitions(msga_harfbuzz PRIVATE ${_hb_off} $<$<NOT:$<BOOL:${WIN32}>>:HAVE_PTHREAD>)
target_compile_options(msga_harfbuzz PRIVATE ${_third_party_flags}
    -fno-exceptions -fno-rtti -fvisibility-inlines-hidden)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(msga_harfbuzz PUBLIC Threads::Threads)
endif()

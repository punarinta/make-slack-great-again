/* FreeType's options for the bundled build (FT_CONFIG_OPTIONS_H, see
 * ../font_libs.cmake): the stock set, less what msga never reaches. Fonts
 * come from msga's own index (text/font_index.cpp: .ttf/.otf/.ttc/.otc,
 * loaded from memory), so no compressed, resource-fork or Type 1 support. */
#ifndef MSGA_FTOPTION_H_
#define MSGA_FTOPTION_H_

#include <freetype/config/ftoption.h>

#undef FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES /* FREETYPE_PROPERTIES */
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef FT_CONFIG_OPTION_USE_BZIP2
#undef FT_CONFIG_OPTION_USE_BROTLI /* WOFF2 */
#undef FT_CONFIG_OPTION_USE_HARFBUZZ
#undef FT_CONFIG_OPTION_ADOBE_GLYPH_LIST /* only synthesises Type 1 charmaps */
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK
#undef FT_CONFIG_OPTION_INCREMENTAL
#undef FT_CONFIG_OPTION_SVG /* OT-SVG glyphs need an external renderer anyway */
#undef TT_CONFIG_OPTION_BDF

/* Colour emoji (CBDT, sbix) are PNG: decoded through msga's stb_image
 * (pngshim_stb.c), not libpng. */
#define FT_CONFIG_OPTION_USE_PNG
/* LCD filtering, as distributions build it. */
#define FT_CONFIG_OPTION_SUBPIXEL_RENDERING

#endif /* MSGA_FTOPTION_H_ */

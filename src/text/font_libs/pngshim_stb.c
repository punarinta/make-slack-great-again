/* FreeType's PNG hook (upstream src/sfnt/pngshim.c, replaced in the bundled
 * build, see ../font_libs.cmake) over msga's stb_image instead of libpng:
 * colour emoji glyphs (CBDT, sbix) are PNG, and stb_image is in the binary
 * already, so libpng and zlib need not be. Same contract as upstream's
 * Load_SBit_Png: premultiplied BGRA into the slot's bitmap, at an offset
 * into an existing one when composing. */
#include <ft2build.h>
#include <freetype/internal/ftdebug.h>
#include <freetype/internal/ftobjs.h>

#include "pngshim.h"
#include "sferrors.h"

#include "stb/stb_config.h"
#include "stb/stb_image.h"

#if defined(TT_CONFIG_OPTION_EMBEDDED_BITMAPS) && defined(FT_CONFIG_OPTION_USE_PNG)

/* As upstream (after cairo-png.c). */
static unsigned int multiply_alpha(unsigned int alpha, unsigned int color) {
    unsigned int temp = alpha * color + 0x80;
    return (temp + (temp >> 8)) >> 8;
}

FT_LOCAL_DEF(FT_Error)
Load_SBit_Png(
    FT_GlyphSlot    slot,
    FT_Int          x_offset,
    FT_Int          y_offset,
    FT_Int          pix_bits,
    TT_SBit_Metrics metrics,
    FT_Memory       memory,
    FT_Byte        *data,
    FT_UInt         png_len,
    FT_Bool         populate_map_and_metrics,
    FT_Bool         metrics_only
) {
    FT_Bitmap *map   = &slot->bitmap;
    FT_Error   error = FT_Err_Ok;
    int        w, h, comp;
    stbi_uc   *rgba;
    FT_Int     x, y;

    FT_UNUSED(memory);
    if (x_offset < 0 || y_offset < 0)
        return FT_THROW(Invalid_Argument);
    if (!populate_map_and_metrics && ((FT_UInt)x_offset + metrics->width > map->width ||
                                      (FT_UInt)y_offset + metrics->height > map->rows ||
                                      pix_bits != 32 || map->pixel_mode != FT_PIXEL_MODE_BGRA))
        return FT_THROW(Invalid_Argument);
    if (png_len > 0x7FFFFFFF || !stbi_info_from_memory(data, (int)png_len, &w, &h, &comp))
        return FT_THROW(Invalid_File_Format);
    /* A size that disagrees with the metrics: skipped, as upstream does. */
    if (!populate_map_and_metrics && (w != metrics->width || h != metrics->height))
        return FT_Err_Ok;

    if (populate_map_and_metrics) {
        /* reject too large bitmaps similarly to the rasterizer */
        if (h > 0x7FFF || w > 0x7FFF)
            return FT_THROW(Array_Too_Large);
        metrics->width  = (FT_UShort)w;
        metrics->height = (FT_UShort)h;
        map->width      = metrics->width;
        map->rows       = metrics->height;
        map->pixel_mode = FT_PIXEL_MODE_BGRA;
        map->pitch      = (int)(map->width * 4);
        map->num_grays  = 256;
    }
    if (metrics_only)
        return FT_Err_Ok;

    /* Palette, grey, tRNS, 16-bit and interlaced images all come out RGBA8. */
    rgba = stbi_load_from_memory(data, (int)png_len, &w, &h, &comp, 4);
    if (!rgba)
        return FT_THROW(Invalid_File_Format);
    if (populate_map_and_metrics) {
        /* this doesn't overflow: 0x7FFF * 0x7FFF * 4 < 2^32 */
        error = ft_glyphslot_alloc_bitmap(slot);
        if (error)
            goto Exit;
    }
    for (y = 0; y < h; y++) {
        const stbi_uc *src = rgba + (size_t)y * (size_t)w * 4;
        FT_Byte       *dst = map->buffer + (y_offset + y) * map->pitch + x_offset * 4;
        for (x = 0; x < w; x++, src += 4, dst += 4) {
            unsigned int r = src[0], g = src[1], b = src[2], a = src[3];
            if (a == 0) {
                r = g = b = 0;
            } else if (a != 0xFF) {
                r = multiply_alpha(a, r);
                g = multiply_alpha(a, g);
                b = multiply_alpha(a, b);
            }
            dst[0] = (FT_Byte)b;
            dst[1] = (FT_Byte)g;
            dst[2] = (FT_Byte)r;
            dst[3] = (FT_Byte)a;
        }
    }
Exit:
    stbi_image_free(rgba);
    return error;
}

#else

typedef int pngshim_dummy_;

#endif

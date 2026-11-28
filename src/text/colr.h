// COLRv1 colour glyphs (Noto Color Emoji as Fedora ships it, Noto-COLRv1.ttf):
// FreeType hands out the paint graph but leaves the drawing to the client,
// so this walks it — layers, glyph clips, solid/linear/radial/sweep fills,
// transforms, compositing — into premultiplied ARGB. Internal to text/.
#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>

namespace gfx {
class Bitmap;
}

namespace text::fonts {

// Paints COLRv1 glyph `glyph` at `ppem` pixels per em, the pen `dx` pixels
// right of the origin (subpixel phase). The result lands in *out, its top-left
// pixel at (*left, *top) relative to the pen (top: above the baseline).
// False when the face has no COLRv1 paint for the glyph or it cannot be drawn.
bool renderColrV1(
    FT_Library   lib,
    FT_Face      face,
    uint32_t     glyph,
    float        ppem,
    float        dx,
    gfx::Bitmap *out,
    int         *left,
    int         *top
);

} // namespace text::fonts

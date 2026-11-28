// Pixel helpers the backends share for icons and drag images (plat::Image is
// premultiplied ARGB32; plat can't use gfx).
#pragma once

#include "plat/plat.h"

#include <cstdint>

namespace plat::core {

// Premultiplied ARGB32 to straight ARGB32 (rounded; alpha 0 is all zero), the
// format _NET_WM_ICON, SNI pixmaps and Win32 icon masks want.
uint32_t unpremultiply(uint32_t argb);

// src resampled to w×h: an area average when shrinking (each output pixel
// mixes the fractional source rectangle it covers), bilinear when growing.
// Premultiplied channels mix correctly as they are. A copy at the same size;
// w×h of transparent pixels for an empty src. The result's scale is 1.
Image scaleImage(const Image &src, int w, int h);

// The image of `sizes` for a px×px slot: the smallest one at least that big,
// else the largest (empty and malformed ones are skipped; null when none is
// left). Between equally big ones the Image::scale closest to `scale` wins
// (when `scale` > 0). `onlyAtScale`: when any image is drawn for `scale`,
// only those are considered.
const Image *pickImage(const std::vector<Image> &sizes, int px, double scale, bool onlyAtScale);

} // namespace plat::core

// Fitting pictures into avatar, thumbnail and icon shapes: the cover crop and
// the anti-aliased masks the image caches, notifications and tray share.
#include "gfx/gfx.h"

#include <algorithm>
#include <cmath>

namespace gfx {

namespace {

// A premultiplied pixel scaled by coverage k (every channel, alpha too).
uint32_t scaled(uint32_t p, float k) {
    uint32_t q = 0;
    for (int sh = 0; sh < 32; sh += 8)
        q |= uint32_t(float((p >> sh) & 0xff) * k + 0.5f) << sh;
    return q;
}

} // namespace

Bitmap coverResize(const BitmapView &src, int w, int h) {
    if (!src.pixels || src.width <= 0 || src.height <= 0 || w <= 0 || h <= 0)
        return {};
    // The centred part of src with the target's aspect, then one resize.
    int cw = src.width, ch = src.height;
    if (int64_t(src.width) * h > int64_t(src.height) * w)
        cw = std::clamp(int(std::lround(double(src.height) * w / h)), 1, src.width);
    else
        ch = std::clamp(int(std::lround(double(src.width) * h / w)), 1, src.height);
    const int        ox = (src.width - cw) / 2, oy = (src.height - ch) / 2;
    const BitmapView crop = {
        src.pixels + size_t(oy) * size_t(src.stride) + size_t(ox), cw, ch, src.stride
    };
    if (cw != w || ch != h)
        return resize(crop, w, h);
    Bitmap out(w, h);
    for (int y = 0; y < h; ++y)
        std::copy_n(
            crop.pixels + size_t(y) * size_t(crop.stride), size_t(w), out.pixels() + size_t(y) * w
        );
    return out;
}

bool renderSvgCover(std::string_view svg, int w, int h, Bitmap *out) {
    float sw = 0, sh = 0;
    if (!svgSize(svg, &sw, &sh) || sw <= 0 || sh <= 0)
        return false;
    const float k = w <= 0 && h <= 0 ? 1.f : std::max(float(w) / sw, float(h) / sh);
    return renderSvg(
        svg,
        std::max({w, 1, int(std::lround(sw * k))}),
        std::max({h, 1, int(std::lround(sh * k))}),
        out
    );
}

void maskRoundedRect(Bitmap &b, float radius) {
    const int   w = b.width(), h = b.height();
    const float r = std::min(radius, float(std::min(w, h)) / 2.f);
    if (r <= 0)
        return;
    uint32_t *px = b.pixels();
    // Only the corner squares change: the first and last rc rows and columns.
    const int rc = std::min(w, int(std::ceil(r)));
    for (int y = 0; y < h; ++y) {
        const float fy = float(y) + 0.5f;
        const float cy = fy < r ? r : fy > float(h) - r ? float(h) - r : fy;
        if (cy == fy)
            continue; // a middle row: fully inside
        for (int x = 0; x < w; x = x + 1 == rc ? std::max(x + 1, w - rc) : x + 1) {
            const float fx = float(x) + 0.5f;
            const float cx = fx < r ? r : fx > float(w) - r ? float(w) - r : fx;
            if (cx == fx || cy == fy)
                continue; // not in a corner square: fully inside
            const float cov = std::clamp(r - std::hypot(fx - cx, fy - cy) + 0.5f, 0.f, 1.f);
            if (cov < 1) {
                uint32_t &p = px[size_t(y) * size_t(w) + size_t(x)];
                p           = scaled(p, cov);
            }
        }
    }
}

void clearDisc(Bitmap &b, float cx, float cy, float r) {
    uint32_t   *px    = b.pixels();
    // Only pixels whose centre is within r + 0.5 of the centre change.
    const float reach = r + 0.5f;
    const int   y0    = std::max(0, int(std::floor(cy - reach)));
    const int   y1    = std::min(b.height(), int(std::ceil(cy + reach)) + 1);
    const int   x0    = std::max(0, int(std::floor(cx - reach)));
    const int   x1    = std::min(b.width(), int(std::ceil(cx + reach)) + 1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float k = std::clamp(
                std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy) - r + 0.5f, 0.f, 1.f
            );
            if (k < 1) {
                uint32_t &p = px[size_t(y) * size_t(b.width()) + size_t(x)];
                p           = scaled(p, k);
            }
        }
}

} // namespace gfx

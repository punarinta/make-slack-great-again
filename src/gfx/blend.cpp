// The hot loops: span blending, analytic coverage rows, gradients, image
// sampling and the scanline rasterizer.
//
// Built with -O2 (see CMakeLists): every painted pixel goes through these
// loops each frame, so this file is the one place speed beats size (-O2 costs
// ~8 KB here and makes translucent fills ~4× faster than -Os). Shape setup,
// shadows, resize and stroking live in -Os files.
//
// The rasterizer samples 16 sub-scanlines per pixel row and computes exact
// horizontal coverage on each, applying the non-zero rule per sub-scanline.
// Unlike signed-area accumulation (font-rs style), overlapping pieces of one
// fill union exactly — which the capsule-union stroker relies on.
#include "gfx/internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace gfx {

namespace {

// Box-filtered coverage of the pixel centred at (cx, cy). Straight edges are
// exact (pixel ∩ rect area); corners use the distance to the corner circle,
// which is symmetric under mirroring, so shapes never look lopsided.
inline float rrCov(const RR &q, float cx, float cy) {
    const float h = clamp01(std::fmin(cx + 0.5f, q.x1) - std::fmax(cx - 0.5f, q.x0));
    const float v = clamp01(std::fmin(cy + 0.5f, q.y1) - std::fmax(cy - 0.5f, q.y0));
    float       c = h * v;
    if (q.r > 0) {
        const float qx = std::fmax(q.x0 + q.r - cx, cx - (q.x1 - q.r));
        const float qy = std::fmax(q.y0 + q.r - cy, cy - (q.y1 - q.r));
        if (qx > 0 && qy > 0)
            c = std::fmin(c, clamp01(q.r + 0.5f - std::sqrt(qx * qx + qy * qy)));
    }
    return c;
}

// Linear interpolation of premultiplied pixels, f in 0..256. Equal inputs
// come back unchanged (no truncation drift).
inline uint32_t lerpPx(uint32_t a, uint32_t b, uint32_t f) {
    const uint32_t g  = 256 - f;
    const uint32_t rb = (((a & 0x00ff00ffu) * g + (b & 0x00ff00ffu) * f) >> 8) & 0x00ff00ffu;
    const uint32_t ag = (((a >> 8) & 0x00ff00ffu) * g + ((b >> 8) & 0x00ff00ffu) * f) & 0xff00ff00u;
    return rb | ag;
}

// Only the few edge pixels of a row need the full formula; one out-of-line
// copy keeps -O2 from duplicating it per call site.
[[gnu::noinline]] void edgeCov(const RR &q, int xa, int xb, float cy, uint8_t *out) {
    for (int x = xa; x < xb; ++x)
        *out++ = toByte(rrCov(q, float(x) + 0.5f, cy));
}

} // namespace

bool rrRow(const RR &q, int y, int x0, int x1, uint8_t *out, int *full) {
    const int n = x1 - x0;
    if (full)
        full[0] = full[1] = x1;
    if (n <= 0)
        return false;
    if (float(y + 1) <= q.y0 || float(y) >= q.y1) {
        std::memset(out, 0, size_t(n));
        return false;
    }
    const int a = clampi(floori(q.x0), x0, x1), b = clampi(ceili(q.x1), x0, x1);
    // Columns certainly fully covered on this row.
    int       fa = b, fb = b;
    if (float(y) >= q.y0 && float(y + 1) <= q.y1) {
        const bool mid = float(y) >= q.y0 + q.r && float(y + 1) <= q.y1 - q.r;
        fa             = clampi(ceili(mid ? q.x0 : q.x0 + q.r), a, b);
        fb             = clampi(floori(mid ? q.x1 : q.x1 - q.r), a, b);
        if (fb < fa)
            fa = fb = b;
    }
    const float cy = float(y) + 0.5f;
    std::memset(out, 0, size_t(a - x0));
    edgeCov(q, a, fa, cy, out + (a - x0));
    std::memset(out + (fa - x0), 255, size_t(fb - fa));
    edgeCov(q, fb, b, cy, out + (fb - x0));
    std::memset(out + (b - x0), 0, size_t(x1 - b));
    if (full)
        full[0] = fa, full[1] = fb;
    return fa == x0 && fb == x1;
}

// ── Spans ───────────────────────────────────────────────────────────────────
const uint8_t *PainterImpl::clipMask(Painter &p, int y, int x0, int x1, const uint8_t *cov) {
    if (p._s.roundClip < 0)
        return cov;
    uint8_t  *m   = nullptr;
    uint8_t  *tmp = row8(p, 3);
    const int n   = x1 - x0;
    for (int i = p._s.roundClip; i >= 0; i = p._roundClips[size_t(i)].parent) {
        const auto &c = p._roundClips[size_t(i)];
        const RR    q{c.x, c.y, c.x + c.w, c.y + c.h, c.r};
        // Inside the clip's straight middle: nothing to compute.
        if (float(y) >= q.y0 + q.r && float(y + 1) <= q.y1 - q.r && float(x0) >= q.x0 &&
            float(x1) <= q.x1)
            continue;
        if (rrRow(q, y, x0, x1, tmp))
            continue; // this clip leaves the whole span alone
        if (!m) {
            m = row8(p, 2);
            if (cov)
                for (int k = 0; k < n; ++k)
                    m[k] = uint8_t(div255(uint32_t(cov[k]) * tmp[k]));
            else
                std::memcpy(m, tmp, size_t(n));
        } else {
            for (int k = 0; k < n; ++k)
                m[k] = uint8_t(div255(uint32_t(m[k]) * tmp[k]));
        }
    }
    return m ? m : cov;
}

void PainterImpl::span(Painter &p, int y, int x0, int x1, const uint8_t *cov, uint32_t pm) {
    if (x1 <= x0 || !pm)
        return;
    const uint8_t *m = clipMask(p, y, x0, x1, cov);
    uint32_t *d = p._target.pixels + size_t(y - p._oy) * size_t(p._target.stride) + (x0 - p._ox);
    const int n = x1 - x0;
    if (!m) {
        if ((pm >> 24) == 255) {
            for (int i = 0; i < n; ++i)
                d[i] = pm;
        } else {
            const uint32_t inv = 255 - (pm >> 24);
            for (int i = 0; i < n; ++i)
                d[i] = pm + mulPx(d[i], inv);
        }
        return;
    }
    // The glyph path for every text pixel: a fully covered pixel of an
    // opaque colour is a plain store.
    const bool opaque = (pm >> 24) == 255;
    for (int i = 0; i < n; ++i) {
        const uint32_t a = m[i];
        if (!a)
            continue;
        if (a == 255)
            d[i] = opaque ? pm : over(pm, d[i]);
        else
            d[i] = over(mulPx(pm, a), d[i]);
    }
}

void PainterImpl::spanPx(
    Painter &p, int y, int x0, int x1, const uint8_t *cov, const uint32_t *src, uint32_t alpha
) {
    if (x1 <= x0 || !alpha)
        return;
    const uint8_t *m = clipMask(p, y, x0, x1, cov);
    uint32_t *d = p._target.pixels + size_t(y - p._oy) * size_t(p._target.stride) + (x0 - p._ox);
    const int n = x1 - x0;
    for (int i = 0; i < n; ++i) {
        uint32_t s = src[i];
        uint32_t a = m ? (alpha == 255 ? m[i] : div255(uint32_t(m[i]) * alpha)) : alpha;
        if (!s || !a)
            continue;
        if (a != 255)
            s = mulPx(s, a);
        d[i] = (s >> 24) == 255 ? s : over(s, d[i]);
    }
}

void PainterImpl::fillRR(Painter &p, const RR &q, const RR *inner, uint32_t pm) {
    if (!pm)
        return;
    const auto &s  = p._s;
    const int   y0 = std::max(floori(q.y0), s.clipY0), y1 = std::min(ceili(q.y1), s.clipY1);
    const int   x0 = std::max(floori(q.x0), s.clipX0), x1 = std::min(ceili(q.x1), s.clipX1);
    if (x1 <= x0 || y1 <= y0)
        return;
    uint8_t  *cov = row8(p, 0);
    uint8_t  *in  = row8(p, 1);
    const int n   = x1 - x0;
    for (int y = y0; y < y1; ++y) {
        const bool full = rrRow(q, y, x0, x1, cov);
        if (!inner) {
            span(p, y, x0, x1, full ? nullptr : cov, pm);
            continue;
        }
        // A border: the columns the inner shape fully covers are left alone,
        // so only the two edge runs are subtracted and blended, not the
        // whole interior.
        int hole[2];
        rrRow(*inner, y, x0, x1, in, hole);
        const int ha = hole[0] - x0, hb = hole[1] - x0;
        for (int i = 0; i < ha; ++i)
            cov[i] = cov[i] > in[i] ? uint8_t(cov[i] - in[i]) : 0;
        for (int i = hb; i < n; ++i)
            cov[i] = cov[i] > in[i] ? uint8_t(cov[i] - in[i]) : 0;
        span(p, y, x0, hole[0], cov, pm);
        span(p, y, hole[1], x1, cov + hb, pm);
    }
}

void Painter::fillRectGradient(RectF r, PointF a, Color c0, PointF b, Color c1) {
    if (r.w <= 0 || r.h <= 0)
        return;
    const float s = _scale;
    const RR    q = makeRR(
        (r.x + _s.tx) * s, (r.y + _s.ty) * s, (r.right() + _s.tx) * s, (r.bottom() + _s.ty) * s, 0
    );
    const uint32_t p0 = premultiply(c0, _s.opacity), p1 = premultiply(c1, _s.opacity);
    const float    ax = (a.x + _s.tx) * s, ay = (a.y + _s.ty) * s;
    const float    dx = (b.x - a.x) * s, dy = (b.y - a.y) * s;
    const float    len2 = dx * dx + dy * dy;
    const float    kx = len2 > 0 ? dx / len2 : 0, ky = len2 > 0 ? dy / len2 : 0;
    const int      y0 = std::max(floori(q.y0), _s.clipY0), y1 = std::min(ceili(q.y1), _s.clipY1);
    const int      x0 = std::max(floori(q.x0), _s.clipX0), x1 = std::min(ceili(q.x1), _s.clipX1);
    if (x1 <= x0 || y1 <= y0)
        return;
    uint8_t  *cov     = PainterImpl::row8(*this, 0);
    uint32_t *px      = PainterImpl::row32(*this);
    auto      fillRow = [&](float fy) {
        for (int x = x0; x < x1; ++x) {
            const float t = clamp01((float(x) + 0.5f - ax) * kx + fy);
            px[x - x0]    = lerpPx(p0, p1, uint32_t(t * 256.0f + 0.5f));
        }
    };
    // Axis-aligned gradients (nearly all of them) cost one colour per row or
    // one row for the whole rect instead of a lerp per pixel.
    if (ky == 0)
        fillRow(0);
    for (int y = y0; y < y1; ++y) {
        const bool     full = rrRow(q, y, x0, x1, cov);
        const float    fy   = (float(y) + 0.5f - ay) * ky;
        const uint8_t *m    = full ? nullptr : cov;
        if (kx == 0) {
            PainterImpl::span(
                *this, y, x0, x1, m, lerpPx(p0, p1, uint32_t(clamp01(fy) * 256.0f + 0.5f))
            );
            continue;
        }
        if (ky != 0)
            fillRow(fy);
        PainterImpl::spanPx(*this, y, x0, x1, m, px, 255);
    }
}

// ── Masks and pixels ────────────────────────────────────────────────────────
void PainterImpl::maskAt(Painter &p, const Mask8 &m, int mx, int my, uint32_t pm) {
    if (!pm || !m.data)
        return;
    const auto &s  = p._s;
    const int   x0 = std::max(mx, s.clipX0), x1 = std::min(mx + m.width, s.clipX1);
    const int   y0 = std::max(my, s.clipY0), y1 = std::min(my + m.height, s.clipY1);
    for (int y = y0; y < y1; ++y)
        span(p, y, x0, x1, m.data + size_t(y - my) * size_t(m.stride) + (x0 - mx), pm);
}

void Painter::snappedSize(RectF dst, int *w, int *h) const {
    // drawBitmap's own rounding (below), so the sizes always agree.
    const float s = _scale;
    *w = int(std::lround((dst.right() + _s.tx) * s)) - int(std::lround((dst.x + _s.tx) * s));
    *h = int(std::lround((dst.bottom() + _s.ty) * s)) - int(std::lround((dst.y + _s.ty) * s));
}

void Painter::drawBitmap(const BitmapView &src, RectF dst, Sampling smp, float opacity) {
    if (!src.pixels || src.width <= 0 || src.height <= 0 || dst.w <= 0 || dst.h <= 0)
        return;
    const float a = opacity * _s.opacity * 255.0f;
    if (a <= 0)
        return;
    const uint32_t alpha = a >= 255 ? 255 : uint32_t(a + 0.5f);
    // Images snap to whole pixels: a 1:1 image at any position stays sharp.
    const float    s     = _scale;
    const int      ix0   = int(std::lround((dst.x + _s.tx) * s)),
                   iy0   = int(std::lround((dst.y + _s.ty) * s));
    const int      ix1   = int(std::lround((dst.right() + _s.tx) * s));
    const int      iy1   = int(std::lround((dst.bottom() + _s.ty) * s));
    const int      dw = ix1 - ix0, dh = iy1 - iy0;
    if (dw <= 0 || dh <= 0)
        return;
    const int x0 = std::max(ix0, _s.clipX0), x1 = std::min(ix1, _s.clipX1);
    const int y0 = std::max(iy0, _s.clipY0), y1 = std::min(iy1, _s.clipY1);
    if (x1 <= x0 || y1 <= y0)
        return;

    BitmapView sv = src;
    Bitmap     shrunk;
    if (smp == Sampling::Smooth && (src.width > dw || src.height > dh)) {
        shrunk = resize(src, dw, dh);
        sv     = shrunk.view();
    }
    uint32_t *line = PainterImpl::row32(*this);
    if (sv.width == dw && sv.height == dh) {
        for (int y = y0; y < y1; ++y)
            PainterImpl::spanPx(
                *this,
                y,
                x0,
                x1,
                nullptr,
                sv.pixels + size_t(y - iy0) * size_t(sv.stride) + (x0 - ix0),
                alpha
            );
        return;
    }
    const int         n  = x1 - x0;
    std::vector<int> &xi = PainterImpl::scratch(*this).xi;
    xi.resize(size_t(n) * 2);
    const float kx = float(sv.width) / float(dw), ky = float(sv.height) / float(dh);
    const bool  nearest = smp == Sampling::Nearest;
    for (int x = x0; x < x1; ++x) {
        const float u = (float(x - ix0) + 0.5f) * kx - (nearest ? 0 : 0.5f);
        if (nearest) {
            xi[size_t(x - x0) * 2] = clampi(int(u), 0, sv.width - 1);
        } else {
            const int   i              = floori(u);
            const float f              = u - float(i);
            xi[size_t(x - x0) * 2]     = i;
            xi[size_t(x - x0) * 2 + 1] = int(f * 256.0f + 0.5f);
        }
    }
    for (int y = y0; y < y1; ++y) {
        const float v = (float(y - iy0) + 0.5f) * ky - (nearest ? 0 : 0.5f);
        if (nearest) {
            const uint32_t *row =
                sv.pixels + size_t(clampi(int(v), 0, sv.height - 1)) * size_t(sv.stride);
            for (int i = 0; i < n; ++i)
                line[i] = row[xi[size_t(i) * 2]];
        } else {
            const int       j  = floori(v);
            const uint32_t  fy = uint32_t(clampi(int((v - float(j)) * 256.0f + 0.5f), 0, 256));
            const uint32_t *r0 =
                sv.pixels + size_t(clampi(j, 0, sv.height - 1)) * size_t(sv.stride);
            const uint32_t *r1 =
                sv.pixels + size_t(clampi(j + 1, 0, sv.height - 1)) * size_t(sv.stride);
            const int wm = sv.width - 1;
            for (int i = 0; i < n; ++i) {
                const int      xa = xi[size_t(i) * 2];
                const uint32_t fx = uint32_t(clampi(xi[size_t(i) * 2 + 1], 0, 256));
                const int      c0 = clampi(xa, 0, wm), c1 = clampi(xa + 1, 0, wm);
                line[i] = lerpPx(lerpPx(r0[c0], r0[c1], fx), lerpPx(r1[c0], r1[c1], fx), fy);
            }
        }
        PainterImpl::spanPx(*this, y, x0, x1, nullptr, line, alpha);
    }
}

namespace {
constexpr int kSub = 16; // sub-scanlines per pixel row
} // namespace

void shaderRow(const Shader &s, int y, int x0, int x1, uint32_t *out) {
    const float py = float(y) + 0.5f;
    float       gx = s.m.a * (float(x0) + 0.5f) + s.m.c * py + s.m.e;
    float       gy = s.m.b * (float(x0) + 0.5f) + s.m.d * py + s.m.f;
    for (int x = x0; x < x1; ++x, gx += s.m.a, gy += s.m.b) {
        const float ux = gx - s.ax, uy = gy - s.ay;
        float t = s.kind ? std::sqrt(ux * ux + uy * uy) * s.inv : (ux * s.dx + uy * s.dy) * s.inv;
        if (s.spread == 1) { // reflect
            t = std::fabs(t);
            t = t - 2 * std::floor(t * 0.5f);
            if (t > 1)
                t = 2 - t;
        } else if (s.spread == 2) { // repeat
            t = t - std::floor(t);
        }
        *out++ = s.lut[t <= 0 ? 0 : t >= 1 ? 255 : int(t * 255.0f + 0.5f)];
    }
}

void PainterImpl::rasterize(
    Painter                &p,
    const std::vector<Seg> &segs,
    uint32_t                pm,
    FillRule                rule,
    const Shader           *shader,
    uint8_t                *a8
) {
    if (segs.empty() || !pm)
        return;
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (const Seg &s : segs) {
        minX = std::fmin(minX, std::fmin(s.x0, s.x1));
        maxX = std::fmax(maxX, std::fmax(s.x0, s.x1));
        minY = std::fmin(minY, std::fmin(s.y0, s.y1));
        maxY = std::fmax(maxY, std::fmax(s.y0, s.y1));
    }
    const auto &st = p._s;
    const int   y0 = std::max(st.clipY0, int(std::floor(std::fmax(minY, -1e7f))));
    const int   y1 = std::min(st.clipY1, int(std::ceil(std::fmin(maxY, 1e7f))));
    const int   x0 = std::max(st.clipX0, int(std::floor(std::fmax(minX, -1e7f))));
    const int   x1 = std::min(st.clipX1, int(std::ceil(std::fmin(maxX, 1e7f))));
    if (x1 <= x0 || y1 <= y0)
        return;
    const int W = x1 - x0;

    using Edge                = RasterEdge;
    PaintScratch::Data &sc    = scratch(p);
    std::vector<Edge>  &edges = sc.edges;
    edges.clear();
    for (const Seg &s : segs) {
        if (s.y0 == s.y1)
            continue;
        const bool  down = s.y1 > s.y0;
        const float ax = down ? s.x0 : s.x1, ay = down ? s.y0 : s.y1;
        const float bx = down ? s.x1 : s.x0, by = down ? s.y1 : s.y0;
        if (by <= float(y0) || ay >= float(y1))
            continue;
        edges.push_back({ax, (bx - ax) / (by - ay), ay, by, down ? 1 : -1});
    }
    // libc's qsort instead of std::sort: one shared copy instead of an
    // inlined introsort instantiation.
    if (!edges.empty())
        std::qsort(edges.data(), edges.size(), sizeof(Edge), [](const void *a, const void *b) {
            const float ta = static_cast<const Edge *>(a)->top,
                        tb = static_cast<const Edge *>(b)->top;
            return ta < tb ? -1 : ta > tb ? 1 : 0;
        });

    std::vector<float> &cov = sc.cov, &diff = sc.diff;
    cov.assign(size_t(W) + 2, 0.0f);
    diff.assign(size_t(W) + 2, 0.0f);
    // Active edges by value (cache-friendly), kept in the previous
    // sub-scanline's x order so the insertion sort below is ~O(n). Edges not
    // spanning the current sub-scanline sort to the end with cx = +inf.
    using Active              = RasterActive;
    constexpr float      kInf = 3.0e38f;
    std::vector<Active> &act  = sc.act;
    act.clear();
    uint8_t    *out  = row8(p, 0);
    size_t      next = 0;
    const float w    = 1.0f / kSub;
    const float fx0  = float(x0);

    for (int y = y0; y < y1; ++y) {
        size_t n = 0;
        for (size_t i = 0; i < act.size(); ++i) // drop edges that ended
            if (act[i].e.bot > float(y))
                act[n++] = act[i];
        act.resize(n);
        while (next < edges.size() && edges[next].top < float(y + 1))
            act.push_back({kInf, edges[next++]});
        if (act.empty()) {
            if (next >= edges.size())
                break;
            continue;
        }
        int          lo = W, hi = 0;
        Active      *A  = act.data();
        const size_t na = act.size();
        for (int k = 0; k < kSub; ++k) {
            const float sy = float(y) + (float(k) + 0.5f) * w;
            for (size_t i = 0; i < na; ++i) {
                const Edge &e = A[i].e;
                A[i].cx       = sy >= e.top && sy < e.bot ? e.x + (sy - e.top) * e.dxdy : kInf;
            }
            for (size_t i = 1; i < na; ++i) {
                if (A[i - 1].cx <= A[i].cx)
                    continue;
                const Active v = A[i];
                size_t       j = i;
                for (; j > 0 && A[j - 1].cx > v.cx; --j)
                    A[j] = A[j - 1];
                A[j] = v;
            }
            int        wind = 0;
            float      xa   = 0;
            const bool eo   = rule == FillRule::EvenOdd;
            for (size_t i = 0; i < na && A[i].cx < kInf; ++i) {
                const bool was = eo ? (wind & 1) : wind != 0;
                wind += A[i].e.dir;
                const bool is = eo ? (wind & 1) : wind != 0;
                if (!was && is) {
                    xa = A[i].cx;
                } else if (was && !is) {
                    const float a = std::fmax(xa - fx0, 0.0f),
                                b = std::fmin(A[i].cx - fx0, float(W));
                    if (b <= a)
                        continue;
                    const int ia = int(a), ib = int(b);
                    if (ia == ib) {
                        cov[size_t(ia)] += (b - a) * w;
                    } else {
                        cov[size_t(ia)] += (float(ia + 1) - a) * w;
                        diff[size_t(ia) + 1] += w;
                        diff[size_t(ib)] -= w;
                        cov[size_t(ib)] += (b - float(ib)) * w;
                    }
                    lo = std::min(lo, ia);
                    hi = std::max(hi, ib + 1);
                }
            }
        }
        if (hi > W)
            hi = W;
        if (lo >= hi)
            continue;
        float run = 0;
        for (int i = lo; i < hi; ++i) {
            run += diff[size_t(i)];
            const float c = cov[size_t(i)] + run;
            out[i - lo]   = c >= 1 ? 255 : c <= 0 ? 0 : uint8_t(c * 255.0f + 0.5f);
        }
        std::fill(cov.begin() + lo, cov.begin() + hi + 1, 0.0f);
        std::fill(diff.begin() + lo, diff.begin() + hi + 2, 0.0f);
        if (a8) {
            uint8_t *d = a8 + size_t(y - p._oy) * size_t(p._target.stride) + (x0 + lo - p._ox);
            for (int i = 0; i < hi - lo; ++i)
                d[i] = uint8_t(out[i] + div255(uint32_t(d[i]) * (255u - out[i])));
        } else if (shader) {
            uint32_t *px = row32(p);
            shaderRow(*shader, y, x0 + lo, x0 + hi, px);
            spanPx(p, y, x0 + lo, x0 + hi, out, px, 255);
        } else {
            span(p, y, x0 + lo, x0 + hi, out, pm);
        }
    }
}

} // namespace gfx

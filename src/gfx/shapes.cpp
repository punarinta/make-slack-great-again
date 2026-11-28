// Shape entry points, blits, drop shadows and resize (-Os): per-call setup
// and once-per-image work; their per-pixel loops are in blend.cpp.
#include "gfx/internal.h"

#include <algorithm>
#include <cmath>

namespace gfx {

// ── Shapes ──────────────────────────────────────────────────────────────────
void Painter::fillRect(RectF r, Color c) {
    const uint32_t pm = premultiply(c, _s.opacity);
    if (!pm || r.w <= 0 || r.h <= 0)
        return;
    const float x0 = (r.x + _s.tx) * _scale, y0 = (r.y + _s.ty) * _scale;
    const float x1 = (r.right() + _s.tx) * _scale, y1 = (r.bottom() + _s.ty) * _scale;
    const float e   = 1.0f / 256; // "on the pixel grid" tolerance for float noise
    const float rx0 = std::round(x0), ry0 = std::round(y0), rx1 = std::round(x1),
                ry1 = std::round(y1);
    if (std::fabs(x0 - rx0) < e && std::fabs(y0 - ry0) < e && std::fabs(x1 - rx1) < e &&
        std::fabs(y1 - ry1) < e) {
        const int ix0 = std::max(floori(rx0), _s.clipX0), iy0 = std::max(floori(ry0), _s.clipY0);
        const int ix1 = std::min(floori(rx1), _s.clipX1), iy1 = std::min(floori(ry1), _s.clipY1);
        for (int y = iy0; y < iy1; ++y)
            PainterImpl::span(*this, y, ix0, ix1, nullptr, pm);
        return;
    }
    PainterImpl::fillRR(*this, makeRR(x0, y0, x1, y1, 0), nullptr, pm);
}

void Painter::fillRoundRect(RectF r, float radius, Color c) {
    if (r.w <= 0 || r.h <= 0)
        return;
    if (radius <= 0) {
        fillRect(r, c);
        return;
    }
    const float s = _scale;
    const RR    q = makeRR(
        (r.x + _s.tx) * s,
        (r.y + _s.ty) * s,
        (r.right() + _s.tx) * s,
        (r.bottom() + _s.ty) * s,
        radius * s
    );
    PainterImpl::fillRR(*this, q, nullptr, premultiply(c, _s.opacity));
}

void Painter::strokeRoundRect(RectF r, float radius, float width, Color c) {
    if (r.w <= 0 || r.h <= 0 || width <= 0)
        return;
    const float s = _scale, w = width * s;
    const RR    q = makeRR(
        (r.x + _s.tx) * s,
        (r.y + _s.ty) * s,
        (r.right() + _s.tx) * s,
        (r.bottom() + _s.ty) * s,
        radius * s
    );
    if (q.x1 - q.x0 <= 2 * w || q.y1 - q.y0 <= 2 * w) {
        PainterImpl::fillRR(*this, q, nullptr, premultiply(c, _s.opacity));
        return;
    }
    // The inner edge of a border follows the outer one at radius - width.
    const RR in = makeRR(q.x0 + w, q.y0 + w, q.x1 - w, q.y1 - w, q.r > w ? q.r - w : 0);
    PainterImpl::fillRR(*this, q, &in, premultiply(c, _s.opacity));
}

void Painter::fillCircle(PointF c, float radius, Color col) {
    if (radius <= 0)
        return;
    const float s = _scale, cx = (c.x + _s.tx) * s, cy = (c.y + _s.ty) * s, r = radius * s;
    PainterImpl::fillRR(
        *this, makeRR(cx - r, cy - r, cx + r, cy + r, r), nullptr, premultiply(col, _s.opacity)
    );
}

void Painter::strokeCircle(PointF c, float radius, float width, Color col) {
    if (radius <= 0 || width <= 0)
        return;
    if (width >= radius) {
        fillCircle(c, radius, col);
        return;
    }
    const float s = _scale, cx = (c.x + _s.tx) * s, cy = (c.y + _s.ty) * s, r = radius * s;
    const float ri = (radius - width) * s;
    const RR    in = makeRR(cx - ri, cy - ri, cx + ri, cy + ri, ri);
    PainterImpl::fillRR(
        *this, makeRR(cx - r, cy - r, cx + r, cy + r, r), &in, premultiply(col, _s.opacity)
    );
}

void Painter::blitMask(const Mask8 &m, int physX, int physY, Color c) {
    const PointF o = PainterImpl::origin(*this);
    PainterImpl::maskAt(
        *this,
        m,
        int(std::lround(o.x)) + physX,
        int(std::lround(o.y)) + physY,
        premultiply(c, _s.opacity)
    );
}

void Painter::blitColor(const BitmapView &src, int physX, int physY, float opacity) {
    const float a = opacity * _s.opacity * 255.0f;
    if (a <= 0 || !src.pixels)
        return;
    const PointF   o  = PainterImpl::origin(*this);
    const int      ox = int(std::lround(o.x)) + physX, oy = int(std::lround(o.y)) + physY;
    const int      x0 = std::max(ox, _s.clipX0), x1 = std::min(ox + src.width, _s.clipX1);
    const int      y0 = std::max(oy, _s.clipY0), y1 = std::min(oy + src.height, _s.clipY1);
    const uint32_t alpha = a >= 255 ? 255 : uint32_t(a + 0.5f);
    for (int y = y0; y < y1; ++y)
        PainterImpl::spanPx(
            *this,
            y,
            x0,
            x1,
            nullptr,
            src.pixels + size_t(y - oy) * size_t(src.stride) + (x0 - ox),
            alpha
        );
}

// ── Drop shadow ─────────────────────────────────────────────────────────────
namespace {

// Three box blurs ≈ a Gaussian; runs in place on one line with a stride.
void boxBlurLine(uint8_t *p, int n, int step, int r, uint8_t *tmp) {
    for (int i = 0; i < n; ++i)
        tmp[i] = p[size_t(i) * size_t(step)];
    const uint32_t win = uint32_t(2 * r + 1), inv = (1u << 16) / win + 1;
    uint32_t       sum = 0;
    for (int i = 0; i <= r && i < n; ++i)
        sum += tmp[i];
    for (int i = 0; i < n; ++i) {
        p[size_t(i) * size_t(step)] =
            uint8_t(std::min<uint32_t>(255, (sum * inv + (1u << 15)) >> 16));
        const int add = i + r + 1, sub = i - r;
        if (add < n)
            sum += tmp[add];
        if (sub >= 0)
            sum -= tmp[sub];
    }
}

struct ShadowEntry {
    int                  w = 0, h = 0, r = 0, rb = -1;
    uint32_t             used = 0;
    std::vector<uint8_t> mask;
};
// Menus and popups repaint the same few shadow sizes every frame; the blur
// is the expensive part, so keep the last few masks. UI thread only.
ShadowEntry g_shadows[6];
uint32_t    g_shadowTick = 0;

const ShadowEntry &shadowMask(int w, int h, int r4, int rb, int pad) {
    ShadowEntry *slot = &g_shadows[0];
    for (auto &e : g_shadows) {
        if (e.w == w && e.h == h && e.r == r4 && e.rb == rb) {
            e.used = ++g_shadowTick;
            return e;
        }
        if (e.used < slot->used)
            slot = &e;
    }
    const int mw = w + 2 * pad, mh = h + 2 * pad;
    slot->w = w, slot->h = h, slot->r = r4, slot->rb = rb, slot->used = ++g_shadowTick;
    slot->mask.assign(size_t(mw) * size_t(mh), 0);
    const RR q = makeRR(float(pad), float(pad), float(pad + w), float(pad + h), float(r4) / 4);
    for (int y = pad; y < pad + h; ++y)
        rrRow(q, y, 0, mw, slot->mask.data() + size_t(y) * size_t(mw));
    std::vector<uint8_t> tmp(size_t(std::max(mw, mh)));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < mh; ++y)
            boxBlurLine(slot->mask.data() + size_t(y) * size_t(mw), mw, 1, rb, tmp.data());
        for (int x = 0; x < mw; ++x)
            boxBlurLine(slot->mask.data() + x, mh, mw, rb, tmp.data());
    }
    return *slot;
}

} // namespace

void Painter::dropShadow(RectF r, float radius, float blur, Color c) {
    dropShadow(r, radius, blur, {0, 0}, c);
}

void Painter::dropShadow(RectF box, float radius, float blur, PointF offset, Color c) {
    const uint32_t pm = premultiply(c, _s.opacity);
    if (!pm || box.w <= 0 || box.h <= 0)
        return;
    const RectF r{box.x + offset.x, box.y + offset.y, box.w, box.h};
    const float s = _scale, b = blur * s;
    if (b < 0.5f) {
        fillRoundRect(r, radius, c);
        return;
    }
    // CSS-style blur: sigma = blur / 2; three boxes of width w have variance
    // 3·(w²−1)/12 = sigma².
    const float sigma = b * 0.5f;
    const int   rb  = std::max(1, int(std::lround((std::sqrt(4 * sigma * sigma + 1) - 1) * 0.5f)));
    const int   pad = 3 * rb + 1;
    const int   x = int(std::lround((r.x + _s.tx) * s)), y = int(std::lround((r.y + _s.ty) * s));
    const int   w = int(std::lround(r.w * s)), h = int(std::lround(r.h * s));
    if (w <= 0 || h <= 0)
        return;
    const float        rr = std::fmin(radius * s, float(std::min(w, h)) * 0.5f);
    // Nine-patch: away from the corners every column (row) of the blurred
    // mask is the same, so only a corner-sized rect is blurred and cached —
    // a full-window popup costs no more than a tooltip.
    const int          m  = int(std::ceil(rr)) + pad; // corner + blur reach
    const int          rw = std::min(w, 2 * m + 1), rh = std::min(h, 2 * m + 1);
    const ShadowEntry &e  = shadowMask(rw, rh, int(std::lround(rr * 4)), rb, pad);
    const int          mw = rw + 2 * pad, mh = rh + 2 * pad, MW = w + 2 * pad, MH = h + 2 * pad;
    const int          ox = x - pad, oy = y - pad;
    const int          x0 = std::max(ox, _s.clipX0), x1 = std::min(ox + MW, _s.clipX1);
    const int          y0 = std::max(oy, _s.clipY0), y1 = std::min(oy + MH, _s.clipY1);
    if (x1 <= x0 || y1 <= y0)
        return;
    auto map = [m, pad](int l, int big, int small) { // big-mask row/column → cached one
        return l < pad + m ? l : l >= big - (pad + m) ? l - (big - small) : pad + m;
    };
    // Like CSS box-shadow, nothing is painted under the box itself (it is
    // drawn on top anyway): skip the cross-shaped part certainly inside it.
    const int cr  = int(std::ceil(rr));
    const int bx  = int(std::lround((box.x + _s.tx) * s)),
              by  = int(std::lround((box.y + _s.ty) * s));
    uint8_t  *cov = PainterImpl::row8(*this, 0);
    for (int yy = y0; yy < y1; ++yy) {
        const uint8_t *row = e.mask.data() + size_t(map(yy - oy, MH, mh)) * size_t(mw);
        // Left corner part, the repeated middle column, right corner part.
        const int L = std::clamp(ox + pad + m, x0, x1), R = std::clamp(ox + MW - (pad + m), L, x1);
        std::memcpy(cov, row + (x0 - ox), size_t(L - x0));
        std::memset(cov + (L - x0), row[pad + m], size_t(R - L));
        std::memcpy(cov + (R - x0), row + (R - ox - (MW - mw)), size_t(x1 - R));
        int skip0 = x1, skip1 = x1;
        if (yy >= by && yy < by + h) {
            const bool mid = yy >= by + cr && yy < by + h - cr;
            skip0          = std::clamp(mid ? bx : bx + cr, x0, x1);
            skip1          = std::clamp(mid ? bx + w : bx + w - cr, skip0, x1);
        }
        PainterImpl::span(*this, yy, x0, skip0, cov, pm);
        PainterImpl::span(*this, yy, skip1, x1, cov + (skip1 - x0), pm);
    }
}

// ── Resize ──────────────────────────────────────────────────────────────────
namespace {

constexpr uint32_t kOne = 1u << 14;

struct Taps {
    std::vector<int>      first, count;
    std::vector<uint16_t> w; // count[i] weights per output, summing to kOne
};

// Area-average when shrinking an axis, bilinear when enlarging it.
Taps makeTaps(int srcN, int dstN) {
    Taps t;
    t.first.resize(size_t(dstN));
    t.count.resize(size_t(dstN));
    const double k = double(srcN) / double(dstN);
    double       f[68];
    for (int i = 0; i < dstN; ++i) {
        int n = 0, first = 0;
        if (dstN < srcN) {
            const double a = i * k, b = a + k;
            first          = int(a);
            const int last = std::min(srcN, int(std::ceil(b)));
            for (int j = first; j < last && n < 68; ++j)
                f[n++] = (std::min(b, double(j + 1)) - std::max(a, double(j))) / k;
        } else {
            const double u = (i + 0.5) * k - 0.5;
            first          = int(std::floor(u));
            double fr      = u - first;
            if (first < 0)
                first = 0, fr = 0;
            if (first >= srcN - 1)
                first = srcN - 1, fr = 0;
            f[n++] = 1 - fr;
            if (fr > 0)
                f[n++] = fr;
        }
        t.first[size_t(i)] = first;
        t.count[size_t(i)] = n;
        // Integer weights that sum exactly to kOne, so flat areas stay flat.
        uint32_t sum       = 0;
        int      big       = 0;
        for (int j = 0; j < n; ++j) {
            const uint32_t v = uint32_t(f[j] * kOne + 0.5);
            t.w.push_back(uint16_t(v));
            sum += v;
            if (f[j] > f[big])
                big = j;
        }
        uint16_t &wb = t.w[t.w.size() - size_t(n) + size_t(big)];
        wb           = uint16_t(int(wb) + int(kOne) - int(sum));
    }
    return t;
}

uint32_t packTaps(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return ((a >> 14) << 24) | ((r >> 14) << 16) | ((g >> 14) << 8) | (b >> 14);
}

// Horizontal pass: dst[i] = Σ src[first + j] · w over taps, for n outputs.
void applyTaps(const Taps &t, const uint32_t *src, uint32_t *dst, int n) {
    const uint16_t *w = t.w.data();
    for (int i = 0; i < n; ++i) {
        const uint32_t *s = src + t.first[size_t(i)];
        uint32_t        a = kOne / 2, r = kOne / 2, g = kOne / 2, b = kOne / 2;
        for (int j = 0, c = t.count[size_t(i)]; j < c; ++j, ++w) {
            const uint32_t p = s[j];
            a += (p >> 24) * *w;
            r += ((p >> 16) & 255) * *w;
            g += ((p >> 8) & 255) * *w;
            b += (p & 255) * *w;
        }
        dst[i] = packTaps(a, r, g, b);
    }
}

// Vertical pass a row at a time (sequential reads, unlike walking columns):
// out row i = Σ src row (first + j) · w, accumulated per column in `acc`.
// The same integer sums as per column, so the same pixels.
void applyTapsRows(const Taps &t, const uint32_t *src, int width, uint32_t *dst, int n) {
    std::vector<uint32_t> acc(size_t(width) * 4);
    const uint16_t       *w = t.w.data();
    for (int i = 0; i < n; ++i) {
        std::fill(acc.begin(), acc.end(), kOne / 2);
        for (int j = 0, c = t.count[size_t(i)]; j < c; ++j, ++w) {
            const uint32_t *s  = src + size_t(t.first[size_t(i)] + j) * size_t(width);
            const uint32_t  wt = *w;
            uint32_t       *q  = acc.data();
            for (int x = 0; x < width; ++x, q += 4) {
                const uint32_t p = s[x];
                q[0] += (p >> 24) * wt;
                q[1] += ((p >> 16) & 255) * wt;
                q[2] += ((p >> 8) & 255) * wt;
                q[3] += (p & 255) * wt;
            }
        }
        uint32_t       *d = dst + size_t(i) * size_t(width);
        const uint32_t *q = acc.data();
        for (int x = 0; x < width; ++x, q += 4)
            d[x] = packTaps(q[0], q[1], q[2], q[3]);
    }
}

} // namespace

Bitmap resize(const BitmapView &src, int width, int height) {
    if (!src.pixels || src.width <= 0 || src.height <= 0 || width <= 0 || height <= 0)
        return {};
    // Area averaging over more than 64 source pixels per output would only
    // happen for > 64× shrinks; do those in two steps.
    if (src.width > width * 64 || src.height > height * 64) {
        const Bitmap mid =
            resize(src, std::max(width, src.width / 32), std::max(height, src.height / 32));
        return resize(mid.view(), width, height);
    }
    const Taps tx = makeTaps(src.width, width), ty = makeTaps(src.height, height);
    Bitmap     mid(width, src.height), out(width, height);
    for (int y = 0; y < src.height; ++y)
        applyTaps(
            tx,
            src.pixels + size_t(y) * size_t(src.stride),
            mid.pixels() + size_t(y) * size_t(width),
            width
        );
    applyTapsRows(ty, mid.pixels(), width, out.pixels(), height);
    return out;
}

} // namespace gfx

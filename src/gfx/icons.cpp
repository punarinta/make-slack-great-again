// drawIcon: decodes the compact path data tools/icons.py generates and
// rasterises it. Cold code (-Os); the rasterizer it feeds is the hot part.
#include "gfx/icons_generated.h"
#include "gfx/internal.h"

#include <algorithm>
#include <cmath>

namespace gfx {

namespace {

struct Reader {
    const uint8_t *p, *end;
    bool           ok = true;

    uint32_t u() {
        uint32_t v = 0;
        for (int shift = 0; shift < 32; shift += 7) {
            if (p >= end) {
                ok = false;
                return 0;
            }
            const uint8_t b = *p++;
            v |= uint32_t(b & 0x7f) << shift;
            if (!(b & 0x80))
                return v;
        }
        ok = false;
        return v;
    }
    int32_t delta() {
        const uint32_t v = u();
        return (v & 1) ? -int32_t((v + 1) >> 1) : int32_t(v >> 1);
    }
    uint8_t byte() {
        if (p >= end) {
            ok = false;
            return 0;
        }
        return *p++;
    }
};

enum Op : uint8_t { OpMove, OpLine, OpCubic, OpClose, OpCircle, OpEnd };

// Rasterises icon i with its viewBox origin at physical (ox, oy), k physical
// px per quantum. Returns false (drawing nothing) when `plainOnly` is set and
// the icon has fixed-colour parts, which an A8 mask cannot hold. `rot`, when
// set, turns every point about the viewBox centre: {cos, sin, cx, cy} in quanta.
bool paintIcon(
    Painter     &p,
    int          i,
    float        k,
    float        ox,
    float        oy,
    Color        tint,
    bool         plainOnly,
    const float *rot = nullptr
) {
    Reader rd{iconData::kData + iconData::kOffsets[i], iconData::kData + iconData::kOffsets[i + 1]};
    rd.u(), rd.u(); // viewBox, read by the caller
    const float         opacity = PainterImpl::opacity(p);
    // Geometry in the painter's scratch: a spinner repaints this every frame.
    PaintScratch::Data &sc      = PainterImpl::scratch(p);
    std::vector<Seg>   &segs    = sc.segs;
    Path               &path    = sc.path;
    Flat               &flat    = sc.flat;
    segs.clear();
    uint32_t segsPm = 0;
    int32_t  px = 0, py = 0;
    while (rd.ok && rd.p < rd.end) {
        const uint8_t flags = rd.byte();
        const float   hw    = (flags & 2) ? float(rd.u()) * k * 0.5f : 0;
        uint32_t      pm    = premultiply(tint, opacity);
        if (flags & 4) {
            if (plainOnly)
                return false;
            uint32_t c = 0;
            for (int b = 0; b < 4; ++b)
                c = (c << 8) | rd.byte();
            pm = premultiply(c, opacity * float(tint >> 24) / 255.0f);
        }
        path.clear();
        for (bool more = true; more && rd.ok;) {
            const uint8_t ob = rd.byte(), op = ob & 7;
            const int     n  = (ob >> 3) + 1;
            auto          pt = [&](float *x, float *y) {
                px += rd.delta();
                py += rd.delta();
                *x = float(px), *y = float(py);
                if (rot) {
                    const float dx = *x - rot[2], dy = *y - rot[3];
                    *x = rot[2] + dx * rot[0] - dy * rot[1];
                    *y = rot[3] + dx * rot[1] + dy * rot[0];
                }
            };
            float a, b, c, d, e, f;
            switch (op) {
            case OpMove:
                pt(&a, &b);
                path.moveTo(a, b);
                break;
            case OpLine:
                for (int j = 0; j < n; ++j) {
                    pt(&a, &b);
                    path.lineTo(a, b);
                }
                break;
            case OpCubic:
                for (int j = 0; j < n; ++j) {
                    pt(&a, &b), pt(&c, &d), pt(&e, &f);
                    path.cubicTo(a, b, c, d, e, f);
                }
                break;
            case OpClose:
                path.close();
                break;
            case OpCircle:
                pt(&a, &b);
                path.addCircle(a, b, float(rd.u()));
                break;
            default:
                more = false;
                break;
            }
        }
        if (!rd.ok)
            return true;
        // Consecutive strokes of one colour are rasterised together, so
        // crossing lines union without darker seams where their edges
        // overlap (all stroke pieces wind the same way). Fills go alone: their
        // winding is the SVG author's and may cancel against a stroke's.
        if (pm != segsPm && !segs.empty()) {
            PainterImpl::rasterize(p, segs, segsPm);
            segs.clear();
        }
        segsPm = pm;
        flat.pts.clear();
        flat.polys.clear();
        flatten(path, k, ox, oy, &flat);
        if (flags & 1) {
            sc.fill.clear();
            fillSegs(flat, &sc.fill);
            PainterImpl::rasterize(p, sc.fill, pm);
        }
        if (flags & 2)
            strokeSegs(flat, hw, &segs);
    }
    PainterImpl::rasterize(p, segs, segsPm);
    return true;
}

struct IconMask {
    int      icon = -1, w = 0, h = 0, dx = 0, dy = 0; // d = offset from the viewBox origin
    float    k    = 0;
    uint32_t used = 0;
    std::vector<uint8_t> a;
};
// Icons repaint at a handful of sizes; caching their coverage turns each
// draw into one mask blit (a 20 px icon is ~0.4 KB, cropped to its ink).
// Bounded by bytes, LRU; the budget holds every icon at three sizes at 2×.
// UI thread only.
constexpr size_t      kMaskBudget = 512 << 10;
std::vector<IconMask> g_masks;
size_t                g_maskBytes = 0;
uint32_t              g_maskTick  = 0;
uint8_t               g_fixedColour[kIconCount]; // 0 unknown, 1 plain, 2 has fixed colours
// Per icon, where its last two masks sat in g_masks (most recent first):
// icons repeat at one or two sizes (a row's icon and a toolbar's), so a draw
// rarely scans, even when two sizes alternate within a frame. Stale after an
// eviction moved one: checked.
uint16_t              g_lastMask[kIconCount][2];

} // namespace

void drawIcon(Painter &p, Icon icon, RectF r, Color tint) {
    const int i = int(icon);
    if (i < 0 || i >= kIconCount || r.w <= 0 || r.h <= 0)
        return;
    Reader rd{iconData::kData + iconData::kOffsets[i], iconData::kData + iconData::kOffsets[i + 1]};
    const float vbw = float(rd.u()), vbh = float(rd.u()); // in quanta
    if (!rd.ok || vbw <= 0 || vbh <= 0)
        return;
    const float  scale = PainterImpl::scale(p);
    const float  k     = std::fmin(r.w / vbw, r.h / vbh) * scale; // physical px per quantum
    const PointF o     = PainterImpl::origin(p);
    // Snap the icon origin to the pixel grid: the icons are drawn on a 24-unit
    // grid, so at integer sizes their strokes then land on pixel boundaries
    // (and the cached mask below is valid at any position).
    const float  ox    = std::round(o.x + r.x * scale + (r.w * scale - vbw * k) * 0.5f);
    const float  oy    = std::round(o.y + r.y * scale + (r.h * scale - vbh * k) * 0.5f);
    if (g_fixedColour[i] == 2 || k * iconData::kUnit > 64) { // rare / huge: no cache
        paintIcon(p, i, k, ox, oy, tint, false);
        return;
    }
    IconMask *m = nullptr;
    for (const size_t last : g_lastMask[i])
        if (!m && last < g_masks.size() && g_masks[last].icon == i && g_masks[last].k == k)
            m = &g_masks[last];
    for (size_t j = 0; !m && j < g_masks.size(); ++j)
        if (g_masks[j].icon == i && g_masks[j].k == k)
            m = &g_masks[j];
    // Strokes may reach up to 2 viewBox units outside it.
    const int margin = int(std::ceil(2 * iconData::kUnit * k)) + 1;
    if (!m) {
        const int w = int(std::ceil(vbw * k)) + 2 * margin,
                  h = int(std::ceil(vbh * k)) + 2 * margin;
        Bitmap    tmp(w, h);
        Painter   tp(tmp.view(), 1);
        if (!paintIcon(tp, i, k, float(margin), float(margin), rgb(0xffffff), true)) {
            g_fixedColour[i] = 2;
            paintIcon(p, i, k, ox, oy, tint, false);
            return;
        }
        g_fixedColour[i] = 1;
        // Crop to the ink: icons leave a padding inside their 24-unit box.
        int cx0 = w, cy0 = h, cx1 = 0, cy1 = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (tmp.pixels()[size_t(y) * size_t(w) + size_t(x)]) {
                    cx0 = std::min(cx0, x), cx1 = std::max(cx1, x + 1);
                    cy0 = std::min(cy0, y), cy1 = std::max(cy1, y + 1);
                }
        if (cx1 <= cx0)
            cx0 = cy0 = 0, cx1 = cy1 = 1;
        const int    cw = cx1 - cx0, ch = cy1 - cy0;
        const size_t bytes = size_t(cw) * size_t(ch);
        while (!g_masks.empty() && g_maskBytes + bytes > kMaskBudget) {
            size_t lru = 0;
            for (size_t j = 1; j < g_masks.size(); ++j)
                if (g_masks[j].used < g_masks[lru].used)
                    lru = j;
            g_maskBytes -= g_masks[lru].a.size();
            g_masks[lru] = std::move(g_masks.back());
            g_masks.pop_back();
        }
        g_masks.push_back(
            {i, cw, ch, cx0 - margin, cy0 - margin, k, 0, std::vector<uint8_t>(bytes)}
        );
        g_maskBytes += bytes;
        m = &g_masks.back();
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                m->a[size_t(y) * size_t(cw) + size_t(x)] =
                    uint8_t(tmp.pixels()[size_t(y + cy0) * size_t(w) + size_t(x + cx0)] >> 24);
    }
    m->used              = ++g_maskTick;
    const uint16_t  slot = uint16_t(m - g_masks.data());
    uint16_t *const hint = g_lastMask[i];
    if (hint[0] != slot)
        hint[1] = hint[0], hint[0] = slot;
    PainterImpl::maskAt(
        p,
        {m->a.data(), m->w, m->h, m->w},
        int(ox) + m->dx,
        int(oy) + m->dy,
        premultiply(tint, PainterImpl::opacity(p))
    );
}

// Uncached: a turning icon (the footer's background-task cog) never repeats
// a mask, and one small icon rasterised per frame is cheap.
void drawIconRotated(Painter &p, Icon icon, RectF r, Color tint, float degrees) {
    const int i = int(icon);
    if (i < 0 || i >= kIconCount || r.w <= 0 || r.h <= 0)
        return;
    Reader rd{iconData::kData + iconData::kOffsets[i], iconData::kData + iconData::kOffsets[i + 1]};
    const float vbw = float(rd.u()), vbh = float(rd.u());
    if (!rd.ok || vbw <= 0 || vbh <= 0)
        return;
    const float  scale  = PainterImpl::scale(p);
    const float  k      = std::fmin(r.w / vbw, r.h / vbh) * scale;
    const PointF o      = PainterImpl::origin(p);
    // Not snapped: the centre stays put while the strokes turn.
    const float  ox     = o.x + r.x * scale + (r.w * scale - vbw * k) * 0.5f;
    const float  oy     = o.y + r.y * scale + (r.h * scale - vbh * k) * 0.5f;
    const float  rad    = degrees * 3.14159265f / 180.0f;
    const float  rot[4] = {std::cos(rad), std::sin(rad), vbw / 2, vbh / 2};
    paintIcon(p, i, k, ox, oy, tint, false, &rot[0]);
}

} // namespace gfx

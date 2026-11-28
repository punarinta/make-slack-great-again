// Path flattening and stroking (-Os); the scanline rasterizer they feed is
// in blend.cpp.
#include "gfx/internal.h"

#include <algorithm>
#include <cmath>

namespace gfx {

namespace {

constexpr float kTol = 0.1f; // max flattening error, physical px (chords sit inside curves)

inline int segsFor(float dd, float k) {
    const float n = std::sqrt(dd * k / kTol);
    return n < 1 ? 1 : n > 200 ? 200 : int(std::ceil(n));
}

inline float len(float x, float y) {
    return std::sqrt(x * x + y * y);
}

} // namespace

void flatten(const Path &path, float s, float ox, float oy, Flat *out) {
    const auto  &cmds = path.cmds();
    const float *p    = path.pts().data();
    PointF       start{}, cur{};
    bool         open  = false;
    auto         begin = [&](PointF at) {
        out->polys.push_back({uint32_t(out->pts.size()), 1, false});
        out->pts.push_back(at);
        open = true;
    };
    auto add = [&](PointF q) {
        if (!open)
            begin(cur);
        out->pts.push_back(q);
        out->polys.back().count++;
        cur = q;
    };
    auto finish = [&](bool closed) {
        if (!open)
            return;
        Poly &pl  = out->polys.back();
        pl.closed = closed;
        if (pl.count < 2) { // a lone moveTo draws nothing
            out->pts.resize(pl.start);
            out->polys.pop_back();
        }
        open = false;
    };
    auto map = [&](int i) { return PointF{p[i] * s + ox, p[i + 1] * s + oy}; };
    for (uint8_t c : cmds) {
        switch (c) {
        case Path::Move:
            finish(false);
            start = cur = map(0);
            begin(cur);
            p += 2;
            break;
        case Path::Line:
            add(map(0));
            p += 2;
            break;
        case Path::Cubic: {
            const PointF p0 = cur, p1 = map(0), p2 = map(2), p3 = map(4);
            const float  dd = std::fmax(
                len(p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y),
                len(p1.x - 2 * p2.x + p3.x, p1.y - 2 * p2.y + p3.y)
            );
            const int n = segsFor(dd, 0.75f);
            for (int i = 1; i <= n; ++i) {
                const float t = float(i) / float(n), u = 1 - t;
                const float a = u * u * u, b = 3 * u * u * t, cc = 3 * u * t * t, d = t * t * t;
                add(
                    {a * p0.x + b * p1.x + cc * p2.x + d * p3.x,
                     a * p0.y + b * p1.y + cc * p2.y + d * p3.y}
                );
            }
            p += 6;
            break;
        }
        case Path::Close:
            finish(true);
            cur = start; // SVG: drawing after Z starts again at the subpath start
            break;
        }
    }
    finish(false);
}

void fillSegs(const Flat &f, std::vector<Seg> *segs) {
    for (const Poly &pl : f.polys) {
        const PointF *q = f.pts.data() + pl.start;
        for (uint32_t i = 0; i < pl.count; ++i) {
            const PointF a = q[i], b = q[(i + 1) % pl.count];
            if (a.y != b.y)
                segs->push_back({a.x, a.y, b.x, b.y});
        }
    }
}

namespace {

void disk(std::vector<Seg> *segs, PointF c, float r, int n) {
    // Negative orientation, matching the segment quads below.
    float px = c.x + r, py = c.y;
    for (int i = 1; i <= n; ++i) {
        const float a = -6.2831853f * float(i) / float(n);
        const float x = i == n ? c.x + r : c.x + r * std::cos(a);
        const float y = i == n ? c.y : c.y + r * std::sin(a);
        segs->push_back({px, py, x, y});
        px = x, py = y;
    }
}

} // namespace

namespace {

// A convex polygon wound negatively (like every other stroke piece), so the
// non-zero rule unions the pieces.
void piece(std::vector<Seg> *segs, const PointF *q, int n) {
    float area = 0;
    for (int i = 0; i < n; ++i)
        area += q[i].x * q[(i + 1) % n].y - q[(i + 1) % n].x * q[i].y;
    if (std::fabs(area) < 1e-6f)
        return;
    PointF r[4];
    for (int i = 0; i < n; ++i)
        r[i] = q[area < 0 ? i : n - 1 - i];
    for (int i = 0; i < n; ++i)
        segs->push_back({r[i].x, r[i].y, r[(i + 1) % n].x, r[(i + 1) % n].y});
}

} // namespace

void strokeSegs(const Flat &f, float hw, std::vector<Seg> *segs) {
    strokeSegs(f, {hw, Stroke::Cap::Round, Stroke::Join::Round, 4}, segs);
}

void strokeSegs(const Flat &f, const StrokeGeom &g, std::vector<Seg> *segs) {
    const float hw = g.hw;
    if (hw <= 0)
        return;
    // Enough sides that the polygon is within kTol of the true circle.
    const float cosv = 1 - kTol / std::fmax(hw, kTol);
    int         n    = cosv <= -1 ? 8 : int(std::ceil(3.14159265f / std::acos(cosv)));
    n                = n < 8 ? 8 : n > 64 ? 64 : n;
    std::vector<PointF> v;
    for (const Poly &pl : f.polys) {
        v.clear();
        const PointF *q = f.pts.data() + pl.start;
        for (uint32_t i = 0; i < pl.count; ++i)
            if (v.empty() || len(q[i].x - v.back().x, q[i].y - v.back().y) > 1e-3f)
                v.push_back(q[i]);
        const bool closed = pl.closed && v.size() > 2;
        if (closed && len(v.front().x - v.back().x, v.front().y - v.back().y) <= 1e-3f)
            v.pop_back();
        const size_t m = v.size();
        if (m == 1) { // zero-length subpath: round and square caps make a dot
            const PointF c = v[0];
            if (g.cap == Stroke::Cap::Round) {
                disk(segs, c, hw, n);
            } else if (g.cap == Stroke::Cap::Square) {
                const PointF box[4] = {
                    {c.x - hw, c.y - hw},
                    {c.x + hw, c.y - hw},
                    {c.x + hw, c.y + hw},
                    {c.x - hw, c.y + hw}
                };
                piece(segs, box, 4);
            }
            continue;
        }
        const size_t nseg = closed ? m : m - 1;
        for (size_t i = 0; i < nseg; ++i) {
            const PointF a = v[i], b = v[(i + 1) % m];
            const float  dx = b.x - a.x, dy = b.y - a.y, l = len(dx, dy);
            const float  nx = -dy / l * hw, ny = dx / l * hw;
            const PointF quad[4] = {
                {a.x + nx, a.y + ny},
                {b.x + nx, b.y + ny},
                {b.x - nx, b.y - ny},
                {a.x - nx, a.y - ny}
            };
            piece(segs, quad, 4);
        }
        // Caps, then joins. Flattened curves turn by a few degrees per
        // vertex; there a wedge on each side closes the gap between the
        // segment quads (any join style differs from it by < 4% of the
        // half-width below 30°) for 6 edges instead of a disk's 8–64.
        for (size_t i = 0; i < m; ++i) {
            if (!closed && (i == 0 || i == m - 1)) {
                if (g.cap == Stroke::Cap::Round) {
                    disk(segs, v[i], hw, n);
                } else if (g.cap == Stroke::Cap::Square) {
                    const PointF a = v[i], o = v[i == 0 ? 1 : m - 2];
                    const float  l  = len(a.x - o.x, a.y - o.y);
                    const float  ex = (a.x - o.x) / l * hw, ey = (a.y - o.y) / l * hw; // outwards
                    const PointF box[4] = {
                        {a.x - ey, a.y + ex},
                        {a.x - ey + ex, a.y + ex + ey},
                        {a.x + ey + ex, a.y - ex + ey},
                        {a.x + ey, a.y - ex}
                    };
                    piece(segs, box, 4);
                }
                continue;
            }
            const PointF a = v[(i + m - 1) % m], b = v[i], c = v[(i + 1) % m];
            const float  l0 = len(b.x - a.x, b.y - a.y), l1 = len(c.x - b.x, c.y - b.y);
            const float  d0x = (b.x - a.x) / l0, d0y = (b.y - a.y) / l0;
            const float  d1x = (c.x - b.x) / l1, d1y = (c.y - b.y) / l1;
            const float  dot = d0x * d1x + d0y * d1y;
            PointF       n0{-d0y * hw, d0x * hw}, n1{-d1y * hw, d1x * hw};
            if (dot > 0.866f) {
                const PointF w0[3] = {b, {b.x + n0.x, b.y + n0.y}, {b.x + n1.x, b.y + n1.y}};
                const PointF w1[3] = {b, {b.x - n0.x, b.y - n0.y}, {b.x - n1.x, b.y - n1.y}};
                piece(segs, w0, 3);
                piece(segs, w1, 3);
                continue;
            }
            if (g.join == Stroke::Join::Round) {
                disk(segs, b, hw, n);
                continue;
            }
            // Only the outer side needs filling: the side the path turns away from.
            if (n0.x * d1x + n0.y * d1y > 0)
                n0 = {-n0.x, -n0.y}, n1 = {-n1.x, -n1.y};
            const PointF o0{b.x + n0.x, b.y + n0.y}, o1{b.x + n1.x, b.y + n1.y};
            const float  ratio = std::sqrt(2 / std::fmax(1 + dot, 1e-6f)); // miter length / hw
            if (g.join == Stroke::Join::Miter && ratio <= g.miterLimit) {
                const float  mx = n0.x + n1.x, my = n0.y + n1.y, ml = len(mx, my);
                const PointF tip{b.x + mx / ml * hw * ratio, b.y + my / ml * hw * ratio};
                const PointF mq[4] = {b, o0, tip, o1};
                piece(segs, mq, 4);
            } else {
                const PointF bv[3] = {b, o0, o1};
                piece(segs, bv, 3);
            }
        }
    }
}

bool dashFlat(const Flat &in, const float *dashes, int count, float offset, Flat *out) {
    if (!dashes || count <= 0 || count > 64)
        return false;
    float pat[128];
    int   n = 0;
    for (int r = 0; r < (count & 1 ? 2 : 1); ++r) // odd lists repeat (SVG)
        for (int i = 0; i < count; ++i) {
            if (!(dashes[i] >= 0))
                return false;
            pat[n++] = dashes[i];
        }
    float total = 0;
    for (int i = 0; i < n; ++i)
        total += pat[i];
    if (!(total > 0.05f) || !std::isfinite(total))
        return false; // sub-pixel patterns: solid looks the same
    Flat         res;
    const size_t kMaxPts = 1u << 20;
    for (const Poly &pl : in.polys) {
        // Dash state at the start of every subpath: `offset` into the pattern.
        float o = std::fmod(offset, total);
        if (o < 0)
            o += total;
        int k = 0;
        while (o > 0 && o >= pat[k]) {
            o -= pat[k];
            k = (k + 1) % n;
        }
        float          left  = pat[k] - o; // remaining in the current dash/gap
        bool           on    = !(k & 1);
        const PointF  *q     = in.pts.data() + pl.start;
        const uint32_t segsN = pl.closed ? pl.count : pl.count - 1;
        if (on) {
            res.polys.push_back({uint32_t(res.pts.size()), 1, false});
            res.pts.push_back(q[0]);
        }
        for (uint32_t i = 0; i < segsN; ++i) {
            PointF       a = q[i];
            const PointF b = q[(i + 1) % pl.count];
            float        l = len(b.x - a.x, b.y - a.y);
            while (l > 0) {
                const float step = std::fmin(l, left);
                const float t    = step / l;
                a                = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                l -= step;
                left -= step;
                if (on) {
                    res.pts.push_back(a);
                    res.polys.back().count++;
                }
                if (left <= 0) {
                    k    = (k + 1) % n;
                    left = pat[k];
                    on   = !(k & 1);
                    if (on) {
                        res.polys.push_back({uint32_t(res.pts.size()), 1, false});
                        res.pts.push_back(a);
                    }
                }
                if (res.pts.size() > kMaxPts)
                    return false;
            }
        }
    }
    // Zero-length dashes stay: with round or square caps they are dots (the
    // "0 4" dotted-line idiom).
    *out = std::move(res);
    return true;
}

// ── Painter entry points ────────────────────────────────────────────────────
namespace {

// The path's fill edges in the painter's scratch (an animation fills paths
// every frame): flattened at phys = p * scale + (ox, oy).
const std::vector<Seg> &fillEdges(Painter &p, const Path &path, float scale, float ox, float oy) {
    PaintScratch::Data &sc = PainterImpl::scratch(p);
    sc.flat.pts.clear();
    sc.flat.polys.clear();
    flatten(path, scale, ox, oy, &sc.flat);
    sc.fill.clear();
    fillSegs(sc.flat, &sc.fill);
    return sc.fill;
}

void fillWith(Painter &p, const Path &path, FillRule rule, uint32_t pm, const Shader *sh) {
    const Affine m = PainterImpl::physToLogical(p);
    PainterImpl::rasterize(p, fillEdges(p, path, 1 / m.a, -m.e / m.a, -m.f / m.a), pm, rule, sh);
}

void strokeWith(Painter &p, const Path &path, const Stroke &st, uint32_t pm, const Shader *sh) {
    const Affine        m  = PainterImpl::physToLogical(p);
    const float         s  = 1 / m.a;
    PaintScratch::Data &sc = PainterImpl::scratch(p);
    Flat               &f  = sc.flat;
    f.pts.clear();
    f.polys.clear();
    flatten(path, s, -m.e * s, -m.f * s, &f);
    float d[64];
    int   nd = st.dashes ? std::min(st.dashCount, 64) : 0;
    for (int i = 0; i < nd; ++i)
        d[i] = st.dashes[i] * s;
    const Flat &use = nd && dashFlat(f, d, nd, st.dashOffset * s, &sc.dashed) ? sc.dashed : f;
    sc.fill.clear();
    strokeSegs(use, {st.width * 0.5f * s, st.cap, st.join, st.miterLimit}, &sc.fill);
    PainterImpl::rasterize(p, sc.fill, pm, FillRule::NonZero, sh);
}

} // namespace

void fillMask(const Path &path, FillRule rule, uint8_t *mask, int x, int y, int w, int h) {
    if (path.empty() || w <= 0 || h <= 0)
        return;
    Painter p({nullptr, w, h, w}, 1); // only its clip and scratch rows are used
    PainterImpl::placeTarget(p, x, y);
    PainterImpl::rasterize(p, fillEdges(p, path, 1, 0, 0), 0xffffffffu, rule, nullptr, mask);
}

void Painter::fillPath(const Path &path, Color c) {
    fillPath(path, c, FillRule::NonZero);
}

void Painter::fillPath(const Path &path, Color c, FillRule rule) {
    const uint32_t pm = premultiply(c, _s.opacity);
    if (pm && !path.empty())
        fillWith(*this, path, rule, pm, nullptr);
}

void Painter::fillPath(const Path &path, const Gradient &g, FillRule rule) {
    Shader sh;
    if (!path.empty() && makeShader(g, PainterImpl::physToLogical(*this), _s.opacity, &sh))
        fillWith(*this, path, rule, 1, &sh);
}

void Painter::strokePath(const Path &path, float width, Color c) {
    Stroke st;
    st.width = width;
    strokePath(path, st, c);
}

void Painter::strokePath(const Path &path, const Stroke &st, Color c) {
    const uint32_t pm = premultiply(c, _s.opacity);
    if (pm && !path.empty() && st.width > 0)
        strokeWith(*this, path, st, pm, nullptr);
}

void Painter::strokePath(const Path &path, const Stroke &st, const Gradient &g) {
    Shader sh;
    if (!path.empty() && st.width > 0 &&
        makeShader(g, PainterImpl::physToLogical(*this), _s.opacity, &sh))
        strokeWith(*this, path, st, 1, &sh);
}

void Painter::drawLine(PointF a, PointF b, float width, Color c) {
    if (width <= 0)
        return;
    const float hw = width * 0.5f;
    if (a.y == b.y) {
        fillRect({std::fmin(a.x, b.x), a.y - hw, std::fabs(b.x - a.x), width}, c);
        return;
    }
    if (a.x == b.x) {
        fillRect({a.x - hw, std::fmin(a.y, b.y), width, std::fabs(b.y - a.y)}, c);
        return;
    }
    const float dx = b.x - a.x, dy = b.y - a.y, l = len(dx, dy);
    const float nx = -dy / l * hw, ny = dx / l * hw;
    Path        p;
    p.moveTo(a.x + nx, a.y + ny);
    p.lineTo(b.x + nx, b.y + ny);
    p.lineTo(b.x - nx, b.y - ny);
    p.lineTo(a.x - nx, a.y - ny);
    p.close();
    fillPath(p, c);
}

} // namespace gfx

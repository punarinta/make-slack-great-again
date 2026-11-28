#include "text/colr.h"

#include "gfx/gfx.h"

#include FT_COLOR_H
#include FT_OUTLINE_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace text::fonts {

namespace {

constexpr int   kMaxDepth = 64;   // paint nesting: a cyclic graph stops here
constexpr int   kMaxSide  = 1024; // canvas side in pixels
constexpr int   kMaxStops = 64;
constexpr float kPi       = 3.14159265f;

// Canvas space is y-up with the origin at the bottom-left corner of the
// canvas (as FT_Outline_Get_Bitmap places outlines); pixel rows are stored
// top-down, so row r spans y = h-1-r .. h-r.

// x' = a·x + c·y + e, y' = b·x + d·y + f.
struct Xf {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

Xf mul(const Xf &p, const Xf &q) { // q, then p
    return {
        p.a * q.a + p.c * q.b,
        p.b * q.a + p.d * q.b,
        p.a * q.c + p.c * q.d,
        p.b * q.c + p.d * q.d,
        p.a * q.e + p.c * q.f + p.e,
        p.b * q.e + p.d * q.f + p.f
    };
}

bool invert(const Xf &m, Xf *out) {
    const float det = m.a * m.d - m.b * m.c;
    if (std::fabs(det) < 1e-12f)
        return false;
    const float id = 1 / det;
    out->a         = m.d * id;
    out->b         = -m.b * id;
    out->c         = -m.c * id;
    out->d         = m.a * id;
    out->e         = -(out->a * m.e + out->c * m.f);
    out->f         = -(out->b * m.e + out->d * m.f);
    return true;
}

// m applied about (cx, cy).
Xf around(const Xf &m, float cx, float cy) {
    return mul(Xf{1, 0, 0, 1, cx, cy}, mul(m, Xf{1, 0, 0, 1, -cx, -cy}));
}

float fx(FT_Fixed v) {
    return float(v) / 65536.f;
}

struct Px { // premultiplied, 0..1
    float r = 0, g = 0, b = 0, a = 0;
};

Px over(const Px &s, const Px &d) {
    const float k = 1 - s.a;
    return {s.r + d.r * k, s.g + d.g * k, s.b + d.b * k, s.a + d.a * k};
}

// What a paint may cover: coverage per canvas pixel (null = all of it) and
// the rect outside which it is zero.
struct Clip {
    const float *m  = nullptr;
    int          x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool         empty() const { return x0 >= x1 || y0 >= y1; }
};

// The separable and non-separable blend functions of the W3C compositing
// spec on straight colours.
float lum(const float *c) {
    return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
}

void setLum(float *c, float l) {
    const float d = l - lum(c);
    for (int i = 0; i < 3; ++i)
        c[i] += d;
    l              = lum(c);
    const float mn = std::min({c[0], c[1], c[2]}), mx = std::max({c[0], c[1], c[2]});
    for (int i = 0; i < 3; ++i) {
        if (mn < 0 && l - mn > 1e-6f)
            c[i] = l + (c[i] - l) * l / (l - mn);
        if (mx > 1 && mx - l > 1e-6f)
            c[i] = l + (c[i] - l) * (1 - l) / (mx - l);
    }
}

float sat(const float *c) {
    return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
}

void setSat(float *c, float s) {
    const float mn = std::min({c[0], c[1], c[2]}), mx = std::max({c[0], c[1], c[2]});
    for (int i = 0; i < 3; ++i)
        c[i] = mx > mn ? (c[i] - mn) * s / (mx - mn) : 0;
}

float blendChannel(int mode, float cb, float cs) {
    switch (mode) {
    case FT_COLR_COMPOSITE_SCREEN:
        return cb + cs - cb * cs;
    case FT_COLR_COMPOSITE_OVERLAY:
        return blendChannel(FT_COLR_COMPOSITE_HARD_LIGHT, cs, cb);
    case FT_COLR_COMPOSITE_DARKEN:
        return std::min(cb, cs);
    case FT_COLR_COMPOSITE_LIGHTEN:
        return std::max(cb, cs);
    case FT_COLR_COMPOSITE_COLOR_DODGE:
        return cb <= 0 ? 0 : cs >= 1 ? 1 : std::min(1.f, cb / (1 - cs));
    case FT_COLR_COMPOSITE_COLOR_BURN:
        return cb >= 1 ? 1 : cs <= 0 ? 0 : 1 - std::min(1.f, (1 - cb) / cs);
    case FT_COLR_COMPOSITE_HARD_LIGHT:
        return cs <= 0.5f ? cb * 2 * cs : blendChannel(FT_COLR_COMPOSITE_SCREEN, cb, 2 * cs - 1);
    case FT_COLR_COMPOSITE_SOFT_LIGHT: {
        if (cs <= 0.5f)
            return cb - (1 - 2 * cs) * cb * (1 - cb);
        const float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
        return cb + (2 * cs - 1) * (d - cb);
    }
    case FT_COLR_COMPOSITE_DIFFERENCE:
        return std::fabs(cb - cs);
    case FT_COLR_COMPOSITE_EXCLUSION:
        return cb + cs - 2 * cb * cs;
    default: // multiply
        return cb * cs;
    }
}

// Source s composited onto backdrop d (both premultiplied).
Px composite(int mode, const Px &s, const Px &d) {
    auto pd = [&](float fs, float fd) {
        return Px{
            s.r * fs + d.r * fd, s.g * fs + d.g * fd, s.b * fs + d.b * fd, s.a * fs + d.a * fd
        };
    };
    switch (mode) {
    case FT_COLR_COMPOSITE_CLEAR:
        return {};
    case FT_COLR_COMPOSITE_SRC:
        return s;
    case FT_COLR_COMPOSITE_DEST:
        return d;
    case FT_COLR_COMPOSITE_SRC_OVER:
        return pd(1, 1 - s.a);
    case FT_COLR_COMPOSITE_DEST_OVER:
        return pd(1 - d.a, 1);
    case FT_COLR_COMPOSITE_SRC_IN:
        return pd(d.a, 0);
    case FT_COLR_COMPOSITE_DEST_IN:
        return pd(0, s.a);
    case FT_COLR_COMPOSITE_SRC_OUT:
        return pd(1 - d.a, 0);
    case FT_COLR_COMPOSITE_DEST_OUT:
        return pd(0, 1 - s.a);
    case FT_COLR_COMPOSITE_SRC_ATOP:
        return pd(d.a, 1 - s.a);
    case FT_COLR_COMPOSITE_DEST_ATOP:
        return pd(1 - d.a, s.a);
    case FT_COLR_COMPOSITE_XOR:
        return pd(1 - d.a, 1 - s.a);
    case FT_COLR_COMPOSITE_PLUS:
        return {
            std::min(1.f, s.r + d.r),
            std::min(1.f, s.g + d.g),
            std::min(1.f, s.b + d.b),
            std::min(1.f, s.a + d.a)
        };
    default:
        break;
    }
    if (mode < FT_COLR_COMPOSITE_SCREEN || mode >= FT_COLR_COMPOSITE_MAX)
        return pd(1, 1 - s.a);
    // Blend modes: B(cb, cs) where both are present, plain source-over and
    // destination-over elsewhere.
    float cs[3] = {}, cb[3] = {}, b[3];
    if (s.a > 0)
        cs[0] = s.r / s.a, cs[1] = s.g / s.a, cs[2] = s.b / s.a;
    if (d.a > 0)
        cb[0] = d.r / d.a, cb[1] = d.g / d.a, cb[2] = d.b / d.a;
    switch (mode) {
    case FT_COLR_COMPOSITE_HSL_HUE:
        std::copy(cs, cs + 3, b);
        setSat(b, sat(cb));
        setLum(b, lum(cb));
        break;
    case FT_COLR_COMPOSITE_HSL_SATURATION:
        std::copy(cb, cb + 3, b);
        setSat(b, sat(cs));
        setLum(b, lum(cb));
        break;
    case FT_COLR_COMPOSITE_HSL_COLOR:
        std::copy(cs, cs + 3, b);
        setLum(b, lum(cb));
        break;
    case FT_COLR_COMPOSITE_HSL_LUMINOSITY:
        std::copy(cb, cb + 3, b);
        setLum(b, lum(cs));
        break;
    default:
        for (int i = 0; i < 3; ++i)
            b[i] = blendChannel(mode, cb[i], cs[i]);
    }
    const float both = s.a * d.a;
    Px          r    = pd(1 - d.a, 1 - s.a);
    r.r += both * b[0];
    r.g += both * b[1];
    r.b += both * b[2];
    r.a = s.a + d.a - both;
    return r;
}

struct Stop {
    float at;
    Px    c;
};

// A fill: a solid colour or a gradient's geometry (in font units) and its
// colour line.
struct Shader {
    enum Kind { None, Solid, Linear, Radial, Sweep } kind = None;
    Px                solid;
    std::vector<Stop> stops;
    FT_PaintExtend    extend = FT_COLR_PAINT_EXTEND_PAD;
    float x0 = 0, y0 = 0, dx = 0, dy = 0; // linear p0, p0→p1'; radial c0, c0→c1; sweep centre
    float r0 = 0, dr = 0, qa = 0;         // radial
    float start = 0, end = 0;             // sweep, degrees

    Px at(float t) const {
        if (stops.empty())
            return {};
        const float lo = stops.front().at, hi = stops.back().at;
        if (hi - lo > 1e-6f && extend != FT_COLR_PAINT_EXTEND_PAD) {
            float u = (t - lo) / (hi - lo);
            if (extend == FT_COLR_PAINT_EXTEND_REPEAT) {
                u -= std::floor(u);
            } else {
                u = std::fmod(std::fabs(u), 2.f);
                if (u > 1)
                    u = 2 - u;
            }
            t = lo + u * (hi - lo);
        }
        if (t <= lo)
            return stops.front().c;
        if (t >= hi)
            return stops.back().c;
        size_t i = 0;
        while (i + 2 < stops.size() && stops[i + 1].at <= t)
            ++i;
        const Stop &a = stops[i], &b = stops[i + 1];
        const float k = b.at > a.at ? (t - a.at) / (b.at - a.at) : 1;
        return {
            a.c.r + (b.c.r - a.c.r) * k,
            a.c.g + (b.c.g - a.c.g) * k,
            a.c.b + (b.c.b - a.c.b) * k,
            a.c.a + (b.c.a - a.c.a) * k
        };
    }

    // The colour at font-space point (x, y); false where nothing is painted.
    bool shade(float x, float y, Px *out) const {
        float t = 0;
        switch (kind) {
        case Solid:
            *out = solid;
            return true;
        case Linear:
            t = ((x - x0) * dx + (y - y0) * dy); // dx, dy pre-divided by |p0→p1'|²
            break;
        case Radial: {
            // The largest t whose circle (c0 + t·Δc, r0 + t·Δr) passes
            // through the point with a radius that is not negative.
            const float px = x - x0, py = y - y0;
            const float b = px * dx + py * dy + r0 * dr, c = px * px + py * py - r0 * r0;
            if (std::fabs(qa) < 1e-6f) {
                if (std::fabs(b) < 1e-9f)
                    return false;
                t = c / (2 * b);
            } else {
                const float disc = b * b - qa * c;
                if (disc < 0)
                    return false;
                const float sq = std::sqrt(disc);
                const float t1 = (b + sq) / qa, t2 = (b - sq) / qa;
                t = std::max(t1, t2);
                if (r0 + t * dr < 0)
                    t = std::min(t1, t2);
            }
            if (r0 + t * dr < 0)
                return false;
            break;
        }
        case Sweep: {
            float deg = std::atan2(y - y0, x - x0) * (180 / kPi);
            if (deg < 0)
                deg += 360;
            if (std::fabs(end - start) < 1e-6f)
                t = deg < start ? 0 : 1;
            else
                t = (deg - start) / (end - start);
            break;
        }
        default:
            return false;
        }
        *out = at(t);
        return true;
    }
};

class Colr {
public:
    Colr(FT_Library lib, FT_Face face) : _lib(lib), _face(face) {
        FT_Palette_Data pd;
        if (FT_Palette_Data_Get(face, &pd) == 0 && FT_Palette_Select(face, 0, &_pal) == 0)
            _npal = pd.num_palette_entries;
        else
            _pal = nullptr;
    }

    int w = 0, h = 0;

    // The canvas-space box of every outline the graph draws (x0 y0 x1 y1).
    void bounds(const FT_OpaquePaint &op, const Xf &t, int depth, float *bb) {
        FT_COLR_Paint  p;
        FT_OpaquePaint child{nullptr, 1};
        Xf             ct;
        if (depth > kMaxDepth || !FT_Get_Paint(_face, op, &p))
            return;
        if (transformed(p, t, &child, &ct))
            return bounds(child, ct, depth + 1, bb);
        switch (p.format) {
        case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
            FT_LayerIterator it = p.u.colr_layers.layer_iterator;
            while (FT_Get_Paint_Layers(_face, &it, &child))
                bounds(child, t, depth + 1, bb);
            break;
        }
        case FT_COLR_PAINTFORMAT_GLYPH:
            if (outline(p.u.glyph.glyphID, t))
                for (const FT_Vector &v : _pts) {
                    bb[0] = std::min(bb[0], float(v.x) / 64);
                    bb[1] = std::min(bb[1], float(v.y) / 64);
                    bb[2] = std::max(bb[2], float(v.x) / 64);
                    bb[3] = std::max(bb[3], float(v.y) / 64);
                }
            break;
        case FT_COLR_PAINTFORMAT_COLR_GLYPH:
            if (FT_Get_Color_Glyph_Paint(
                    _face, p.u.colr_glyph.glyphID, FT_COLOR_NO_ROOT_TRANSFORM, &child
                ))
                bounds(child, t, depth + 1, bb);
            break;
        case FT_COLR_PAINTFORMAT_COMPOSITE:
            bounds(p.u.composite.backdrop_paint, t, depth + 1, bb);
            bounds(p.u.composite.source_paint, t, depth + 1, bb);
            break;
        default:
            break;
        }
    }

    void paint(const FT_OpaquePaint &op, const Xf &t, Px *dst, const Clip &clip, int depth) {
        FT_COLR_Paint  p;
        FT_OpaquePaint child{nullptr, 1};
        Xf             ct;
        if (depth > kMaxDepth || clip.empty() || !FT_Get_Paint(_face, op, &p))
            return;
        if (transformed(p, t, &child, &ct))
            return paint(child, ct, dst, clip, depth + 1);
        switch (p.format) {
        case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
            FT_LayerIterator it = p.u.colr_layers.layer_iterator;
            while (FT_Get_Paint_Layers(_face, &it, &child))
                paint(child, t, dst, clip, depth + 1);
            break;
        }
        case FT_COLR_PAINTFORMAT_GLYPH: {
            std::vector<float> m;
            Clip               c;
            if (mask(p.u.glyph.glyphID, t, clip, m, &c))
                paint(p.u.glyph.paint, t, dst, c, depth + 1);
            break;
        }
        case FT_COLR_PAINTFORMAT_COLR_GLYPH:
            if (FT_Get_Color_Glyph_Paint(
                    _face, p.u.colr_glyph.glyphID, FT_COLOR_NO_ROOT_TRANSFORM, &child
                ))
                paint(child, t, dst, clip, depth + 1);
            break;
        case FT_COLR_PAINTFORMAT_COMPOSITE: {
            std::vector<Px> back(size_t(w) * h), src(size_t(w) * h);
            paint(p.u.composite.backdrop_paint, t, back.data(), clip, depth + 1);
            paint(p.u.composite.source_paint, t, src.data(), clip, depth + 1);
            const int mode = int(p.u.composite.composite_mode);
            for (int y = clip.y0; y < clip.y1; ++y)
                for (int x = clip.x0; x < clip.x1; ++x) {
                    const size_t i = size_t(y) * w + x;
                    dst[i]         = over(composite(mode, src[i], back[i]), dst[i]);
                }
            break;
        }
        default:
            fill(p, t, dst, clip);
        }
    }

private:
    FT_Library             _lib;
    FT_Face                _face;
    FT_Color              *_pal  = nullptr;
    int                    _npal = 0;
    std::vector<FT_Vector> _pts; // the last outline, in canvas 26.6
    std::vector<uint8_t>   _cov;

    // The transform paints: their child and the transform it is drawn with.
    static bool transformed(const FT_COLR_Paint &p, const Xf &t, FT_OpaquePaint *child, Xf *ct) {
        Xf m;
        switch (p.format) {
        case FT_COLR_PAINTFORMAT_TRANSFORM: {
            const FT_Affine23 &a = p.u.transform.affine;
            m                    = {fx(a.xx), fx(a.yx), fx(a.xy), fx(a.yy), fx(a.dx), fx(a.dy)};
            *child               = p.u.transform.paint;
            break;
        }
        case FT_COLR_PAINTFORMAT_TRANSLATE:
            m      = {1, 0, 0, 1, fx(p.u.translate.dx), fx(p.u.translate.dy)};
            *child = p.u.translate.paint;
            break;
        case FT_COLR_PAINTFORMAT_SCALE: {
            const FT_PaintScale &s = p.u.scale;
            m = around({fx(s.scale_x), 0, 0, fx(s.scale_y), 0, 0}, fx(s.center_x), fx(s.center_y));
            *child = s.paint;
            break;
        }
        case FT_COLR_PAINTFORMAT_ROTATE: { // counter-clockwise, angle in half turns
            const FT_PaintRotate &r = p.u.rotate;
            const float           a = fx(r.angle) * kPi, cs = std::cos(a), sn = std::sin(a);
            m      = around({cs, sn, -sn, cs, 0, 0}, fx(r.center_x), fx(r.center_y));
            *child = r.paint;
            break;
        }
        case FT_COLR_PAINTFORMAT_SKEW: { // positive angles lean counter-clockwise
            const FT_PaintSkew &s  = p.u.skew;
            const float         kx = std::tan(fx(s.x_skew_angle) * kPi);
            const float         ky = std::tan(fx(s.y_skew_angle) * kPi);
            m                      = around({1, ky, -kx, 1, 0, 0}, fx(s.center_x), fx(s.center_y));
            *child                 = s.paint;
            break;
        }
        default:
            return false;
        }
        *ct = mul(t, m);
        return true;
    }

    // Glyph `gid`'s outline in font units, mapped through t into _pts.
    bool outline(uint32_t gid, const Xf &t) {
        if (FT_Load_Glyph(
                _face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM
            ) != 0 ||
            _face->glyph->format != FT_GLYPH_FORMAT_OUTLINE)
            return false;
        const FT_Outline &o = _face->glyph->outline;
        _pts.resize(size_t(o.n_points));
        for (size_t i = 0; i < _pts.size(); ++i) {
            const float x = float(o.points[i].x), y = float(o.points[i].y);
            _pts[i] = {
                FT_Pos(std::lround((t.a * x + t.c * y + t.e) * 64)),
                FT_Pos(std::lround((t.b * x + t.d * y + t.f) * 64))
            };
        }
        return true;
    }

    // The glyph's coverage times the enclosing clip, as a clip of its own.
    bool mask(uint32_t gid, const Xf &t, const Clip &clip, std::vector<float> &m, Clip *out) {
        if (!outline(gid, t) || _pts.empty())
            return false;
        FT_Pos mnx = _pts[0].x, mny = _pts[0].y, mxx = mnx, mxy = mny;
        for (const FT_Vector &v : _pts) {
            mnx = std::min(mnx, v.x), mny = std::min(mny, v.y);
            mxx = std::max(mxx, v.x), mxy = std::max(mxy, v.y);
        }
        *out    = clip;
        out->x0 = std::max(clip.x0, int(mnx >> 6));
        out->x1 = std::min(clip.x1, int((mxx + 63) >> 6));
        out->y0 = std::max(clip.y0, h - int((mxy + 63) >> 6));
        out->y1 = std::min(clip.y1, h - int(mny >> 6));
        if (out->empty())
            return false;
        FT_Outline o = _face->glyph->outline;
        o.points     = _pts.data();
        _cov.assign(size_t(w) * h, 0);
        FT_Bitmap bm{};
        bm.rows       = unsigned(h);
        bm.width      = unsigned(w);
        bm.pitch      = w;
        bm.buffer     = _cov.data();
        bm.pixel_mode = FT_PIXEL_MODE_GRAY;
        bm.num_grays  = 256;
        if (FT_Outline_Get_Bitmap(_lib, &o, &bm) != 0)
            return false;
        m.assign(size_t(w) * h, 0);
        for (int y = out->y0; y < out->y1; ++y)
            for (int x = out->x0; x < out->x1; ++x) {
                const size_t i = size_t(y) * w + x;
                m[i]           = _cov[i] * (1 / 255.f) * (clip.m ? clip.m[i] : 1);
            }
        out->m = m.data();
        return true;
    }

    Px color(const FT_ColorIndex &ci) const {
        float r = 0, g = 0, b = 0, a = 1; // 0xFFFF, the text colour: emoji have none, black
        if (ci.palette_index != 0xFFFF) {
            if (!_pal || ci.palette_index >= _npal)
                return {};
            const FT_Color &c = _pal[ci.palette_index];
            r = c.red / 255.f, g = c.green / 255.f, b = c.blue / 255.f, a = c.alpha / 255.f;
        }
        a *= std::clamp(float(ci.alpha) / 16384.f, 0.f, 1.f);
        return {r * a, g * a, b * a, a};
    }

    void colorLine(FT_ColorLine cl, Shader &s) const {
        FT_ColorStop st;
        while (s.stops.size() < size_t(kMaxStops) &&
               FT_Get_Colorline_Stops(_face, &st, &cl.color_stop_iterator)) {
            // Insertion keeps equal offsets in file order (a hard edge).
            const Stop stop{fx(st.stop_offset), color(st.color)};
            auto       at = s.stops.end();
            while (at != s.stops.begin() && (at - 1)->at > stop.at)
                --at;
            s.stops.insert(at, stop);
        }
        s.extend = cl.extend;
    }

    void fill(const FT_COLR_Paint &p, const Xf &t, Px *dst, const Clip &clip) const {
        Shader s;
        switch (p.format) {
        case FT_COLR_PAINTFORMAT_SOLID:
            s.kind  = Shader::Solid;
            s.solid = color(p.u.solid.color);
            break;
        case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT: {
            // The colour line runs from p0 to p1, turned so that lines of
            // equal colour are parallel to p0→p2: p1 projected onto the
            // normal of p0→p2.
            const FT_PaintLinearGradient &l = p.u.linear_gradient;
            const float x0 = fx(l.p0.x), y0 = fx(l.p0.y), x1 = fx(l.p1.x), y1 = fx(l.p1.y);
            const float nx = -(fx(l.p2.y) - y0), ny = fx(l.p2.x) - x0;
            float       dx = x1 - x0, dy = y1 - y0;
            if (const float nn = nx * nx + ny * ny; nn > 1e-6f) {
                const float k = (dx * nx + dy * ny) / nn;
                dx = nx * k, dy = ny * k;
            }
            const float len2 = dx * dx + dy * dy;
            if (len2 < 1e-6f)
                return;
            s.kind = Shader::Linear;
            s.x0 = x0, s.y0 = y0, s.dx = dx / len2, s.dy = dy / len2;
            colorLine(l.colorline, s);
            break;
        }
        case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT: {
            const FT_PaintRadialGradient &r = p.u.radial_gradient;
            s.kind                          = Shader::Radial;
            s.x0 = fx(r.c0.x), s.y0 = fx(r.c0.y), s.r0 = fx(r.r0);
            s.dx = fx(r.c1.x) - s.x0, s.dy = fx(r.c1.y) - s.y0, s.dr = fx(r.r1) - s.r0;
            s.qa = s.dx * s.dx + s.dy * s.dy - s.dr * s.dr;
            colorLine(r.colorline, s);
            break;
        }
        case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT: {
            const FT_PaintSweepGradient &g = p.u.sweep_gradient;
            s.kind                         = Shader::Sweep;
            s.x0 = fx(g.center.x), s.y0 = fx(g.center.y);
            // Stored biased by one half turn since OpenType 1.9.1 (0..360°
            // fits F2Dot14); FreeType passes the stored value through.
            s.start = (fx(g.start_angle) + 1) * 180, s.end = (fx(g.end_angle) + 1) * 180;
            colorLine(g.colorline, s);
            break;
        }
        default:
            return;
        }
        Xf inv;
        if (!invert(t, &inv))
            return;
        for (int y = clip.y0; y < clip.y1; ++y) {
            const float cy = float(h - y) - 0.5f;
            for (int x = clip.x0; x < clip.x1; ++x) {
                const size_t i   = size_t(y) * w + x;
                const float  cov = clip.m ? clip.m[i] : 1;
                if (cov <= 0)
                    continue;
                const float cx = float(x) + 0.5f;
                Px          c;
                if (!s.shade(inv.a * cx + inv.c * cy + inv.e, inv.b * cx + inv.d * cy + inv.f, &c))
                    continue;
                c.r *= cov, c.g *= cov, c.b *= cov, c.a *= cov;
                dst[i] = over(c, dst[i]);
            }
        }
    }
};

uint8_t byte(float v) {
    return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255));
}

} // namespace

bool renderColrV1(
    FT_Library   lib,
    FT_Face      face,
    uint32_t     glyph,
    float        ppem,
    float        dx,
    gfx::Bitmap *out,
    int         *left,
    int         *top
) {
    FT_OpaquePaint root{nullptr, 1};
    if (!FT_Get_Color_Glyph_Paint(face, glyph, FT_COLOR_NO_ROOT_TRANSFORM, &root))
        return false;
    const float s = ppem / float(face->units_per_EM ? face->units_per_EM : 1000);
    const Xf    pen{s, 0, 0, s, dx, 0}; // font units → pixels about the pen
    Colr        c(lib, face);
    float       bb[4] = {1e30f, 1e30f, -1e30f, -1e30f};
    c.bounds(root, pen, 0, bb);
    *out  = gfx::Bitmap();
    *left = *top = 0;
    if (!(bb[0] < bb[2] && bb[1] < bb[3]))
        return true; // draws nothing
    const int x0 = int(std::floor(bb[0])), y0 = int(std::floor(bb[1]));
    const int x1 = int(std::ceil(bb[2])), y1 = int(std::ceil(bb[3]));
    if (x1 - x0 > kMaxSide || y1 - y0 > kMaxSide)
        return false;
    c.w = x1 - x0, c.h = y1 - y0;
    std::vector<Px> canvas(size_t(c.w) * c.h);
    c.paint(
        root,
        mul(Xf{1, 0, 0, 1, -float(x0), -float(y0)}, pen),
        canvas.data(),
        {nullptr, 0, 0, c.w, c.h},
        0
    );
    *out         = gfx::Bitmap(c.w, c.h);
    uint32_t *px = out->pixels();
    for (size_t i = 0; i < canvas.size(); ++i) {
        const Px     &p = canvas[i];
        const uint8_t a = byte(p.a);
        px[i]           = uint32_t(a) << 24 | uint32_t(std::min(a, byte(p.r))) << 16 |
                          uint32_t(std::min(a, byte(p.g))) << 8 | std::min(a, byte(p.b));
    }
    *left = x0;
    *top  = y1;
    return true;
}

} // namespace text::fonts

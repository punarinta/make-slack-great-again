// Painter state, Path, Bitmap — the cold, generic part of gfx (-Os).
#include "gfx/internal.h"

#include "base/str.h"

#include <cmath>

namespace gfx {

Bitmap::Bitmap(int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    _w = w;
    _h = h;
    _px.assign(size_t(w) * size_t(h), 0);
}

Color withAlpha(Color c, float opacity) {
    float a = float(c >> 24) * opacity;
    a       = a < 0 ? 0 : a > 255 ? 255 : a;
    return (uint32_t(a + 0.5f) << 24) | (c & 0xffffff);
}

bool parseHexColor(std::string_view s, Color *out) {
    if (!s.empty() && s[0] == '#')
        s.remove_prefix(1);
    const size_t n = s.size();
    if (n != 3 && n != 4 && n != 6 && n != 8)
        return false;
    uint32_t v = 0;
    for (const char ch : s) {
        const int d = str::hexDigit(ch);
        if (d < 0)
            return false;
        v = v << 4 | uint32_t(d);
        if (n < 6)
            v = v << 4 | uint32_t(d);
    }
    // RRGGBB[AA] → AARRGGBB
    *out = n == 3 || n == 6 ? 0xff000000u | v : v << 24 | v >> 8;
    return true;
}

std::string hexColor(Color c) {
    std::string s = "#";
    for (int shift = 20; shift >= 0; shift -= 4)
        s += str::kHexLower[(c >> shift) & 0xf];
    return s;
}

uint32_t premultiply(Color c, float opacity) {
    float af = float(c >> 24) * opacity;
    if (af <= 0)
        return 0;
    const uint32_t a = af >= 255 ? 255 : uint32_t(af + 0.5f);
    return (a << 24) | (div255(((c >> 16) & 255) * a) << 16) | (div255(((c >> 8) & 255) * a) << 8) |
           div255((c & 255) * a);
}

RR makeRR(float x0, float y0, float x1, float y1, float r) {
    const float half = std::fmin(x1 - x0, y1 - y0) * 0.5f;
    if (r > half)
        r = half;
    if (r < 0)
        r = 0;
    return {x0, y0, x1, y1, r};
}

// ── Path ────────────────────────────────────────────────────────────────────
void Path::clear() {
    _cmds.clear();
    _pts.clear();
}
void Path::moveTo(float x, float y) {
    _cmds.push_back(Move);
    _pts.insert(_pts.end(), {x, y});
}
void Path::lineTo(float x, float y) {
    _cmds.push_back(Line);
    _pts.insert(_pts.end(), {x, y});
}
void Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    _cmds.push_back(Cubic);
    _pts.insert(_pts.end(), {c1x, c1y, c2x, c2y, x, y});
}
void Path::close() {
    _cmds.push_back(Close);
}

void Path::addCircle(float cx, float cy, float r) {
    // 0.5523 = 4/3·tan(π/8): the standard quarter-circle cubic.
    const float k = 0.55228475f * r;
    moveTo(cx + r, cy);
    cubicTo(cx + r, cy + k, cx + k, cy + r, cx, cy + r);
    cubicTo(cx - k, cy + r, cx - r, cy + k, cx - r, cy);
    cubicTo(cx - r, cy - k, cx - k, cy - r, cx, cy - r);
    cubicTo(cx + k, cy - r, cx + r, cy - k, cx + r, cy);
    close();
}

// ── Painter state ───────────────────────────────────────────────────────────
PaintScratch::~PaintScratch() {
    delete _d;
}

PaintScratch::Data &PaintScratch::data() {
    if (!_d)
        _d = new Data;
    return *_d;
}

Painter::Painter(BitmapView target, float scale, PaintScratch *scratch)
    : _target(target), _scale(scale > 0 ? scale : 1), _lent(scratch) {
    _s.clipX1 = target.width;
    _s.clipY1 = target.height;
}

void PainterImpl::placeTarget(Painter &p, int x, int y) {
    p._ox = x, p._oy = y;
    p._s.clipX0 = x, p._s.clipY0 = y;
    p._s.clipX1 = x + p._target.width, p._s.clipY1 = y + p._target.height;
}

void Painter::save() {
    _stack.push_back(_s);
}

void Painter::restore() {
    if (_stack.empty())
        return;
    _s = _stack.back();
    _stack.pop_back();
    // Round clips form a chain that only grows at the end, so everything past
    // the restored state's clip was added after the matching save().
    _roundClips.resize(size_t(_s.roundClip + 1));
}

void Painter::translate(float dx, float dy) {
    _s.tx += dx;
    _s.ty += dy;
}

void Painter::setOpacity(float o) {
    _s.opacity *= o < 0 ? 0 : o > 1 ? 1 : o;
}

static int clampCoord(float v) {
    // Keeps float→int conversions defined for absurd inputs.
    return v < -1e7f ? -10000000 : v > 1e7f ? 10000000 : int(v);
}

static void intersectClip(int &x0, int &y0, int &x1, int &y1, int a0, int b0, int a1, int b1) {
    x0 = x0 > a0 ? x0 : a0;
    y0 = y0 > b0 ? y0 : b0;
    x1 = x1 < a1 ? x1 : a1;
    y1 = y1 < b1 ? y1 : b1;
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
}

void Painter::clipRect(RectF r) {
    // Rect clips snap to whole pixels (crisp scroll areas); rounded clips
    // below keep their fractional geometry for anti-aliasing.
    const float s = _scale;
    intersectClip(
        _s.clipX0,
        _s.clipY0,
        _s.clipX1,
        _s.clipY1,
        clampCoord(std::round((r.x + _s.tx) * s)),
        clampCoord(std::round((r.y + _s.ty) * s)),
        clampCoord(std::round((r.right() + _s.tx) * s)),
        clampCoord(std::round((r.bottom() + _s.ty) * s))
    );
}

void Painter::clipRoundRect(RectF r, float radius) {
    if (radius <= 0) {
        clipRect(r);
        return;
    }
    const float s = _scale, x0 = (r.x + _s.tx) * s, y0 = (r.y + _s.ty) * s;
    const float x1 = (r.right() + _s.tx) * s, y1 = (r.bottom() + _s.ty) * s;
    intersectClip(
        _s.clipX0,
        _s.clipY0,
        _s.clipX1,
        _s.clipY1,
        clampCoord(std::floor(x0)),
        clampCoord(std::floor(y0)),
        clampCoord(std::ceil(x1)),
        clampCoord(std::ceil(y1))
    );
    const RR q = makeRR(x0, y0, x1, y1, radius * s);
    _roundClips.push_back({q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0, q.r, _s.roundClip});
    _s.roundClip = int(_roundClips.size()) - 1;
}

RectF Painter::clipBounds() const {
    const float s = _scale;
    return {
        _s.clipX0 / s - _s.tx,
        _s.clipY0 / s - _s.ty,
        (_s.clipX1 - _s.clipX0) / s,
        (_s.clipY1 - _s.clipY0) / s
    };
}

PointF Painter::toPhysical(PointF l) const {
    return {(l.x + _s.tx) * _scale, (l.y + _s.ty) * _scale};
}

uint8_t *PainterImpl::row8(Painter &p, int which) {
    const size_t          w = size_t(p._target.width) + 2;
    std::vector<uint8_t> &b = scratch(p).buf8;
    if (b.size() < 4 * w)
        b.resize(4 * w);
    return b.data() + size_t(which) * w;
}

uint32_t *PainterImpl::row32(Painter &p) {
    std::vector<uint32_t> &b = scratch(p).buf32;
    if (b.size() < size_t(p._target.width) + 2)
        b.resize(size_t(p._target.width) + 2);
    return b.data();
}

// ── Affine + gradients ──────────────────────────────────────────────────────
Affine mul(const Affine &l, const Affine &r) {
    return {
        l.a * r.a + l.c * r.b,
        l.b * r.a + l.d * r.b,
        l.a * r.c + l.c * r.d,
        l.b * r.c + l.d * r.d,
        l.a * r.e + l.c * r.f + l.e,
        l.b * r.e + l.d * r.f + l.f
    };
}

bool invert(const Affine &m, Affine *out) {
    const double det = double(m.a) * m.d - double(m.b) * m.c;
    if (!(std::fabs(det) > 1e-12))
        return false;
    const double k = 1 / det;
    *out           = {
        float(m.d * k),
        float(-m.b * k),
        float(-m.c * k),
        float(m.a * k),
        float((double(m.c) * m.f - double(m.d) * m.e) * k),
        float((double(m.b) * m.e - double(m.a) * m.f) * k)
    };
    return true;
}

bool makeShader(const Gradient &g, const Affine &physToLogical, float opacity, Shader *o) {
    const size_t n = g.stops.size();
    if (!n)
        return false;
    o->m      = mul(g.toGradient, physToLogical);
    o->kind   = g.kind == Gradient::Kind::Radial;
    o->spread = uint8_t(g.spread);
    o->ax = g.a.x, o->ay = g.a.y;
    o->dx = g.b.x - g.a.x, o->dy = g.b.y - g.a.y;
    const float len2 = o->dx * o->dx + o->dy * o->dy;
    o->inv = o->kind ? (g.radius > 1e-6f ? 1 / g.radius : 0) : (len2 > 1e-12f ? 1 / len2 : 0);
    // Degenerate geometry paints the last stop (SVG).
    const bool flat = o->inv == 0;
    float      pre[4][2]; // premultiplied a, r, g, b of the two stops around t
    for (int i = 0; i < 256; ++i) {
        const float t = flat ? 1.0f : float(i) / 255.0f;
        size_t      k = 0;
        while (k < n && g.stops[k].offset < t)
            ++k;
        const GradientStop &s1   = g.stops[k < n ? k : n - 1];
        const GradientStop &s0   = g.stops[k > 0 ? k - 1 : 0];
        const float         span = s1.offset - s0.offset;
        const float         u =
            k == 0 || k >= n || span <= 0 ? (k >= n ? 0.0f : 1.0f) : (t - s0.offset) / span;
        const Color cs[2] = {s0.color, s1.color};
        for (int j = 0; j < 2; ++j) {
            const float a = float(cs[j] >> 24) / 255.0f * opacity;
            pre[0][j]     = a;
            pre[1][j]     = float((cs[j] >> 16) & 255) * a;
            pre[2][j]     = float((cs[j] >> 8) & 255) * a;
            pre[3][j]     = float(cs[j] & 255) * a;
        }
        float v[4];
        for (int c = 0; c < 4; ++c)
            v[c] = pre[c][0] + (pre[c][1] - pre[c][0]) * u;
        const uint32_t a  = uint32_t(clamp01(v[0]) * 255.0f + 0.5f);
        auto           ch = [a](float x) {
            uint32_t c = uint32_t(x < 0 ? 0 : x + 0.5f);
            return c > a ? a : c;
        };
        o->lut[i] = (a << 24) | (ch(v[1]) << 16) | (ch(v[2]) << 8) | ch(v[3]);
    }
    return true;
}

} // namespace gfx

// gfx's own basic SVG renderer, used on every OS. Cold code (-Oz); it turns
// the document into device-space Paths and hands them to the Painter.
//
// Pipeline: a tiny XML tokenizer builds a flat node array (views into the
// input; entity-decoded values are copied), then one recursive walk resolves
// presentation attributes + style="" declarations, transforms and paints.
//
// Supported: svg (viewBox, preserveAspectRatio, nested), g, a, switch (all
// children), defs, symbol and use (href, x/y, symbol viewBox), path (all
// commands), rect (rx/ry), circle, ellipse, line, polyline, polygon;
// fill/stroke (none, colours, currentColor, url(#gradient) with fallback),
// opacities, fill-rule, clip-rule, stroke width/caps/joins/miterlimit/dashes,
// transform, display/visibility, linear + radial gradients (units, transform,
// spread, href inheritance; the radial focal point is ignored), clipPath
// (userSpaceOnUse). Group opacity and clipping render through a layer the
// size of what the group paints (a geometry-only pass measures it first),
// clipped through an 8-bit coverage mask.
// Skipped: text, images, filters, masks, patterns, markers, CSS <style>.
//
// Limits: 4 MB input, 200k nodes, depth 64, 8 nested uses, 3 nested layers,
// 100k element visits. Never recurses on its own input beyond those.
#include "gfx/internal.h"

#include "base/str.h"

#include <cmath>
#include <cstdlib>
#include <memory>

namespace gfx {

namespace {

using sv = std::string_view;

constexpr size_t kMaxInput = 4u << 20;
constexpr int    kMaxNodes = 200000, kMaxDepth = 64, kMaxUse = 8, kMaxLayers = 3;
constexpr int    kMaxVisits = 100000; // elements walked, uses expanded

// ── Names ───────────────────────────────────────────────────────────────────
enum Tag : uint8_t {
    TOther,
    TSvg,
    TG,
    TDefs,
    TPath,
    TRect,
    TCircle,
    TEllipse,
    TLine,
    TPolyline,
    TPolygon,
    TUse,
    TSymbol,
    TLinear,
    TRadial,
    TStop,
    TClip,
    TA,
    TSwitch,
};
// Tag names in Tag order, NUL-separated (index 1 = TSvg).
const char kTags[] = "svg\0g\0defs\0path\0rect\0circle\0ellipse\0line\0polyline\0polygon\0use\0symb"
                     "ol\0linearGradient\0radialGradient\0stop\0clipPath\0a\0switch\0";

enum Key : uint8_t {
    KNone,
    KId,
    KViewBox,
    KWidth,
    KHeight,
    KX,
    KY,
    KX1,
    KY1,
    KX2,
    KY2,
    KCx,
    KCy,
    KR,
    KRx,
    KRy,
    KD,
    KPoints,
    KTransform,
    KHref,
    KFill,
    KStroke,
    KStrokeWidth,
    KCap,
    KJoin,
    KMiter,
    KDash,
    KDashOffset,
    KOpacity,
    KFillOpacity,
    KStrokeOpacity,
    KFillRule,
    KClipRule,
    KClipPath,
    KColor,
    KDisplay,
    KVisibility,
    KOffset,
    KStopColor,
    KStopOpacity,
    KGradUnits,
    KGradTransform,
    KSpread,
    KAspect,
    KClipUnits,
    KStyle,
};
// Attribute/property names in Key order, NUL-separated (index 1 = KId).
const char kKeys[] =
    "id\0viewBox\0width\0height\0x\0y\0x1\0y1\0x2\0y2\0cx\0cy\0r\0rx\0ry\0d\0points\0transform\0hre"
    "f\0fill\0stroke\0stroke-width\0stroke-linecap\0stroke-linejoin\0stroke-miterlimit\0stroke-"
    "dasharray\0stroke-dashoffset\0opacity\0fill-opacity\0stroke-opacity\0fill-rule\0clip-"
    "rule\0clip-path\0color\0display\0visibility\0offset\0stop-color\0stop-"
    "opacity\0gradientUnits\0gradientTransform\0spreadMethod\0preserveAspectRatio\0clipPathUnits\0s"
    "tyle\0";

uint8_t lookup(const char *table, sv name) {
    uint8_t i = 1;
    for (const char *t = table; *t; t += std::strlen(t) + 1, ++i)
        if (name == t)
            return i;
    return 0;
}

// ── XML ─────────────────────────────────────────────────────────────────────
struct Attr {
    uint8_t key;
    sv      value;
};
struct Node {
    uint8_t  tag;
    int32_t  parent, first = -1, last = -1, next = -1;
    uint32_t a0 = 0, an = 0;
};
struct Doc {
    std::vector<Node>                    nodes;
    std::vector<Attr>                    attrs;
    std::vector<std::unique_ptr<char[]>> store; // entity-decoded values
    std::vector<std::pair<sv, int>>      ids;   // sorted by id

    [[gnu::noinline]] sv get(int n, Key k) const {
        const Node &nd = nodes[size_t(n)];
        for (uint32_t i = nd.an; i-- > 0;) // last wins: style="" comes after attributes
            if (attrs[nd.a0 + i].key == k)
                return attrs[nd.a0 + i].value;
        return {};
    }
    [[gnu::noinline]] int byId(sv id) const {
        size_t lo = 0, hi = ids.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (ids[mid].first < id)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo < ids.size() && ids[lo].first == id ? ids[lo].second : -1;
    }
};

inline bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

using str::trim;

// Entity-decoded copy of v, owned by the document (views into it stay valid).
sv decode(Doc &d, sv v) {
    if (v.find('&') == sv::npos)
        return v;
    const std::string o   = str::decodeEntities(v);
    auto              buf = std::make_unique<char[]>(o.size() + 1);
    std::memcpy(buf.get(), o.data(), o.size());
    const sv r(buf.get(), o.size());
    d.store.push_back(std::move(buf));
    return r;
}

// Appends the declarations of a style="" value (no selectors, no cascade).
void parseStyle(Doc &d, sv s) {
    while (!s.empty()) {
        const size_t semi  = s.find(';');
        const sv     decl  = s.substr(0, semi);
        s                  = semi == sv::npos ? sv{} : s.substr(semi + 1);
        const size_t colon = decl.find(':');
        if (colon == sv::npos)
            continue;
        sv value = trim(decl.substr(colon + 1));
        if (const size_t imp = value.find('!'); imp != sv::npos)
            value = trim(value.substr(0, imp));
        if (const uint8_t k = lookup(kKeys, trim(decl.substr(0, colon))); k && k != KStyle)
            d.attrs.push_back({k, value});
    }
}

sv localName(sv n) {
    const size_t c = n.find(':');
    return c == sv::npos ? n : n.substr(c + 1);
}

// Builds the node tree. Tolerant: unclosed elements close at the end, stray
// end tags are ignored. `rootOnly` stops after the first <svg> start tag.
bool parseXml(sv s, Doc *d, bool rootOnly) {
    std::vector<int32_t> stack;
    int                  skipDepth = 0; // levels beyond kMaxDepth, counted only
    size_t               i         = 0;
    auto                 skipTo    = [&](sv end) {
        const size_t e = s.find(end, i);
        i              = e == sv::npos ? s.size() : e + end.size();
    };
    while (i < s.size()) {
        const size_t lt = s.find('<', i);
        if (lt == sv::npos)
            break;
        i = lt + 1;
        if (s.substr(i, 3) == "!--") {
            skipTo("-->");
        } else if (s.substr(i, 8) == "![CDATA[") {
            skipTo("]]>");
        } else if (i < s.size() && (s[i] == '?' || s[i] == '!')) {
            // <?xml …?>, <!DOCTYPE … [internal subset]>
            const size_t gt = s.find('>', i), br = s.find('[', i);
            if (s[i] == '!' && br != sv::npos && br < gt)
                skipTo("]>");
            else
                i = gt == sv::npos ? s.size() : gt + 1;
        } else if (i < s.size() && s[i] == '/') {
            if (skipDepth)
                --skipDepth;
            else if (!stack.empty())
                stack.pop_back();
            skipTo(">");
        } else {
            size_t e = i;
            while (e < s.size() && !isSpace(s[e]) && s[e] != '/' && s[e] != '>')
                ++e;
            const sv name = localName(s.substr(i, e - i));
            i             = e;
            Node nd{lookup(kTags, name), stack.empty() ? -1 : stack.back()};
            nd.a0 = uint32_t(d->attrs.size());
            sv   style;
            bool selfClose = false;
            for (;;) {
                while (i < s.size() && isSpace(s[i]))
                    ++i;
                if (i >= s.size())
                    break;
                if (s[i] == '>') {
                    ++i;
                    break;
                }
                if (s[i] == '/') {
                    selfClose = true;
                    ++i;
                    continue;
                }
                size_t ne = i;
                while (ne < s.size() && !isSpace(s[ne]) && s[ne] != '=' && s[ne] != '>' &&
                       s[ne] != '/')
                    ++ne;
                const sv an = localName(s.substr(i, ne - i));
                i           = ne;
                while (i < s.size() && isSpace(s[i]))
                    ++i;
                if (i >= s.size() || s[i] != '=') {
                    if (ne == i && i < s.size() && s[i] != '>' && s[i] != '/')
                        ++i; // garbage: make progress
                    continue;
                }
                ++i;
                while (i < s.size() && isSpace(s[i]))
                    ++i;
                if (i >= s.size() || (s[i] != '"' && s[i] != '\''))
                    continue;
                const char   q  = s[i++];
                const size_t ve = s.find(q, i);
                if (ve == sv::npos)
                    return false; // truncated inside an attribute
                const sv value = decode(*d, s.substr(i, ve - i));
                i              = ve + 1;
                if (an == "style")
                    style = value;
                else if (const uint8_t k = lookup(kKeys, an))
                    d->attrs.push_back({k, value});
            }
            parseStyle(*d, style); // after the attributes, so it overrides them
            nd.an = uint32_t(d->attrs.size()) - nd.a0;
            if (skipDepth || int(stack.size()) >= kMaxDepth) {
                d->attrs.resize(nd.a0);
                if (!selfClose)
                    ++skipDepth;
                continue;
            }
            if (d->nodes.size() >= size_t(kMaxNodes))
                return false;
            const int32_t id = int32_t(d->nodes.size());
            d->nodes.push_back(nd);
            if (nd.parent >= 0) {
                Node &p = d->nodes[size_t(nd.parent)];
                if (p.last >= 0)
                    d->nodes[size_t(p.last)].next = id;
                else
                    p.first = id;
                p.last = id;
            }
            if (rootOnly && nd.tag == TSvg)
                return true;
            if (!selfClose)
                stack.push_back(id);
        }
    }
    return true;
}

int findRoot(const Doc &d) {
    for (size_t i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].tag == TSvg && d.nodes[i].parent < 0)
            return int(i);
    return -1;
}

// ── Values ──────────────────────────────────────────────────────────────────
// Own float parser: locale-independent and it reports where it stopped.
bool number(const char *&p, const char *e, float *out) {
    while (p < e && (isSpace(*p) || *p == ','))
        ++p;
    const char *s   = p;
    double      v   = 0;
    bool        neg = false, digits = false;
    if (p < e && (*p == '+' || *p == '-'))
        neg = *p++ == '-';
    while (p < e && *p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0'), digits = true;
    if (p < e && *p == '.') {
        double f = 0.1;
        for (++p; p < e && *p >= '0' && *p <= '9'; ++p, f *= 0.1)
            v += (*p - '0') * f, digits = true;
    }
    if (!digits) {
        p = s;
        return false;
    }
    if (p < e && (*p == 'e' || *p == 'E') && p + 1 < e &&
        (p[1] == '-' || p[1] == '+' || (p[1] >= '0' && p[1] <= '9'))) {
        const char *q  = p + 1;
        bool        en = false;
        if (*q == '+' || *q == '-')
            en = *q++ == '-';
        int ex = 0;
        while (q < e && *q >= '0' && *q <= '9')
            ex = ex < 400 ? ex * 10 + (*q++ - '0') : (++q, ex);
        v *= std::pow(10.0, en ? -ex : ex);
        p = q;
    }
    *out = float(neg ? -v : v);
    return std::isfinite(*out);
}

bool numberOf(sv s, float *out) {
    const char *p = s.data();
    return number(p, s.data() + s.size(), out);
}

// A length: number + optional unit. `ref` is what 100% means.
[[gnu::noinline]] float length(sv s, float ref, float def) {
    const char *p = s.data(), *e = s.data() + s.size();
    float       v;
    if (s.empty() || !number(p, e, &v))
        return def;
    const sv u = trim(sv(p, size_t(e - p)));
    if (u.empty() || u == "px")
        return v;
    if (u == "%")
        return v * ref / 100;
    struct Unit {
        char  n[3];
        float k;
    };
    static const Unit units[] = {
        {"pt", 4.0f / 3},
        {"pc", 16},
        {"mm", 3.7795f},
        {"cm", 37.795f},
        {"in", 96},
        {"em", 16},
        {"ex", 8}
    };
    for (const Unit &un : units)
        if (u == un.n)
            return v * un.k;
    return v;
}

const struct {
    char     name[8];
    uint32_t rgb;
} kNamed[] = {
    {"black", 0x000000},   {"white", 0xffffff},   {"red", 0xff0000},   {"green", 0x008000},
    {"blue", 0x0000ff},    {"yellow", 0xffff00},  {"cyan", 0x00ffff},  {"aqua", 0x00ffff},
    {"magenta", 0xff00ff}, {"fuchsia", 0xff00ff}, {"gray", 0x808080},  {"grey", 0x808080},
    {"silver", 0xc0c0c0},  {"maroon", 0x800000},  {"olive", 0x808000}, {"lime", 0x00ff00},
    {"navy", 0x000080},    {"purple", 0x800080},  {"teal", 0x008080},  {"orange", 0xffa500},
    {"gold", 0xffd700},    {"pink", 0xffc0cb},
};

// Straight 0xAARRGGBB; false if unparseable. currentColor → `current`.
bool color(sv s, Color current, Color *out) {
    s = trim(s);
    if (s.size() > 1 && s[0] == '#')
        return parseHexColor(s, out);
    if (s.substr(0, 4) == "rgb(" || s.substr(0, 5) == "rgba(") {
        const char *p = s.data() + s.find('(') + 1, *e = s.data() + s.size();
        float       c[4] = {0, 0, 0, 1};
        for (int i = 0; i < 4; ++i) {
            while (p < e && (isSpace(*p) || *p == ',' || *p == '/'))
                ++p;
            if (!number(p, e, &c[i]))
                break;
            if (p < e && *p == '%')
                c[i] = i < 3 ? c[i] * 2.55f : c[i] / 100, ++p;
        }
        auto b = [](float v, float k) { return uint32_t(clamp01(v / k) * 255 + 0.5f); };
        *out   = b(c[3], 1) << 24 | b(c[0], 255) << 16 | b(c[1], 255) << 8 | b(c[2], 255);
        return true;
    }
    if (s == "currentColor") {
        *out = current;
        return true;
    }
    if (s == "transparent") {
        *out = 0;
        return true;
    }
    for (const auto &n : kNamed)
        if (s.size() == std::strlen(n.name) &&
            std::equal(s.begin(), s.end(), n.name, [](char a, char b) { return (a | 32) == b; })) {
            *out = 0xff000000u | n.rgb;
            return true;
        }
    return false;
}

Affine transform(sv s) {
    Affine      m;
    const char *p = s.data(), *e = s.data() + s.size();
    while (p < e) {
        while (p < e && (isSpace(*p) || *p == ','))
            ++p;
        const char *n = p;
        while (p < e && *p != '(')
            ++p;
        const sv fn = trim(sv(n, size_t(p - n)));
        if (p >= e)
            break;
        ++p;
        float a[6] = {0, 0, 0, 0, 0, 0};
        int   k    = 0;
        while (k < 6 && number(p, e, &a[k]))
            ++k;
        while (p < e && *p != ')')
            ++p;
        ++p;
        Affine t;
        if (fn == "matrix" && k == 6)
            t = {a[0], a[1], a[2], a[3], a[4], a[5]};
        else if (fn == "translate")
            t.e = a[0], t.f = k > 1 ? a[1] : 0;
        else if (fn == "scale")
            t.a = a[0], t.d = k > 1 ? a[1] : a[0];
        else if (fn == "rotate") {
            const float r = a[0] * 0.017453293f, c = std::cos(r), sn = std::sin(r);
            t = {c, sn, -sn, c, 0, 0};
            if (k == 3)
                t = mul(mul(Affine{1, 0, 0, 1, a[1], a[2]}, t), Affine{1, 0, 0, 1, -a[1], -a[2]});
        } else if (fn == "skewX")
            t.c = std::tan(a[0] * 0.017453293f);
        else if (fn == "skewY")
            t.b = std::tan(a[0] * 0.017453293f);
        else
            break; // unknown: ignore the rest, as browsers do
        m = mul(m, t);
    }
    return m;
}

// viewBox → viewport per preserveAspectRatio.
Affine viewBoxMap(const float vb[4], float w, float h, sv par) {
    float sx = w / vb[2], sy = h / vb[3];
    par = trim(par);
    if (par.substr(0, 5) == "defer")
        par = trim(par.substr(5));
    int ax = 1, ay = 1; // 0 min, 1 mid, 2 max
    if (par.substr(0, 4) != "none") {
        if (par.size() >= 8) {
            ax = par.substr(1, 3) == "Min" ? 0 : par.substr(1, 3) == "Max" ? 2 : 1;
            ay = par.substr(5, 3) == "Min" ? 0 : par.substr(5, 3) == "Max" ? 2 : 1;
        }
        sx = sy = par.find("slice") != sv::npos ? std::fmax(sx, sy) : std::fmin(sx, sy);
    } else {
        ax = ay = 0;
    }
    return {
        sx,
        0,
        0,
        sy,
        -vb[0] * sx + (w - vb[2] * sx) * float(ax) * 0.5f,
        -vb[1] * sy + (h - vb[3] * sy) * float(ay) * 0.5f
    };
}

bool viewBox(sv s, float vb[4]) {
    const char *p = s.data(), *e = s.data() + s.size();
    for (int i = 0; i < 4; ++i)
        if (!number(p, e, &vb[i]))
            return false;
    return vb[2] > 0 && vb[3] > 0;
}

// ── Geometry ────────────────────────────────────────────────────────────────
// Collects a path in user space with its bounds, emitting device space.
struct Builder {
    Affine m;
    Path   path;
    float  x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f; // user-space bounds
    bool   ok = true;

    [[gnu::noinline]] PointF pt(float x, float y) {
        x0 = std::fmin(x0, x), y0 = std::fmin(y0, y), x1 = std::fmax(x1, x), y1 = std::fmax(y1, y);
        const PointF d = apply(m, x, y);
        ok &= std::fabs(d.x) < 1e7f && std::fabs(d.y) < 1e7f;
        return d;
    }
    [[gnu::noinline]] void move(float x, float y) {
        const PointF p = pt(x, y);
        path.moveTo(p.x, p.y);
    }
    [[gnu::noinline]] void line(float x, float y) {
        const PointF p = pt(x, y);
        path.lineTo(p.x, p.y);
    }
    [[gnu::noinline]] void cubic(float a, float b, float c, float d, float x, float y) {
        const PointF p = pt(a, b), q = pt(c, d), r = pt(x, y);
        path.cubicTo(p.x, p.y, q.x, q.y, r.x, r.y);
    }
    void close() { path.close(); }
    // Rect with elliptical corners (rx, ry ≤ half the size), clockwise from
    // the top edge; an ellipse is the rect with full radii.
    void roundRect(float x, float y, float w, float h, float rx, float ry) {
        static const int8_t C[5] = {0, 1, 0, -1, 0}, S[5] = {-1, 0, 1, 0, -1};
        const float         k = 0.5522847f;
        for (int i = 0; i < 4; ++i) {
            const float cx = i < 2 ? x + w - rx : x + rx,
                        cy = i == 1 || i == 2 ? y + h - ry : y + ry;
            const float x0 = cx + rx * C[i], y0 = cy + ry * S[i];
            const float x1 = cx + rx * C[i + 1], y1 = cy + ry * S[i + 1];
            i ? line(x0, y0) : move(x0, y0);
            if (rx > 0 && ry > 0)
                cubic(
                    x0 - k * rx * S[i],
                    y0 + k * ry * C[i],
                    x1 + k * rx * S[i + 1],
                    y1 - k * ry * C[i + 1],
                    x1,
                    y1
                );
        }
        close();
    }
    // SVG endpoint arc → cubics (SVG implementation notes F.6).
    void
    arc(float x1_,
        float y1_,
        float rx,
        float ry,
        float phi,
        bool  large,
        bool  sweep,
        float x2,
        float y2) {
        if (rx == 0 || ry == 0) {
            line(x2, y2);
            return;
        }
        if (x1_ == x2 && y1_ == y2)
            return;
        rx = std::fabs(rx), ry = std::fabs(ry);
        const float cp = std::cos(phi * 0.017453293f), sp = std::sin(phi * 0.017453293f);
        const float dx = (x1_ - x2) / 2, dy = (y1_ - y2) / 2;
        const float xp = cp * dx + sp * dy, yp = -sp * dx + cp * dy;
        const float lam = xp * xp / (rx * rx) + yp * yp / (ry * ry);
        if (lam > 1)
            rx *= std::sqrt(lam), ry *= std::sqrt(lam);
        const float num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp;
        const float den = rx * rx * yp * yp + ry * ry * xp * xp;
        float       co  = den > 0 ? std::sqrt(std::fmax(0.0f, num / den)) : 0;
        if (large == sweep)
            co = -co;
        const float cxp = co * rx * yp / ry, cyp = -co * ry * xp / rx;
        const float cx  = cp * cxp - sp * cyp + (x1_ + x2) / 2,
                    cy  = sp * cxp + cp * cyp + (y1_ + y2) / 2;
        auto        ang = [](float ux, float uy, float vx, float vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const float t1 = ang(1, 0, (xp - cxp) / rx, (yp - cyp) / ry);
        float       dt = ang((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry);
        if (!sweep && dt > 0)
            dt -= 6.2831853f;
        else if (sweep && dt < 0)
            dt += 6.2831853f;
        const int   n    = std::max(1, int(std::ceil(std::fabs(dt) / 1.5707964f - 1e-4f)));
        const float step = dt / float(n), k = 4.0f / 3 * std::tan(step / 4);
        float       t = t1;
        for (int i = 0; i < n; ++i, t += step) {
            const float ca = std::cos(t), sa = std::sin(t), cb = std::cos(t + step),
                        sb = std::sin(t + step);
            const float ax = cp * rx * ca - sp * ry * sa + cx,
                        ay = sp * rx * ca + cp * ry * sa + cy;
            float bx = cp * rx * cb - sp * ry * sb + cx, by = sp * rx * cb + cp * ry * sb + cy;
            const float dax = -cp * rx * sa - sp * ry * ca, day = -sp * rx * sa + cp * ry * ca;
            const float dbx = -cp * rx * sb - sp * ry * cb, dby = -sp * rx * sb + cp * ry * cb;
            if (i == n - 1)
                bx = x2, by = y2;
            cubic(ax + k * dax, ay + k * day, bx - k * dbx, by - k * dby, bx, by);
        }
    }
};

void pathData(sv d, Builder &b) {
    const char *p = d.data(), *e = d.data() + d.size();
    float cx = 0, cy = 0, sx = 0, sy = 0, lx = 0, ly = 0; // current, subpath start, last control
    char  cmd = 0, prev = 0;
    bool  started = false;
    auto  flag    = [&](bool *out) {
        while (p < e && (isSpace(*p) || *p == ','))
            ++p;
        if (p >= e || (*p != '0' && *p != '1'))
            return false;
        *out = *p++ == '1';
        return true;
    };
    while (p < e) {
        while (p < e && (isSpace(*p) || *p == ','))
            ++p;
        if (p >= e)
            break;
        if ((*p | 32) >= 'a' && (*p | 32) <= 'z') {
            cmd = *p++;
        } else if (!cmd) {
            return;
        }
        const bool  rel = cmd >= 'a';
        const char  c   = char(cmd & ~32);
        const float ox = rel ? cx : 0, oy = rel ? cy : 0;
        float       v[7];
        auto        nums = [&](int n) {
            for (int i = 0; i < n; ++i)
                if (!number(p, e, &v[i]))
                    return false;
            return true;
        };
        if (c == 'Z') {
            b.close();
            cx = sx, cy = sy;
            prev = 'Z';
            // Z takes no numbers: a following number is an error, stop.
            while (p < e && (isSpace(*p) || *p == ','))
                ++p;
            if (p < e && !((*p | 32) >= 'a' && (*p | 32) <= 'z'))
                return;
            continue;
        }
        if (!started && c != 'M')
            return; // must start with a moveto
        bool ok = true;
        switch (c) {
        case 'M':
            if ((ok = nums(2))) {
                cx = v[0] + ox, cy = v[1] + oy, sx = cx, sy = cy;
                b.move(cx, cy);
                started = true;
                cmd     = rel ? 'l' : 'L'; // implicit lineto after the first pair
            }
            break;
        case 'L':
            if ((ok = nums(2)))
                cx = v[0] + ox, cy = v[1] + oy, b.line(cx, cy);
            break;
        case 'H':
            if ((ok = nums(1)))
                cx = v[0] + ox, b.line(cx, cy);
            break;
        case 'V':
            if ((ok = nums(1)))
                cy = v[0] + oy, b.line(cx, cy);
            break;
        case 'C':
        case 'S': {
            float x1, y1;
            if (c == 'C') {
                if (!(ok = nums(6)))
                    break;
                x1 = v[0] + ox, y1 = v[1] + oy;
                v[0] = v[2], v[1] = v[3], v[2] = v[4], v[3] = v[5];
            } else {
                if (!(ok = nums(4)))
                    break;
                const bool refl = prev == 'C' || prev == 'S';
                x1 = refl ? 2 * cx - lx : cx, y1 = refl ? 2 * cy - ly : cy;
            }
            lx = v[0] + ox, ly = v[1] + oy;
            b.cubic(x1, y1, lx, ly, cx = v[2] + ox, cy = v[3] + oy);
            break;
        }
        case 'Q':
        case 'T': {
            float qx, qy;
            if (c == 'Q') {
                if (!(ok = nums(4)))
                    break;
                qx = v[0] + ox, qy = v[1] + oy, v[0] = v[2], v[1] = v[3];
            } else {
                if (!(ok = nums(2)))
                    break;
                const bool refl = prev == 'Q' || prev == 'T';
                qx = refl ? 2 * cx - lx : cx, qy = refl ? 2 * cy - ly : cy;
            }
            const float x = v[0] + ox, y = v[1] + oy;
            b.cubic(
                cx + 2.0f / 3 * (qx - cx),
                cy + 2.0f / 3 * (qy - cy),
                x + 2.0f / 3 * (qx - x),
                y + 2.0f / 3 * (qy - y),
                x,
                y
            );
            lx = qx, ly = qy, cx = x, cy = y;
            break;
        }
        case 'A': {
            bool large, sweep;
            if (!(ok = nums(3) && flag(&large) && flag(&sweep) && number(p, e, &v[3]) &&
                       number(p, e, &v[4])))
                break;
            const float x = v[3] + ox, y = v[4] + oy;
            b.arc(cx, cy, v[0], v[1], v[2], large, sweep, x, y);
            cx = x, cy = y;
            break;
        }
        default:
            return;
        }
        if (!ok)
            return; // render up to the first error, like browsers
        prev = c;
    }
}

void points(sv s, Builder &b, bool close) {
    const char *p = s.data(), *e = s.data() + s.size();
    float       x, y;
    for (bool first = true; number(p, e, &x) && number(p, e, &y); first = false)
        first ? b.move(x, y) : b.line(x, y);
    if (close)
        b.close();
}

// ── Rendering ───────────────────────────────────────────────────────────────
struct Paint {
    uint8_t kind = 0; // 0 none, 1 colour, 2 gradient node
    Color   c    = 0;
    int     ref  = -1;
};
struct Style {
    Paint        fill{1, 0xff000000u}, stroke;
    Color        color  = 0xff000000u;
    float        fillOp = 1, strokeOp = 1, alpha = 1; // alpha: group opacity folded in
    float        width = 1, miter = 4, dashOff = 0;
    float        dash[16];
    int          ndash    = 0;
    FillRule     fillRule = FillRule::NonZero, clipRule = FillRule::NonZero;
    Stroke::Cap  cap     = Stroke::Cap::Butt;
    Stroke::Join join    = Stroke::Join::Miter;
    bool         visible = true;
};

struct Ctx {
    Doc     &d;
    Painter *p;
    int      w, h;
    float    vw, vh; // viewport, for percentages
    int      visits = 0, useDepth = 0, layers = 0;
    // The current target's rect in output pixels (a layer covers only part).
    int      tx = 0, ty = 0, tw = w, th = h;
    // Measuring: paint nothing, grow the device-space bounds instead.
    bool     measure = false;
    float    bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;

    [[gnu::noinline]] float lx(sv s, float def = 0) const { return length(s, vw, def); }
    [[gnu::noinline]] float ly(sv s, float def = 0) const { return length(s, vh, def); }
    [[gnu::noinline]] float ld(sv s, float def = 0) const {
        return length(s, std::sqrt((vw * vw + vh * vh) / 2), def);
    }
};

void paint(Ctx &c, sv v, const Style &s, Paint *out) {
    v = trim(v);
    if (v.empty() || v == "inherit")
        return;
    if (v == "none") {
        *out = {};
        return;
    }
    if (v.substr(0, 4) == "url(") {
        const size_t close = v.find(')');
        sv           id    = trim(v.substr(4, close == sv::npos ? sv::npos : close - 4));
        if (!id.empty() && (id.front() == '"' || id.front() == '\''))
            id = id.substr(1, id.size() >= 2 ? id.size() - 2 : 0);
        if (!id.empty() && id[0] == '#')
            id.remove_prefix(1);
        const int ref = c.d.byId(id);
        if (ref >= 0 &&
            (c.d.nodes[size_t(ref)].tag == TLinear || c.d.nodes[size_t(ref)].tag == TRadial)) {
            *out = {2, 0, ref};
            return;
        }
        // Missing or unsupported (pattern): the fallback colour, else none.
        const sv fb = close == sv::npos ? sv{} : trim(v.substr(close + 1));
        Color    col;
        *out = !fb.empty() && color(fb, s.color, &col) ? Paint{1, col} : Paint{};
        return;
    }
    Color col;
    if (color(v, s.color, &col))
        *out = {1, col};
}

float opacityOf(sv v, float def) {
    float o;
    if (!numberOf(v, &o))
        return def;
    if (v.find('%') != sv::npos)
        o /= 100;
    return clamp01(o);
}

// Inherited properties of node n on top of the parent's.
Style computeStyle(Ctx &c, int n, const Style &parent) {
    Style s = parent;
    sv    v;
    if (!(v = c.d.get(n, KColor)).empty())
        color(v, parent.color, &s.color);
    paint(c, c.d.get(n, KFill), s, &s.fill);
    paint(c, c.d.get(n, KStroke), s, &s.stroke);
    if (!(v = c.d.get(n, KFillOpacity)).empty())
        s.fillOp = opacityOf(v, 1);
    if (!(v = c.d.get(n, KStrokeOpacity)).empty())
        s.strokeOp = opacityOf(v, 1);
    if (!(v = c.d.get(n, KStrokeWidth)).empty())
        s.width = std::fmax(0.0f, c.ld(v, s.width));
    if (!(v = c.d.get(n, KMiter)).empty())
        numberOf(v, &s.miter);
    if (!(v = c.d.get(n, KDashOffset)).empty())
        s.dashOff = c.ld(v);
    if (!(v = c.d.get(n, KDash)).empty()) {
        s.ndash       = 0;
        const char *p = v.data(), *e = v.data() + v.size();
        float       x;
        while (s.ndash < 16 && number(p, e, &x)) {
            if (p < e && *p == '%')
                x = x * std::sqrt((c.vw * c.vw + c.vh * c.vh) / 2) / 100, ++p;
            s.dash[s.ndash++] = x;
            while (p < e && !isSpace(*p) && *p != ',' && !(*p >= '0' && *p <= '9') && *p != '.' &&
                   *p != '-')
                ++p; // units
        }
    }
    if (!(v = c.d.get(n, KFillRule)).empty())
        s.fillRule = v == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
    if (!(v = c.d.get(n, KClipRule)).empty())
        s.clipRule = v == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
    if (!(v = c.d.get(n, KCap)).empty())
        s.cap = v == "round"    ? Stroke::Cap::Round
                : v == "square" ? Stroke::Cap::Square
                                : Stroke::Cap::Butt;
    if (!(v = c.d.get(n, KJoin)).empty())
        s.join = v == "round"   ? Stroke::Join::Round
                 : v == "bevel" ? Stroke::Join::Bevel
                                : Stroke::Join::Miter;
    if (!(v = c.d.get(n, KVisibility)).empty())
        s.visible = v != "hidden" && v != "collapse";
    return s;
}

// Resolves a gradient (following href for missing attributes and stops).
bool gradient(Ctx &c, int n, const Builder &bb, const Affine &ctm, float opacity, Gradient *g) {
    int chain[kMaxUse], len = 0;
    for (int k = n; k >= 0 && len < kMaxUse;) {
        chain[len++] = k;
        sv href      = c.d.get(k, KHref);
        if (href.empty() || href[0] != '#')
            break;
        k = c.d.byId(href.substr(1));
        for (int j = 0; j < len; ++j)
            if (chain[j] == k)
                k = -1; // cycle
        if (k >= 0 && c.d.nodes[size_t(k)].tag != TLinear && c.d.nodes[size_t(k)].tag != TRadial)
            k = -1;
    }
    auto attr = [&](Key key) {
        for (int j = 0; j < len; ++j)
            if (sv v = c.d.get(chain[j], key); !v.empty())
                return v;
        return sv{};
    };
    const bool obb = attr(KGradUnits) != "userSpaceOnUse";
    // Percentages refer to the bounding box (obb, where 1 = 100%) or viewport.
    auto       val = [&](Key key, float def, int axis) {
        const sv v = attr(key);
        if (v.empty())
            return def;
        if (obb) {
            float x;
            if (!numberOf(v, &x))
                return def;
            return v.find('%') != sv::npos ? x / 100 : x;
        }
        return axis == 0 ? c.lx(v, def) : axis == 1 ? c.ly(v, def) : c.ld(v, def);
    };
    const bool radial = c.d.nodes[size_t(n)].tag == TRadial;
    g->kind           = radial ? Gradient::Kind::Radial : Gradient::Kind::Linear;
    const float fw = obb ? 1 : c.vw, fh = obb ? 1 : c.vh;
    if (radial) {
        g->a      = {val(KCx, 0.5f * fw, 0), val(KCy, 0.5f * fh, 1)};
        g->radius = val(KR, obb ? 0.5f : 0.5f * std::sqrt((c.vw * c.vw + c.vh * c.vh) / 2), 2);
    } else {
        g->a = {val(KX1, 0, 0), val(KY1, 0, 1)};
        g->b = {val(KX2, fw, 0), val(KY2, 0, 1)};
    }
    const sv sp = attr(KSpread);
    g->spread   = sp == "reflect"  ? Gradient::Spread::Reflect
                  : sp == "repeat" ? Gradient::Spread::Repeat
                                   : Gradient::Spread::Pad;
    Affine m    = transform(attr(KGradTransform));
    if (obb) {
        const float bw = bb.x1 - bb.x0, bh = bb.y1 - bb.y0;
        if (!(bw > 0 && bh > 0))
            return false; // SVG: objectBoundingBox on a zero-area box paints nothing
        m = mul(Affine{bw, 0, 0, bh, bb.x0, bb.y0}, m);
    }
    if (!invert(mul(ctm, m), &g->toGradient))
        return false;
    g->stops.clear();
    for (int j = 0; j < len && g->stops.empty(); ++j)
        for (int k = c.d.nodes[size_t(chain[j])].first; k >= 0 && g->stops.size() < 256;
             k     = c.d.nodes[size_t(k)].next) {
            if (c.d.nodes[size_t(k)].tag != TStop)
                continue;
            float off = 0;
            sv    ov  = c.d.get(k, KOffset);
            if (numberOf(ov, &off) && ov.find('%') != sv::npos)
                off /= 100;
            off = clamp01(off);
            if (!g->stops.empty())
                off = std::fmax(off, g->stops.back().offset);
            Color col = 0xff000000u;
            color(c.d.get(k, KStopColor), 0xff000000u, &col);
            const float op = opacityOf(c.d.get(k, KStopOpacity), 1) * opacity;
            g->stops.push_back({off, withAlpha(col, op)});
        }
    return !g->stops.empty();
}

// Grows the measured bounds by the path's points (curves stay inside their
// control points), `pad` around them.
void grow(Ctx &c, const Path &path, float pad) {
    const std::vector<float> &v = path.pts();
    for (size_t i = 0; i + 1 < v.size(); i += 2) {
        c.bx0 = std::fmin(c.bx0, v[i] - pad), c.bx1 = std::fmax(c.bx1, v[i] + pad);
        c.by0 = std::fmin(c.by0, v[i + 1] - pad), c.by1 = std::fmax(c.by1, v[i + 1] + pad);
    }
}

void fillOrStroke(Ctx &c, const Builder &b, const Style &s, const Affine &ctm, bool fill) {
    const Paint &pt = fill ? s.fill : s.stroke;
    const float  op = (fill ? s.fillOp : s.strokeOp) * s.alpha;
    if (!pt.kind || op <= 0)
        return;
    Stroke st;
    float  dash[16];
    if (!fill) {
        // Device-space stroking: widths scale by the transform's mean scale
        // (exact for uniform scales, approximate under skew/non-uniform).
        const float k = std::sqrt(std::fabs(ctm.a * ctm.d - ctm.b * ctm.c));
        st.width      = s.width * k;
        if (!(st.width > 0))
            return;
        st.cap = s.cap, st.join = s.join, st.miterLimit = s.miter;
        for (int i = 0; i < s.ndash; ++i)
            dash[i] = s.dash[i] * k;
        st.dashes = dash, st.dashCount = s.ndash, st.dashOffset = s.dashOff * k;
    }
    if (c.measure) { // miter tips reach miterLimit × half width, square caps √2 ×
        grow(c, b.path, fill ? 0 : st.width * 0.5f * std::fmax(st.miterLimit, 1.5f));
        return;
    }
    if (pt.kind == 1) {
        const Color col = withAlpha(pt.c, op);
        fill ? c.p->fillPath(b.path, col, s.fillRule) : c.p->strokePath(b.path, st, col);
        return;
    }
    Gradient g;
    if (!gradient(c, pt.ref, b, ctm, op, &g))
        return;
    fill ? c.p->fillPath(b.path, g, s.fillRule) : c.p->strokePath(b.path, st, g);
}

bool shapeGeometry(Ctx &c, int n, Builder &b) {
    const Doc &d = c.d;
    switch (d.nodes[size_t(n)].tag) {
    case TPath:
        pathData(d.get(n, KD), b);
        break;
    case TRect: {
        const float x = c.lx(d.get(n, KX)), y = c.ly(d.get(n, KY));
        const float w = c.lx(d.get(n, KWidth)), h = c.ly(d.get(n, KHeight));
        if (!(w > 0 && h > 0))
            return false;
        const sv srx = d.get(n, KRx), sry = d.get(n, KRy);
        float    rx = c.lx(srx, -1), ry = c.ly(sry, -1);
        if (rx < 0)
            rx = ry;
        if (ry < 0)
            ry = rx;
        rx = std::fmin(std::fmax(rx, 0.0f), w / 2), ry = std::fmin(std::fmax(ry, 0.0f), h / 2);
        b.roundRect(x, y, w, h, rx, ry);
        break;
    }
    case TCircle:
    case TEllipse: {
        const bool  circ = d.nodes[size_t(n)].tag == TCircle;
        const float r    = c.ld(d.get(n, KR));
        const float rx = circ ? r : c.lx(d.get(n, KRx)), ry = circ ? r : c.ly(d.get(n, KRy));
        if (!(rx > 0 && ry > 0))
            return false;
        b.roundRect(c.lx(d.get(n, KCx)) - rx, c.ly(d.get(n, KCy)) - ry, 2 * rx, 2 * ry, rx, ry);
        break;
    }
    case TLine:
        b.move(c.lx(d.get(n, KX1)), c.ly(d.get(n, KY1)));
        b.line(c.lx(d.get(n, KX2)), c.ly(d.get(n, KY2)));
        break;
    case TPolyline:
    case TPolygon:
        points(d.get(n, KPoints), b, d.nodes[size_t(n)].tag == TPolygon);
        break;
    default:
        return false;
    }
    return b.ok && !b.path.empty();
}

void renderNode(Ctx &c, int n, const Style &parent, const Affine &ctm, int depth);

void renderChildren(Ctx &c, int n, const Style &s, const Affine &ctm, int depth) {
    for (int k = c.d.nodes[size_t(n)].first; k >= 0 && c.visits < kMaxVisits;
         k     = c.d.nodes[size_t(k)].next)
        renderNode(c, k, s, ctm, depth + 1);
}

// Coverage of a clipPath (the union of its shapes) into the 8-bit `mask` for
// the output rect (mx, my, mw × mh); without a mask, measures its bounds.
// False when unsupported: render unclipped.
bool clipMask(Ctx &c, int clip, const Affine &ctm, uint8_t *mask, int mx, int my, int mw, int mh) {
    if (c.d.get(clip, KClipUnits) == "objectBoundingBox")
        return false;
    const Affine m = mul(ctm, transform(c.d.get(clip, KTransform)));
    Style        base;
    for (int k = c.d.nodes[size_t(clip)].first; k >= 0; k = c.d.nodes[size_t(k)].next) {
        int shape = k;
        if (c.d.nodes[size_t(k)].tag == TUse) { // a use of a plain shape
            const sv href = c.d.get(k, KHref);
            shape         = href.size() > 1 && href[0] == '#' ? c.d.byId(href.substr(1)) : -1;
            if (shape < 0)
                continue;
        }
        const Style s = computeStyle(c, shape, base);
        if (c.d.get(k, KDisplay) == "none")
            continue;
        Builder b;
        b.m = mul(m, transform(c.d.get(k, KTransform)));
        if (shape != k)
            b.m =
                mul(mul(b.m, Affine{1, 0, 0, 1, c.lx(c.d.get(k, KX)), c.ly(c.d.get(k, KY))}),
                    transform(c.d.get(shape, KTransform)));
        if (!shapeGeometry(c, shape, b))
            continue;
        if (mask)
            fillMask(b.path, s.clipRule, mask, mx, my, mw, mh);
        else
            grow(c, b.path, 0);
    }
    return true;
}

// The element's own content: what a layer, if any, captures. Returns the
// transform its clip-path applies in.
Affine renderBody(Ctx &c, int n, const Style &s, Affine m, int depth) {
    const Doc  &d   = c.d;
    const Node &nd  = d.nodes[size_t(n)];
    const Tag   tag = Tag(nd.tag);
    if (tag == TUse) {
        const sv  href   = d.get(n, KHref);
        const int target = href.size() > 1 && href[0] == '#' ? d.byId(href.substr(1)) : -1;
        if (target >= 0 && c.useDepth < kMaxUse) {
            Affine t = mul(m, Affine{1, 0, 0, 1, c.lx(d.get(n, KX)), c.ly(d.get(n, KY))});
            ++c.useDepth;
            if (d.nodes[size_t(target)].tag == TSymbol) {
                float vb[4];
                if (viewBox(d.get(target, KViewBox), vb))
                    t =
                        mul(t,
                            viewBoxMap(
                                vb,
                                c.lx(d.get(n, KWidth), c.vw),
                                c.ly(d.get(n, KHeight), c.vh),
                                d.get(target, KAspect)
                            ));
                renderChildren(c, target, s, t, depth);
            } else {
                renderNode(c, target, s, t, depth + 1);
            }
            --c.useDepth;
        }
    } else if (tag == TSvg) { // nested viewport (the root is set up by the caller)
        if (nd.parent >= 0) {
            float vb[4];
            m = mul(m, Affine{1, 0, 0, 1, c.lx(d.get(n, KX)), c.ly(d.get(n, KY))});
            if (viewBox(d.get(n, KViewBox), vb))
                m =
                    mul(m,
                        viewBoxMap(
                            vb,
                            c.lx(d.get(n, KWidth), c.vw),
                            c.ly(d.get(n, KHeight), c.vh),
                            d.get(n, KAspect)
                        ));
        }
        renderChildren(c, n, s, m, depth);
    } else if (tag == TG || tag == TA || tag == TSwitch) {
        renderChildren(c, n, s, m, depth);
    } else if (s.visible) {
        Builder b;
        b.m = m;
        if (shapeGeometry(c, n, b)) {
            if (tag != TLine) // polylines fill too, implicitly closed
                fillOrStroke(c, b, s, m, true);
            fillOrStroke(c, b, s, m, false);
        }
    }
    return m;
}

void renderNode(Ctx &c, int n, const Style &parent, const Affine &ctm, int depth) {
    const Doc  &d   = c.d;
    const Node &nd  = d.nodes[size_t(n)];
    const Tag   tag = Tag(nd.tag);
    if (++c.visits > kMaxVisits)
        return;
    if (tag == TOther || tag == TDefs || tag == TLinear || tag == TRadial || tag == TStop ||
        tag == TClip || tag == TSymbol || depth > kMaxDepth + kMaxUse * 2 ||
        d.get(n, KDisplay) == "none")
        return;
    Style  s = computeStyle(c, n, parent);
    Affine m = mul(ctm, transform(d.get(n, KTransform)));

    // Group opacity and clip-path need an offscreen layer; without the budget
    // for one, opacity folds into the children (overlaps then show through)
    // and the clip is dropped.
    const float op   = opacityOf(d.get(n, KOpacity), 1);
    int         clip = -1;
    if (sv cp = trim(d.get(n, KClipPath)); cp.substr(0, 5) == "url(#") {
        const size_t e = cp.find(')');
        clip           = d.byId(cp.substr(5, e == sv::npos ? sv::npos : e - 5));
        if (clip >= 0 && d.nodes[size_t(clip)].tag != TClip)
            clip = -1;
    }
    if (op <= 0)
        return;
    const bool leaf      = tag != TG && tag != TA && tag != TSwitch && tag != TSvg && tag != TUse;
    const bool needLayer = clip >= 0 || (op < 1 && (!leaf || (s.fill.kind && s.stroke.kind)));
    bool       useLayer  = needLayer && !c.measure && c.layers < kMaxLayers;
    int        lx = 0, ly = 0, lw = 0, lh = 0; // the layer, in output pixels
    if (useLayer) {
        // Measure what the group paints (and its clip lets through), padded
        // for anti-aliasing and kept inside the current target.
        const int visits = c.visits;
        c.measure        = true;
        c.bx0 = c.by0 = 1e30f, c.bx1 = c.by1 = -1e30f;
        const Affine cm = renderBody(c, n, s, m, depth);
        c.measure       = false;
        float x0 = c.bx0, y0 = c.by0, x1 = c.bx1, y1 = c.by1;
        c.bx0 = c.by0 = 1e30f, c.bx1 = c.by1 = -1e30f;
        if (clip >= 0 && clipMask(c, clip, cm, nullptr, 0, 0, 0, 0))
            x0 = std::fmax(x0, c.bx0), y0 = std::fmax(y0, c.by0), x1 = std::fmin(x1, c.bx1),
            y1 = std::fmin(y1, c.by1);
        lx = std::max(c.tx, floori(x0) - 1), ly = std::max(c.ty, floori(y0) - 1);
        lw = std::min(c.tx + c.tw, ceili(x1) + 1) - lx,
        lh = std::min(c.ty + c.th, ceili(y1) + 1) - ly;
        if (lw <= 0 || lh <= 0)
            return; // nothing shows; the measuring counted the visits rendering would
        c.visits = visits;
        useLayer = int64_t(lw) * lh <= (16 << 20);
    }
    Bitmap                   layer;
    Painter                 *outer = c.p;
    const int                otx = c.tx, oty = c.ty, otw = c.tw, oth = c.th;
    std::unique_ptr<Painter> lp;
    if (useLayer) {
        layer = Bitmap(lw, lh);
        lp    = std::make_unique<Painter>(layer.view(), 1);
        PainterImpl::placeTarget(*lp, lx, ly); // same coordinates as the output
        c.p  = lp.get();
        c.tx = lx, c.ty = ly, c.tw = lw, c.th = lh;
        ++c.layers;
    } else {
        s.alpha *= op;
    }

    m = renderBody(c, n, s, m, depth);

    if (useLayer) {
        --c.layers;
        c.p  = outer;
        c.tx = otx, c.ty = oty, c.tw = otw, c.th = oth;
        std::vector<uint8_t> mask;
        if (clip >= 0) {
            mask.assign(size_t(lw) * size_t(lh), 0);
            if (!clipMask(c, clip, m, mask.data(), lx, ly, lw, lh))
                mask.clear();
        }
        // Composite the layer: × opacity × clip coverage, source-over.
        const uint32_t   a  = uint32_t(op * 255 + 0.5f);
        const BitmapView lv = layer.view();
        for (int y = 0; y < lh; ++y) {
            uint32_t      *row = lv.pixels + size_t(y) * size_t(lw);
            const uint8_t *mr  = mask.empty() ? nullptr : mask.data() + size_t(y) * size_t(lw);
            for (int x = 0; x < lw; ++x) {
                const uint32_t f = mr ? div255(mr[x] * a) : a;
                row[x]           = f >= 255 ? row[x] : mulPx(row[x], f);
            }
        }
        c.p->blitColor(lv, lx, ly);
    }
}

} // namespace

bool svgSize(std::string_view svg, float *w, float *h) {
    Doc d;
    if (svg.size() > kMaxInput || !parseXml(svg, &d, true))
        return false;
    const int r = findRoot(d);
    if (r < 0)
        return false;
    float      vb[4];
    const bool hasVb = viewBox(d.get(r, KViewBox), vb);
    const sv   sw = d.get(r, KWidth), sh = d.get(r, KHeight);
    // Percentages say nothing about the intrinsic size.
    float      W = sw.find('%') == sv::npos ? length(sw, 0, -1) : -1;
    float      H = sh.find('%') == sv::npos ? length(sh, 0, -1) : -1;
    if (W <= 0 && H <= 0 && hasVb)
        W = vb[2], H = vb[3];
    else if (W <= 0 && H > 0)
        W = hasVb ? H * vb[2] / vb[3] : -1;
    else if (H <= 0 && W > 0)
        H = hasVb ? W * vb[3] / vb[2] : -1;
    if (!(W > 0 && H > 0))
        return false;
    *w = W, *h = H;
    return true;
}

bool renderSvg(std::string_view svg, int width, int height, Bitmap *out) {
    if (width <= 0 || height <= 0 || int64_t(width) * height > kMaxImagePixels ||
        svg.size() > kMaxInput)
        return false;
    Doc d;
    if (!parseXml(svg, &d, false))
        return false;
    const int root = findRoot(d);
    if (root < 0)
        return false;
    for (size_t i = 0; i < d.nodes.size(); ++i)
        if (sv id = d.get(int(i), KId); !id.empty())
            d.ids.push_back({id, int(i)});
    if (!d.ids.empty())
        std::qsort(d.ids.data(), d.ids.size(), sizeof(d.ids[0]), [](const void *a, const void *b) {
            const sv x = static_cast<const std::pair<sv, int> *>(a)->first;
            const sv y = static_cast<const std::pair<sv, int> *>(b)->first;
            return x < y ? -1 : y < x ? 1 : 0;
        });

    // The root's coordinate system: its viewBox, else its width × height,
    // fitted into the output.
    float vb[4];
    if (!viewBox(d.get(root, KViewBox), vb)) {
        vb[0] = vb[1] = 0;
        vb[2]         = length(d.get(root, KWidth), float(width), float(width));
        vb[3]         = length(d.get(root, KHeight), float(height), float(height));
        if (!(vb[2] > 0 && vb[3] > 0))
            vb[2] = float(width), vb[3] = float(height);
    }
    *out = Bitmap(width, height);
    Painter p(out->view(), 1);
    Ctx     c{d, &p, width, height, vb[2], vb[3]};
    Style   base;
    renderNode(c, root, base, viewBoxMap(vb, float(width), float(height), d.get(root, KAspect)), 0);
    return true;
}

} // namespace gfx

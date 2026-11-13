// Rich-text layout: itemise (span × font fallback × bidi level × script),
// shape each run once with HarfBuzz, break lines over shaped clusters, reorder
// each line visually (UAX #9 L2), then paint through the glyph cache.
//
// Everything inside is in physical pixels (the layout is built for one
// scale); the public API converts to logical units at the boundary.
#include "text/fonts.h"
#include "text/glyph_cache.h"
#include "text/text.h"
#include "text/unicode.h"

#include "base/utf8.h"

#include <hb.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace text {

using fonts::FontKey;
using fonts::kNoFont;

namespace {

// The test diagnostics layoutBuilds()/layoutTextOwned(): counted only in
// test and demo builds (MSGA_TEXT_COUNTERS), so release builds pay nothing.
#ifdef MSGA_TEXT_COUNTERS
std::atomic<size_t> gBuilds{0};
std::atomic<size_t> gOwned{0};
#endif
void countBuild() {
#ifdef MSGA_TEXT_COUNTERS
    gBuilds.fetch_add(1, std::memory_order_relaxed);
#endif
}
void countOwned([[maybe_unused]] size_t n) {
#ifdef MSGA_TEXT_COUNTERS
    gOwned.fetch_add(n, std::memory_order_relaxed);
#endif
}

// Same primary font at the same size.
bool sameFont(const Style &a, const Style &b) {
    return a.size == b.size && a.weight == b.weight && a.italic == b.italic && a.mono == b.mono;
}

enum Kind : uint8_t { KText, KBox, KNewline, KEllipsis };

// One shaped glyph. x/w: the glyph's cell in the run (cells tile the run, so
// widths add up); dx/dy: HarfBuzz offsets in 1/64 px, drawn at x + dx.
struct G {
    uint32_t cluster;
    float    x, w;
    uint16_t gid;
    int16_t  dx, dy;
};

struct Run {
    uint32_t start, end; // bytes
    uint32_t g0, g1;     // glyphs, visual order
    FontKey  font;
    uint32_t ppem64;
    uint16_t span;
    uint8_t  level, para, kind;
};

struct Unit { // a shaped cluster, logical order: what line breaking works on
    uint32_t   start, end, run;
    float      w;
    uni::Break brk; // before this unit
    bool       space, nl;
};

struct Piece { // a run's part on one line, in visual order
    uint32_t run, g0, g1, start, end;
    float    x, w;
};

struct Line {
    uint32_t p0, p1;      // pieces
    uint32_t start, next; // bytes: first, and start of the next line
    float    x, w;        // alignment offset, visible width (no trailing spaces)
    float    top, height, baseline, asc, desc;
    uint8_t  para;
};

struct Seg { // one grapheme's box on a line (caret, hit testing, selection)
    uint32_t s, e;
    float    x0, x1;
    uint16_t span;
    bool     rtl;
};

struct SpanInfo {
    FontKey prim;
    float   size, asc, desc, lh; // physical
};

// Build scratch, reused across layouts so building allocates only the
// layout's own arrays.
struct Scratch {
    std::vector<uint32_t> cps, offs;
    std::vector<uint16_t> spanOf;
    std::vector<uint8_t>  levels, types, gstart;
    std::vector<uint32_t> scripts;
    std::vector<FontKey>  fontOf;
    std::vector<Unit>     units;
    std::vector<Piece>    linePieces;
    hb_buffer_t          *buf = nullptr;
};
// Outside Scratch: shutdown() may run after Scratch's static destructor (an
// App that is itself a static), and a plain pointer is still valid then.
hb_buffer_t *g_buf = nullptr;
Scratch     &scratch() {
    static Scratch s;
    if (!g_buf)
        g_buf = hb_buffer_create();
    s.buf = g_buf;
    return s;
}

uint32_t prevCp(std::string_view t, uint32_t at) {
    if (!at)
        return 0;
    size_t k = utf8::prevBoundary(t, at);
    return utf8::decode(t, k);
}

uint32_t cpAt(std::string_view t, uint32_t at) {
    if (at >= t.size())
        return 0;
    size_t k = at;
    return utf8::decode(t, k);
}

bool commonScript(uint32_t s) {
    return s == HB_SCRIPT_COMMON || s == HB_SCRIPT_INHERITED || s == HB_SCRIPT_UNKNOWN;
}

class LayoutImpl final : public Layout {
public:
    float width() const override { return _width / _scale; }
    float height() const override { return _height / _scale; }
    int   lineCount() const override { return int(_lines.size()); }
    float baseline(int line) const override {
        if (line < 0 || line >= int(_lines.size()))
            return 0;
        return _lines[line].baseline / _scale;
    }
    bool truncated() const override { return _truncated; }
    void paint(gfx::Painter &p, gfx::PointF origin) const override {
        paintImpl(p, origin, nullptr);
    }
    void paintAs(gfx::Painter &p, gfx::PointF origin, gfx::Color color) const override {
        paintImpl(p, origin, &color);
    }
    void setColor(gfx::Color color) override {
        for (Style &st : _styles)
            st.color = color;
    }
    void       setUnderlinedLink(uint32_t linkId) override { _underlinedLink = linkId; }
    gfx::RectF inkBounds() const override;
    float      inkLean() const override;
    std::vector<gfx::RectF>       selectionRects(uint32_t from, uint32_t to) const override;
    gfx::RectF                    lineRect(int line) const override;
    HitResult                     hitTest(gfx::PointF p) const override;
    gfx::RectF                    caretRect(uint32_t offset) const override;
    uint32_t                      moveCaret(uint32_t offset, int dx, int dy) const override;
    uint32_t                      wordStart(uint32_t offset) const override;
    uint32_t                      wordEnd(uint32_t offset) const override;
    const std::vector<InlineBox> &boxes() const override { return _boxes; }

    // The layout's own copy of the text, when it is not borrowed.
    void adopt(std::string text) {
        countOwned(text.size());
        _own = std::move(text);
    }
    const std::string &own() const { return _own; }
    void               build(
        std::string_view text, const std::vector<Span> &spans, const LayoutOptions &o, float scale
    );

private:
    void paintImpl(gfx::Painter &p, gfx::PointF origin, const gfx::Color *tint) const;
    void shapeRun(Run &r, const char *utf8, int len, int itemOff, int itemLen, uint32_t script);
    void makeLine(uint32_t u0, uint32_t u1, bool ellipsis, bool soft);
    int  lineOf(uint32_t off) const;
    template <class F>
    void     forEachInk(F &&f) const;
    void     segments(int line, std::vector<Seg> &out) const;
    uint32_t nextGrapheme(uint32_t off) const;
    uint32_t prevGrapheme(uint32_t off) const;
    int      boxSpanAt(uint32_t off) const; // span index if off is inside an inline box
    int      spanAt(uint32_t off) const;
    uint32_t spanEnd(int span) const;
    bool     sameRun(uint16_t a, uint16_t b) const;

    std::string                   _own;  // empty when the text is borrowed
    std::string_view              _text; // _own, or the caller's bytes (buildBorrowed)
    std::vector<Style>            _styles;
    std::vector<uint32_t>         _spanStart; // byte start per span
    std::vector<SpanInfo>         _spanInfo;
    std::vector<G>                _glyphs;
    std::vector<Run>              _runs;
    std::vector<Piece>            _pieces;
    std::vector<Line>             _lines;
    std::vector<InlineBox>        _boxes;
    // Grapheme starts, one bit per byte: made by the first prevGrapheme()
    // (caret moves, double-click words), so stepping back is no rescan.
    mutable std::vector<uint64_t> _graphemeStarts;
    float                         _scale = 1, _width = 0, _height = 0, _maxW = 1e9f;
    uint32_t                      _underlinedLink = 0; // setUnderlinedLink()
    bool                          _truncated      = false;

    bool underlined(const Style &st) const {
        return st.underline || (_underlinedLink && st.linkId == _underlinedLink);
    }
};

// Spans that differ only in colour, links or decoration shape as one run,
// so kerning and contextual forms survive style boundaries. Backgrounds and
// inline boxes always get their own runs (padding, atomic boxes).
bool LayoutImpl::sameRun(uint16_t a, uint16_t b) const {
    if (a == b)
        return true;
    const Style &x = _styles[a], &y = _styles[b];
    return sameFont(x, y) && !x.background && !y.background && !x.inlineBoxId && !y.inlineBoxId;
}

void LayoutImpl::shapeRun(
    Run &r, const char *utf8, int len, int itemOff, int itemLen, uint32_t script
) {
    Scratch     &sc   = scratch();
    hb_buffer_t *buf  = sc.buf;
    hb_font_t   *font = fonts::hbFont(r.font, _styles[r.span].size);
    r.g0 = r.g1 = uint32_t(_glyphs.size());
    if (!font)
        return;
    hb_buffer_clear_contents(buf);
    hb_buffer_add_utf8(buf, utf8, len, unsigned(itemOff), itemLen);
    hb_buffer_set_direction(buf, (r.level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    if (commonScript(script))
        hb_buffer_guess_segment_properties(buf);
    else
        hb_buffer_set_script(buf, hb_script_t(script));
    static hb_language_t lang = hb_language_get_default();
    hb_buffer_set_language(buf, lang);
    hb_shape(font, buf, nullptr, 0);

    unsigned             n     = 0;
    hb_glyph_info_t     *info  = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t *pos   = hb_buffer_get_glyph_positions(buf, nullptr);
    const float          ppem  = float(r.ppem64) / 64.f;
    const float          k     = ppem / fonts::upem(r.font);
    const float          extra = fonts::synthBold(r.font) ? ppem / 24.f : 0.f;
    float                pen   = 0;
    for (unsigned i = 0; i < n; ++i) {
        G g;
        g.cluster = info[i].cluster;
        g.gid     = uint16_t(info[i].codepoint);
        g.x       = pen;
        g.w       = float(pos[i].x_advance) * k + (pos[i].x_advance ? extra : 0.f);
        g.dx      = int16_t(std::clamp(std::lround(pos[i].x_offset * k * 64), -32000L, 32000L));
        g.dy      = int16_t(std::clamp(std::lround(-pos[i].y_offset * k * 64), -32000L, 32000L));
        pen += g.w;
        _glyphs.push_back(g);
    }
    r.g1 = uint32_t(_glyphs.size());
}

void LayoutImpl::build(
    std::string_view text, const std::vector<Span> &spans, const LayoutOptions &o, float scale
) {
    Scratch &sc               = scratch();
    _scale                    = scale > 0 ? scale : 1;
    _text                     = text;
    _maxW                     = o.maxWidth * _scale;
    const std::string_view &s = _text;

    // Spans → styles (a missing or partial span list falls back to defaults).
    _styles.clear();
    _spanStart.clear();
    for (auto &sp : spans) {
        _styles.push_back(sp.style);
        _spanStart.push_back(sp.start);
    }
    if (_styles.empty()) {
        _styles.push_back(Style{});
        _spanStart.push_back(0);
    }
    _spanInfo.resize(_styles.size());
    for (size_t i = 0; i < _styles.size(); ++i) {
        const Style &st = _styles[i];
        SpanInfo    &si = _spanInfo[i];
        si.prim         = fonts::primary(st);
        const auto &m   = fonts::metrics(si.prim);
        si.size         = st.size * _scale;
        si.asc          = m.ascent * si.size;
        si.desc         = m.descent * si.size;
        si.lh           = st.size * o.lineHeight * _scale;
    }

    // Code points, span per code point, grapheme starts.
    sc.cps.clear();
    sc.offs.clear();
    sc.spanOf.clear();
    sc.gstart.clear();
    {
        uni::GraphemeScanner gs;
        size_t               span = 0;
        for (size_t i = 0; i < s.size();) {
            const auto off = uint32_t(i);
            while (span + 1 < _spanStart.size() && _spanStart[span + 1] <= off)
                ++span;
            const uint32_t cp = utf8::decode(s, i);
            sc.cps.push_back(cp);
            sc.offs.push_back(off);
            sc.spanOf.push_back(uint16_t(span));
            bool start = gs.next(cp);
            // Style boundaries and inline boxes always start a new cluster.
            if (!sc.spanOf.empty() && sc.spanOf.size() > 1 &&
                sc.spanOf[sc.spanOf.size() - 2] != span &&
                (_styles[span].inlineBoxId || _styles[sc.spanOf[sc.spanOf.size() - 2]].inlineBoxId))
                start = true;
            sc.gstart.push_back(start);
        }
    }
    const size_t n = sc.cps.size();
    sc.offs.push_back(uint32_t(s.size()));

    // Bidi levels per paragraph (paragraphs end at newlines).
    sc.levels.assign(n, 0);
    sc.types.resize(n + 1);
    std::vector<uint8_t> &paraOf = sc.gstart; // reuse bits 1..: paragraph level in bit 1
    for (size_t a = 0; a < n;) {
        size_t b = a;
        while (b < n && !uni::isNewline(sc.cps[b]))
            ++b;
        const uint8_t para =
            uni::resolveBidi(sc.cps.data() + a, b - a, sc.levels.data() + a, sc.types.data());
        for (size_t i = a; i < b; ++i)
            paraOf[i] |= uint8_t(para << 1);
        if (b < n) {
            sc.levels[b] = para;
            paraOf[b] |= uint8_t(para << 1);
            ++b;
            if (b < n && sc.cps[b - 1] == '\r' && sc.cps[b] == '\n') {
                sc.levels[b] = para;
                paraOf[b] |= uint8_t(para << 1);
                ++b;
            }
        }
        a = b;
    }

    // Scripts (Common/Inherited take the surrounding script) and fonts per
    // grapheme cluster.
    sc.scripts.resize(n);
    uint32_t firstReal = HB_SCRIPT_COMMON;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t cp = sc.cps[i];
        const uint32_t sc0 =
            cp < 0x80 ? ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z' ? uint32_t(HB_SCRIPT_LATIN)
                                                                  : uint32_t(HB_SCRIPT_COMMON))
                      : uni::script(cp);
        sc.scripts[i] = sc0;
        if (firstReal == HB_SCRIPT_COMMON && !commonScript(sc0))
            firstReal = sc0;
    }
    uint32_t cur = firstReal;
    for (size_t i = 0; i < n; ++i) {
        if (commonScript(sc.scripts[i]))
            sc.scripts[i] = cur;
        else
            cur = sc.scripts[i];
    }

    sc.fontOf.assign(n, kNoFont);
    const FontKey emojiFont = fonts::emoji();
    FontKey       prev      = kNoFont;
    uint16_t      prevSpan  = 0xFFFF;
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && !(sc.gstart[j] & 1))
            ++j;
        const uint16_t span = sc.spanOf[i];
        const Style   &st   = _styles[span];
        const uint32_t base = sc.cps[i];
        FontKey        f    = kNoFont;
        if (!st.inlineBoxId && !uni::isNewline(base)) {
            bool vs15 = false, vs16 = false, joined = false;
            for (size_t k = i + 1; k < j; ++k) {
                const uint32_t c = sc.cps[k];
                vs15 |= c == 0xFE0E;
                vs16 |= c == 0xFE0F;
                joined |= c == 0x200D || (c >= 0x1F3FB && c <= 0x1F3FF) || c == 0x20E3;
            }
            const bool emo = !vs15 && (vs16 || uni::isEmojiDefault(base) || uni::isRegional(base) ||
                                       (joined && uni::isExtPict(base)));
            const FontKey prim = _spanInfo[span].prim;
            if (emo && emojiFont != kNoFont && fonts::hasGlyph(emojiFont, base)) {
                f = emojiFont;
            } else if (
                prev != kNoFont && prev != prim && prevSpan != 0xFFFF && sameRun(prevSpan, span) &&
                commonScript(uni::script(base)) && !fonts::isColor(prev) &&
                fonts::hasGlyph(prev, base)
            ) {
                f = prev; // spaces/punctuation stay in the surrounding fallback font
            } else if (fonts::hasGlyph(prim, base)) {
                f = prim;
            } else {
                f = fonts::fallback(base, st);
                if (f == kNoFont)
                    f = prim; // tofu from the primary font
            }
        }
        for (size_t k = i; k < j; ++k)
            sc.fontOf[k] = f;
        prev     = f;
        prevSpan = span;
        i        = j;
    }

    // Runs: maximal stretches with the same span, font, level and script.
    _glyphs.clear();
    _runs.clear();
    _glyphs.reserve(n + 8);
    for (size_t i = 0; i < n;) {
        const uint16_t span = sc.spanOf[i];
        const Style   &st   = _styles[span];
        Run            r{};
        r.span   = span;
        r.level  = sc.levels[i];
        r.para   = uint8_t(paraOf[i] >> 1);
        size_t j = i + 1;
        if (st.inlineBoxId) {
            r.kind = KBox;
            while (j < n && sc.spanOf[j] == span)
                ++j;
        } else if (uni::isNewline(sc.cps[i])) {
            r.kind = KNewline;
            if (sc.cps[i] == '\r' && j < n && sc.cps[j] == '\n')
                ++j;
        } else {
            r.kind = KText;
            while (j < n && sameRun(sc.spanOf[j], span) && sc.fontOf[j] == sc.fontOf[i] &&
                   sc.levels[j] == r.level && !uni::isNewline(sc.cps[j]) &&
                   (sc.scripts[j] == sc.scripts[i] || !(sc.gstart[j] & 1)))
                ++j;
        }
        r.start  = sc.offs[i];
        r.end    = sc.offs[j];
        r.font   = sc.fontOf[i];
        r.ppem64 = uint32_t(std::lround(st.size * _scale * 64));
        if (r.kind == KText) {
            shapeRun(r, s.data(), int(s.size()), int(r.start), int(r.end - r.start), sc.scripts[i]);
            // Inline code / mention backgrounds get 2 px of padding at the
            // span's ends, so the rounded box doesn't touch neighbouring text.
            if (st.background && r.g1 > r.g0) {
                const float pad  = 2 * _scale;
                const bool  rtl  = r.level & 1;
                const bool  lead = r.start == _spanStart[span];
                const bool  trail =
                    r.end ==
                    (size_t(span) + 1 < _spanStart.size() ? _spanStart[span + 1] : s.size());
                if (rtl ? trail : lead) { // visual left edge
                    for (uint32_t gI = r.g0; gI < r.g1; ++gI)
                        _glyphs[gI].x += gI == r.g0 ? 0 : pad;
                    _glyphs[r.g0].w += pad;
                    _glyphs[r.g0].dx = int16_t(_glyphs[r.g0].dx + int(pad * 64));
                }
                if (rtl ? lead : trail)
                    _glyphs[r.g1 - 1].w += pad;
            }
        } else {
            r.g0 = r.g1 = uint32_t(_glyphs.size());
        }
        _runs.push_back(r);
        i = j;
    }

    // Units (shaped clusters in logical order) with break opportunities.
    auto &units = sc.units;
    units.clear();
    for (uint32_t ri = 0; ri < _runs.size(); ++ri) {
        const Run &r = _runs[ri];
        if (r.kind != KText || r.g1 == r.g0) {
            const float w = r.kind == KBox ? _styles[r.span].boxWidth * _scale : 0.f;
            units.push_back({r.start, r.end, ri, w, uni::Break::None, false, r.kind == KNewline});
            continue;
        }
        const bool rtl  = r.level & 1;
        // Walk glyph groups (same cluster) in logical order.
        const int  step = rtl ? -1 : 1;
        int        gi   = rtl ? int(r.g1) - 1 : int(r.g0);
        const int  gEnd = rtl ? int(r.g0) - 1 : int(r.g1);
        while (gi != gEnd) {
            const uint32_t cl = _glyphs[gi].cluster;
            float          w  = 0;
            while (gi != gEnd && _glyphs[gi].cluster == cl) {
                w += _glyphs[gi].w;
                gi += step;
            }
            const uint32_t end = gi != gEnd ? _glyphs[gi].cluster : r.end;
            const uint32_t c0  = cpAt(s, cl);
            units.push_back(
                {cl, end, ri, w, uni::Break::None, uni::isBreakSpace(c0) && end - cl <= 3, false}
            );
        }
    }
    for (size_t u = 1; u < units.size(); ++u) {
        const bool     boxA = _runs[units[u - 1].run].kind == KBox;
        const bool     boxB = _runs[units[u].run].kind == KBox;
        const uint32_t a    = boxA ? 0xFFFC : prevCp(s, units[u].start);
        const uint32_t b    = boxB ? 0xFFFC : cpAt(s, units[u].start);
        units[u].brk        = uni::breakBetween(a, b);
        if (units[u].brk == uni::Break::None && (boxA || boxB) && !uni::isBreakSpace(b) &&
            !uni::isNewline(b) && a != 0xA0 && b != 0xA0)
            units[u].brk = uni::Break::Allowed; // inline boxes behave like ideographs
        if (units[u - 1].nl)
            units[u].brk = uni::Break::Mandatory;
    }

    // Greedy line filling.
    _pieces.clear();
    _lines.clear();
    _boxes.clear();
    _truncated       = false;
    const float maxW = _maxW;
    size_t      u0   = 0;
    bool        done = units.empty();
    if (done)
        makeLine(0, 0, false, false);
    while (!done) {
        float  x         = 0;
        size_t lastBreak = 0, end = units.size();
        for (size_t u = u0; u < units.size(); ++u) {
            if (u > u0 && units[u].brk == uni::Break::Mandatory) {
                end = u;
                break;
            }
            if (u > u0 && units[u].brk == uni::Break::Allowed)
                lastBreak = u;
            if (!units[u].space && !units[u].nl && u > u0 && x + units[u].w > maxW + 0.01f) {
                end = lastBreak > u0 ? lastBreak : u; // else: break inside the word
                break;
            }
            x += units[u].w;
        }
        const bool more = end < units.size();
        const bool last = o.maxLines > 0 && int(_lines.size()) + 1 >= o.maxLines;
        if (more && last) {
            _truncated = true;
            // With an ellipsis the last line takes the rest of its paragraph,
            // cut wherever "…" still fits (like CSS text-overflow).
            size_t cut = end;
            if (o.ellipsis)
                while (cut < units.size() && !(cut > u0 && units[cut].brk == uni::Break::Mandatory))
                    ++cut;
            makeLine(uint32_t(u0), uint32_t(cut), o.ellipsis, false);
            break;
        }
        const bool soft = more && units[end].brk != uni::Break::Mandatory;
        makeLine(uint32_t(u0), uint32_t(end), false, soft);
        u0   = end;
        done = !more;
        // A trailing newline opens one more (empty) line, like an editor.
        if (done && !units.empty() && units.back().nl &&
            !(o.maxLines > 0 && int(_lines.size()) >= o.maxLines))
            makeLine(uint32_t(units.size()), uint32_t(units.size()), false, false);
    }

    // Vertical placement and alignment.
    float y = 0, maxLine = 0;
    for (auto &l : _lines) {
        l.top = y;
        l.baseline += y;
        y += l.height;
        maxLine = std::max(maxLine, l.w);
    }
    _width           = maxLine;
    _height          = y;
    const float boxW = o.maxWidth < 1e8f ? maxW : maxLine;
    for (auto &l : _lines) {
        if (o.align == LayoutOptions::Align::Center)
            l.x = std::floor((boxW - l.w) / 2);
        else if (o.align == LayoutOptions::Align::Right)
            l.x = boxW - l.w;
        for (uint32_t p = l.p0; p < l.p1; ++p) {
            const Piece &pc = _pieces[p];
            const Run   &r  = _runs[pc.run];
            if (r.kind != KBox)
                continue;
            const Style &st  = _styles[r.span];
            const float  h   = st.boxHeight * _scale;
            const float  mid = l.baseline - 0.35f * _spanInfo[r.span].size;
            _boxes.push_back(
                {st.inlineBoxId,
                 {(l.x + pc.x) / _scale, (mid - h / 2) / _scale, pc.w / _scale, h / _scale}}
            );
        }
    }
}

// Lays out units [u0, u1) as one line: pieces per run (logical), visual
// reorder, metrics. With `ellipsis`, cuts the line so "…" fits after it.
// `soft`: the line ends at a wrap, so its trailing spaces hang (they don't
// count towards its width or alignment).
void LayoutImpl::makeLine(uint32_t u0, uint32_t u1, bool ellipsis, bool soft) {
    Scratch &sc    = scratch();
    auto    &units = sc.units;
    Line     l{};
    l.start         = u0 < units.size() ? units[u0].start : uint32_t(_text.size());
    l.next          = u1 < units.size() ? units[u1].start : uint32_t(_text.size());
    uint32_t cutEnd = u1;
    while (cutEnd > u0 && units[cutEnd - 1].nl)
        --cutEnd;

    // The ellipsis run is shaped up front so its width is known for the cut.
    int ellRun = -1;
    if (ellipsis) {
        const uint16_t span = cutEnd > u0 ? _runs[units[cutEnd - 1].run].span
                                          : (u0 < units.size() ? _runs[units[u0].run].span : 0);
        Run            r{};
        r.span  = span;
        r.kind  = KEllipsis;
        r.para  = u0 < units.size() ? _runs[units[u0].run].para : 0;
        r.level = r.para;
        r.font  = _spanInfo[span].prim;
        if (!fonts::hasGlyph(r.font, 0x2026)) {
            const FontKey f = fonts::fallback(0x2026, _styles[span]);
            if (f != kNoFont)
                r.font = f;
        }
        r.ppem64                 = uint32_t(std::lround(_styles[span].size * _scale * 64));
        static const char kEll[] = "\xE2\x80\xA6";
        shapeRun(r, kEll, 3, 0, 3, HB_SCRIPT_COMMON);
        float ew = 0;
        for (uint32_t g = r.g0; g < r.g1; ++g)
            ew += _glyphs[g].w;
        float    x   = 0;
        uint32_t fit = u0;
        while (fit < cutEnd && x + units[fit].w + ew <= _maxW + 0.01f)
            x += units[fit++].w;
        while (fit > u0 && units[fit - 1].space)
            --fit;
        cutEnd            = fit;
        const uint32_t at = cutEnd > u0 ? units[cutEnd - 1].end : l.start;
        for (uint32_t g = r.g0; g < r.g1; ++g)
            _glyphs[g].cluster = at;
        r.start = r.end = at;
        _runs.push_back(r);
        ellRun = int(_runs.size() - 1);
    }

    // Pieces in logical order.
    auto &lp = sc.linePieces;
    lp.clear();
    float trailing = 0;
    for (uint32_t u = u0; u < cutEnd;) {
        const uint32_t ri = units[u].run;
        const Run     &r  = _runs[ri];
        Piece          pc{ri, 0, 0, units[u].start, 0, 0, 0};
        float          w = 0;
        while (u < cutEnd && units[u].run == ri) {
            w += units[u].w;
            pc.end = units[u].end;
            ++u;
        }
        pc.w = w;

        // Glyphs of this run whose clusters fall inside [start, end): one
        // stretch, found by bisection — a run's clusters are monotonic in
        // glyph order (ascending, descending when right-to-left).
        const G   *g0 = _glyphs.data() + r.g0, *g1 = _glyphs.data() + r.g1;
        const bool rtl    = r.level & 1;
        const auto before = [&](uint32_t at) { // first glyph past the clusters before `at`
            return std::partition_point(g0, g1, [&](const G &g) {
                return rtl ? g.cluster >= at : g.cluster < at;
            });
        };
        const G *a = before(rtl ? pc.end : pc.start), *b = before(rtl ? pc.start : pc.end);
        pc.g0 = pc.g1 = r.g0;
        if (a < b) {
            pc.g0 = uint32_t(a - _glyphs.data());
            pc.g1 = uint32_t(b - _glyphs.data());
        }
        lp.push_back(pc);
    }
    for (uint32_t u = cutEnd; soft && u-- > u0 && units[u].space;)
        trailing += units[u].w;
    if (ellRun >= 0) {
        const Run &r = _runs[ellRun];
        float      w = 0;
        for (uint32_t g = r.g0; g < r.g1; ++g)
            w += _glyphs[g].w;
        lp.push_back({uint32_t(ellRun), r.g0, r.g1, r.start, r.end, 0, w});
    }

    // L2: reverse runs from the highest level down to the lowest odd level.
    uint8_t hi = 0, loOdd = 0xff;
    for (auto &pc : lp) {
        const uint8_t lv = _runs[pc.run].level;
        hi               = std::max(hi, lv);
        if (lv & 1)
            loOdd = std::min(loOdd, lv);
    }
    for (int lv = hi; lv >= int(loOdd) && loOdd != 0xff; --lv) {
        for (size_t i = 0; i < lp.size();) {
            if (_runs[lp[i].run].level < lv) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < lp.size() && _runs[lp[j].run].level >= lv)
                ++j;
            std::reverse(lp.begin() + long(i), lp.begin() + long(j));
            i = j;
        }
    }
    float x = 0;
    for (auto &pc : lp) {
        pc.x = x;
        x += pc.w;
    }
    l.w  = x - trailing;
    l.p0 = uint32_t(_pieces.size());
    _pieces.insert(_pieces.end(), lp.begin(), lp.end());
    l.p1   = uint32_t(_pieces.size());
    l.para = u0 < units.size() ? _runs[units[u0].run].para : 0;

    // Vertical metrics: CSS-style half-leading per span's primary font, so a
    // fallback or emoji glyph never makes one line taller than its siblings.
    float A = 0, D = 0, tA = 0, tD = 0, bA = 0, bD = 0;
    auto  addSpan = [&](uint16_t span) {
        const SpanInfo &si   = _spanInfo[span];
        const float     half = (si.lh - (si.asc + si.desc)) / 2;
        A                    = std::max(A, si.asc + half);
        D                    = std::max(D, si.desc + half);
        tA                   = std::max(tA, si.asc);
        tD                   = std::max(tD, si.desc);
    };
    for (auto &pc : lp) {
        const Run &r = _runs[pc.run];
        addSpan(r.span);
        if (r.kind == KBox) {
            const float h   = _styles[r.span].boxHeight * _scale;
            const float mid = 0.35f * _spanInfo[r.span].size;
            bA              = std::max(bA, mid + h / 2);
            bD              = std::max(bD, h / 2 - mid);
        }
    }
    if (lp.empty()) { // empty line: the style at its start (or the newline's)
        uint16_t span = 0;
        if (u0 < units.size())
            span = _runs[units[u0].run].span;
        else if (!_runs.empty())
            span = _runs.back().span;
        addSpan(span);
    }
    // Whole physical pixels keep baselines crisp; boxes never poke out.
    A          = std::max(std::round(A), std::ceil(bA - 0.01f));
    D          = std::max(std::round(D), std::ceil(bD - 0.01f));
    l.height   = A + D;
    l.baseline = A; // made absolute once all lines are known
    l.asc      = tA;
    l.desc     = tD;
    _lines.push_back(l);
}

// A glyph whose cell starts at physical x `cellX` on baseline `blY`: the
// cached raster (null when nothing draws) and its bitmap's top-left.
const cache::Glyph *
placeGlyph(const Run &r, const G &g, float cellX, float blY, bool color, int *X, int *Y) {
    const float px    = cellX + float(g.dx) / 64.f;
    const float py    = blY + float(g.dy) / 64.f;
    float       ix    = std::floor(px);
    int         phase = int((px - ix) * 4.f + 0.5f);
    if (phase == 4)
        ix += 1, phase = 0;
    const cache::Glyph *cg = cache::get(r.font, g.gid, r.ppem64, color ? 0 : phase);
    if (!cg || cg->page == 0xFFFF)
        return nullptr;
    *X = int(color ? float(std::lround(px)) : ix) + cg->left;
    *Y = int(std::lround(py)) - cg->top;
    return cg;
}

// Each glyph that leaves ink, as painted from a whole-pixel origin: its mask
// (null for a colour glyph), its physical top-left, and the inked part of the
// mask [l0, r0) × [t0, b0).
template <class F>
void LayoutImpl::forEachInk(F &&f) const {
    for (const Line &l : _lines)
        for (uint32_t i = l.p0; i < l.p1; ++i) {
            const Piece &pc = _pieces[i];
            const Run   &r  = _runs[pc.run];
            if (pc.g1 <= pc.g0)
                continue;
            const bool color = fonts::isColor(r.font);
            for (uint32_t gi = pc.g0; gi < pc.g1; ++gi) {
                const G            &g = _glyphs[gi];
                int                 X, Y;
                const cache::Glyph *cg = placeGlyph(
                    r,
                    g,
                    l.x + pc.x + (g.x - _glyphs[pc.g0].x),
                    std::round(l.baseline),
                    color,
                    &X,
                    &Y
                );
                if (!cg)
                    continue;
                if (cg->color) {
                    f(nullptr, X, Y, 0, 0, int(cg->w), int(cg->h));
                    continue;
                }
                // A mask's box can carry blank or barely touched edge rows
                // and columns: they are not ink.
                const gfx::Mask8 m   = cache::mask(*cg);
                auto             ink = [&](int x, int y) { return m.data[y * m.stride + x] >= 32; };
                auto             col = [&](int x) {
                    for (int y = 0; y < m.height; ++y)
                        if (ink(x, y))
                            return true;
                    return false;
                };
                auto row = [&](int y) {
                    for (int x = 0; x < m.width; ++x)
                        if (ink(x, y))
                            return true;
                    return false;
                };
                int l0 = 0, t0 = 0, r0 = int(cg->w), b0 = int(cg->h);
                while (l0 < r0 && !col(l0))
                    ++l0;
                while (r0 > l0 && !col(r0 - 1))
                    --r0;
                while (t0 < b0 && !row(t0))
                    ++t0;
                while (b0 > t0 && !row(b0 - 1))
                    --b0;
                if (l0 < r0)
                    f(&m, X, Y, l0, t0, r0, b0);
            }
        }
}

gfx::RectF LayoutImpl::inkBounds() const {
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    forEachInk([&](const gfx::Mask8 *, int X, int Y, int l0, int t0, int r0, int b0) {
        x0 = std::min(x0, X + l0), y0 = std::min(y0, Y + t0);
        x1 = std::max(x1, X + r0), y1 = std::max(y1, Y + b0);
    });
    if (x0 >= x1 || y0 >= y1)
        return {};
    const float s = _scale;
    return {float(x0) / s, float(y0) / s, float(x1 - x0) / s, float(y1 - y0) / s};
}

float LayoutImpl::inkLean() const {
    double sum = 0;
    int    n   = 0;
    forEachInk([&](const gfx::Mask8 *m, int, int, int l0, int, int r0, int) {
        ++n;
        if (!m)
            return;
        double mx = 0, w = 0;
        for (int y = 0; y < m->height; ++y)
            for (int x = l0; x < r0; ++x) {
                const double v = m->data[y * m->stride + x];
                mx += v * (x + 0.5), w += v;
            }
        if (w > 0)
            sum += mx / w - (l0 + r0) / 2.0;
    });
    return n ? float(sum / n) / _scale : 0;
}

// tint: paintAs's one colour for every glyph and bar, backgrounds left out.
void LayoutImpl::paintImpl(gfx::Painter &p, gfx::PointF origin, const gfx::Color *tint) const {
    cache::tick();
    const gfx::PointF phys = p.toPhysical(origin);
    const gfx::PointF base = p.toPhysical({0, 0});
    const float       bx = std::round(base.x), by = std::round(base.y);
    const gfx::RectF  clip = p.clipBounds();
    const float       s    = _scale;
    const float       top0 = clip.y - origin.y, bot0 = clip.bottom() - origin.y;
    // Absolute physical → logical in the painter's current coordinates.
    auto              toLx = [&](float X) { return (X - base.x) / s; };
    auto              toLy = [&](float Y) { return (Y - base.y) / s; };
    // Lines stack top down: from the first that reaches into the clip to
    // the last that starts above its bottom.
    const auto first       = std::partition_point(_lines.begin(), _lines.end(), [&](const Line &l) {
        return (l.top + l.height) / s < top0;
    });
    for (auto it = first; it != _lines.end(); ++it) {
        const Line &l = *it;
        if (l.top / s > bot0)
            break;
        const float lineX = phys.x + l.x;
        const float blY   = std::round(phys.y + l.baseline);
        // Backgrounds first (merged across adjacent pieces of one span).
        for (uint32_t i = l.p0; i < l.p1;) {
            const Run   &r  = _runs[_pieces[i].run];
            const Style &st = _styles[r.span];
            if (tint || !(st.background >> 24)) { // none, or a transparent placeholder
                ++i;
                continue;
            }
            const float x0    = _pieces[i].x;
            float       x1    = x0 + _pieces[i].w;
            uint32_t    j     = i + 1;
            // An inline box with a background (an icon inside a chip) joins
            // the neighbouring spans of the same background into one shape.
            auto        joins = [&](uint32_t k) {
                const Run &rk = _runs[_pieces[k].run], &rp = _runs[_pieces[k - 1].run];
                return rk.span == r.span || (_styles[rk.span].background == st.background &&
                                             (rk.kind == KBox || rp.kind == KBox));
            };
            while (j < l.p1 && joins(j))
                x1 = _pieces[j].x + _pieces[j].w, ++j;
            const SpanInfo &si = _spanInfo[r.span];
            const float t = blY - std::round(si.asc * 0.92f), b = blY + std::round(si.desc * 0.92f);
            p.fillRoundRect(
                {toLx(lineX + x0), toLy(t), (x1 - x0) / s, (b - t) / s}, 3, st.background
            );
            i = j;
        }
        // Glyphs, coloured per span (a run may cover several spans that
        // differ only in colour/decoration), then underline/strike bars.
        for (uint32_t i = l.p0; i < l.p1; ++i) {
            const Piece &pc = _pieces[i];
            const Run   &r  = _runs[pc.run];
            if (pc.g1 <= pc.g0)
                continue;
            const float  gx0   = _glyphs[pc.g0].x;
            const bool   color = fonts::isColor(r.font);
            int          span  = -1;
            uint32_t     spanA = 1, spanB = 0; // byte range of `span`
            const Style *st      = nullptr;
            gfx::Color   ink     = 0;
            float        opac    = 1;
            int          barSpan = -1;
            float        barX0 = 0, barX1 = 0;
            auto         flushBar = [&] {
                if (barSpan < 0)
                    return;
                const Style &bs  = _styles[barSpan];
                const auto  &m   = fonts::metrics(_spanInfo[barSpan].prim);
                const float  sz  = _spanInfo[barSpan].size;
                auto         bar = [&](float yOff, float thick) {
                    const float th = std::max(1.f, std::round(thick * sz));
                    const float y  = std::round(blY + yOff);
                    p.fillRect(
                        {toLx(barX0), toLy(y), (barX1 - barX0) / s, th / s}, tint ? *tint : bs.color
                    );
                };
                if (underlined(bs))
                    bar(std::max(1.f, std::round(m.underlinePos * sz)), m.underlineThick);
                if (bs.strike)
                    bar(-std::round(m.strikePos * sz), m.strikeThick);
                barSpan = -1;
            };
            for (uint32_t gi = pc.g0; gi < pc.g1; ++gi) {
                const G &g = _glyphs[gi];
                if (g.cluster < spanA || g.cluster >= spanB || !st) {
                    span  = r.kind == KEllipsis ? r.span : spanAt(g.cluster);
                    spanA = _spanStart[span];
                    spanB = spanEnd(span);
                    st    = &_styles[span];
                    ink   = tint ? *tint : st->color;
                    opac  = float(ink >> 24) / 255.f;
                    if (r.kind == KEllipsis)
                        spanA = 0, spanB = ~0u;
                }
                const float cellX = lineX + pc.x + (g.x - gx0);
                if (underlined(*st) || st->strike) {
                    if (barSpan != span) {
                        flushBar();
                        barSpan = span;
                        barX0   = cellX;
                    }
                    barX0 = std::min(barX0, cellX);
                    barX1 = std::max(barX1, cellX + g.w);
                } else {
                    flushBar();
                }
                int                 X, Y;
                const cache::Glyph *cg = placeGlyph(r, g, cellX, blY, color, &X, &Y);
                if (!cg)
                    continue;
                X -= int(bx);
                Y -= int(by);
                if (cg->color)
                    p.blitColor(cache::colorView(*cg), X, Y, opac);
                else
                    p.blitMask(cache::mask(*cg), X, Y, ink);
            }
            flushBar();
        }
    }
}

int LayoutImpl::spanAt(uint32_t off) const {
    const auto it = std::upper_bound(_spanStart.begin(), _spanStart.end(), off);
    return it == _spanStart.begin() ? 0 : int(it - _spanStart.begin()) - 1;
}

uint32_t LayoutImpl::spanEnd(int span) const {
    return span + 1 < int(_spanStart.size()) ? _spanStart[span + 1] : uint32_t(_text.size());
}

int LayoutImpl::lineOf(uint32_t off) const {
    if (_lines.size() < 2)
        return 0;
    const auto it =
        std::upper_bound(_lines.begin() + 1, _lines.end(), off, [](uint32_t o, const Line &l) {
            return o < l.start;
        });
    return int(it - _lines.begin()) - 1;
}

// segments() output for hitTest/caretRect/selectionRects: they run per
// mouse move, so the vector is reused (per thread: no locking).
std::vector<Seg> &segScratch() {
    static thread_local std::vector<Seg> segs;
    return segs;
}

void LayoutImpl::segments(int li, std::vector<Seg> &out) const {
    out.clear();
    const Line &l = _lines[li];
    for (uint32_t i = l.p0; i < l.p1; ++i) {
        const Piece &pc   = _pieces[i];
        const Run   &r    = _runs[pc.run];
        const bool   rtl  = r.level & 1;
        const float  base = l.x + pc.x;
        if (r.kind == KBox || r.kind == KEllipsis) {
            out.push_back(
                {pc.start, r.kind == KBox ? pc.end : pc.start, base, base + pc.w, r.span, rtl}
            );
            continue;
        }
        if (pc.g1 <= pc.g0)
            continue;
        const float gx0 = _glyphs[pc.g0].x;
        for (uint32_t g = pc.g0; g < pc.g1;) {
            const uint32_t cl = _glyphs[g].cluster;
            const float    x0 = base + _glyphs[g].x - gx0;
            float          x1 = x0;
            uint32_t       h  = g;
            while (h < pc.g1 && _glyphs[h].cluster == cl) {
                x1 = base + _glyphs[h].x - gx0 + _glyphs[h].w;
                ++h;
            }
            // Cluster end: the next cluster in logical order.
            uint32_t ce = pc.end;
            if (!rtl && h < pc.g1)
                ce = _glyphs[h].cluster;
            else if (rtl && g > pc.g0)
                ce = _glyphs[g - 1].cluster;
            // Graphemes inside a ligature cluster share its width evenly.
            uint32_t bounds[32];
            int      nb  = 0;
            bounds[nb++] = cl;
            {
                uni::GraphemeScanner gs;
                for (size_t k = cl; k < ce && nb < 31;) {
                    const auto     at = uint32_t(k);
                    const uint32_t cp = utf8::decode(std::string_view(_text).substr(0, ce), k);
                    if (gs.next(cp) && at != cl)
                        bounds[nb++] = at;
                }
            }
            bounds[nb]     = ce;
            const float gw = (x1 - x0) / float(nb);
            // Left to right, like everything else here: a right-to-left
            // cluster's last grapheme is its leftmost.
            for (int j = 0; j < nb; ++j) {
                const int   k = rtl ? nb - 1 - j : j;
                const float a = rtl ? x1 - gw * float(k + 1) : x0 + gw * float(k);
                out.push_back({bounds[k], bounds[k + 1], a, a + gw, uint16_t(spanAt(cl)), rtl});
            }
            g = h;
        }
    }
}

std::vector<gfx::RectF> LayoutImpl::selectionRects(uint32_t from, uint32_t to) const {
    std::vector<gfx::RectF> out;
    if (from > to)
        std::swap(from, to);
    if (from == to)
        return out;
    std::vector<Seg> &segs = segScratch();
    if (_lines.empty())
        return out;
    // From the first line that reaches past `from` (else the last).
    const auto first = std::partition_point(_lines.begin(), _lines.end() - 1, [&](const Line &l) {
        return l.next <= from;
    });
    for (int li = int(first - _lines.begin()); li < int(_lines.size()); ++li) {
        const Line &l = _lines[li];
        if (l.start >= to)
            break;
        segments(li, segs); // already in visual order, left to right
        float x0 = 0, x1 = -1;
        auto  flush = [&] {
            if (x1 > x0)
                out.push_back({x0 / _scale, l.top / _scale, (x1 - x0) / _scale, l.height / _scale});
            x1 = -1;
        };
        for (auto &sg : segs) {
            const bool sel = sg.s >= from && sg.s < to && sg.e > sg.s;
            if (!sel) {
                flush();
                continue;
            }
            if (x1 >= 0 && std::fabs(sg.x0 - x1) < 0.5f)
                x1 = sg.x1;
            else {
                flush();
                x0 = sg.x0, x1 = sg.x1;
            }
        }
        flush();
        // A selected line break shows as a space-wide stub at the line end.
        if (li + 1 < int(_lines.size()) && l.next > from && l.next <= to) {
            float end = l.x;
            for (auto &sg : segs)
                end = std::max(end, sg.x1);
            const bool brokenAtNewline = l.next > 0 && uni::isNewline(uint8_t(_text[l.next - 1]));
            if (brokenAtNewline)
                out.push_back(
                    {end / _scale,
                     l.top / _scale,
                     0.3f * _spanInfo[0].size / _scale,
                     l.height / _scale}
                );
        }
    }
    return out;
}

gfx::RectF LayoutImpl::lineRect(int line) const {
    if (line < 0 || line >= int(_lines.size()))
        return {};
    const Line &l = _lines[line];
    return {l.x / _scale, l.top / _scale, l.w / _scale, l.height / _scale};
}

HitResult LayoutImpl::hitTest(gfx::PointF pt) const {
    HitResult r;
    if (_lines.empty())
        return r;
    const float px = pt.x * _scale, py = pt.y * _scale;
    // The first line whose bottom is below py, else the last.
    const auto  below = std::partition_point(_lines.begin(), _lines.end() - 1, [&](const Line &l) {
        return py >= l.top + l.height;
    });
    const int   li    = int(below - _lines.begin());
    const Line &l     = _lines[li];
    std::vector<Seg> &segs = segScratch();
    segments(li, segs);
    if (segs.empty()) {
        r.offset = l.start;
        return r;
    }
    const bool inLine = py >= l.top && py < l.top + l.height;
    float      best   = 1e30f;
    for (auto &sg : segs) {
        if (px >= sg.x0 && px < sg.x1) {
            const bool left = px < (sg.x0 + sg.x1) / 2;
            r.offset        = (left != sg.rtl) ? sg.s : sg.e;
            if (inLine) {
                r.inside = true;
                r.linkId = _styles[sg.span].linkId;
            }
            // Inline boxes and ellipsis are atomic: hit their start/end only.
            return r;
        }
        const float d0 = std::fabs(px - sg.x0), d1 = std::fabs(px - sg.x1);
        if (d0 < best)
            best = d0, r.offset = sg.rtl ? sg.e : sg.s;
        if (d1 < best)
            best = d1, r.offset = sg.rtl ? sg.s : sg.e;
    }
    // Never land past the line's own end (on its newline or the next line).
    if (li + 1 < int(_lines.size()) && r.offset >= _lines[li + 1].start) {
        uint32_t end = _lines[li + 1].start;
        while (end > l.start && uni::isNewline(uint8_t(_text[end - 1])))
            --end;
        if (r.offset > end || (end == _lines[li + 1].start && r.offset == end))
            r.offset = end == _lines[li + 1].start ? prevGrapheme(end) : end;
        if (r.offset < l.start)
            r.offset = l.start;
    }
    return r;
}

gfx::RectF LayoutImpl::caretRect(uint32_t off) const {
    if (_lines.empty())
        return {};
    off            = std::min<uint32_t>(off, uint32_t(_text.size()));
    const int   li = lineOf(off);
    const Line &l  = _lines[li];
    float       x  = l.x;
    if (l.para & 1)
        x = l.x + l.w;
    std::vector<Seg> &segs = segScratch();
    segments(li, segs);
    const Seg *lastLogical = nullptr;
    bool       found       = false;
    for (auto &sg : segs) {
        if (sg.s == off && sg.e > sg.s) {
            x     = sg.rtl ? sg.x1 : sg.x0;
            found = true;
            break;
        }
        if (!lastLogical || sg.s > lastLogical->s)
            lastLogical = &sg;
    }
    if (!found && lastLogical && off >= lastLogical->s)
        x = lastLogical->rtl ? lastLogical->x0 : lastLogical->x1;
    const float top = l.baseline - l.asc, h = l.asc + l.desc;
    return {std::round(x) / _scale, std::round(top) / _scale, 1.f, std::round(h) / _scale};
}

int LayoutImpl::boxSpanAt(uint32_t off) const {
    const int i = spanAt(off);
    return _styles[i].inlineBoxId && off > _spanStart[i] && off < spanEnd(i) ? i : -1;
}

uint32_t LayoutImpl::nextGrapheme(uint32_t off) const {
    if (off >= _text.size())
        return uint32_t(_text.size());
    uni::GraphemeScanner gs;
    size_t               k = off;
    gs.next(utf8::decode(_text, k));
    while (k < _text.size()) {
        size_t         m  = k;
        const uint32_t cp = utf8::decode(_text, m);
        if (gs.next(cp))
            break;
        k = m;
    }
    uint32_t r = uint32_t(k);
    if (int b = boxSpanAt(r); b >= 0)
        r = b + 1 < int(_spanStart.size()) ? _spanStart[b + 1] : uint32_t(_text.size());
    return r;
}

uint32_t LayoutImpl::prevGrapheme(uint32_t off) const {
    if (off == 0)
        return 0;
    if (_graphemeStarts.empty()) { // nextGrapheme's steps from the start, once
        _graphemeStarts.assign(_text.size() / 64 + 1, 0);
        for (uint32_t p = 0; p < _text.size(); p = nextGrapheme(p))
            _graphemeStarts[p >> 6] |= uint64_t(1) << (p & 63);
    }
    off           = std::min<uint32_t>(off, uint32_t(_text.size()));
    uint32_t last = off - 1;
    while (last > 0 && !(_graphemeStarts[last >> 6] >> (last & 63) & 1))
        --last;
    if (int b = boxSpanAt(last); b >= 0)
        last = _spanStart[b];
    return last;
}

uint32_t LayoutImpl::moveCaret(uint32_t off, int dx, int dy) const {
    off = std::min<uint32_t>(off, uint32_t(_text.size()));
    for (; dx > 0; --dx)
        off = nextGrapheme(off);
    for (; dx < 0; ++dx)
        off = prevGrapheme(off);
    if (dy && !_lines.empty()) {
        const gfx::RectF c  = caretRect(off);
        const int        li = lineOf(off) + dy;
        if (li < 0)
            return 0;
        if (li >= int(_lines.size()))
            return uint32_t(_text.size());
        const Line &l = _lines[li];
        off           = hitTest({c.x, (l.top + l.height / 2) / _scale}).offset;
    }
    return off;
}

// Word classes for double-click: word characters, spaces, and everything
// else one grapheme at a time.
int wordClass(std::string_view t, uint32_t off) {
    const uint32_t cp = cpAt(t, off);
    if (uni::isSelectWordChar(cp))
        return 1;
    if (uni::isBreakSpace(cp))
        return 2;
    return 0;
}

uint32_t LayoutImpl::wordStart(uint32_t off) const {
    off = std::min<uint32_t>(off, uint32_t(_text.size()));
    if (off == _text.size() && off > 0)
        off = prevGrapheme(off);
    const int cls = wordClass(_text, off);
    if (cls == 0)
        return off;
    while (off > 0) {
        const uint32_t p = prevGrapheme(off);
        if (wordClass(_text, p) != cls)
            break;
        off = p;
    }
    return off;
}

uint32_t LayoutImpl::wordEnd(uint32_t off) const {
    off = std::min<uint32_t>(off, uint32_t(_text.size()));
    if (off >= _text.size())
        return off;
    const int cls = wordClass(_text, off);
    off           = nextGrapheme(off);
    if (cls == 0)
        return off;
    while (off < _text.size() && wordClass(_text, off) == cls)
        off = nextGrapheme(off);
    return off;
}

} // namespace

void AttributedText::append(std::string_view utf8, const Style &s) {
    if (utf8.empty())
        return;
    const auto start = uint32_t(text.size());
    text.append(utf8);
    if (!spans.empty() && spans.back().end == start && !s.inlineBoxId && spans.back().style == s)
        spans.back().end = uint32_t(text.size());
    else
        spans.push_back({start, uint32_t(text.size()), s});
}

bool init(std::string *error) {
    return fonts::init(error);
}

void shutdown() {
    hb_buffer_destroy(g_buf); // the vectors go with Scratch's static destructor
    g_buf = nullptr;
    fonts::shutdown();
}

std::unique_ptr<Layout>
Layout::build(const AttributedText &t, const LayoutOptions &o, float scale) {
    countBuild();
    auto l = std::make_unique<LayoutImpl>();
    l->adopt(t.text);
    l->build(l->own(), t.spans, o, scale);
    return l;
}

std::unique_ptr<Layout> Layout::build(AttributedText &&t, const LayoutOptions &o, float scale) {
    countBuild();
    auto l = std::make_unique<LayoutImpl>();
    l->adopt(std::move(t.text));
    l->build(l->own(), t.spans, o, scale);
    return l;
}

std::unique_ptr<Layout> Layout::buildBorrowed(
    std::string_view utf8, const std::vector<Span> &spans, const LayoutOptions &o, float scale
) {
    countBuild();
    auto l = std::make_unique<LayoutImpl>();
    l->build(utf8, spans, o, scale);
    return l;
}

std::unique_ptr<Layout>
layoutPlain(std::string_view utf8, const Style &s, float scale, float maxWidth) {
    AttributedText t;
    t.append(utf8, s);
    LayoutOptions o;
    o.maxWidth = maxWidth;
    return Layout::build(std::move(t), o, scale);
}

size_t layoutBuilds() {
#ifdef MSGA_TEXT_COUNTERS
    return gBuilds.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

size_t layoutTextOwned() {
#ifdef MSGA_TEXT_COUNTERS
    return gOwned.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

// The layout lives only for this call: it borrows the text. Callers keep
// the width (they ask once per scale or text).
float measure(std::string_view utf8, const Style &s, float scale) {
    std::vector<Span> spans;
    if (!utf8.empty())
        spans.push_back({0, uint32_t(utf8.size()), s});
    return Layout::buildBorrowed(utf8, spans, {}, scale)->width();
}

Metrics metrics(const Style &s, float scale) {
    const auto &m = fonts::metrics(fonts::primary(s));
    return {m.ascent * s.size, m.descent * s.size, m.lineGap * s.size, m.capHeight * s.size};
}

} // namespace text

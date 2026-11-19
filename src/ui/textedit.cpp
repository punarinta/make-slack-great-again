#include "ui/textedit.h"
#include "ui/widgets.h"

#include "base/i18n.h"
#include "base/utf8.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

constexpr size_t   kUndoLimit  = 200;
constexpr double   kCoalesceMs = 1500;
constexpr float    kWheelStep  = 50;
constexpr uint16_t kLinkMask   = 0xff00;

inline bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

// Menu ids for the built-in context menu.
enum : int { kCut = 1, kCopy, kPaste, kSelectAll };

} // namespace

TextEdit::TextEdit() : _alive(std::make_shared<char>(0)) {
    setRole(Role::TextInput);
    setFocusable(true);
    setCursor(plat::Cursor::IBeam);
    style().padding(8, 6);
}

TextEdit::~TextEdit() {
    if (_blinkTimer)
        app()->cancelTimer(_blinkTimer);
}

// ── Offsets ─────────────────────────────────────────────────────────────────

uint32_t TextEdit::prevChar(uint32_t o) const {
    return uint32_t(utf8::prevBoundary(_text, o));
}

uint32_t TextEdit::nextChar(uint32_t o) const {
    return uint32_t(utf8::nextBoundary(_text, o));
}

// Masked text shows one 3-byte bullet per byte (keys are ASCII), so offsets
// scale by 3 and there is never a preedit.
constexpr uint32_t kBulletBytes = 3;

uint32_t TextEdit::toDisplay(uint32_t m) const {
    if (_masked)
        return m * kBulletBytes;
    return !_preedit.empty() && m >= _preeditPos ? m + uint32_t(_preedit.size()) : m;
}

uint32_t TextEdit::toModel(uint32_t d) const {
    if (_masked)
        return std::min(d / kBulletBytes, uint32_t(_text.size()));
    if (_preedit.empty() || d < _preeditPos)
        return std::min(d, uint32_t(_text.size()));
    if (d < _preeditPos + _preedit.size())
        return _preeditPos;
    return std::min(d - uint32_t(_preedit.size()), uint32_t(_text.size()));
}

// ── Paragraphs ──────────────────────────────────────────────────────────────

// Paragraphs of [from, end): one per '\n' ("\r\n" breaks as one), and with
// `last` the one after the final break, up to the end of the text.
void TextEdit::scanParas(uint32_t from, uint32_t end, bool last, std::vector<Para> &out) const {
    for (;;) {
        const size_t k = _text.find('\n', from);
        if (k == std::string::npos || k >= end) {
            if (last)
                out.push_back({from, end - from, 0});
            return;
        }
        Para p;
        p.start = from;
        p.len   = uint32_t(k) - from;
        p.brk   = 1;
        if (k > from && _text[k - 1] == '\r') {
            --p.len;
            p.brk = 2;
        }
        out.push_back(std::move(p));
        from = uint32_t(k) + 1;
        if (from >= end && !last)
            return;
    }
}

void TextEdit::splitParas() {
    _paras.clear();
    if (_masked) // one line of bullets, breaks included
        _paras.push_back({0, uint32_t(_text.size()), 0});
    else
        scanParas(0, uint32_t(_text.size()), true, _paras);
    _parasDirty = true;
}

// _text[pos, pos + removed) became `inserted` bytes: re-split the paragraphs
// that held the old range (their layouts go) and shift the ones after it.
void TextEdit::editParas(uint32_t pos, size_t removed, size_t inserted) {
    if (_masked || _paras.empty())
        return splitParas();
    auto holder = [&](uint32_t off) { // the last paragraph starting at or before off
        size_t lo = 0, hi = _paras.size();
        while (hi - lo > 1) {
            const size_t mid                     = (lo + hi) / 2;
            (_paras[mid].start <= off ? lo : hi) = mid;
        }
        return lo;
    };
    const size_t   i0    = holder(pos);
    const size_t   i1    = holder(pos + uint32_t(removed));
    const bool     toEnd = i1 + 1 == _paras.size();
    const int64_t  delta = int64_t(inserted) - int64_t(removed);
    const uint32_t end =
        toEnd ? uint32_t(_text.size()) : uint32_t(int64_t(_paras[i1 + 1].start) + delta);
    std::vector<Para> fresh;
    scanParas(_paras[i0].start, end, toEnd, fresh);
    for (size_t i = i1 + 1; i < _paras.size(); ++i)
        _paras[i].start = uint32_t(int64_t(_paras[i].start) + delta);
    _paras.erase(_paras.begin() + long(i0), _paras.begin() + long(i1) + 1);
    _paras.insert(
        _paras.begin() + long(i0),
        std::make_move_iterator(fresh.begin()),
        std::make_move_iterator(fresh.end())
    );
    if (!_preedit.empty())
        dropParaAt(_preeditPos);
    _parasDirty = true;
}

// The paragraph holding model offset `modelPos` is to be laid out again.
void TextEdit::dropParaAt(uint32_t modelPos) {
    if (_paras.empty())
        return;
    size_t lo = 0, hi = _paras.size();
    while (hi - lo > 1) {
        const size_t mid                          = (lo + hi) / 2;
        (_paras[mid].start <= modelPos ? lo : hi) = mid;
    }
    _paras[lo].layout.reset();
    _parasDirty = true;
}

void TextEdit::dropLayouts() {
    for (Para &p : _paras)
        p.layout.reset();
    _parasDirty = true;
}

bool TextEdit::holdsPreedit(const Para &p) const {
    return !_preedit.empty() && _preeditPos >= p.start && _preeditPos <= p.start + p.len;
}

uint32_t TextEdit::paraStart(size_t i) const {
    if (_masked)
        return 0;
    const uint32_t s = _paras[i].start;
    return !_preedit.empty() && s > _preeditPos ? s + uint32_t(_preedit.size()) : s;
}

uint32_t TextEdit::paraLen(size_t i) const {
    const Para &p = _paras[i];
    if (_masked)
        return p.len * kBulletBytes;
    return p.len + (holdsPreedit(p) ? uint32_t(_preedit.size()) : 0);
}

size_t TextEdit::paraAt(uint32_t d) const {
    size_t lo = 0, hi = _paras.size();
    while (hi - lo > 1) {
        const size_t mid                = (lo + hi) / 2;
        (paraStart(mid) <= d ? lo : hi) = mid;
    }
    return lo;
}

size_t TextEdit::paraAtY(float y) const {
    size_t lo = 0, hi = _paras.size();
    while (hi - lo > 1) {
        const size_t mid                 = (lo + hi) / 2;
        (_paras[mid].top <= y ? lo : hi) = mid;
    }
    return lo;
}

// ── Layout ──────────────────────────────────────────────────────────────────

float TextEdit::contentWidth() const {
    const Style &s = currentStyle();
    return std::max(1.f, width() - s.pad.l - s.pad.r);
}

text::Style TextEdit::baseStyle(C c) const {
    return font(_font, c);
}

float TextEdit::lineHeight() const {
    return std::ceil(baseStyle(C::Text).size * 1.4f);
}

std::unique_ptr<text::Layout> TextEdit::buildPara(const Para &p, float w) const {
    const text::Style    base = baseStyle(C::Text);
    text::AttributedText t;
    auto                 styleFor = [&](uint16_t f) {
        text::Style st = base;
        if (f & Bold)
            st.weight = text::Weight::Bold;
        if (f & Italic)
            st.italic = true;
        if (f & Strike)
            st.strike = true;
        if (f & Code) {
            st.mono       = true;
            st.size       = std::max(10.f, base.size - 2);
            st.color      = color(C::CodeText);
            st.background = color(C::CodeBg);
        }
        if (f & kLinkMask) {
            st.color       = color(C::Link);
            const size_t i = f >> 8;
            if (i && i <= _links.size() && pillLink(_links[i - 1])) {
                if (_linkBg != C::None) // pills: mentions in the composer
                    st.background = color(_linkBg);
            } else {
                st.underline = true; // a URL reads as one: blue and underlined
            }
        }
        return st;
    };
    // Runs of equal format; the preedit goes in underlined at its position.
    auto appendRange = [&](uint32_t a, uint32_t b) {
        uint32_t i = a;
        while (i < b) {
            uint32_t j = i + 1;
            while (j < b && _fmt[j] == _fmt[i])
                ++j;
            t.append(std::string_view(_text).substr(i, j - i), styleFor(_fmt[i]));
            i = j;
        }
    };
    const uint32_t a = p.start, b = p.start + p.len;
    if (_masked) {
        std::string dots;
        for (size_t i = 0; i < _text.size(); ++i)
            dots += "\xE2\x80\xA2";
        t.append(dots, base);
    } else if (!holdsPreedit(p)) {
        appendRange(a, b);
    } else {
        appendRange(a, _preeditPos);
        text::Style pe = _preeditPos > 0 ? styleFor(_fmt[_preeditPos - 1]) : base;
        pe.underline   = true;
        t.append(_preedit, pe);
        appendRange(_preeditPos, b);
    }
    // An empty line is as tall as its line break's style (for the last
    // line, the break before it), as in one layout of the whole text.
    if (t.text.empty() && !_masked) {
        uint32_t k = UINT32_MAX;
        if (p.brk)
            k = b;
        else if (a > 0)
            k = a >= 2 && _text[a - 2] == '\r' ? a - 2 : a - 1;
        if (k < _fmt.size())
            t.spans.push_back({0, 0, styleFor(_fmt[k])});
    }
    text::LayoutOptions o;
    o.maxWidth = w;
    return text::Layout::build(std::move(t), o, windowScale());
}

void TextEdit::layoutFor(float w) {
    if (_paras.empty())
        splitParas();
    if (w != _layoutW) {
        dropLayouts();
        _layoutW = w;
    }
    if (!_parasDirty)
        return;
    _wavesValid       = false; // squiggles sit on the lines laid out here
    // Tops add up in whole physical pixels (every line box is one), as one
    // layout of the whole text stacks its lines.
    const float scale = windowScale() > 0 ? windowScale() : 1;
    float       y     = 0;
    for (Para &p : _paras) {
        if (!p.layout)
            p.layout = buildPara(p, w);
        p.top = y / scale;
        y += std::round(p.layout->height() * scale);
    }
    _parasDirty = false;
}

void TextEdit::currentLayout() {
    layoutFor(contentWidth());
}

float TextEdit::docHeight() const {
    const Para &p = _paras.back();
    return p.top + p.layout->height();
}

RectF TextEdit::docCaretRect(uint32_t d) const {
    const size_t i = paraAt(d);
    const Para  &p = _paras[i];
    RectF        r = p.layout->caretRect(std::min(d - std::min(d, paraStart(i)), paraLen(i)));
    r.y += p.top;
    return r;
}

uint32_t TextEdit::docHitTest(PointF pt) const {
    const size_t i = paraAtY(pt.y);
    const Para  &p = _paras[i];
    return paraStart(i) + p.layout->hitTest({pt.x, pt.y - p.top}).offset;
}

uint32_t TextEdit::docMoveCaret(uint32_t d, int dx, int dy) const {
    const size_t   n   = _paras.size();
    const uint32_t end = paraStart(n - 1) + paraLen(n - 1);
    d                  = std::min(d, end);
    // Over a paragraph's edge the next grapheme is the next paragraph's start
    // (a line break is one grapheme), the previous one the previous one's end.
    for (; dx > 0; --dx) {
        const size_t   i = paraAt(d);
        const uint32_t s = paraStart(i), len = paraLen(i), l = std::min(d - s, len);
        if (l >= len)
            d = i + 1 < n ? paraStart(i + 1) : s + len;
        else
            d = s + _paras[i].layout->moveCaret(l, 1, 0);
    }
    for (; dx < 0; ++dx) {
        const size_t   i = paraAt(d);
        const uint32_t s = paraStart(i), l = std::min(d - s, paraLen(i));
        if (l == 0)
            d = i > 0 ? paraStart(i - 1) + paraLen(i - 1) : 0;
        else
            d = s + _paras[i].layout->moveCaret(l, -1, 0);
    }
    // Lines: within the paragraph, else to the previous one's last line (the
    // next one's first) at the caret's x; past either end, to that end.
    for (const int step = dy < 0 ? -1 : 1; dy; dy -= step) {
        const size_t        i = paraAt(d);
        const uint32_t      s = paraStart(i), len = paraLen(i), l = std::min(d - s, len);
        const text::Layout *pl = _paras[i].layout.get();
        const RectF         c  = pl->caretRect(l);
        if (c.y != pl->caretRect(step < 0 ? 0 : len).y) // not on its first / last line
            d = s + pl->moveCaret(l, 0, step);
        else if (step < 0)
            d = i == 0 ? 0 : paraStart(i - 1) + _paras[i - 1].layout->hitTest({c.x, 1e6f}).offset;
        else
            d = i + 1 == n ? end
                           : paraStart(i + 1) + _paras[i + 1].layout->hitTest({c.x, -1e6f}).offset;
    }
    return d;
}

// A line break is a word of its own: at a paragraph's end, before the break,
// the word starts there and ends past it.
uint32_t TextEdit::docWordStart(uint32_t d) const {
    const size_t   i = paraAt(d);
    const uint32_t s = paraStart(i);
    if (d - s >= paraLen(i) && i + 1 < _paras.size())
        return d;
    return s + _paras[i].layout->wordStart(d - s);
}

uint32_t TextEdit::docWordEnd(uint32_t d) const {
    const size_t   i = paraAt(d);
    const uint32_t s = paraStart(i), len = paraLen(i);
    if (d - s >= len)
        return i + 1 < _paras.size() ? paraStart(i + 1) : s + len;
    return s + _paras[i].layout->wordEnd(d - s);
}

std::vector<RectF>
TextEdit::docSelectionRects(uint32_t from, uint32_t to, float top, float bottom) const {
    std::vector<RectF> out;
    if (from > to)
        std::swap(from, to);
    if (from == to)
        return out;
    // The selected paragraphs that reach between top and bottom.
    const size_t last = std::min(paraAt(to), paraAtY(bottom));
    for (size_t i = std::max(paraAt(from), paraAtY(top)); i <= last; ++i) {
        const Para    &p = _paras[i];
        const uint32_t s = paraStart(i), len = paraLen(i);
        const uint32_t a = std::min(from - std::min(from, s), len);
        const uint32_t b = std::min(to - std::min(to, s), len);
        if (a < b)
            for (RectF r : p.layout->selectionRects(a, b)) {
                r.y += p.top;
                out.push_back(r);
            }
        // A selected line break shows as a stub at its line's end, 0.3 of
        // the size the text starts in wide.
        if (i + 1 < _paras.size() && from < paraStart(i + 1) && paraStart(i + 1) <= to) {
            const RectF lr = p.layout->lineRect(p.layout->lineCount() - 1);
            float       sz = baseStyle(C::Text).size;
            if ((_preedit.empty() || _preeditPos > 0) && (_fmt[0] & Code))
                sz = std::max(10.f, sz - 2);
            out.push_back({lr.x + lr.w, p.top + lr.y, 0.3f * sz, lr.h});
        }
    }
    return out;
}

float TextEdit::clampedHeight() const {
    const float lh = lineHeight();
    float       h  = std::max(docHeight(), lh);
    h              = std::max(h, lh * _minLines);
    if (_maxLines > 0)
        h = std::min(h, lh * _maxLines);
    return std::ceil(h);
}

SizeF TextEdit::measureContent(float aw, float ah) {
    const float w = aw < kInf / 2 ? aw : 400;
    layoutFor(std::max(1.f, w));
    return {w, clampedHeight()};
}

void TextEdit::layout() {
    ensureCaretVisible(); // the layout itself is cached per content width
    updateIme();
}

void TextEdit::styleChanged() {
    dropLayouts();
    _placeholderLayout.reset();
    _layoutW = -1;
    update();
}

void TextEdit::windowChanged() {
    if (!window())
        stopBlink();
}

bool TextEdit::caretOnEdgeLine(bool top) const {
    // Edge: Up / Down lands on the same line (the layout may still move the
    // caret along it, to the line's end).
    const_cast<TextEdit *>(this)->currentLayout();
    const uint32_t d  = toDisplay(_caret);
    const uint32_t to = docMoveCaret(d, 0, top ? -1 : 1);
    return to == d || docCaretRect(to).y == docCaretRect(d).y;
}

RectF TextEdit::caretRect() const {
    const_cast<TextEdit *>(this)->currentLayout();
    uint32_t d = toDisplay(_caret);
    if (!_preedit.empty())
        d = _preeditPos + uint32_t(
                              std::clamp(
                                  _preeditCursor < 0 ? int(_preedit.size()) : _preeditCursor,
                                  0,
                                  int(_preedit.size())
                              )
                          );
    RectF        r = docCaretRect(d);
    const Style &s = currentStyle();
    r.x += s.pad.l;
    r.y += s.pad.t - _scrollY;
    r.w = std::max(r.w, 1.f);
    if (r.h <= 0)
        r.h = lineHeight();
    return r;
}

void TextEdit::ensureCaretVisible() {
    if (width() <= 0)
        return;
    currentLayout();
    const Style &s    = currentStyle();
    const float  view = std::max(1.f, height() - s.pad.t - s.pad.b);
    RectF        r    = docCaretRect(toDisplay(_caret));
    const float  old  = _scrollY;
    if (r.y < _scrollY)
        _scrollY = r.y;
    if (r.y + r.h > _scrollY + view)
        _scrollY = r.y + r.h - view;
    _scrollY = std::clamp(_scrollY, 0.f, std::max(0.f, docHeight() - view));
    if (_scrollY != old)
        update();
}

void TextEdit::updateIme() {
    if (!window() || !focused())
        return;
    const RectF  r = caretRect();
    const PointF o = mapToWindow({r.x, r.y});
    window()->setTextInput(true, {o.x, o.y, r.w, r.h});
}

// ── Caret blink ─────────────────────────────────────────────────────────────

// The caret blinks for kBlinkForMs after the last edit, move or focus, then
// stays on: an idle field wakes nothing.
constexpr int kBlinkForMs = 10000;

void TextEdit::startBlink() {
    stopBlink();
    _caretOn     = true;
    _blinkFlips  = 0;
    const int ms = app()->settings().caretBlinkMs;
    if (ms > 0 && focused())
        _blinkTimer = app()->addTimer(ms, true, [this, ms] {
            const bool done = ++_blinkFlips >= std::max(1, kBlinkForMs / ms);
            const bool was  = _caretOn;
            _caretOn        = done || !_caretOn;
            if (done)
                stopBlink();
            // With a selection the caret isn't drawn: nothing to repaint.
            if (_caretOn != was && !hasSelection()) {
                const RectF r = caretRect();
                update({r.x - 1, r.y - 1, r.w + 2, r.h + 2});
            }
        });
}

void TextEdit::stopBlink() {
    if (_blinkTimer)
        app()->cancelTimer(_blinkTimer);
    _blinkTimer = 0;
}

// ── Editing core ────────────────────────────────────────────────────────────

uint16_t TextEdit::typingFormat() const {
    if (_typingSet)
        return _typing;
    const uint32_t a = std::min(_caret, _anchor);
    // Continue the format of the text before the caret, but never a link.
    return a > 0 && a <= _fmt.size() ? uint16_t(_fmt[a - 1] & ~kLinkMask) : 0;
}

void TextEdit::apply(const Edit &e, bool reverse) {
    const std::string           &out = reverse ? e.inserted : e.removed;
    const std::string           &in  = reverse ? e.removed : e.inserted;
    const std::vector<uint16_t> &inF = reverse ? e.removedFmt : e.insertedFmt;
    shiftSquiggles(e.pos, out.size(), in.size());
    _text.replace(e.pos, out.size(), in);
    _fmt.erase(_fmt.begin() + e.pos, _fmt.begin() + e.pos + out.size());
    _fmt.insert(_fmt.begin() + e.pos, inF.begin(), inF.end());
    editParas(e.pos, out.size(), in.size());
}

void TextEdit::replace(
    uint32_t         from,
    uint32_t         to,
    std::string_view ins,
    const uint16_t  *fmt,
    uint16_t         fill,
    EditKind         kind
) {
    from = std::min(from, uint32_t(_text.size()));
    to   = std::clamp(to, from, uint32_t(_text.size()));
    if (_maxLength && kind != EditKind::Format) {
        // Only what fits goes in: the edit itself is cut, so undo never
        // steps through text over the limit.
        const std::string_view t(_text);
        const size_t           kept =
            utf8::countCodePoints(t.substr(0, from)) + utf8::countCodePoints(t.substr(to));
        ins = ins.substr(0, utf8::prefixBytes(ins, kept < _maxLength ? _maxLength - kept : 0));
        if (ins.empty() && from == to)
            return;
    }
    Edit e;
    e.pos     = from;
    e.removed = _text.substr(from, to - from);
    e.removedFmt.assign(_fmt.begin() + from, _fmt.begin() + to);
    e.inserted = std::string(ins);
    if (fmt)
        e.insertedFmt.assign(fmt, fmt + ins.size());
    else
        e.insertedFmt.assign(ins.size(), fill);
    e.anchorBefore = _anchor;
    e.caretBefore  = _caret;
    e.kind         = kind;
    e.time         = app()->nowMs();
    apply(e, false);
    if (kind == EditKind::Format) {
        e.anchorAfter = _anchor; // formatting keeps the selection
        e.caretAfter  = _caret;
    } else {
        _caret = _anchor = from + uint32_t(ins.size());
        e.anchorAfter = e.caretAfter = _caret;
        _caretTyped                  = true;
    }
    // Coalesce runs of typing / deleting into one undo step.
    Edit *last   = _undo.empty() ? nullptr : &_undo.back();
    bool  merged = false;
    if (last && last->kind == kind && e.time - last->time < kCoalesceMs) {
        if (kind == EditKind::Typing && e.removed.empty() &&
            last->pos + last->inserted.size() == from && !(ins.size() == 1 && ins[0] == '\n') &&
            !(!last->inserted.empty() && last->inserted.back() == '\n')) {
            last->inserted += e.inserted;
            last->insertedFmt.insert(
                last->insertedFmt.end(), e.insertedFmt.begin(), e.insertedFmt.end()
            );
            merged = true;
        } else if (kind == EditKind::Backspace && e.inserted.empty() && last->pos == to) {
            last->removed.insert(0, e.removed);
            last->removedFmt.insert(
                last->removedFmt.begin(), e.removedFmt.begin(), e.removedFmt.end()
            );
            last->pos = from;
            merged    = true;
        } else if (kind == EditKind::DeleteForward && e.inserted.empty() && last->pos == from) {
            last->removed += e.removed;
            last->removedFmt.insert(
                last->removedFmt.end(), e.removedFmt.begin(), e.removedFmt.end()
            );
            merged = true;
        }
        if (merged) {
            last->anchorAfter = e.anchorAfter;
            last->caretAfter  = e.caretAfter;
            last->time        = e.time;
        }
    }
    if (!merged) {
        _undo.push_back(std::move(e));
        if (_undo.size() > kUndoLimit)
            _undo.erase(_undo.begin());
    }
    _redo.clear();
    if (kind != EditKind::Format)
        _typingSet = false;
    contentChanged();
}

void TextEdit::contentChanged() {
    if (width() > 0) {
        currentLayout();
        const Style &s    = currentStyle();
        const float  want = clampedHeight() + s.pad.t + s.pad.b;
        if (std::abs(want - height()) > 0.5f)
            invalidateLayout(); // grow/shrink the composer
    } else {
        invalidateLayout();
    }
    update();
    ensureCaretVisible();
    startBlink();
    updateIme();
    if (onChange) {
        auto cb = onChange;
        cb();
    }
    selectionChanged();
}

void TextEdit::selectionChanged() {
    if (onSelectionChange) {
        auto cb = onSelectionChange;
        cb();
    }
}

void TextEdit::deleteSelection(EditKind kind) {
    if (!hasSelection())
        return;
    replace(std::min(_caret, _anchor), std::max(_caret, _anchor), {}, nullptr, 0, kind);
}

void TextEdit::moveTo(uint32_t c, bool extend) {
    c = std::min(c, uint32_t(_text.size()));
    if (_caretTyped) {
        _caretTyped = false; // put there, not typed: its squiggle shows
        update();
    }
    if (c == _caret && (extend || c == _anchor))
        return;
    _caret = c;
    if (!extend)
        _anchor = c;
    _typingSet = false;
    update();
    ensureCaretVisible();
    startBlink();
    updateIme();
    selectionChanged();
}

// ── Public editing API ──────────────────────────────────────────────────────

void TextEdit::setText(std::string_view plain) {
    _squiggles.clear();
    _wavesValid = false;
    _text.assign(_maxLength ? plain.substr(0, utf8::prefixBytes(plain, _maxLength)) : plain);
    _fmt.assign(_text.size(), 0);
    _links.clear();
    _undo.clear();
    _redo.clear();
    _preedit.clear();
    _caret = _anchor = uint32_t(_text.size());
    _typingSet       = false;
    _caretTyped      = false;
    _scrollY         = 0;
    splitParas();
    contentChanged();
}

void TextEdit::insertText(std::string_view s) {
    const uint16_t f = typingFormat();
    replace(std::min(_caret, _anchor), std::max(_caret, _anchor), s, nullptr, f, EditKind::Other);
}

void TextEdit::replaceTyped(uint32_t from, uint32_t to, std::string_view s) {
    // The caret stays where it was in the text around the range (after the
    // space that finished a smiley), not after what went in.
    const uint32_t caret = _caret, anchor = _anchor;
    const auto     moved = [&](uint32_t o) {
        return o >= to ? o - (to - from) + uint32_t(s.size()) : std::min(o, from);
    };
    // Not Typing: what is typed next must not join this edit's undo step.
    replace(from, to, s, nullptr, typingFormat(), EditKind::Other);
    _caret  = std::min(moved(caret), uint32_t(_text.size()));
    _anchor = std::min(moved(anchor), uint32_t(_text.size()));
    if (!_undo.empty()) { // redo puts the caret back here too
        _undo.back().caretAfter  = _caret;
        _undo.back().anchorAfter = _anchor;
    }
    update();
    ensureCaretVisible();
    updateIme();
    selectionChanged();
}

void TextEdit::insertHtml(std::string_view html) {
    std::string              t;
    std::vector<uint16_t>    f;
    std::vector<std::string> links;
    rich::fromHtml(html, &t, &f, &links);
    // Remap the fragment's link indices into ours.
    const size_t base = _links.size();
    for (auto &l : links)
        _links.push_back(std::move(l));
    for (uint16_t &x : f)
        if (x & kLinkMask) {
            const size_t idx = ((x >> 8) - 1) + base + 1;
            x                = uint16_t((x & 0xff) | (idx <= 255 ? idx << 8 : 0));
        }
    replace(std::min(_caret, _anchor), std::max(_caret, _anchor), t, f.data(), 0, EditKind::Other);
}

std::vector<TextEdit::Run> TextEdit::runs() const {
    std::vector<Run> out;
    uint32_t         i = 0, n = uint32_t(_text.size());
    while (i < n) {
        uint32_t j = i + 1;
        while (j < n && _fmt[j] == _fmt[i])
            ++j;
        Run r;
        r.start           = i;
        r.end             = j;
        r.format          = uint8_t(_fmt[i] & 0xff);
        const size_t link = _fmt[i] >> 8;
        if (link && link <= _links.size())
            r.link = _links[link - 1];
        out.push_back(r);
        i = j;
    }
    return out;
}

std::string TextEdit::html() const {
    uint32_t a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
    if (a == b) {
        a = 0;
        b = uint32_t(_text.size());
    }
    return rich::toHtml(std::string_view(_text).substr(a, b - a), _fmt.data() + a, _links);
}

void TextEdit::setSelection(uint32_t a, uint32_t c) {
    a       = std::min(a, uint32_t(_text.size()));
    _anchor = a;
    _caret  = uint32_t(-1); // force moveTo to act
    moveTo(c, true);
}

void TextEdit::selectAll() {
    setSelection(0, uint32_t(_text.size()));
}

std::string TextEdit::selectedText() const {
    const uint32_t a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
    return _text.substr(a, b - a);
}

void TextEdit::toggleFormat(Format f) {
    if (!hasSelection()) {
        _typing    = uint16_t(typingFormat() ^ f);
        _typingSet = true;
        selectionChanged(); // toolbar state
        return;
    }
    const uint32_t a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
    bool           all = true;
    for (uint32_t i = a; i < b; ++i)
        all &= (_fmt[i] & f) != 0;
    std::vector<uint16_t> nf(_fmt.begin() + a, _fmt.begin() + b);
    for (uint16_t &x : nf)
        x = all ? uint16_t(x & ~f) : uint16_t(x | f);
    const std::string same = _text.substr(a, b - a);
    replace(a, b, same, nf.data(), 0, EditKind::Format);
}

bool TextEdit::formatActive(Format f) const {
    if (!hasSelection())
        return (typingFormat() & f) != 0;
    const uint32_t a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
    for (uint32_t i = a; i < b; ++i)
        if (!(_fmt[i] & f))
            return false;
    return true;
}

void TextEdit::setLink(std::string url) {
    if (!hasSelection())
        return;
    uint16_t idx = 0;
    if (!url.empty() && _links.size() < 255) {
        _links.push_back(std::move(url));
        idx = uint16_t(_links.size());
    }
    const uint32_t        a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
    std::vector<uint16_t> nf(_fmt.begin() + a, _fmt.begin() + b);
    for (uint16_t &x : nf)
        x = uint16_t((x & 0xff) | (idx << 8));
    const std::string same = _text.substr(a, b - a);
    replace(a, b, same, nf.data(), 0, EditKind::Format);
}

bool TextEdit::pillLink(std::string_view url) {
    return !url.empty() && (url[0] == '<' || url[0] == '@' || url[0] == '#' || url[0] == '!');
}

void TextEdit::setPlaceholder(std::string s) {
    _placeholder = std::move(s);
    _placeholderLayout.reset();
    update();
}

void TextEdit::setMasked(bool on) {
    _masked = on;
    _preedit.clear();
    splitParas();
    invalidateLayout();
    update();
}

void TextEdit::setMaxLength(int n) {
    _maxLength = uint32_t(std::max(0, n));
}

void TextEdit::setMinLines(int n) {
    _minLines = uint8_t(std::clamp(n, 1, 255));
    invalidateLayout();
}

void TextEdit::setMaxLines(int n) {
    _maxLines = uint8_t(std::clamp(n, 0, 255));
    invalidateLayout();
}

void TextEdit::setFont(Font f) {
    _font = f;
    styleChanged();
    invalidateLayout();
}

bool TextEdit::undo() {
    if (_undo.empty())
        return false;
    Edit e = std::move(_undo.back());
    _undo.pop_back();
    apply(e, true);
    _anchor     = e.anchorBefore;
    _caret      = e.caretBefore;
    _caretTyped = true;
    _redo.push_back(std::move(e));
    contentChanged();
    return true;
}

bool TextEdit::redo() {
    if (_redo.empty())
        return false;
    Edit e = std::move(_redo.back());
    _redo.pop_back();
    apply(e, false);
    _anchor     = e.anchorAfter;
    _caret      = e.caretAfter;
    _caretTyped = true;
    e.time      = 0; // never coalesce into a redone step
    _undo.push_back(std::move(e));
    contentChanged();
    return true;
}

// ── Clipboard ───────────────────────────────────────────────────────────────

void TextEdit::copy() {
    if (!hasSelection() || _masked)
        return;
    app()->platform().setClipboard(
        {{"text/plain;charset=utf-8", selectedText()}, {"text/html", html()}}
    );
}

void TextEdit::cut() {
    if (!hasSelection() || _masked)
        return;
    copy();
    deleteSelection();
}

void TextEdit::setLinkBackground(C c) {
    _linkBg = c;
    dropLayouts();
    update();
}

void TextEdit::paste(bool plainText, plat::Selection sel) {
    if (!onPasteMedia)
        return pasteText(plainText, sel);
    std::weak_ptr<char> alive = _alive;
    app()->platform().requestClipboardMimes(
        [this, alive, plainText, sel](std::vector<std::string> m) {
            if (alive.expired())
                return;
            if (!(onPasteMedia && onPasteMedia(m, sel)))
                pasteText(plainText, sel);
        },
        sel
    );
}

void TextEdit::pasteText(bool plainText, plat::Selection sel) {
    plainText                      = plainText || _plainPaste;
    std::weak_ptr<char> alive      = _alive;
    auto                pastePlain = [this, alive, sel] {
        app()->platform().requestClipboard(
            "text/plain;charset=utf-8",
            [this, alive](std::optional<std::string> t) {
                if (alive.expired() || !t || t->empty())
                    return;
                _pasting = true; // the owner links a pasted URL at once
                insertText(*t);
                _pasting = false;
            },
            sel
        );
    };
    if (plainText) {
        pastePlain();
        return;
    }
    app()->platform().requestClipboard(
        "text/html",
        [this, alive, pastePlain](std::optional<std::string> h) {
            if (alive.expired())
                return;
            if (h && !h->empty()) {
                _pasting = true;
                insertHtml(*h);
                _pasting = false;
            } else
                pastePlain();
        },
        sel
    );
}

void TextEdit::setPrimarySelection() {
#if defined(MSGA_FREEDESKTOP) || defined(MSGA_PORT) // a port: if it has PORT_CAP_PRIMARY_SELECTION
    if (hasSelection())
        app()->platform().setClipboardText(selectedText(), plat::Selection::Primary);
#endif
}

// ── Navigation helpers ──────────────────────────────────────────────────────

uint32_t TextEdit::hitOffset(PointF local) {
    const Style &s = currentStyle();
    currentLayout();
    return toModel(docHitTest({local.x - s.pad.l, local.y - s.pad.t + _scrollY}));
}

uint32_t TextEdit::wordLeft(uint32_t o) {
    while (o > 0 && isSpace(_text[o - 1]))
        --o;
    if (o == 0)
        return 0;
    const uint32_t p = prevChar(o);
    currentLayout();
    const uint32_t w = toModel(docWordStart(toDisplay(p)));
    return std::min(w, p);
}

uint32_t TextEdit::wordRight(uint32_t o) {
    const uint32_t n = uint32_t(_text.size());
    while (o < n && isSpace(_text[o]))
        ++o;
    if (o >= n)
        return n;
    currentLayout();
    const uint32_t w = toModel(docWordEnd(toDisplay(o)));
    return w > o ? w : nextChar(o);
}

uint32_t TextEdit::lineEdge(uint32_t o, bool end) {
    currentLayout();
    const RectF r = docCaretRect(toDisplay(o));
    return toModel(docHitTest({end ? 1e6f : -1e6f, r.y + r.h / 2}));
}

std::vector<MenuItem> TextEdit::standardMenuItems() const {
    std::vector<MenuItem> items;
    const bool            sel = hasSelection();
    const char           *mod = plat::primaryMod() == plat::ModSuper ? "Cmd+" : "Ctrl+";
    items.push_back({kCut, i18n::tr("Cut"), std::string(mod) + "X", Button::kNoIcon, sel});
    items.push_back({kCopy, i18n::tr("Copy"), std::string(mod) + "C", Button::kNoIcon, sel});
    items.push_back({kPaste, i18n::tr("Paste"), std::string(mod) + "V"});
    items.push_back({0, {}, {}, Button::kNoIcon, true, false, true});
    items.push_back(
        {kSelectAll,
         i18n::tr("Select all"),
         std::string(mod) + "A",
         Button::kNoIcon,
         !_text.empty()}
    );
    return items;
}

void TextEdit::runStandardItem(int id) {
    focus();
    switch (id) {
    case kCut:
        cut();
        break;
    case kCopy:
        copy();
        break;
    case kPaste:
        paste();
        break;
    case kSelectAll:
        selectAll();
        break;
    }
}

void TextEdit::showContextMenu(PointF local) {
    if (!window())
        return;
    const PointF w = mapToWindow(local);
    if (onContextMenu && !_masked && onContextMenu(hitOffset(local), w))
        return;
    std::weak_ptr<char> alive = _alive;
    Menu::show(
        *window(),
        {w.x, w.y, 0, 0},
        standardMenuItems(),
        [this, alive](int id) {
            if (!alive.expired())
                runStandardItem(id);
        },
        Popup::Place::Over
    );
}

// ── Squiggles ───────────────────────────────────────────────────────────────

void TextEdit::setSquiggles(std::vector<Range> ranges) {
    if (ranges == _squiggles)
        return;
    _squiggles  = std::move(ranges);
    _wavesValid = false;
    update();
}

const TextEdit::Range *TextEdit::squiggleAt(uint32_t offset) const {
    for (const Range &r : _squiggles)
        if (offset >= r.from && offset <= r.to)
            return &r;
    return nullptr;
}

bool TextEdit::squiggleShown(const Range &r) const {
    return !(_caretTyped && focused() && !hasSelection() && _caret >= r.from && _caret <= r.to);
}

void TextEdit::shiftSquiggles(uint32_t pos, size_t removed, size_t inserted) {
    if (_squiggles.empty())
        return;
    _wavesValid              = false;
    const uint32_t     end   = pos + uint32_t(removed);
    const int64_t      delta = int64_t(inserted) - int64_t(removed);
    std::vector<Range> kept;
    for (const Range &r : _squiggles) {
        if (r.to < pos)
            kept.push_back(r);
        else if (r.from > end)
            kept.push_back({uint32_t(r.from + delta), uint32_t(r.to + delta)});
        // else: the edit touches the word; the owner re-checks it
    }
    _squiggles = std::move(kept);
}

// Where each squiggle's lines run (document coordinates), found once per
// layout and set of squiggles rather than on every paint.
void TextEdit::placeWaves() {
    _waves.clear();
    for (size_t s = 0; s < _squiggles.size(); ++s) {
        const Range &r = _squiggles[s];
        if (r.to > _text.size())
            continue;
        const uint32_t a = toDisplay(r.from), z = toDisplay(r.to);
        for (const RectF &b : docSelectionRects(a, z)) {
            float y = b.y + b.h - 2;
            for (size_t k = paraAt(a), e = paraAt(z); k <= e; ++k) {
                const Para &pa    = _paras[k];
                bool        found = false;
                for (int i = 0; i < pa.layout->lineCount() && !found; ++i) {
                    const float base = pa.top + pa.layout->baseline(i);
                    if (base >= b.y && base <= b.y + b.h) {
                        y     = base + 2;
                        found = true;
                    }
                }
                if (found)
                    break;
            }
            _waves.push_back({b.x, y, b.w, uint32_t(s)});
        }
    }
    _wavesValid = true;
}

// A 1-px zigzag 2 px below each line's baseline (spell-check underline).
void TextEdit::paintSquiggles(gfx::Painter &p) {
    if (!_wavesValid)
        placeWaves();
    const Color c = color(C::Danger);
    for (const Wave &w : _waves) {
        if (!squiggleShown(_squiggles[w.squiggle]))
            continue;
        gfx::Path       path;
        constexpr float kStep = 2, kAmp = 1;
        path.moveTo(w.x, w.y);
        int k = 0;
        for (float x = w.x + kStep; x <= w.x + w.w + 0.01f; x += kStep, ++k)
            path.lineTo(x, (k & 1) ? w.y : w.y + kAmp);
        p.strokePath(path, 1, c);
    }
}

// ── Painting ────────────────────────────────────────────────────────────────

void TextEdit::paint(gfx::Painter &p) {
    View::paint(p);
    const Style &s = currentStyle();
    currentLayout();
    p.save();
    p.clipRect({s.pad.l, s.pad.t, contentWidth(), std::max(0.f, height() - s.pad.t - s.pad.b)});
    p.translate(snapPx(s.pad.l), snapPx(s.pad.t - _scrollY));
    if (_text.empty() && _preedit.empty() && !_placeholder.empty()) {
        if (!_placeholderLayout) {
            text::AttributedText t;
            t.append(_placeholder, baseStyle(C::Placeholder));
            text::LayoutOptions o;
            o.maxWidth         = contentWidth();
            o.maxLines         = _maxLines; // a multi-line field may show a list of examples
            o.ellipsis         = true;
            _placeholderLayout = text::Layout::build(std::move(t), o, windowScale());
        }
        _placeholderLayout->paint(p, {0, 0});
    }
    // Only the paragraphs in view.
    const float view = height() - s.pad.t - s.pad.b;
    if (hasSelection()) {
        const uint32_t a = toDisplay(std::min(_caret, _anchor));
        const uint32_t b = toDisplay(std::max(_caret, _anchor));
        const Color c = focused() ? color(C::Selection) : gfx::withAlpha(color(C::Selection), 0.5f);
        for (const RectF &r : docSelectionRects(a, b, _scrollY, _scrollY + view))
            p.fillRect(r, c);
    }
    for (size_t i = paraAtY(_scrollY); i < _paras.size() && _paras[i].top <= _scrollY + view; ++i)
        _paras[i].layout->paint(p, {0, _paras[i].top});
    if (!_squiggles.empty())
        paintSquiggles(p);
    p.restore();
    const bool active = window() && window()->isActive();
    if (focused() && active && _caretOn && !hasSelection()) {
        RectF r = caretRect();
        p.save();
        p.clipRect({s.pad.l - 1, s.pad.t, contentWidth() + 2, height() - s.pad.t - s.pad.b});
        p.fillRect({std::round(r.x), r.y, 1.5f, r.h}, color(C::Caret));
        p.restore();
    }
}

// ── Events ──────────────────────────────────────────────────────────────────

bool TextEdit::onEvent(Event &e) {
    using K = plat::Key;
    switch (e.type) {
    case EventType::FocusIn:
        startBlink();
        updateIme();
        update();
        if (onFocusChange)
            onFocusChange(true);
        return true;
    case EventType::FocusOut:
        _caretTyped = false;
        if (onFocusChange)
            onFocusChange(false);
        stopBlink();
        if (!_preedit.empty()) {
            dropParaAt(_preeditPos);
            _preedit.clear();
            contentChanged();
        }
        if (window())
            window()->setTextInput(false, {});
        update();
        return true;
    case EventType::TextInput: {
        if (!e.raw)
            return false;
        std::string t;
        for (char c : e.raw->text) // drop control characters (except newline)
            if ((uint8_t(c) >= 0x20 || c == '\n') && c != 0x7f)
                t.push_back(c);
        const bool hadPreedit = !_preedit.empty();
        if (hadPreedit)
            dropParaAt(_preeditPos);
        _preedit.clear();
        if (t.empty()) {
            if (hadPreedit)
                contentChanged();
            return true;
        }
        const uint16_t f    = typingFormat();
        const bool     keep = _typingSet;
        replace(
            std::min(_caret, _anchor), std::max(_caret, _anchor), t, nullptr, f, EditKind::Typing
        );
        if (keep) { // an explicit toggle holds for the rest of the typing run
            _typing    = f;
            _typingSet = true;
        }
        return true;
    }
    case EventType::TextPreedit: {
        if (!e.raw || _masked)
            return false;
        if (!_preedit.empty())
            dropParaAt(_preeditPos); // where it showed
        if (e.raw->text.empty()) {
            if (_preedit.empty())
                return true;
            _preedit.clear();
        } else {
            if (_preedit.empty() && hasSelection())
                deleteSelection();
            if (_preedit.empty())
                _preeditPos = std::min(_caret, uint32_t(_text.size()));
            _preedit       = e.raw->text;
            _preeditCursor = e.raw->preeditCursorBegin;
            dropParaAt(_preeditPos);
        }
        invalidateLayout();
        update();
        updateIme();
        return true;
    }
    case EventType::PointerDown: {
        if (e.button == plat::Button::Middle) {
#if defined(MSGA_FREEDESKTOP) || defined(MSGA_PORT) // a port: if it has PORT_CAP_PRIMARY_SELECTION
            // The primary selection, as plain text — or, to an owner that
            // takes media (the composer: the middle click attaches like a
            // paste), its files and pictures.
            moveTo(hitOffset(e.pos), false);
            paste(true, plat::Selection::Primary);
            return true;
#else
            return false;
#endif
        }
        if (e.button != plat::Button::Left)
            return false;
        const uint32_t o = hitOffset(e.pos);
        if (e.clicks == 2) {
            currentLayout();
            _selOriginA = toModel(docWordStart(toDisplay(o)));
            _selOriginB = toModel(docWordEnd(toDisplay(o)));
            _selMode    = 1;
            setSelection(_selOriginA, _selOriginB);
        } else if (e.clicks >= 3) {
            uint32_t a = o, b = o;
            while (a > 0 && _text[a - 1] != '\n')
                --a;
            while (b < _text.size() && _text[b] != '\n')
                ++b;
            _selOriginA = a;
            _selOriginB = b;
            _selMode    = 2;
            setSelection(a, b);
        } else {
            _selMode = 0;
            moveTo(o, (e.mods & plat::ModShift) != 0);
        }
        _dragging = true;
        return true;
    }
    case EventType::PointerMove:
        if (!_dragging)
            return false;
        {
            const uint32_t o = hitOffset(e.pos);
            if (_selMode == 0) {
                moveTo(o, true);
            } else {
                currentLayout();
                uint32_t a = _selOriginA, b = _selOriginB;
                if (_selMode == 1) {
                    a = std::min(a, toModel(docWordStart(toDisplay(o))));
                    b = std::max(b, toModel(docWordEnd(toDisplay(o))));
                } else {
                    a = std::min(a, o);
                    b = std::max(b, o);
                }
                if (o < _selOriginA)
                    setSelection(b, a);
                else
                    setSelection(a, b);
            }
        }
        return true;
    case EventType::PointerUp:
        if (!_dragging)
            return false;
        _dragging = false;
        setPrimarySelection();
        return true;
    case EventType::Scroll: {
        const Style &s = currentStyle();
        currentLayout();
        const float view = height() - s.pad.t - s.pad.b;
        const float max  = std::max(0.f, docHeight() - view);
        if (max <= 0)
            return false;
        const float old = _scrollY;
        _scrollY        = std::clamp(_scrollY + (e.precise ? e.dy : e.dy * kWheelStep), 0.f, max);
        if (_scrollY == old)
            return false;
        update();
        updateIme();
        return true;
    }
    case EventType::ContextMenu:
        showContextMenu(e.pos);
        return true;
    case EventType::KeyDown:
        break;
    default:
        return false;
    }

    // ── KeyDown ─────────────────────────────────────────────────────────────
    if (onKey) {
        auto cb = onKey;
        if (cb(e))
            return true;
    }
    if (!_preedit.empty())
        return true; // the IME owns the keyboard while composing
    currentLayout(); // caret movement asks the paragraphs' layouts
    const uint32_t m       = e.mods & kModMask;
    const uint32_t primary = plat::primaryMod();
    const bool     shift   = m & plat::ModShift;
    const bool     cmd     = (m & primary) && !(m & ~(primary | plat::ModShift));
#ifdef __APPLE__
    const uint32_t wordMod = plat::ModAlt;
#else
    const uint32_t wordMod = plat::ModCtrl;
#endif
    const bool     word = (m & wordMod) != 0;
    const uint32_t a = std::min(_caret, _anchor), b = std::max(_caret, _anchor);
#ifndef __APPLE__
    // Alt+arrows edit nothing here: leave them to the
    // window's shortcuts (Alt+Left/Right = back/forward).
    if ((m & plat::ModAlt) &&
        (e.key == K::Left || e.key == K::Right || e.key == K::Up || e.key == K::Down))
        return false;
#endif
    switch (e.key) {
    case K::Left:
    case K::Right: {
        const bool left = e.key == K::Left;
        if (hasSelection() && !shift && !word && !(m & plat::ModSuper)) {
            moveTo(left ? a : b, false);
            return true;
        }
        uint32_t c = _caret;
#ifdef __APPLE__
        if (m & plat::ModSuper)
            c = lineEdge(c, !left);
        else
#endif
            if (word)
            c = left ? wordLeft(c) : wordRight(c);
        else
            c = toModel(docMoveCaret(toDisplay(c), left ? -1 : 1, 0));
        moveTo(c, shift);
        return true;
    }
    case K::Up:
    case K::Down: {
        const bool up = e.key == K::Up;
        uint32_t   c;
#ifdef __APPLE__
        if (m & plat::ModSuper)
            c = up ? 0 : uint32_t(_text.size());
        else
#endif
        {
            c = toModel(docMoveCaret(toDisplay(_caret), 0, up ? -1 : 1));
            if (c == _caret)
                c = up ? 0 : uint32_t(_text.size()); // first/last line: to the edge
        }
        moveTo(c, shift);
        return true;
    }
    case K::Home:
    case K::End: {
        const bool end = e.key == K::End;
        moveTo(
            (m & plat::ModCtrl) ? (end ? uint32_t(_text.size()) : 0) : lineEdge(_caret, end), shift
        );
        return true;
    }
    case K::Backspace:
        if (hasSelection())
            deleteSelection(EditKind::Backspace);
        else if (_caret > 0) {
            uint32_t from =
                word ? wordLeft(_caret) : toModel(docMoveCaret(toDisplay(_caret), -1, 0));
            if (from >= _caret)
                from = prevChar(_caret);
            replace(from, _caret, {}, nullptr, 0, EditKind::Backspace);
        }
        return true;
    case K::Delete:
        if (hasSelection())
            deleteSelection(EditKind::DeleteForward);
        else if (_caret < _text.size()) {
            uint32_t to = word ? wordRight(_caret) : toModel(docMoveCaret(toDisplay(_caret), 1, 0));
            if (to <= _caret)
                to = nextChar(_caret);
            replace(_caret, to, {}, nullptr, 0, EditKind::DeleteForward);
        }
        return true;
    case K::Enter:
    case K::KpEnter:
        if (onSubmit && !shift && !(m & (plat::ModCtrl | plat::ModAlt | plat::ModSuper))) {
            auto cb = onSubmit;
            if (cb())
                return true;
        }
        replace(a, b, "\n", nullptr, typingFormat(), EditKind::Typing);
        return true;
    default:
        break;
    }
    if (cmd) {
        switch (e.key) {
        case K::A:
            selectAll();
            return true;
        // Format keys (Ctrl+B, Ctrl+Shift+X, …) belong to the composer's
        // shortcut table, not to every text field.
        case K::C:
            if (shift)
                return false;
            copy();
            return true;
        case K::X:
            if (shift)
                return false;
            cut();
            return true;
        case K::V:
            paste(shift);
            return true;
        case K::Z:
            if (shift)
                redo();
            else
                undo();
            return true;
        case K::Y:
            if (primary == plat::ModCtrl) {
                redo();
                return true;
            }
            return false;
        default:
            return false;
        }
    }
    // Printable keys arrive as TextInput; claim their KeyDown so plain-key
    // shortcuts never fire while typing.
    const bool printable = (e.key >= K::A && e.key <= K::Num9) || e.key == K::Space ||
                           (e.key >= K::Minus && e.key <= K::Slash) ||
                           (e.key >= K::Kp0 && e.key <= K::KpEqual && e.key != K::KpEnter);
    return printable && !(m & (plat::ModCtrl | plat::ModSuper));
}

} // namespace ui

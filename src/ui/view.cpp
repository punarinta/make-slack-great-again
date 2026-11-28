#include "ui/view.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
// v within [lo, hi]; lo wins when the two cross (a min above the max).
inline float clampSize(float v, float lo, float hi) {
    return std::clamp(v, lo, std::max(lo, hi));
}
} // namespace

float View::windowScale() const {
    return _window ? _window->scale() : 1.f;
}

RectF intersect(RectF a, RectF b) {
    const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

RectF unite(RectF a, RectF b) {
    if (empty(a))
        return b;
    if (empty(b))
        return a;
    const float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const float x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

View::View() = default;

View::~View() {
    // Children go first (their destructors still see a valid _window), then
    // this view drops out of the window's hover/focus/capture bookkeeping.
    _children.clear();
    unwatchHide();
    if (_window)
        _window->forget(this);
}

void View::unwatchHide() {
    if (!_window || !flag(WatchesHide) || _window->_dying)
        return;
    auto &l = _window->_hideWatchers;
    l.erase(std::remove(l.begin(), l.end(), this), l.end());
}

View *View::adopt(std::unique_ptr<View> v, int index) {
    View *raw    = v.get();
    raw->_parent = this;
    if (index < 0 || size_t(index) >= _children.size())
        _children.push_back(std::move(v));
    else
        _children.insert(_children.begin() + index, std::move(v));
    raw->setWindow(_window);
    invalidateLayout();
    return raw;
}

std::unique_ptr<View> View::remove(View *child) {
    for (auto it = _children.begin(); it != _children.end(); ++it) {
        if (it->get() != child)
            continue;
        if (_window && child->visible())
            _window->damage(child->damageRect());
        std::unique_ptr<View> out = std::move(*it);
        _children.erase(it);
        out->setWindow(nullptr);
        out->_parent = nullptr;
        invalidateLayout();
        return out;
    }
    return nullptr;
}

void View::clearChildren() {
    if (_children.empty())
        return;
    update();
    _children.clear();
    invalidateLayout();
}

bool View::isAncestorOf(const View *v) const {
    for (; v; v = v->_parent)
        if (v == this)
            return true;
    return false;
}

void View::setWindow(Window *w) {
    if (_window == w)
        return;
    if (_window)
        _window->forget(this);
    unwatchHide();
    _window = w;
    if (w && flag(WatchesHide))
        w->_hideWatchers.push_back(this);
    // Newly attached subtrees must be laid out and painted.
    _flags |= NeedsLayout;
    _flags &= ~MeasureValid;
    for (auto &c : _children)
        c->setWindow(w);
    windowChanged();
    if (w && flag(Ticking)) {
        _flags &= ~Ticking;
        startTicking();
    }
}

Style &View::style() {
    invalidateLayout();
    return _style;
}

void View::setFrame(RectF f) {
    if (sameRect(f, _frame))
        return;
    if (_window && visible())
        _window->damage(damageRect());
    const bool resized = f.w != _frame.w || f.h != _frame.h;
    _frame             = f;
    if (resized)
        _flags |= NeedsLayout;
    if (_window && visible())
        _window->damage(damageRect());
}

void View::setFrameQuiet(RectF f) {
    if (f.w != _frame.w || f.h != _frame.h)
        _flags |= NeedsLayout;
    _frame = f;
}

PointF View::mapToWindow(PointF p) const {
    for (const View *v = this; v; v = v->_parent) {
        p.x += v->_frame.x;
        p.y += v->_frame.y;
    }
    return p;
}

PointF View::mapFromWindow(PointF p) const {
    const PointF o = mapToWindow({0, 0});
    return {p.x - o.x, p.y - o.y};
}

RectF View::windowRect() const {
    return clippedRect(0);
}

RectF View::damageRect() const {
    return clippedRect(_outset);
}

RectF View::clippedRect(float out) const {
    // One walk up: the rect in each ancestor's coordinates, clipped by the
    // clipping ones (so offscreen rows in a list damage nothing).
    RectF r{_frame.x - out, _frame.y - out, _frame.w + 2 * out, _frame.h + 2 * out};
    bool  clipped = false;
    for (const View *v = _parent; v; v = v->_parent) {
        if (v->flag(ClipChildren)) {
            r       = intersect(r, {0, 0, v->_frame.w, v->_frame.h});
            clipped = true;
        }
        r.x += v->_frame.x;
        r.y += v->_frame.y;
    }
    return clipped && empty(r) ? RectF{} : r;
}

void View::setVisible(bool on) {
    if (visible() == on)
        return;
    // Hiding the focus holder (or an ancestor of it) moves the focus away
    // like disabling does: it gets its FocusOut (a field stops its caret).
    if (!on && _window && _window->_focus && isAncestorOf(_window->_focus)) {
        _window->setFocus(nullptr);
        if (!visible())
            return; // its handler hid it already
    }
    if (!on)
        update();
    setFlag(Visible, on);
    if (on)
        update();
    // Hidden children take no space: the parent re-lays out.
    if (_parent)
        _parent->invalidateLayout();
    if (!on && _window) {
        _window->forget(this); // drops focus/hover/capture held inside
        // Watchers inside learn it (a copy: a handler may hide or drop views).
        if (!_window->_hideWatchers.empty()) {
            const std::vector<View *> watchers = _window->_hideWatchers;
            for (View *w : watchers) {
                const auto &live = _window->_hideWatchers; // one may be gone by now
                if (w != this && std::find(live.begin(), live.end(), w) != live.end() &&
                    isAncestorOf(w))
                    w->hiddenByAncestor();
            }
        }
    }
    visibilityChanged(on);
}

void View::watchAncestorHide() {
    if (flag(WatchesHide))
        return;
    setFlag(WatchesHide, true);
    if (_window)
        _window->_hideWatchers.push_back(this);
}

bool View::enabled() const {
    for (const View *v = this; v; v = v->_parent)
        if (v->flag(Disabled))
            return false;
    return true;
}

void View::setEnabled(bool on) {
    if (flag(Disabled) == !on)
        return;
    setFlag(Disabled, !on);
    // A disabled widget loses keyboard focus, so typing can't reach it.
    if (!on && _window && _window->_focus && isAncestorOf(_window->_focus))
        _window->setFocus(nullptr);
    update();
}

void View::setFocusable(bool on, bool focusOnClick) {
    setFlag(Focusable, on);
    setFlag(FocusOnClick, on && focusOnClick);
}

void View::focus() {
    if (_window)
        _window->setFocus(this);
}

void View::setCursor(plat::Cursor c) {
    _cursor = uint8_t(c);
    if (_window && hovered())
        _window->refreshCursor();
}

void View::setBackground(C token, float radius) {
    _bg     = uint8_t(token);
    _radius = radius;
    update();
}

float View::snapPx(float v) const {
    const float s = windowScale();
    return std::floor(v * s + 0.5f + 1e-3f) / s; // biased: ties always round one way
}

bool View::opaqueBackground() const {
    return _bg && _radius <= 0 && (color(C(_bg)) >> 24) == 0xff;
}

void View::setBorder(C token) {
    _border = uint8_t(token);
    update();
}

void View::update() {
    if (_window && visible())
        _window->damage(damageRect());
}

void View::update(RectF local) {
    if (!_window || !visible())
        return;
    const PointF o = mapToWindow({local.x, local.y});
    RectF        r{o.x, o.y, local.w, local.h};
    r = intersect(r, damageRect());
    _window->damage(r);
}

void View::invalidateLayout() {
    View *v = this;
    v->_flags |= NeedsLayout;
    v->_flags &= ~MeasureValid;
    // A boundary's size does not depend on its content, so the dirtiness
    // stops there; ancestors only learn that something below needs a pass.
    while (!v->flag(LayoutBoundary) && v->_parent) {
        v = v->_parent;
        v->_flags |= NeedsLayout;
        v->_flags &= ~MeasureValid;
    }
    v->markParentsDirty();
    if (_window)
        _window->requestFrame();
}

void View::markParentsDirty() {
    for (View *p = _parent; p && !p->flag(SubtreeDirty); p = p->_parent)
        p->_flags |= SubtreeDirty;
}

void View::startTicking() {
    if (flag(Ticking) && _window)
        return;
    setFlag(Ticking, true);
    if (_window) {
        _window->_ticking.push_back(this);
        _window->requestFrame();
    }
}

View *View::hitTest(PointF p) {
    for (size_t i = _children.size(); i-- > 0;) {
        View *c = _children[i].get();
        if (!c->visible() || c->flag(Disabled))
            continue;
        const RectF &f = c->_frame;
        if (p.x < f.x || p.y < f.y || p.x >= f.x + f.w || p.y >= f.y + f.h)
            continue;
        if (View *h = c->hitTest({p.x - f.x, p.y - f.y}))
            return h;
    }
    return flag(HitTransparent) ? nullptr : this;
}

void View::paint(gfx::Painter &p) {
    if (_bg) {
        if (_radius > 0)
            p.fillRoundRect(bounds(), _radius, color(C(_bg)));
        else
            p.fillRect(bounds(), color(C(_bg)));
    }
    if (_border)
        p.strokeRoundRect(bounds(), _radius, 1, color(C(_border)));
}

// ── Measuring ───────────────────────────────────────────────────────────────

SizeF View::measure(float aw, float ah) {
    // Two answers kept: a flex pass asks at the room offered, then at the
    // size it resolved, and the layout that follows asks both again.
    if (flag(MeasureValid)) {
        if (aw == _mcW && ah == _mcH)
            return _mc;
        if (aw == _mc2W && ah == _mc2H) {
            std::swap(_mc, _mc2);
            std::swap(_mcW, _mc2W);
            std::swap(_mcH, _mc2H);
            return _mc;
        }
        _mc2  = _mc; // the newer one stays second
        _mc2W = _mcW;
        _mc2H = _mcH;
    } else {
        _mc2W = _mc2H = -1e30f; // no width or height asks this
    }
    const Style &s = _style;
    float        w = s.w, h = s.h;
    if (w < 0 || h < 0) {
        const float padX = s.pad.l + s.pad.r, padY = s.pad.t + s.pad.b;
        const float cw = (w >= 0 ? w : std::min(aw, s.maxW)) - padX;
        const float ch = (h >= 0 ? h : std::min(ah, s.maxH)) - padY;
        const SizeF c  = measureContent(std::max(0.f, cw), std::max(0.f, ch));
        if (w < 0)
            w = c.w + padX;
        if (h < 0)
            h = c.h + padY;
    }
    w    = clampSize(w, s.minW, s.maxW);
    h    = clampSize(h, s.minH, s.maxH);
    _mc  = {w, h};
    _mcW = aw;
    _mcH = ah;
    _flags |= MeasureValid;
    return _mc;
}

SizeF View::measureContent(float aw, float ah) {
    return measureFlex(aw, ah);
}

void View::layout() {
    layoutFlex();
}

namespace {

// One child's working values; main/cross are along the parent's axis.
struct Item {
    View *v;
    float main, cross, minMain, maxMain, marginMain, marginCross;
    bool  flexible; // auto main size (may shrink)
    float grow, shrink;
};

// A child's working values before sizing.
Item makeItem(View *c, const Style &cs, bool row) {
    const float fixed = row ? cs.w : cs.h;
    return {
        c,
        0,
        0,
        row ? cs.minW : cs.minH,
        row ? cs.maxW : cs.maxH,
        row ? cs.margin.l + cs.margin.r : cs.margin.t + cs.margin.b,
        row ? cs.margin.t + cs.margin.b : cs.margin.l + cs.margin.r,
        fixed < 0,
        cs.grow,
        cs.shrink
    };
}

// A shared scratch stack: layouts nest (measure recurses), so each call
// pushes its items and pops them afterwards instead of allocating.
std::vector<Item> &scratch() {
    static std::vector<Item> s;
    return s;
}

inline bool finite(float v) {
    return v < kInf / 2;
}
inline float fixedMain(const Style &s, bool row) {
    return row ? s.w : s.h;
}
inline float fixedCross(const Style &s, bool row) {
    return row ? s.h : s.w;
}

Align effectiveAlign(const Style &parent, const Style &child) {
    return child.self != Align::Auto ? child.self : parent.align;
}

float alignOffset(Align a, float space, float size) {
    switch (a) {
    case Align::Center:
        return std::floor((space - size) / 2);
    case Align::End:
        return space - size;
    default:
        return 0;
    }
}

// Main sizes for the visible children of `v` in [base, end) of the scratch
// stack, given the main space available. Returns the free space left over.
float resolveMain(std::vector<Item> &items, size_t base, float mainAvail, float gap) {
    const size_t n    = items.size() - base;
    float        used = n > 1 ? gap * float(n - 1) : 0, growSum = 0, shrinkSum = 0;
    for (size_t i = base; i < items.size(); ++i) {
        Item &it = items[i];
        used += it.main + it.marginMain;
        if (it.grow > 0)
            growSum += it.grow;
        if (it.flexible && it.shrink > 0)
            shrinkSum += it.shrink * it.main;
    }
    float free = mainAvail - used;
    if (!finite(mainAvail))
        return 0;
    if (free > 0 && growSum > 0) {
        for (size_t i = base; i < items.size(); ++i) {
            Item &it = items[i];
            if (const float g = it.grow; g > 0) {
                const float before = it.main;
                it.main = std::clamp(it.main + free * g / growSum, it.minMain, it.maxMain);
                used += it.main - before;
            }
        }
        free = mainAvail - used;
    } else if (free < 0 && shrinkSum > 0) {
        const float over = -free;
        for (size_t i = base; i < items.size(); ++i) {
            Item       &it = items[i];
            const float sh = it.shrink;
            if (!it.flexible || sh <= 0)
                continue;
            const float before = it.main;
            it.main            = std::max(it.minMain, it.main - over * sh * it.main / shrinkSum);
            used += it.main - before;
        }
        free = mainAvail - used;
    }
    return free;
}

} // namespace

SizeF View::measureFlex(float aw, float ah) {
    const Style &s = _style;
    if (s.dir == Dir::None)
        return {0, 0};
    if (s.dir == Dir::Stack) {
        SizeF out;
        for (auto &cp : _children) {
            View *c = cp.get();
            if (!c->visible())
                continue;
            const Edges &m  = c->_style.margin;
            const SizeF  cs = c->measure(aw - m.l - m.r, ah - m.t - m.b);
            out.w           = std::max(out.w, cs.w + m.l + m.r);
            out.h           = std::max(out.h, cs.h + m.t + m.b);
        }
        return out;
    }
    const bool   row       = s.dir == Dir::Row;
    const float  mainAvail = row ? aw : ah, crossAvail = row ? ah : aw;
    auto        &items = scratch();
    const size_t base  = items.size();
    for (auto &cp : _children) {
        View *c = cp.get();
        if (!c->visible())
            continue;
        const Style &cs     = c->_style;
        Item         it     = makeItem(c, cs, row);
        const float  mMain  = it.marginMain;
        const float  mCross = it.marginCross;
        if (!it.flexible)
            it.main = fixedMain(cs, row);
        else if (cs.grow > 0 && finite(mainAvail))
            it.main = it.minMain; // flex: grow 1 0 — sized from the free space
        else {
            const SizeF sz = row ? c->measure(mainAvail - mMain, crossAvail - mCross)
                                 : c->measure(crossAvail - mCross, mainAvail - mMain);
            it.main        = row ? sz.w : sz.h;
            it.cross       = row ? sz.h : sz.w;
        }
        items.push_back(it);
    }
    resolveMain(items, base, mainAvail, s.gap);
    float main = items.size() > base + 1 ? s.gap * float(items.size() - base - 1) : 0, cross = 0;
    for (size_t i = base; i < items.size(); ++i) {
        // Cross size at the resolved main size (wrapped text gets taller).
        // measure() pushes onto the scratch stack, so no reference is held.
        View       *v  = items[i].v;
        const float mn = items[i].main, mc = items[i].marginCross;
        const SizeF sz = row ? v->measure(mn, crossAvail - mc) : v->measure(crossAvail - mc, mn);
        Item       &it = items[i];
        it.cross       = row ? sz.h : sz.w;
        main += it.main + it.marginMain;
        cross = std::max(cross, it.cross + it.marginCross);
    }
    items.resize(base);
    return row ? SizeF{main, cross} : SizeF{cross, main};
}

void View::layoutFlex() {
    const Style &s = _style;
    if (s.dir == Dir::None)
        return;
    const float ix = s.pad.l, iy = s.pad.t;
    const float iw = std::max(0.f, _frame.w - s.pad.l - s.pad.r);
    const float ih = std::max(0.f, _frame.h - s.pad.t - s.pad.b);
    // Edges land on the physical pixel grid (snapPx, not whole logical px):
    // at 1.5x nothing sits on a half-pixel tie, where float noise would flip
    // glyph and fill rounding between two paints of the same view.
    if (s.dir == Dir::Stack) {
        for (auto &cp : _children) {
            View *c = cp.get();
            if (!c->visible())
                continue;
            const Style &cs = c->_style;
            const Edges &m  = cs.margin;
            const float  aw = iw - m.l - m.r, ah = ih - m.t - m.b;
            const Align  a = effectiveAlign(s, cs);
            SizeF        sz;
            if (a == Align::Stretch) {
                sz = {
                    cs.w >= 0 ? cs.w : clampSize(aw, cs.minW, cs.maxW),
                    cs.h >= 0 ? cs.h : clampSize(ah, cs.minH, cs.maxH)
                };
            } else {
                sz = c->measure(aw, ah);
            }
            const float x  = ix + m.l + (a == Align::Stretch ? 0 : alignOffset(a, aw, sz.w));
            const float y  = iy + m.t + (a == Align::Stretch ? 0 : alignOffset(a, ah, sz.h));
            const float x0 = snapPx(x), y0 = snapPx(y);
            c->setFrame({x0, y0, snapPx(x + sz.w) - x0, snapPx(y + sz.h) - y0});
        }
        return;
    }
    const bool   row       = s.dir == Dir::Row;
    const float  mainAvail = row ? iw : ih, crossAvail = row ? ih : iw;
    auto        &items = scratch();
    const size_t base  = items.size();
    for (auto &cp : _children) {
        View *c = cp.get();
        if (!c->visible())
            continue;
        const Style &cs           = c->_style;
        Item         it           = makeItem(c, cs, row);
        const float  mMain        = it.marginMain;
        const float  mCross       = it.marginCross;
        const Align  a            = effectiveAlign(s, cs);
        const float  fc           = fixedCross(cs, row);
        const float  stretchCross = fc >= 0 ? fc : crossAvail - mCross;
        if (!it.flexible)
            it.main = fixedMain(cs, row);
        else if (cs.grow > 0)
            it.main = it.minMain;
        else if (row)
            it.main = c->measure(mainAvail - mMain, crossAvail - mCross).w;
        else // a column child's height depends on the width it will get
            it.main =
                c->measure(
                     a == Align::Stretch ? stretchCross : crossAvail - mCross, mainAvail - mMain
                )
                    .h;
        items.push_back(it);
    }
    float free = resolveMain(items, base, mainAvail, s.gap);
    for (size_t i = base; i < items.size(); ++i) {
        View        *v  = items[i].v;
        const Style &cs = v->_style;
        const Align  a  = effectiveAlign(s, cs);
        const float  fc = fixedCross(cs, row);
        const float  mn = items[i].main, mc = items[i].marginCross;
        float        cross;
        if (fc >= 0)
            cross = fc;
        else if (a == Align::Stretch)
            cross = crossAvail - mc;
        else // measure() may grow the scratch stack: no Item& across it
            cross = row ? v->measure(mn, crossAvail - mc).h : v->measure(crossAvail - mc, mn).w;
        Item &it = items[i];
        it.cross = row ? clampSize(cross, cs.minH, cs.maxH) : clampSize(cross, cs.minW, cs.maxW);
    }
    const size_t n   = items.size() - base;
    float        pos = row ? ix : iy, between = s.gap;
    if (free > 0 && n > 0) {
        switch (s.justify) {
        case Justify::Center:
            pos += std::floor(free / 2);
            break;
        case Justify::End:
            pos += free;
            break;
        case Justify::SpaceBetween:
            if (n > 1)
                between += free / float(n - 1);
            break;
        default:
            break;
        }
    }
    for (size_t i = base; i < items.size(); ++i) {
        Item        &it  = items[i];
        const Style &cs  = it.v->_style;
        const Align  a   = effectiveAlign(s, cs);
        const float  mmS = row ? cs.margin.l : cs.margin.t;
        const float  mcS = row ? cs.margin.t : cs.margin.l;
        const float  crossPos =
            (row ? iy : ix) + mcS +
            (a == Align::Stretch ? 0 : alignOffset(a, crossAvail - it.marginCross, it.cross));
        const float m0 = snapPx(pos + mmS), m1 = snapPx(pos + mmS + it.main);
        const float c0 = snapPx(crossPos), c1 = snapPx(crossPos + it.cross);
        it.v->setFrame(row ? RectF{m0, c0, m1 - m0, c1 - c0} : RectF{c0, m0, c1 - c0, m1 - m0});
        pos += it.main + it.marginMain + between;
    }
    items.resize(base);
}

RectF View::tooltipAnchor() const {
    return windowRect();
}

bool View::showsBitmap(const gfx::Bitmap *) const {
    return false;
}

} // namespace ui

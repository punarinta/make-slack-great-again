// Window: event routing (hover chain, capture, focus, shortcuts, popups,
// DnD), the layout pass and damage-driven painting. App: the singleton that
// owns the plat app and the theme.
#include "base/str.h"
#include "ui/view.h"
#include "ui/widgets.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace ui {

App *App::s_instance = nullptr;

namespace {

constexpr int kTooltipDelayMs = 600;
constexpr int kLongPressMs    = 550; // press-and-hold → context menu

// The overlay layer: every child is a Popup, placed by its anchor.
class Overlay final : public View {
public:
    Overlay() {
        style().dir = Dir::None;
        setHitTransparent(true);
        setLayoutBoundary(true);
    }
    void layout() override {
        const SizeF win{width(), height()};
        for (size_t i = 0; i < childCount(); ++i) {
            auto *p = static_cast<Popup *>(child(i));
            if (p->visible())
                p->setFrame(p->placeIn(win));
        }
    }
};

// The tooltip: a near-black chip in both
// themes, medium-weight white text, 10/5 padding, 6-px radius, and an arrow
// (7-px half base, 6 high) pointing at the target's centre; above the target,
// below it when there is no room.
class TooltipPopup final : public Popup {
public:
    explicit TooltipPopup(std::string text) : _text(std::move(text)) {
        setModal(false);
        setCard(false);
        setRole(Role::Tooltip);
        setPaintOutset(6);
    }
    View *hitTest(PointF) override { return nullptr; } // never blocks the pointer
    SizeF measureContent(float, float) override {
        if (!_l) {
            text::AttributedText t;
            text::Style          st = font(Font::Body);
            st.weight               = text::Weight::Medium;
            st.color                = color(C::TooltipText); // text.onDark
            t.append(_text, st);
            _l = text::Layout::build(std::move(t), {}, windowScale());
        }
        return {std::ceil(_l->width()) + 2 * kPadH, std::ceil(_l->height()) + 2 * kPadV + kArrowH};
    }
    void styleChanged() override { _l.reset(); }
    void paint(gfx::Painter &p) override {
        if (!_l)
            measureContent(0, 0);
        const RectF w     = windowRect();
        const bool  below = w.y > anchor().y;
        const RectF body{0, below ? kArrowH : 0, width(), height() - kArrowH};
        const Color bg = color(C::TooltipBg); // tooltip.bg
        p.dropShadow(body, kRadius, 5, {0, 1}, 0x30000000U);
        p.fillRoundRect(body, kRadius, bg);
        const float cx = std::clamp(anchor().x + anchor().w / 2 - w.x, kArrowW, width() - kArrowW);
        gfx::Path   a;
        if (below) {
            a.moveTo(cx - kArrowW, body.y + 3);
            a.lineTo(cx + kArrowW, body.y + 3);
            a.lineTo(cx, 0);
        } else {
            a.moveTo(cx - kArrowW, body.bottom() - 3);
            a.lineTo(cx + kArrowW, body.bottom() - 3);
            a.lineTo(cx, body.bottom() + kArrowH);
        }
        a.close();
        p.fillPath(a, bg);
        _l->paint(p, snapPx({std::floor((width() - _l->width()) / 2), body.y + kPadV}));
    }

private:
    static constexpr float        kPadH = 10, kPadV = 5, kRadius = 6, kArrowW = 7, kArrowH = 6;
    std::string                   _text;
    std::unique_ptr<text::Layout> _l;
};

bool contains(RectF outer, RectF inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

} // namespace

// ── Window ──────────────────────────────────────────────────────────────────

Window::Window(const plat::WindowDesc &desc) {
    _native           = app()->platform().createWindow(desc);
    _native->userData = this;
    _scale            = float(_native->scale());
    _root             = std::make_unique<View>();
    _root->_style.dir = Dir::Stack;
    _root->setLayoutBoundary(true);
    _root->setBackground(C::WindowBg);
    _overlay = std::make_unique<Overlay>();
    _root->setWindow(this);
    _overlay->setWindow(this);
    app()->addWindow(this);
#ifdef MSGA_UI_VERIFY
    if (const char *v = std::getenv("UI_VERIFY"); v && *v && *v != '0')
        _verify = true;
#endif
    resized();
}

Window::~Window() {
    _dying = true;
    if (_tooltipTimer)
        app()->cancelTimer(_tooltipTimer);
    if (_longPressTimer)
        app()->cancelTimer(_longPressTimer);
    _overlay.reset();
    _root.reset();
    _graveyard.clear();
    app()->removeWindow(this);
    _native->userData = nullptr;
    _native.reset();
}

SizeF Window::size() const {
    const plat::Size s = _native->size();
    return {float(s.w), float(s.h)};
}

void Window::resized() {
    const SizeF s     = size();
    const float scale = float(_native->scale());
    _root->setFrame({0, 0, s.w, s.h});
    _overlay->setFrame({0, 0, s.w, s.h});
    _root->invalidateLayout();
    _overlay->invalidateLayout();
    if (scale != _scale) {
        _scale = scale;
        styleChangedAll(); // text layouts are rasterised per scale
    }
    _blits.clear();
    damageAll();
}

void Window::styleChangedAll() {
    auto walk = [](auto &self, View *v) -> void {
        v->_flags &= ~View::MeasureValid;
        v->_flags |= View::NeedsLayout | View::SubtreeDirty;
        v->styleChanged();
        for (auto &c : v->_children)
            self(self, c.get());
    };
    walk(walk, _root.get());
    walk(walk, _overlay.get());
    damageAll();
}

void Window::forget(View *v) {
    if (_dying)
        return;
    // The flag goes with the focus, or a view put back later still thinks
    // it has it (setVisible(false) moved it away properly before this).
    if (_focus && v->isAncestorOf(_focus)) {
        _focus->_flags &= ~View::Focused;
        _focus = nullptr;
    }
    if (_capture && v->isAncestorOf(_capture))
        _capture = nullptr;
    if (_dropView && v->isAncestorOf(_dropView))
        _dropView = nullptr;
    if (_scrollLatch && v->isAncestorOf(_scrollLatch))
        _scrollLatch = nullptr;
    if (_handled == v)
        _handled = nullptr;
    if (_tooltipFor && v->isAncestorOf(_tooltipFor))
        _tooltipFor = nullptr;
    if (_tooltip == v)
        _tooltip = nullptr;
    // The hover chain is leaf-first: v and everything before it are inside v.
    auto it = std::find(_hoverChain.begin(), _hoverChain.end(), v);
    if (it != _hoverChain.end()) {
        for (auto j = _hoverChain.begin(); j != it + 1; ++j)
            (*j)->_flags &= ~View::Hovered;
        _hoverChain.erase(_hoverChain.begin(), it + 1);
    }
    // The flag goes too, or a later startTicking() would think it still ticks.
    _ticking.erase(std::remove(_ticking.begin(), _ticking.end(), v), _ticking.end());
    v->_flags &= ~View::Ticking;
    for (auto &f : _focusStack)
        if (f == v)
            f = nullptr;
}

// ── Focus ───────────────────────────────────────────────────────────────────

void Window::setFocus(View *v, bool visible) {
    if (v == _focus || (v && !v->enabled()))
        return;
    _focusVisible = visible;
    View *old     = _focus;
    _focus        = v;
    if (old) {
        old->_flags &= ~View::Focused;
        Event e{EventType::FocusOut};
        old->onEvent(e);
        old->update();
    }
    if (v && _focus == v) {
        v->_flags |= View::Focused;
        Event e{EventType::FocusIn};
        v->onEvent(e);
        v->update();
    }
}

bool Window::focusNext(bool backward) {
    View *scope = _root.get();
    for (size_t i = _overlay->childCount(); i-- > 0;) {
        auto *p = static_cast<Popup *>(_overlay->child(i));
        if (p->modal()) {
            scope = p;
            break;
        }
    }
    std::vector<View *> list;
    auto                walk = [&](auto &self, View *v) -> void {
        if (!v->visible() || v->flag(View::Disabled))
            return;
        if (v->flag(View::Focusable))
            list.push_back(v);
        for (auto &c : v->_children)
            self(self, c.get());
    };
    walk(walk, scope);
    if (list.empty())
        return false;
    auto            it  = std::find(list.begin(), list.end(), _focus);
    ptrdiff_t       idx = it == list.end() ? (backward ? 0 : -1) : it - list.begin();
    const ptrdiff_t n   = ptrdiff_t(list.size());
    idx                 = ((idx + (backward ? -1 : 1)) % n + n) % n;
    setFocus(list[size_t(idx)]);
    _focusVisible = true;
    if (_focus) {
        _focus->update();
        for (View *p = _focus->parent(); p; p = p->parent())
            p->revealFocus(_focus);
    }
    return true;
}

// ── Shortcuts ───────────────────────────────────────────────────────────────

int Window::addShortcut(plat::Key key, uint32_t mods, std::function<void()> fn) {
    _shortcuts.push_back({_nextShortcut, key, mods, std::move(fn)});
    return _nextShortcut++;
}

void Window::removeShortcut(int id) {
    _shortcuts.erase(
        std::remove_if(
            _shortcuts.begin(), _shortcuts.end(), [id](const Shortcut &s) { return s.id == id; }
        ),
        _shortcuts.end()
    );
}

// ── Popups ──────────────────────────────────────────────────────────────────

Popup *Window::showPopup(std::unique_ptr<Popup> p) {
    Popup *raw = p.get();
    _overlay->adopt(std::move(p));
    raw->update();
    if (raw->modal()) {
        _focusStack.push_back(_focus);
        raw->setFocusable(true);
        setFocus(raw);
    }
    return raw;
}

void Window::closePopup(Popup *p) {
    if (!p || p->parent() != _overlay.get())
        return;
    p->willClose(); // first: a menu closes its submenu, so focus unwinds in order
    if (p->parent() != _overlay.get())
        return;
    const bool            modal    = p->modal();
    const bool            hadFocus = !_focus || p->isAncestorOf(_focus);
    auto                  owned    = _overlay->remove(p);
    std::function<void()> cb       = std::move(p->onClosed);
    if (modal && !_focusStack.empty()) {
        View *prev = _focusStack.back();
        _focusStack.pop_back();
        if (hadFocus)
            setFocus(prev);
    }
    // Deferred: the popup may be closing itself from its own event handler.
    _graveyard.push_back(std::move(owned));
    if (cb)
        cb();
}

void Window::closeAllPopups() {
    while (_overlay->childCount() > 0)
        closePopup(static_cast<Popup *>(_overlay->child(_overlay->childCount() - 1)));
}

Popup *Window::topPopup() const {
    for (size_t i = _overlay->childCount(); i-- > 0;) {
        auto *p = static_cast<Popup *>(_overlay->child(i));
        if (p != _tooltip)
            return p;
    }
    return nullptr;
}

Popup *Window::topPopup(const std::function<bool(const Popup &)> &accept) const {
    for (size_t i = _overlay->childCount(); i-- > 0;) {
        auto *p = static_cast<Popup *>(_overlay->child(i));
        if (p != _tooltip && accept(*p))
            return p;
    }
    return nullptr;
}

void Window::flushGraveyard() {
    // A popup's destructor may close another one, which lands here again.
    while (_depth == 0 && !_graveyard.empty()) {
        std::vector<std::unique_ptr<View>> dead;
        dead.swap(_graveyard);
    }
}

// ── Tooltips ────────────────────────────────────────────────────────────────

void Window::armTooltip() {
    if (_tooltipTimer)
        app()->cancelTimer(_tooltipTimer);
    _tooltipTimer = 0;
    if (_buttonHeld || _hoverChain.empty())
        return;
    for (View *v : _hoverChain)
        if (!v->tooltip().empty()) {
            if (v->tooltipImmediate()) {
                showTooltip();
                return;
            }
            break;
        }
    _tooltipTimer = app()->addTimer(kTooltipDelayMs, false, [this] {
        _tooltipTimer = 0;
        showTooltip();
    });
}

void Window::showTooltip() {
    for (View *v : _hoverChain) {
        std::string t = v->tooltip();
        if (t.empty())
            continue;
        hideTooltip();
        auto p = std::make_unique<TooltipPopup>(std::move(t));
        p->setAnchor(v->tooltipAnchor(), Popup::Place::Tip);
        _tooltipFor = v;
        _tooltip    = showPopup(std::move(p));
        return;
    }
}

void Window::rearmTooltip() {
    hideTooltip();
    armTooltip();
}

void Window::hideTooltip() {
    if (_tooltipTimer) {
        app()->cancelTimer(_tooltipTimer);
        _tooltipTimer = 0;
    }
    if (_tooltip) {
        Popup *t    = _tooltip;
        _tooltip    = nullptr;
        _tooltipFor = nullptr;
        closePopup(t);
    }
}

// ── Pointer ─────────────────────────────────────────────────────────────────

View *Window::hitAt(PointF p) {
    if (View *v = _overlay->hitTest(p))
        return v;
    return _root->hitTest(p);
}

bool Window::dispatch(View *target, Event &e) {
    _handled = nullptr;
    // A disabled view and everything inside it get nothing; its ancestors do.
    for (View *v = target; v; v = v->_parent)
        if (v->flag(View::Disabled))
            target = v->_parent;
    if (!target)
        return false;
    // The target's window origin, then each ancestor's by taking off the
    // frame of the view below it: one walk, not one per ancestor.
    PointF o = target->mapToWindow({0, 0});
    for (View *v = target; v; v = v->_parent) {
        e.pos = {e.windowPos.x - o.x, e.windowPos.y - o.y};
        if (v->onEvent(e)) {
            _handled = v;
            return true;
        }
        o.x -= v->_frame.x;
        o.y -= v->_frame.y;
    }
    return false;
}

void Window::updateHover(View *target) {
    std::vector<View *> &chain = _hoverNext;
    chain.clear();
    for (View *v = target; v; v = v->_parent)
        chain.push_back(v);
    if (chain == _hoverChain)
        return;
    // Both are paths to a root: what they share is a common tail (the
    // ancestors both hover), the rest leaves (old) or enters (new).
    size_t common = 0;
    while (common < chain.size() && common < _hoverChain.size() &&
           chain[chain.size() - 1 - common] == _hoverChain[_hoverChain.size() - 1 - common])
        ++common;
    const View  *oldLeaf = _hoverChain.empty() ? nullptr : _hoverChain.front();
    const size_t leaving = _hoverChain.size() - common;
    for (size_t i = 0; i < leaving && i < _hoverChain.size(); ++i) {
        View *v = _hoverChain[i];
        v->_flags &= ~View::Hovered;
        Event e{EventType::PointerLeave};
        e.windowPos = _pointer;
        e.pos       = v->mapFromWindow(_pointer);
        v->onEvent(e);
        if (v->flag(View::HoverRepaint))
            v->update();
    }
    _hoverChain.swap(chain);
    const size_t entering = _hoverChain.size() - common;
    for (size_t i = 0; i < entering && i < _hoverChain.size(); ++i) {
        View *v = _hoverChain[i];
        v->_flags |= View::Hovered;
        Event e{EventType::PointerEnter};
        e.windowPos = _pointer;
        e.pos       = v->mapFromWindow(_pointer);
        v->onEvent(e);
        if (v->flag(View::HoverRepaint))
            v->update();
    }
    if (_tooltipFor &&
        std::find(_hoverChain.begin(), _hoverChain.end(), _tooltipFor) == _hoverChain.end())
        hideTooltip();
    if ((_hoverChain.empty() ? nullptr : _hoverChain.front()) != oldLeaf && !_tooltip)
        armTooltip();
}

void Window::refreshCursor() {
    View   *src = _capture ? _capture : (_hoverChain.empty() ? nullptr : _hoverChain.front());
    uint8_t c   = uint8_t(plat::Cursor::Arrow);
    PointF  o   = src ? src->mapToWindow({0, 0}) : PointF{};
    for (View *v = src; v; v = v->_parent) {
        const uint8_t cc = v->cursorAt({_pointer.x - o.x, _pointer.y - o.y});
        if (cc != View::kCursorInherit) {
            c = cc;
            break;
        }
        o.x -= v->_frame.x;
        o.y -= v->_frame.y;
    }
    if (c != _cursor) {
        _cursor = c;
        _native->setCursor(plat::Cursor(c));
    }
}

void Window::pointerMove(PointF pos, const plat::Event &pe) {
    _pointer = pos;
    if (_longPressTimer && std::abs(pos.x - _pressPos.x) + std::abs(pos.y - _pressPos.y) > 8) {
        app()->cancelTimer(_longPressTimer); // a drag or a selection, not a long press
        _longPressTimer = 0;
    }
    Event e{EventType::PointerMove};
    e.windowPos = pos;
    e.mods      = pe.mods;
    e.raw       = &pe;
    if (_capture) {
        e.pos = _capture->mapFromWindow(pos);
        _capture->onEvent(e);
        refreshCursor();
        return;
    }
    View *t = hitAt(pos);
    updateHover(t);
    if (t)
        dispatch(t, e);
    refreshCursor();
}

void Window::pointerDown(const plat::Event &pe) {
    _pointer    = {float(pe.pos.x), float(pe.pos.y)};
    _buttonHeld = true;
    for (View *v : std::vector<View *>(_ticking))
        v->interruptAnimation();
    hideTooltip();
    View *t      = hitAt(_pointer);
    // A press outside the top modal popup closes it (and every modal popup
    // above the hit) and is swallowed, like a native menu.
    bool  closed = false;
    while (Popup *top = topPopup()) {
        if (!top->modal() || (t && top->isAncestorOf(t)))
            break;
        closePopup(top);
        closed = true;
    }
    if (closed || !t)
        return;
    for (View *v = t; v; v = v->_parent)
        if (v->flag(View::FocusOnClick) && v->enabled()) {
            setFocus(v);
            break;
        }
    Event e{EventType::PointerDown};
    e.windowPos = _pointer;
    e.button    = pe.button;
    e.clicks    = pe.clicks;
    e.mods      = pe.mods;
    e.raw       = &pe;
    if (pe.button == plat::Button::Left) {
        // Held still, a press becomes a context menu (touch screens, pens).
        if (_longPressTimer)
            app()->cancelTimer(_longPressTimer);
        _pressPos       = _pointer;
        _longPressTimer = app()->addTimer(kLongPressMs, false, [this] { longPress(); });
    }
    if (dispatch(t, e))
        // Not a view the handler just detached (a popup closing itself on
        // the press, like a dialog's backdrop): it is in the graveyard.
        _capture = _handled && _handled->window() == this ? _handled : nullptr;
    else if (pe.button == plat::Button::Right) {
        Event cm{EventType::ContextMenu};
        cm.windowPos = _pointer;
        cm.mods      = pe.mods;
        cm.raw       = &pe;
        dispatch(t, cm);
    }
    refreshCursor();
}

void Window::longPress() {
    _longPressTimer = 0;
    View *t         = _buttonHeld ? hitAt(_pointer) : nullptr;
    if (!t)
        return;
    ++_depth;
    Event cm{EventType::ContextMenu};
    cm.windowPos = _pointer;
    if (dispatch(t, cm)) {
        // A menu opened: the press that led to it must not click on release.
        if (View *c = _capture) {
            _capture = nullptr;
            Event x{EventType::PointerCancel};
            x.windowPos = _pointer;
            x.pos       = c->mapFromWindow(_pointer);
            c->onEvent(x);
        }
    }
    --_depth;
    flushGraveyard();
    requestFrame();
}

void Window::pointerUp(const plat::Event &pe) {
    _pointer    = {float(pe.pos.x), float(pe.pos.y)};
    _buttonHeld = false;
    if (_longPressTimer) {
        app()->cancelTimer(_longPressTimer);
        _longPressTimer = 0;
    }
    Event e{EventType::PointerUp};
    e.windowPos = _pointer;
    e.button    = pe.button;
    e.mods      = pe.mods;
    e.raw       = &pe;
    if (View *c = _capture) {
        _capture = nullptr;
        e.pos    = c->mapFromWindow(_pointer);
        c->onEvent(e);
    } else if (View *t = hitAt(_pointer)) {
        dispatch(t, e);
    }
    updateHover(hitAt(_pointer));
    refreshCursor();
}

void Window::scroll(const plat::Event &pe) {
    _pointer           = {float(pe.pos.x), float(pe.pos.y)};
    // A touchpad gesture stays with the view that took its Begin, so the list
    // under a moving pointer does not change mid-fling.
    const bool latched = pe.precise && pe.phase != plat::ScrollPhase::None &&
                         pe.phase != plat::ScrollPhase::Begin && _scrollLatch;
    View      *t       = latched ? _scrollLatch : hitAt(_pointer);
    if (!t)
        return;
    Event e{EventType::Scroll};
    e.windowPos = _pointer;
    e.dx        = float(pe.dx);
    e.dy        = float(pe.dy);
    e.precise   = pe.precise;
    e.phase     = pe.phase;
    e.mods      = pe.mods;
    e.raw       = &pe;
    const bool handled =
        latched ? (e.pos = t->mapFromWindow(_pointer), t->onEvent(e)) : dispatch(t, e);
    if (!latched)
        _scrollLatch = handled && pe.phase == plat::ScrollPhase::Begin ? _handled : nullptr;
}

void Window::key(const plat::Event &pe) {
    Event e{pe.type == plat::EventType::KeyDown ? EventType::KeyDown : EventType::KeyUp};
    e.key       = pe.key;
    e.mods      = pe.mods;
    e.repeat    = pe.repeat;
    e.raw       = &pe;
    e.windowPos = _pointer;
    if (e.type == EventType::KeyUp) {
        if (_focus)
            dispatch(_focus, e);
        return;
    }
    hideTooltip();
    // Taken, and the focus moved meanwhile: the text that follows this press
    // is not typed (see _dropKeyText).
    View      *focusBefore = _focus;
    const auto taken       = [&] { _dropKeyText = _focus != focusBefore; };
    if (keyFilter) {
        auto filter = keyFilter; // may replace itself
        if (filter(e))
            return taken();
        // A filter that passes the key on may have handed the focus to the
        // view that should get it, text and all (typing on into a field).
        focusBefore = _focus;
    }
    if (_focus && dispatch(_focus, e))
        return taken();
    const uint32_t mods = pe.mods & kModMask;
    for (const Shortcut &s : _shortcuts) {
        uint32_t want = s.mods & ~kPrimary;
        if (s.mods & kPrimary)
            want |= plat::primaryMod();
        if (s.key == pe.key && want == mods) {
            auto fn = s.fn; // the handler may remove its own shortcut
            fn();
            return taken();
        }
    }
    if (pe.key == plat::Key::Tab && !(mods & ~plat::ModShift)) {
        focusNext(mods & plat::ModShift);
        return;
    }
    if (_focus &&
        (pe.key == plat::Key::Menu || (pe.key == plat::Key::F10 && mods == plat::ModShift))) {
        Event       cm{EventType::ContextMenu};
        const RectF r = _focus->windowRect();
        cm.windowPos  = {r.x + std::min(r.w / 2, 24.f), r.y + std::min(r.h / 2, 24.f)};
        dispatch(_focus, cm);
        return;
    }
    if (pe.key == plat::Key::Escape)
        if (Popup *p = topPopup(); p && p->modal())
            closePopup(p);
}

void Window::drop(const plat::Event &pe) {
    _pointer = {float(pe.pos.x), float(pe.pos.y)};
    if (pe.type == plat::EventType::DropLeave) {
        if (View *d = _dropView) {
            _dropView = nullptr;
            Event e{EventType::DropLeave};
            e.raw = &pe;
            d->onEvent(e);
        }
        return;
    }
    Event e{
        pe.type == plat::EventType::Drop        ? EventType::Drop
        : pe.type == plat::EventType::DropEnter ? EventType::DropEnter
                                                : EventType::DropMove
    };
    e.windowPos   = _pointer;
    e.mods        = pe.mods;
    e.raw         = &pe;
    e.dropAction  = plat::DropAction::None;
    View      *t  = hitAt(_pointer);
    const bool ok = t && dispatch(t, e);
    View      *h  = ok ? _handled : nullptr;
    if (_dropView && _dropView != h) {
        Event l{EventType::DropLeave};
        l.raw = &pe;
        _dropView->onEvent(l);
    }
    _dropView = e.type == EventType::Drop ? nullptr : h;
    if (e.type != EventType::Drop)
        _native->setDropAction(ok ? e.dropAction : plat::DropAction::None);
}

bool Window::startDrag(const plat::DragDesc &d) {
    _capture = nullptr; // the OS owns the pointer until DragFinished
    return app()->platform().startDrag(*_native, d);
}

void Window::setTextInput(bool enabled, RectF caret) {
    if (enabled == _textInput && sameRect(caret, _textCaret))
        return;
    _textInput = enabled;
    _textCaret = caret;
    plat::TextInputState s;
    s.enabled = enabled;
    s.caret   = {
        int(std::floor(caret.x)),
        int(std::floor(caret.y)),
        std::max(1, int(std::ceil(caret.w))),
        std::max(1, int(std::ceil(caret.h)))
    };
    _native->setTextInput(s);
}

// ── Events ──────────────────────────────────────────────────────────────────

void Window::handle(const plat::Event &e) {
    ++_depth;
    using T             = plat::EventType;
    // Only the very next event can be that press's text: every platform
    // sends it right after the KeyDown (a key that types nothing must not
    // leave the next IME commit to be dropped).
    const bool dropText = std::exchange(_dropKeyText, false);
    if (inputFilter) {
        switch (e.type) {
        case T::PointerDown:
        case T::PointerUp:
        case T::Scroll:
        case T::GestureBegin:
        case T::GestureUpdate:
        case T::GestureEnd:
        case T::SwipeGesture:
            if (auto filter = inputFilter; filter(e)) { // may replace itself
                --_depth;
                flushGraveyard();
                return;
            }
            break;
        default:
            break;
        }
    }
    switch (e.type) {
    case T::CloseRequested:
        if (onCloseRequested)
            onCloseRequested();
        else
            app()->quit();
        break;
    case T::Resized:
        resized();
        break;
    case T::Frame:
        onFrame(_frameRequested);
        break;
    case T::FocusIn:
    case T::FocusOut: {
        _active = e.type == T::FocusIn;
        if (!_active) {
            hideTooltip();
            _capture    = nullptr;
            _buttonHeld = false;
        }
        if (_focus) {
            Event fe{_active ? EventType::FocusIn : EventType::FocusOut};
            fe.raw = &e;
            _focus->onEvent(fe);
            // The handler may have dropped the focus (a popup that closes
            // itself when its field loses focus).
            if (_focus)
                _focus->update();
        }
        if (onEvent)
            onEvent(e); // the app's own activation handling (mark read, …)
        break;
    }
    case T::PointerEnter:
    case T::PointerMove:
        _pointerInside = true;
        pointerMove({float(e.pos.x), float(e.pos.y)}, e);
        break;
    case T::PointerLeave:
        _pointerInside = false;
        if (!_capture) {
            updateHover(nullptr);
            hideTooltip();
        }
        break;
    case T::PointerDown:
        pointerDown(e);
        break;
    case T::PointerUp:
        pointerUp(e);
        break;
    case T::Scroll:
        scroll(e);
        break;
    case T::KeyDown:
    case T::KeyUp:
        key(e);
        break;
    case T::TextInput:
    case T::TextPreedit:
        if (e.type == T::TextInput && dropText)
            break; // the text of a key press that was taken (see key())
        if (_focus && _focus->enabled()) {
            Event te{e.type == T::TextInput ? EventType::TextInput : EventType::TextPreedit};
            te.raw = &e;
            _focus->onEvent(te);
        }
        break;
    case T::DropEnter:
    case T::DropMove:
    case T::DropLeave:
    case T::Drop:
        drop(e);
        break;
    default:
        if (onEvent)
            onEvent(e);
        break;
    }
    --_depth;
    flushGraveyard();
}

// ── Damage and frames ───────────────────────────────────────────────────────

void Window::requestFrame() {
    if (_inFrame) {
        _frameAgain = true;
        return;
    }
    if (!_frameRequested && _native) {
        _frameRequested = true;
        _native->requestFrame();
    }
}

void Window::damage(RectF r) {
    const SizeF s = size();
    r             = intersect(r, {0, 0, s.w, s.h});
    if (empty(r))
        return;
    for (const RectF &d : _damage)
        if (contains(d, r))
            return;
    _damage.erase(
        std::remove_if(
            _damage.begin(), _damage.end(), [&](const RectF &d) { return contains(r, d); }
        ),
        _damage.end()
    );
    _damage.push_back(r);
    // Many small rects cost more in clip/traversal than one bounding box.
    if (_damage.size() > 16) {
        RectF u;
        for (const RectF &d : _damage)
            u = unite(u, d);
        _damage.assign(1, u);
    }
    requestFrame();
}

void Window::damageAll() {
    const SizeF s = size();
    _damage.assign(1, RectF{0, 0, s.w, s.h});
    requestFrame();
}

void Window::damageShowing(const std::vector<std::shared_ptr<const gfx::Bitmap>> &bitmaps) {
    if (bitmaps.empty())
        return;
    // Hidden subtrees paint nothing: skipped whole.
    auto walk = [&](auto &self, View *v) -> void {
        if (!v->visible())
            return;
        for (const auto &b : bitmaps)
            if (b && v->showsBitmap(b.get())) {
                v->update();
                break;
            }
        for (auto &c : v->_children)
            self(self, c.get());
    };
    walk(walk, _root.get());
    walk(walk, _overlay.get());
}

void Window::scrollBlit(View *area, int dy) {
    const SizeF ws   = size();
    const RectF rect = intersect(area->windowRect(), {0, 0, ws.w, ws.h});
    if (dy == 0 || empty(rect))
        return;
    bool merged = false;
    for (Blit &b : _blits) {
        if (sameRect(b.rect, rect)) {
            b.dy += dy;
            merged = true;
            break;
        }
        if (overlaps(b.rect, rect)) { // nested scroll areas: just repaint both
            damage(b.rect);
            damage(rect);
            b.dy = 0;
            return;
        }
    }
    if (!merged)
        _blits.push_back({rect, dy});
    // Damage recorded before the blit covers stale pixels that now move too.
    const float         ldy   = float(dy) / _scale;
    std::vector<RectF> &moved = _movedScratch;
    moved.clear();
    for (const RectF &d : _damage)
        if (RectF part = intersect(d, rect); !empty(part)) {
            part.y -= ldy;
            part = intersect(part, rect);
            if (!empty(part))
                moved.push_back(part);
        }
    for (const RectF &m : moved)
        damage(m);
    // The strip the content scrolled in from.
    const float strip = std::min(rect.h, std::abs(ldy) + 1);
    damage(
        ldy > 0 ? RectF{rect.x, rect.y + rect.h - strip, rect.w, strip}
                : RectF{rect.x, rect.y, rect.w, strip}
    );
    // Whatever is painted above the area stays put while the pixels under it
    // move: the blit carries its pixels along (repaint where they went) and
    // leaves moved content where it is (repaint it there). Above = later
    // siblings of the area and of each ancestor, and every popup.
    auto occluder = [&](View *v) {
        if (!v->visible())
            return;
        const RectF o = intersect(v->damageRect(), rect);
        if (empty(o))
            return;
        damage(o);
        damage(intersect({o.x, o.y - ldy, o.w, o.h}, rect));
    };
    for (View *a = area; a->_parent; a = a->_parent) {
        bool after = false;
        for (auto &c : a->_parent->_children) {
            if (c.get() == a)
                after = true;
            else if (after)
                occluder(c.get());
        }
    }
    for (auto &c : _overlay->_children)
        occluder(c.get());
    requestFrame();
}

void Window::layoutPass(View *v) {
    if (!v->visible())
        return;
    const bool did = v->flag(View::NeedsLayout);
    if (did) {
        v->layout();
        v->_flags &= ~View::NeedsLayout;
    }
    if (did || v->flag(View::SubtreeDirty)) {
        v->_flags &= ~View::SubtreeDirty;
        for (size_t i = 0; i < v->_children.size(); ++i) {
            View *c = v->_children[i].get();
            if (c->_flags & (View::NeedsLayout | View::SubtreeDirty))
                layoutPass(c);
        }
    }
}

void Window::paintTree(View *v, gfx::Painter &p, RectF dmg, float ox, float oy) {
    if (!v->visible())
        return;
    const RectF &f = v->_frame;
    const float  x = ox + f.x, y = oy + f.y, o = v->_outset;
    if (!overlaps({x - o, y - o, f.w + 2 * o, f.h + 2 * o}, dmg))
        return;
    ++_stats.viewsPainted;
    p.save();
    p.translate(f.x, f.y);
    if (v->flag(View::Disabled))
        p.setOpacity(0.45f); // disabled subtrees dim uniformly
    v->paint(p);
    RectF cd = dmg;
    if (v->flag(View::ClipChildren)) {
        if (v->_radius > 0)
            p.clipRoundRect(v->bounds(), v->_radius); // a rounded card's corners
        else
            p.clipRect(v->bounds());
        cd = intersect(dmg, {x, y, f.w, f.h});
    }
    if (!empty(cd))
        for (auto &c : v->_children)
            paintTree(c.get(), p, cd, x, y);
    v->paintOver(p);
    p.restore();
}

void Window::onFrame(bool requested) {
    const double t0 = app()->nowMs();
    _frameRequested = false;
    if (!requested)
        damageAll(); // map/expose without our asking: the canvas may be new
    _inFrame = true;
    flushGraveyard();
    if (!_ticking.empty()) {
        _tickScratch.assign(_ticking.begin(), _ticking.end()); // a tick may stop others
        for (View *v : _tickScratch) {
            if (std::find(_ticking.begin(), _ticking.end(), v) == _ticking.end())
                continue; // removed by an earlier tick
            if (!v->tick(t0)) {
                v->_flags &= ~View::Ticking;
                // Gone already when its last tick hid it (forget()).
                if (auto it = std::find(_ticking.begin(), _ticking.end(), v); it != _ticking.end())
                    _ticking.erase(it);
            }
        }
    }
    const double tl = app()->nowMs();
    for (int pass = 0; pass < 3; ++pass) {
        layoutPass(_root.get());
        layoutPass(_overlay.get());
        if (!((_root->_flags | _overlay->_flags) & (View::NeedsLayout | View::SubtreeDirty)))
            break;
    }
    _stats.lastLayoutMs = app()->nowMs() - tl;
    if (_hoverDirty) {
        _hoverDirty = false;
        if (_pointerInside && !_capture) {
            updateHover(hitAt(_pointer));
            refreshCursor();
        }
    }
    _frameAgain = false;

    if (!_damage.empty() || !_blits.empty()) {
        const double tp = app()->nowMs();
        plat::Canvas c  = _native->beginPaint();
        if (!c.pixels) { // the backend could not give us a buffer: try again next frame
            _blits.clear();
            damageAll();
            _inFrame = false;
            return;
        }
        if (c.width != _canvasW || c.height != _canvasH) {
            _canvasW = c.width;
            _canvasH = c.height;
            _blits.clear();
            damageAll();
            _frameAgain = false;
        }
        const float              s     = float(c.scale);
        // Blits first: shift what the canvas shows, then repaint the damage.
        // The shifted rects changed as much as repainted ones did: they go to
        // endPaint too, or a backend that presents (or keeps its other
        // buffers up to date) by damage shows the old pixels there.
        std::vector<plat::Rect> &rects = _presented; // the shifted, then the repainted
        rects.clear();
        for (const Blit &b : _blits) {
            const int x0 = std::max(0, int(std::ceil(b.rect.x * s)));
            const int x1 = std::min(c.width, int(std::floor((b.rect.x + b.rect.w) * s)));
            const int y0 = std::max(0, int(std::ceil(b.rect.y * s)));
            const int y1 = std::min(c.height, int(std::floor((b.rect.y + b.rect.h) * s)));
            const int dy = b.dy, h = y1 - y0, w = x1 - x0;
            if (w <= 0 || std::abs(dy) >= h || dy == 0) {
                damage(b.rect);
                continue;
            }
            rects.push_back({x0, y0, w, h});
            const size_t bytes = size_t(w) * 4;
            if (dy > 0)
                for (int y = y0; y < y1 - dy; ++y)
                    std::memmove(
                        c.pixels + size_t(y) * c.stride + x0,
                        c.pixels + size_t(y + dy) * c.stride + x0,
                        bytes
                    );
            else
                for (int y = y1 - 1; y >= y0 - dy; --y)
                    std::memmove(
                        c.pixels + size_t(y) * c.stride + x0,
                        c.pixels + size_t(y + dy) * c.stride + x0,
                        bytes
                    );
            // Edges of a rect that does not sit on whole physical pixels.
            if (x0 / s != b.rect.x || x1 / s != b.rect.x + b.rect.w || y0 / s != b.rect.y ||
                y1 / s != b.rect.y + b.rect.h) {
                const float e = 1.f / s + 0.01f;
                damage({b.rect.x, b.rect.y, b.rect.w, e});
                damage({b.rect.x, b.rect.y + b.rect.h - e, b.rect.w, e});
                damage({b.rect.x, b.rect.y, e, b.rect.h});
                damage({b.rect.x + b.rect.w - e, b.rect.y, e, b.rect.h});
            }
        }
        _blits.clear();
        const size_t shifted = rects.size();
        for (const RectF &d : _damage) {
            const int x0 = std::max(0, int(std::floor(d.x * s)));
            const int y0 = std::max(0, int(std::floor(d.y * s)));
            const int x1 = std::min(c.width, int(std::ceil((d.x + d.w) * s)));
            const int y1 = std::min(c.height, int(std::ceil((d.y + d.h) * s)));
            if (x1 > x0 && y1 > y0)
                rects.push_back({x0, y0, x1 - x0, y1 - y0});
        }
        _damage.clear();
        _frameAgain = false;
        gfx::Painter p({c.pixels, c.width, c.height, c.stride}, s, &_paintScratch);
        _stats.viewsPainted = 0;
        for (size_t i = shifted; i < rects.size(); ++i) {
            const plat::Rect &r = rects[i];
            const RectF       lr{r.x / s, r.y / s, r.w / s, r.h / s};
            p.save();
            p.clipRect(lr);
            paintTree(_root.get(), p, lr, 0, 0);
            paintTree(_overlay.get(), p, lr, 0, 0);
            p.restore();
        }
        _stats.lastDamage.assign(rects.begin() + ptrdiff_t(shifted), rects.end());
        double verifyMs = 0;
#ifdef MSGA_UI_VERIFY
        if (_verify) {
            const double tv = app()->nowMs();
            verifyFrame(c, rects);
            verifyMs = app()->nowMs() - tv;
        }
        _verifyMs = verifyMs;
#endif
        _native->endPaint(rects);
        _stats.lastPaintMs = app()->nowMs() - tp - verifyMs; // the check is not the cost
        ++_stats.frames;
    }
    _inFrame           = false;
    _stats.lastFrameMs = app()->nowMs() - t0;
#ifdef MSGA_UI_VERIFY
    _stats.lastFrameMs -= _verifyMs;
    _verifyMs = 0;
#endif
    if (_frameAgain || !_ticking.empty() || !_damage.empty()) {
        _frameAgain     = false;
        _frameRequested = true;
        _native->requestFrame();
    }
}

// ── Frame verification (debug) ──────────────────────────────────────────────

#ifdef MSGA_UI_VERIFY
namespace {
void writePpm(const std::string &path, const uint32_t *px, int w, int h, int stride) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t p       = px[size_t(y) * size_t(stride) + size_t(x)];
            row[size_t(x) * 3]     = uint8_t(p >> 16);
            row[size_t(x) * 3 + 1] = uint8_t(p >> 8);
            row[size_t(x) * 3 + 2] = uint8_t(p);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}
} // namespace

bool Window::dumpFullRepaint(const std::string &path) {
    const float s = _scale;
    const int   w = int(std::lround(size().w * s)), h = int(std::lround(size().h * s));
    if (w <= 0 || h <= 0)
        return false;
    gfx::Bitmap  full(w, h);
    gfx::Painter p(full.view(), s);
    const RectF  all{0, 0, float(w) / s, float(h) / s};
    const int    painted = _stats.viewsPainted;
    paintTree(_root.get(), p, all, 0, 0);
    paintTree(_overlay.get(), p, all, 0, 0);
    _stats.viewsPainted = painted;
    writePpm(path, full.pixels(), w, h, w);
    return true;
}

void Window::verifyFrame(const plat::Canvas &c, const std::vector<plat::Rect> &presented) {
    const int w = c.width, h = c.height;
    if (_shadowW != w || _shadowH != h) {
        // A new canvas is presented whole (the frame after a resize repaints all).
        _shadowW = w;
        _shadowH = h;
        _shadow.assign(size_t(w) * size_t(h), 0);
        for (int y = 0; y < h; ++y)
            std::memcpy(
                &_shadow[size_t(y) * size_t(w)], c.pixels + size_t(y) * c.stride, size_t(w) * 4
            );
        ++_stats.verifiedFrames;
        return;
    }
    // What a damage-only presenter now shows.
    for (plat::Rect r : presented) {
        const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
        const int x1 = std::min(w, r.x + r.w), y1 = std::min(h, r.y + r.h);
        for (int y = y0; y < y1; ++y)
            std::memcpy(
                &_shadow[size_t(y) * size_t(w) + size_t(x0)],
                c.pixels + size_t(y) * c.stride + x0,
                size_t(std::max(0, x1 - x0)) * 4
            );
    }
    // What it should show: the whole tree painted from scratch.
    gfx::Bitmap  full(w, h);
    const float  s = float(c.scale);
    gfx::Painter p(full.view(), s);
    const int    painted = _stats.viewsPainted;
    const RectF  all{0, 0, float(w) / s, float(h) / s};
    paintTree(_root.get(), p, all, 0, 0);
    paintTree(_overlay.get(), p, all, 0, 0);
    _stats.viewsPainted = painted;
    int    bx0 = w, by0 = h, bx1 = -1, by1 = -1, maxDelta = 0;
    size_t diff = 0, noise = 0;
    for (int y = 0; y < h; ++y) {
        const uint32_t *a = &_shadow[size_t(y) * size_t(w)];
        const uint32_t *b = full.pixels() + size_t(y) * size_t(w);
        if (std::memcmp(a, b, size_t(w) * 4) == 0)
            continue;
        for (int x = 0; x < w; ++x)
            if (a[x] != b[x]) {
                // ±1 per channel is float noise in anti-aliased edges at
                // fractional scales (a shape at k/1.5 logical rasterises a
                // hair differently after moving by whole pixels): invisible,
                // counted apart. Anything larger is a real stale pixel.
                int d = 0;
                for (int sh = 0; sh < 32; sh += 8)
                    d = std::max(d, std::abs(int((a[x] >> sh) & 0xff) - int((b[x] >> sh) & 0xff)));
                if (d <= 1) {
                    ++noise;
                    continue;
                }
                maxDelta = std::max(maxDelta, d);
                ++diff;
                bx0 = std::min(bx0, x);
                bx1 = std::max(bx1, x);
                by0 = std::min(by0, y);
                by1 = std::max(by1, y);
            }
    }
    ++_stats.verifiedFrames;
    if (noise)
        ++_stats.verifyNoiseFrames;
    if (!diff) {
        if (noise) // keep the shadow exact so noise never accumulates
            std::memcpy(_shadow.data(), full.pixels(), _shadow.size() * 4);
        return;
    }
    const plat::Rect r{bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1};
    if (_stats.verifyMismatches++ == 0)
        _stats.firstMismatch = r;
    std::fprintf(
        stderr,
        "ui verify: frame %d: %zu px differ in [%d,%d %dx%d], max channel delta %d\n",
        _stats.frames,
        diff,
        r.x,
        r.y,
        r.w,
        r.h,
        maxDelta
    );
    if (const char *dir = std::getenv("UI_VERIFY_DUMP");
        dir && *dir && _stats.verifyMismatches <= 3) {
        const std::string base = std::string(dir) + "/verify-" + str::number(_stats.frames);
        writePpm(base + "-presented.ppm", _shadow.data(), w, h, w);
        writePpm(base + "-expected.ppm", full.pixels(), w, h, w);
    }
    // Resynchronise, so one bug is reported once rather than every frame after.
    std::memcpy(_shadow.data(), full.pixels(), _shadow.size() * 4);
}
#endif // MSGA_UI_VERIFY

// ── App ─────────────────────────────────────────────────────────────────────

std::unique_ptr<App> App::create(std::string *error) {
    std::unique_ptr<App> a(new App);
    a->_plat = plat::App::create(error);
    if (!a->_plat)
        return nullptr;
    if (!text::init(error))
        return nullptr;
    s_instance = a.get();
    a->_plat->setEventHandler([p = a.get()](const plat::Event &e) { p->handle(e); });
    a->refreshTheme();
    return a;
}

App::~App() {
    if (s_instance == this)
        s_instance = nullptr;
    // The app's views are gone by now (the windows close before their App).
    text::shutdown();
}

void App::handle(const plat::Event &e) {
    if (e.type == plat::EventType::ThemeChanged) {
        refreshTheme(); // sent once per window; the second call finds nothing new
        return;
    }
    if (e.window) {
        if (auto *w = static_cast<Window *>(e.window->userData))
            w->handle(e);
        return;
    }
    if (onEvent)
        onEvent(e);
    else if (e.type == plat::EventType::QuitRequested)
        quit(); // like a window's unset onCloseRequested
}

void App::refreshTheme() {
    const plat::SystemSettings s = _plat->systemSettings();
    const bool dark    = _mode == ThemeMode::System ? _plat->darkMode() : _mode == ThemeMode::Dark;
    const bool changed = dark != _dark || s.textScale != _settings.textScale ||
                         s.highContrast != _settings.highContrast ||
                         s.accentColor != _settings.accentColor;
    _dark              = dark;
    _settings          = s;
    if (changed)
        for (Window *w : _windows)
            w->styleChangedAll();
}

void App::setThemeMode(ThemeMode m) {
    _mode = m;
    refreshTheme();
}

void App::setUserTextScale(float s) {
    s = std::clamp(s, 0.5f, 3.f);
    if (s == _userTextScale)
        return;
    _userTextScale = s;
    for (Window *w : _windows)
        w->styleChangedAll();
}

void App::restyle() {
    for (Window *w : _windows)
        w->styleChangedAll();
}

plat::TimerId App::addTimer(int ms, bool repeat, std::function<void()> fn) {
    return _plat->addTimer(ms, repeat, std::move(fn));
}

void App::cancelTimer(plat::TimerId id) {
    if (id)
        _plat->cancelTimer(id);
}

double App::nowMs() const {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void App::addWindow(Window *w) {
    _windows.push_back(w);
}
void App::removeWindow(Window *w) {
    _windows.erase(std::remove(_windows.begin(), _windows.end(), w), _windows.end());
}

} // namespace ui

#include "ui/scroll.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
constexpr float kWheelStep = 50;    // px per wheel notch
constexpr float kLineStep  = 40;    // arrow keys
constexpr float kGlideTau  = 35;    // ms, wheel glide time constant
constexpr float kFlingTau  = 325;   // ms, kinetic decay (the iOS-like feel)
constexpr float kStopFling = 0.02f; // px/ms
constexpr float kBarZone   = 12;    // px from the right edge that belong to the bar
constexpr float kBarMargin = 2;
constexpr float kMinThumb  = 24;
constexpr float kBarWide   = 10; // hovered/dragged thumb width
constexpr float kThinW     = 4;  // setThinThumb
constexpr float kThinMin   = 20; // … and its minimum thumb height
#ifndef __APPLE__
// px/ms below which a lift does not fling (macOS sends its own Momentum events)
constexpr float kMinFling = 0.08f;
#endif

// +1/-1 when (key, mods) is a page step (next / previous page), 0 otherwise.
int pageStep(plat::Key k, uint32_t mods) {
    using plat::Key;
    const uint32_t m    = mods & kModMask;
    const bool     page = k == Key::PageDown || k == Key::PageUp;
    if (m == 0 && page)
        return k == Key::PageDown ? 1 : -1;
#ifdef __APPLE__
    // On macOS Control (or Option) + PageUp/PageDown pages too.
    if ((m == plat::ModCtrl || m == plat::ModAlt) && page)
        return k == Key::PageDown ? 1 : -1;
    if (m == plat::ModCtrl && (k == Key::Down || k == Key::Up || k == Key::V))
        return k == Key::Up ? -1 : 1;
#endif
    return 0;
}
} // namespace

// ── ScrollArea ──────────────────────────────────────────────────────────────

ScrollArea::ScrollArea() {
    setClipChildren(true);
    setLayoutBoundary(true);
    setRole(Role::ScrollArea);
}

void ScrollArea::setThinThumb(C color) {
    _thinColor = color;
    update(barZone());
}

float ScrollArea::barMargin() const {
    return _thinColor != C::None ? 0 : kBarMargin;
}

RectF ScrollArea::barZone() const {
    return {width() - kBarZone, 0, kBarZone, height()};
}

void ScrollArea::moved(float oldOffset, float newOffset, bool blit) {
    if (oldOffset == newOffset)
        return;
    if (Window *w = window()) {
        if (blit) {
            const float s = w->scale();
            const int   d =
                int(std::lround(snapPx(newOffset) * s) - std::lround(snapPx(oldOffset) * s));
            if (d && opaqueBackground())
                w->scrollBlit(this, d);
            else if (d)
                update(); // transparent: what shows through does not move
        }
        update(barZone());
        w->refreshHover();
    }
    if (onScroll)
        onScroll();
}

RectF ScrollArea::thumbRect() const {
    const float ext = contentExtent(), h = height();
    if (ext <= h + 0.5f)
        return {};
    const bool  thin  = _thinColor != C::None;
    const float m     = barMargin();
    const float track = h - 2 * m;
    const float th    = std::min(track, std::max(thin ? kThinMin : kMinThumb, track * h / ext));
    const float t     = std::clamp(offset() / (ext - h), 0.f, 1.f);
    const float bw    = thin ? kThinW : (_barHover || _dragging) ? kBarWide : metric(M::ScrollbarW);
    return {width() - bw - kBarMargin, m + (track - th) * t, bw, th};
}

bool ScrollArea::inBarZone(PointF p) const {
    if (p.x < width() - kBarZone || !canScroll())
        return false;
    if (_thinColor == C::None)
        return true;
    const RectF r = thumbRect(); // thin: only the thumb belongs to the bar
    return p.y >= r.y && p.y < r.y + r.h;
}

uint8_t ScrollArea::cursorAt(PointF p) const {
    if (_thinColor != C::None && (_dragging || inBarZone(p)))
        return uint8_t(plat::Cursor::ResizeV);
    return View::cursorAt(p);
}

View *ScrollArea::hitTest(PointF p) {
    // The bar sits above the rows: a press there drags it, not the row.
    if (inBarZone(p))
        return this;
    return View::hitTest(p);
}

void ScrollArea::paintOver(gfx::Painter &p) {
    if (!canScroll())
        return;
    const RectF r = thumbRect();
    const C     c = _thinColor != C::None    ? _thinColor
                    : _barHover || _dragging ? C::ScrollbarHover
                                             : C::Scrollbar;
    p.fillRoundRect(r, r.w / 2, color(c));
}

void ScrollArea::stopScrolling() {
    _anim      = Anim::None;
    _glideLeft = 0;
    _velocity  = 0;
}

void ScrollArea::startGlide(float dy) {
    if (_anim != Anim::Glide)
        _glideLeft = 0;
    _glideLeft += dy;
    _anim     = Anim::Glide;
    _lastTick = app()->nowMs();
    startTicking();
}

void ScrollArea::startCustomAnimation() {
    _anim     = Anim::Custom;
    _lastTick = app()->nowMs();
    startTicking();
}

void ScrollArea::scrollBy(float dy, bool animated) {
    if (animated && !app()->reducedMotion()) {
        startGlide(dy);
        return;
    }
    stopScrolling();
    scrollPixels(dy);
}

void ScrollArea::scrollTo(float y, bool animated) {
    scrollBy(y - offset(), animated);
}

bool ScrollArea::tick(double now) {
    const float dt = float(std::clamp(now - _lastTick, 1.0, 64.0));
    _lastTick      = now;
    switch (_anim) {
    case Anim::None:
        return false;
    case Anim::Glide: {
        float step = _glideLeft * (1 - std::exp(-dt / kGlideTau));
        if (std::abs(_glideLeft - step) < 0.5f)
            step = _glideLeft;
        const float m = scrollPixels(step);
        _glideLeft -= step;
        if ((m == 0 && step != 0) || std::abs(_glideLeft) < 0.01f)
            stopScrolling();
        break;
    }
    case Anim::Fling: {
        const float d = _velocity * dt;
        _velocity *= std::exp(-dt / kFlingTau);
        const float m = scrollPixels(d);
        if (std::abs(_velocity) < kStopFling || m == 0)
            stopScrolling();
        break;
    }
    case Anim::Custom:
        if (!tickCustom(dt))
            stopScrolling();
        break;
    }
    return _anim != Anim::None;
}

bool ScrollArea::onEvent(Event &e) {
    switch (e.type) {
    case EventType::Scroll: {
        if (!canScroll() || (e.dy == 0 && e.phase == plat::ScrollPhase::None))
            return false;
        if (!e.precise) {
            scrollBy(e.dy * kWheelStep, true);
            return true;
        }
        const double now = app()->nowMs();
        switch (e.phase) {
        case plat::ScrollPhase::Begin:
            stopScrolling();
            _lastPrecise = now;
            if (e.dy != 0)
                scrollPixels(e.dy);
            return true;
        case plat::ScrollPhase::Update: {
            const float dt = float(std::max(4.0, now - _lastPrecise));
            const float v  = e.dy / dt;
            _velocity      = dt > 60 ? v : _velocity * 0.6f + v * 0.4f;
            _lastPrecise   = now;
            scrollPixels(e.dy);
            return true;
        }
        case plat::ScrollPhase::End:
#ifndef __APPLE__
            // Wayland (and Windows precision touchpads with phases) leave the
            // coast to us; macOS follows with Momentum events of its own.
            if (now - _lastPrecise < 80 && std::abs(_velocity) > kMinFling &&
                !app()->reducedMotion()) {
                _anim     = Anim::Fling;
                _lastTick = now;
                startTicking();
            }
#endif
            return true;
        default: // Momentum, None
            scrollPixels(e.dy);
            return true;
        }
    }
    case EventType::PointerDown: {
        if (e.button != plat::Button::Left || !inBarZone(e.pos))
            return false;
        stopScrolling();
        const RectF r = thumbRect();
        if (e.pos.y >= r.y && e.pos.y < r.y + r.h) {
            _dragging = true;
            _dragGrab = e.pos.y - r.y;
        } else {
            scrollBy((e.pos.y < r.y ? -1.f : 1.f) * height() * 0.9f, true);
        }
        update(barZone());
        return true;
    }
    case EventType::PointerMove: {
        if (_dragging) {
            const float m = barMargin(), track = height() - 2 * m, th = thumbRect().h;
            const float t =
                std::clamp((e.pos.y - _dragGrab - m) / std::max(1.f, track - th), 0.f, 1.f);
            setOffset(t * (contentExtent() - height()));
            update(barZone());
            return true;
        }
        const bool hov = inBarZone(e.pos);
        if (hov != _barHover) {
            _barHover = hov;
            update(barZone());
        }
        return false;
    }
    case EventType::PointerUp:
        if (_dragging) {
            _dragging = false;
            _barHover = inBarZone(e.pos);
            update(barZone());
            return true;
        }
        return false;
    case EventType::PointerLeave:
        if (_barHover && !_dragging) {
            _barHover = false;
            update(barZone());
        }
        return false;
    case EventType::KeyDown: {
        // The usual scroll-area keys: a page for PageUp/PageDown (plus the
        // macOS alternates), a line for Up/Down
        // whatever the modifiers; Home/End are not handled.
        if (const int dir = pageStep(e.key, e.mods)) {
            scrollBy(float(dir) * height() * 0.9f, true);
            return true;
        }
        if (e.key == plat::Key::Down || e.key == plat::Key::Up) {
            scrollBy(e.key == plat::Key::Down ? kLineStep : -kLineStep, true);
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

// ── ScrollView ──────────────────────────────────────────────────────────────

ScrollView::ScrollView() {
    _content = adopt(std::make_unique<View>());
}

View *ScrollView::setContent(std::unique_ptr<View> v) {
    if (_content)
        remove(_content);
    _content = adopt(std::move(v));
    _offset  = 0;
    return _content;
}

SizeF ScrollView::measureContent(float aw, float ah) {
    return _content ? _content->measure(aw, kInf) : SizeF{0, 0};
}

void ScrollView::layout() {
    if (!_content)
        return;
    const float w = width(), h = height();
    const SizeF cs  = _content->measure(w, kInf);
    _contentH       = cs.h;
    const float old = _offset;
    _offset         = std::clamp(_offset, 0.f, std::max(0.f, _contentH - h));
    _content->setFrame({0, -snapPx(_offset), w, std::max(cs.h, h)});
    if (old != _offset && onScroll)
        onScroll();
}

float ScrollView::scrollPixels(float dy) {
    const float old = _offset;
    // The offset keeps sub-pixel precision; the content sits at the snapped
    // offset, so it moves by whole physical pixels and the blit is exact.
    const float nw  = std::clamp(old + dy, 0.f, std::max(0.f, _contentH - height()));
    if (nw == old)
        return 0;
    _offset = nw;
    RectF f = _content->frame();
    f.y     = -snapPx(nw);
    _content->setFrameQuiet(f);
    moved(old, nw);
    return nw - old;
}

void ScrollView::setOffset(float y) {
    scrollPixels(y - _offset);
}

void ScrollView::ensureVisible(const View *d, float margin, bool animated) {
    if (!d || !_content || !_content->isAncestorOf(d))
        return;
    const float top = d->mapToWindow({0, 0}).y - _content->mapToWindow({0, 0}).y;
    const float bot = top + d->height();
    float       target;
    if (top - margin < _offset)
        target = top - margin;
    else if (bot + margin > _offset + height())
        target = bot + margin - height();
    else
        return;
    scrollTo(target, animated);
}

} // namespace ui

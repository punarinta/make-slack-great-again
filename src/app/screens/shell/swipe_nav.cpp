#include "screens/shell/swipe_nav.h"

#include <algorithm>
#include <cmath>

using namespace ui;

namespace shell {

// ── SwipeNav ────────────────────────────────────────────────────────────────

// Horizontal and clearly dominant fires; clearly vertical first (a normal
// message-list scroll) locks the stream out.
SwipeNav::Action SwipeNav::Stream::feed(float dx, float dy, float threshold) {
    if (state == State::Fired)
        return Action::Swallow;
    if (state == State::Ignored)
        return Action::Pass;
    x += dx;
    y += dy;
    const float ax = std::abs(x), ay = std::abs(y);
    if (ay > threshold / 3 && ay >= ax) {
        state = State::Ignored;
        return Action::Pass;
    }
    if (ax >= threshold && ax > 2 * ay) {
        state = State::Fired;
        return x > 0 ? Action::Back : Action::Forward;
    }
    return Action::Pass;
}

SwipeNav::Action SwipeNav::scroll(const plat::Event &e, int64_t ms) {
    using P             = plat::ScrollPhase;
    const bool newSwipe = e.phase == P::Begin || (e.phase == P::None && ms - _lastMs > kIdleGapMs);
    if (newSwipe)
        _wheel = {};
    _lastMs = ms;
    // Where the swipe starts decides whether it is ours: a modifier turns
    // the wheel into something else.
    if (e.mods && _wheel.state == State::Tracking && _wheel.x == 0 && _wheel.y == 0)
        _wheel.state = State::Ignored;
    Action act = Action::Pass;
    // Momentum is the flick coasting after the fingers lifted — not part of
    // the swipe the user made (but still swallowed after a jump). plat's +dx
    // scrolls towards the right, i.e. the content moves left: forward.
    if (e.phase != P::Momentum || _wheel.state == State::Fired)
        act = _wheel.feed(float(-e.dx), float(e.dy), e.precise ? kPixelThreshold : kNotchThreshold);
    if (e.phase == P::End) {
        _wheel = {};
        // Carries no delta; let the view that saw the begin see the end too.
        if (act == Action::Swallow)
            act = Action::Pass;
    }
    return act;
}

SwipeNav::Action SwipeNav::feed(const plat::Event &e, int64_t ms) {
    using T = plat::EventType;
    switch (e.type) {
    case T::Scroll:
        return scroll(e, ms);
    case T::GestureBegin:
        _gesture       = {};
        _gesture.state = e.fingers >= 3 ? State::Tracking : State::Ignored;
        return Action::Pass;
    case T::GestureUpdate: // finger motion: +x = fingers moved right = back
        return _gesture.feed(float(e.dx), float(e.dy), kPixelThreshold);
    case T::GestureEnd:
        _gesture = {State::Ignored};
        return Action::Pass;
    case T::SwipeGesture: // macOS: dx = +1 is "back" (AppKit, WebKit)
        return e.dx > 0 ? Action::Back : e.dx < 0 ? Action::Forward : Action::Pass;
    default:
        return Action::Pass;
    }
}

// ── SwipeIndicator ──────────────────────────────────────────────────────────

namespace {

// Room around the badge for its drift, so it never paints outside its zone.
constexpr float kDrift     = 10;
constexpr float kMargin    = kDrift + 2;
constexpr float kFadeIn    = 0.2f; // share of the run spent fading in
constexpr float kFadeOut   = 0.5f; // share of the run spent fading out
constexpr float kPeakAlpha = 0.85f;

float outCubic(float t) {
    const float u = 1 - t;
    return 1 - u * u * u;
}
float inQuad(float t) {
    return t * t;
}
float outBack(float t) {
    constexpr float s = 1.70158f;
    const float     u = t - 1;
    return u * u * ((s + 1) * u + s) + 1;
}

} // namespace

// Hidden while idle: a visible view above the message list would make every
// scroll blit repaint it (Window::scrollBlit's occluders).
SwipeIndicator::SwipeIndicator() {
    setHitTransparent(true);
    setVisible(false);
}

RectF SwipeIndicator::zone() const {
    const float d = kDiameter + 2 * kMargin;
    return {std::round((width() - d) / 2), std::round((height() - d) / 2), d, d};
}

void SwipeIndicator::flash(bool back) {
    _back  = back;
    _start = -1;
    _t     = 0;
    setVisible(true);
    update(zone());
    startTicking();
}

bool SwipeIndicator::tick(double nowMs) {
    if (_start < 0)
        _start = nowMs;
    _t = float(std::min(1.0, (nowMs - _start) / kDurationMs));
    update(zone());
    if (_t < 1)
        return true;
    _start = -1;
    setVisible(false);
    return false;
}

void SwipeIndicator::paint(gfx::Painter &p) {
    if (_t >= 1)
        return;
    // Opacity: ease in, hold, ease out. The badge drifts toward the direction
    // it points and grows slightly as it appears.
    const float in      = std::min(1.0f, _t / kFadeIn);
    const float out     = _t <= 1 - kFadeOut ? 1.0f : inQuad((1 - _t) / kFadeOut);
    const float opacity = outCubic(in) * out;
    if (opacity <= 0)
        return;
    const float drift = (outCubic(_t) - 0.5f) * kDrift;
    const float scale = 0.85f + 0.15f * outBack(in);

    const RectF  z = zone();
    const PointF c{z.x + z.w / 2 + (_back ? -drift : drift), z.y + z.h / 2};
    const float  r = kDiameter / 2 * scale;

    // Inverted against the content surface, so it reads on light and dark
    // themes alike.
    p.save();
    p.setOpacity(opacity);
    p.fillCircle(c, r, gfx::withAlpha(color(C::Text), kPeakAlpha));

    // Arrow: a shaft with a chevron head, pointing left for back.
    const float dir = _back ? -1 : 1;
    const float len = r * 0.52f, head = r * 0.3f;
    gfx::Path   arrow;
    arrow.moveTo(c.x - dir * len, c.y);
    arrow.lineTo(c.x + dir * len, c.y);
    arrow.moveTo(c.x + dir * (len - head), c.y - head);
    arrow.lineTo(c.x + dir * len, c.y);
    arrow.lineTo(c.x + dir * (len - head), c.y + head);
    p.strokePath(arrow, r * 0.14f, color(C::Surface));
    p.restore();
}

} // namespace shell

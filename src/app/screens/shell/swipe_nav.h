// Back/forward from a trackpad swipe, the way a browser does it:
//  - SwipeNav turns one horizontal swipe into one back/forward step. Two-finger
//    swipes are Scroll events: phased on macOS/Wayland, a bare delta stream on
//    Windows/X11 (segmented here by an idle gap). Three-or-more-finger swipes
//    are GestureBegin/Update/End on X11/Wayland and one SwipeGesture on macOS.
//    The direction follows the content: a swipe that drags the view to the
//    right goes back. Each swipe navigates at most once; the rest of its stream
//    is swallowed so the newly opened conversation doesn't inherit it.
//  - SwipeIndicator is the confirmation: a round badge with an arrow that
//    fades in over the message area, drifts a little and fades out.
#pragma once

#include "plat/plat.h"
#include "ui/ui.h"

#include <cstdint>

namespace shell {

class SwipeNav {
public:
    enum class Action : uint8_t { Pass, Swallow, Back, Forward };

    // Horizontal travel that makes a swipe: logical px for precise scrolling
    // and gestures, notches for plain wheels (tilt wheels).
    static constexpr float   kPixelThreshold = 120;
    static constexpr float   kNotchThreshold = 3;
    // Phase-less streams: a gap this long starts a new swipe.
    static constexpr int64_t kIdleGapMs      = 300;

    // Scroll, Gesture* and SwipeGesture events (others pass); ms is a
    // monotonic clock.
    Action feed(const plat::Event &e, int64_t ms);

private:
    enum class State : uint8_t { Tracking, Fired, Ignored };

    // Accumulated motion, +x = towards "back".
    struct Stream {
        State  state = State::Tracking;
        float  x = 0, y = 0;
        Action feed(float dx, float dy, float threshold);
    };

    Action scroll(const plat::Event &e, int64_t ms);

    Stream  _wheel;
    int64_t _lastMs  = 0;
    Stream  _gesture = {State::Ignored};
};

class SwipeIndicator : public ui::View {
public:
    static constexpr float kDiameter   = 64;
    static constexpr int   kDurationMs = 650;

    SwipeIndicator();
    // Plays the flash centred on this view; a flash still running restarts
    // in the new direction.
    void flash(bool back);
    bool running() const { return _t < 1; }

    bool tick(double nowMs) override;
    void paint(gfx::Painter &p) override;

private:
    ui::RectF zone() const; // the badge plus room for its drift

    bool   _back  = true;
    double _start = -1; // tick time the run started; -1 = not started
    float  _t     = 1;  // progress 0 → 1; 1 = idle (nothing drawn)
};

} // namespace shell

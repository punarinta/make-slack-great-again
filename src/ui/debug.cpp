#include "ui/debug.h"

#include "plat/testing.h"

#include <cmath>
#include <memory>

namespace ui::debug {

namespace {

struct Tour {
    Window               *w;
    RectF                 area;
    int                   events, i = 0, gesture = -1;
    plat::TimerId         timer = 0;
    std::function<void()> done;
    bool                  phasedHook = true;
};

void phased(Tour &t, double dy, plat::ScrollPhase ph) {
    plat::TestHooks *h = app()->platform().testHooks();
    if (t.phasedHook && h->injectPhasedScroll(t.w->native(), 0, dy, ph))
        return;
    t.phasedHook = false; // X11: no phased touchpad path; synthesise it
    plat::Event e;
    e.type    = plat::EventType::Scroll;
    e.window  = &t.w->native();
    e.pos     = {t.w->pointerPos().x, t.w->pointerPos().y};
    e.dy      = dy;
    e.precise = true;
    e.phase   = ph;
    t.w->handle(e);
}

// One step of a fixed 64-step cycle: 10 notches up, a touchpad swipe up
// (fling), 6 notches down, a swipe down, pauses for flings to run out.
void step(Tour &t) {
    plat::TestHooks *h = app()->platform().testHooks();
    const int        k = t.i % 64;
    // Sweep the pointer (hover toolbar, row highlight, reaction "+" pills).
    const float      x = t.area.x + 20 + std::fmod(float(t.i) * 37.f, std::max(1.f, t.area.w - 40));
    const float      y = t.area.y + 20 + std::fmod(float(t.i) * 53.f, std::max(1.f, t.area.h - 40));
    h->injectPointerMove(t.w->native(), {x, y});
    if (k < 10)
        h->injectScroll(t.w->native(), 0, -1);
    else if (k == 12)
        phased(t, 0, plat::ScrollPhase::Begin);
    else if (k > 12 && k < 20)
        phased(t, -7.3 - 0.9 * (k - 12), plat::ScrollPhase::Update);
    else if (k == 20)
        phased(t, 0, plat::ScrollPhase::End);
    else if (k >= 32 && k < 38)
        h->injectScroll(t.w->native(), 0, 1);
    else if (k == 40)
        phased(t, 0, plat::ScrollPhase::Begin);
    else if (k > 40 && k < 46)
        phased(t, 5.7 + 1.3 * (k - 40), plat::ScrollPhase::Update);
    else if (k == 46)
        phased(t, 0, plat::ScrollPhase::End);
}

} // namespace

// One-shot timers, re-armed per step: a repeating timer would have to cancel
// itself from inside its own callback.
static void schedule(const std::shared_ptr<Tour> &t) {
    t->timer = app()->addTimer(16, false, [t] {
        if (t->i >= t->events) {
            auto cb = std::move(t->done);
            if (cb)
                cb();
            return;
        }
        step(*t);
        ++t->i;
        schedule(t);
    });
}

bool scrollTour(Window &w, RectF area, int events, std::function<void()> done) {
    if (!app()->platform().testHooks())
        return false;
    auto t    = std::make_shared<Tour>();
    t->w      = &w;
    t->area   = area;
    t->events = events;
    t->done   = std::move(done);
    schedule(t);
    return true;
}

std::vector<View *> findByRole(View &root, Role role) {
    std::vector<View *> out;
    auto                walk = [&](auto &self, View *v) -> void {
        if (!v->visible())
            return;
        if (v->role() == role)
            out.push_back(v);
        for (size_t i = 0; i < v->childCount(); ++i)
            self(self, v->child(i));
    };
    walk(walk, &root);
    return out;
}

} // namespace ui::debug

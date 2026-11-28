// Debug drivers for reproducing rendering bugs deterministically (hidden
// flags of the apps; nothing here runs in normal use). Pair with
// Window::setVerify(true), which checks every frame.
#pragma once

#include "ui/view.h"

#include <functional>
#include <vector>

namespace ui::debug {

// Scrolls whatever lies under `area` (window coordinates) the way a user
// does, through plat's TestHooks (the real OS input path), one event per
// ~frame: bursts of wheel notches up and down, touchpad gestures (Begin,
// precise Updates with fractional deltas, End — which starts our fling),
// with the pointer sweeping over the area between events so hover follows
// the content. A backend without phased-scroll hooks gets the gestures as
// synthetic plat events instead. `done` runs after `events` steps. Returns
// false (and calls nothing) when the backend has no TestHooks at all.
bool scrollTour(Window &w, RectF area, int events, std::function<void()> done);

// Visible views with the given role, in tree order (Role::List finds the
// message lists without RTTI).
std::vector<View *> findByRole(View &root, Role role);

} // namespace ui::debug

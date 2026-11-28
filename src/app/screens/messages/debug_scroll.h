// The messages screens' scroll torture test (msga --debug-scroll N):
// every conversation in turn, then a thread with the panel open (both lists),
// then the same in the other theme — each scrolled through with
// ui::debug::scrollTour while Window::setVerify checks every frame. Prints a
// line per stage and a summary (verified frames, mismatches, frame times).
#pragma once

#include "app/screens/messages/context_fwd.h"

#include <functional>

namespace screens {

struct DebugScrollHooks {
    std::function<void(ConvRef)>     open;       // the shell's open(conv)
    std::function<void(ConvRef, Ts)> openThread; // … openThread
    std::function<void()>            closeThread;
};

// `done` gets the number of mismatching frames (0 = clean).
void debugScrollTour(
    Context                            &ctx,
    ui::Window                         &w,
    DebugScrollHooks                    hooks,
    int                                 eventsPerStage,
    std::function<void(int mismatches)> done
);

} // namespace screens

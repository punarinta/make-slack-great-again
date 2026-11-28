// The leak soak (`msga --demo demo --soak N`, run by scripts/soak.sh): the
// same round of navigation N times over the demo workspace — every
// conversation (and a thread in each that has one), the Threads, Saved and
// Scheduled pages, the search overlay, the quick switcher, Settings, a
// profile card, a theme switch and back — printing the memory numbers after
// each round:
//
//   msga soak: round 3/20: heap 41234 KB, rss 98765 KB, fds 37, threads 14; images …
//
// Round 1 warms the caches up; after it, a number that keeps climbing round
// after round is a leak in what a round does. It ends by quitting normally,
// so an ASan build's LeakSanitizer report still runs.
// Compiled only into demo builds (-DMSGA_DEMO=ON).
#pragma once

#include "app/model/types.h"

#include <functional>
#include <string>

namespace screens {
struct Context;
}

namespace shell {
class Shell;
}

namespace demo {

// `appStats`: the app's own numbers for the round line (caches, Store).
void runSoak(
    screens::Context            &ctx,
    shell::Shell                &sh,
    model::ConvRef               start,
    int                          rounds,
    std::function<std::string()> appStats,
    std::function<void()>        done
);

} // namespace demo

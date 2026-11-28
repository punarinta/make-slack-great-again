#include "base/observers.h"
#include "support/test.h"

#include <string>

TEST("observers: notify in order, add and remove from inside a callback") {
    base::Observers obs;
    CHECK(obs.empty());
    std::string         log;
    base::Observers::Id second = 0, late = 0;
    const auto          first = obs.add([&](const std::string &a) {
        log += "1" + a;
        if (!late) // added mid-notify: called in the same pass
            late = obs.add([&](const std::string &b) { log += "L" + b; });
        obs.remove(second); // removed mid-notify: not called
    });
    second                    = obs.add([&](const std::string &) { log += "2"; });
    CHECK_FALSE(obs.empty());
    obs.notify("x");
    CHECK_STR(log, "1xLx");
    log.clear();
    obs.notify();
    CHECK_STR(log, "1L");
    obs.remove(first);
    obs.remove(12345); // unknown: ignored
    log.clear();
    obs.notify("y");
    CHECK_STR(log, "Ly");
    // A callback that removes itself runs to its end, once.
    base::Observers::Id self = 0;
    int                 runs = 0;
    self                     = obs.add([&](const std::string &) {
        obs.remove(self);
        ++runs;
    });
    obs.notify();
    obs.notify();
    CHECK(runs == 1);
    obs.remove(late);
    CHECK(obs.empty());
}

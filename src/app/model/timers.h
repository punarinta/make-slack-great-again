// Deferred callbacks that must not outlive their owner: the one-shot timers
// a backend cancels all at once when it goes, and "post this unless the
// owner is gone by then".
#pragma once

#include "plat/plat.h"

#include <functional>
#include <memory>
#include <vector>

namespace model {

// One-shot timers owned together. Each forgets itself as it fires, so
// cancelAll() (and the destructor) only cancel the ones still pending; a
// callback may capture its owner's `this` as long as the owner holds this.
class OneShotTimers {
public:
    explicit OneShotTimers(plat::App &app) : _app(app) {}
    ~OneShotTimers() { cancelAll(); }
    OneShotTimers(const OneShotTimers &)            = delete;
    OneShotTimers &operator=(const OneShotTimers &) = delete;

    void   after(int ms, std::function<void()> fn); // ms < 0 is 0
    void   cancelAll();
    size_t pending() const { return _ids.size(); }

private:
    plat::App                 &_app;
    std::vector<plat::TimerId> _ids;
};

// fn on the loop's next turn, unless its owner is gone by then: `alive` is
// the owner's flag (set false as it goes), or a weak hold on something the
// owner owns (expired once it went).
void postWhileAlive(plat::App &app, std::shared_ptr<bool> alive, std::function<void()> fn);
void postWhileAlive(plat::App &app, std::weak_ptr<void> alive, std::function<void()> fn);

} // namespace model

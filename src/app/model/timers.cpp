#include "app/model/timers.h"

#include <algorithm>
#include <utility>

namespace model {

void OneShotTimers::after(int ms, std::function<void()> fn) {
    auto id = std::make_shared<plat::TimerId>(0);
    *id     = _app.addTimer(std::max(ms, 0), false, [this, id, fn = std::move(fn)] {
        std::erase(_ids, *id);
        fn();
    });
    _ids.push_back(*id);
}

void OneShotTimers::cancelAll() {
    for (plat::TimerId id : std::exchange(_ids, {}))
        _app.cancelTimer(id);
}

void postWhileAlive(plat::App &app, std::shared_ptr<bool> alive, std::function<void()> fn) {
    app.post([alive = std::move(alive), fn = std::move(fn)] {
        if (alive && *alive)
            fn();
    });
}

void postWhileAlive(plat::App &app, std::weak_ptr<void> alive, std::function<void()> fn) {
    app.post([alive = std::move(alive), fn = std::move(fn)] {
        if (!alive.expired())
            fn();
    });
}

} // namespace model

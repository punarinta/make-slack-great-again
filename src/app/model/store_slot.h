// StoreSlot — the Store the screens show: the open workspace's. Every
// signed-in workspace keeps its own Store, alive while it runs in the
// background; switching workspaces points the
// slot at another one instead of reloading anything.
//
// The views observe through the slot, so their observers move with it: on
// setTarget each one is re-registered on the new Store and then sees Roster
// and Users, as after Store::clear(). Data reads go to the current target
// (`slot()` or the implicit conversion); an observer registered on a Store
// directly stays with that Store.
#pragma once

#include "app/model/store.h"

#include <vector>

namespace model {

class StoreSlot {
public:
    StoreSlot(Store &initial) : _s(&initial) {} // implicit: Context{app, store, …}
    ~StoreSlot();
    StoreSlot(const StoreSlot &)            = delete;
    StoreSlot &operator=(const StoreSlot &) = delete;

    Store &operator()() const { return *_s; }
           operator Store &() const { return *_s; }

    // Same contract as Store::observe / unobserve; the ids are the slot's.
    Store::ObserverId observe(ConvRef conv, Store::Observer fn);
    void              unobserve(Store::ObserverId id);
    // The views now show `s`. No-op when it already does.
    void              setTarget(Store &s);

private:
    struct Slot {
        Store::ObserverId id, inner; // ours, and the target Store's
        ConvRef           conv;
        Store::Observer   fn;
    };
    Store            *_s;
    std::vector<Slot> _slots;
    Store::ObserverId _next = 1;
};

} // namespace model

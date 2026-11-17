// BackendSlot — the Backend the screens talk to: the open workspace's. None
// signed in (NullBackend), the demo, or a signed-in workspace; signing in,
// out or switching workspaces points the slot at another one instead of
// rebuilding the screens (as StoreSlot does for the Store). Calls go to the
// current target: `slot().send(…)`, or the implicit conversion.
#pragma once

#include "app/model/backend.h"

#include <functional>
#include <string>

namespace model {

class BackendSlot {
public:
    BackendSlot(Backend &initial) : _b(&initial) {} // implicit: Context{app, store, backend, …}
    BackendSlot(const BackendSlot &)            = delete;
    BackendSlot &operator=(const BackendSlot &) = delete;

    Backend &operator()() const { return *_b; }
             operator Backend &() const { return *_b; }
    void     setTarget(Backend &b) { _b = &b; }

    // Something a screen wants the user to hear about (a failure no call
    // waits for): the shell's error banner, whichever backend is current.
    // Set by the shell.
    std::function<void(const std::string &message)> onError;

private:
    Backend *_b;
};

} // namespace model

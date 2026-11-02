// Back/forward conversation history, with text
// editor undo/redo semantics: going back keeps the forward stack, opening a
// conversation or an overview page by a direct action (sidebar, switcher,
// notification, menu) clears everything in front of the current one.
// Conversations and overview pages (Activity, Threads...): threads are not
// entries. A location is in a (possibly background) workspace, so the
// history crosses workspaces.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace shell {

// A location: the workspace's key and the conversation's id (ids,
// not ConvRefs: a workspace's Store is rebuilt when it restarts), or an
// overview page there (a Sidebar::Nav, conv empty).
struct NavLocation {
    std::string key, conv;
    uint8_t     page = 0;
    bool        valid() const { return !conv.empty() || page != 0; }
    bool        operator==(const NavLocation &) const = default;
};

class NavHistory {
public:
    static constexpr size_t kMaxDepth = 32;
    // Entries go stale (a workspace signed out, a channel left); goBack and
    // goForward drop the ones the validator rejects.
    using Validator                   = std::function<bool(const NavLocation &)>;

    // A direct open: the previous location goes on the back stack, the
    // forward stack is cleared.
    void               recordOpen(const NavLocation &loc);
    // Syncs the current location without touching the stacks (while a
    // back/forward jump is being applied).
    void               setCurrent(const NavLocation &loc) { _current = loc; }
    const NavLocation &current() const { return _current; }
    // The location to show, or an invalid one when there is none.
    NavLocation        goBack(const Validator &valid) { return go(_back, _forward, valid); }
    NavLocation        goForward(const Validator &valid) { return go(_forward, _back, valid); }
    // Drops every trace of a signed-out workspace.
    void               purge(const std::string &key);

private:
    static void push(std::vector<NavLocation> &d, const NavLocation &loc);
    NavLocation
    go(std::vector<NavLocation> &from, std::vector<NavLocation> &to, const Validator &valid);

    NavLocation              _current;
    std::vector<NavLocation> _back, _forward; // newest last
};

} // namespace shell

// A list of callbacks with ids to remove them by (UI thread). A callback may
// add or remove callbacks, itself included, while the list is notifying.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace base {

class Observers {
public:
    using Id = uint32_t;
    using Fn = std::function<void(const std::string &)>;

    Id   add(Fn fn);
    void remove(Id id); // an unknown id is ignored
    // Every callback in the order added, with `arg`: one added meanwhile is
    // called too, one removed meanwhile is not.
    void notify(const std::string &arg = {});
    bool empty() const;

private:
    struct Slot {
        Id id;
        Fn fn; // empty: removed during a notify, dropped after it
    };
    std::vector<Slot> _slots;
    Id                _next  = 1;
    int               _depth = 0; // notify() nesting
};

} // namespace base

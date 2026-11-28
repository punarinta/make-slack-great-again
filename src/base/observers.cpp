#include "base/observers.h"

#include <algorithm>

namespace base {

Observers::Id Observers::add(Fn fn) {
    const Id id = _next++;
    _slots.push_back({id, std::move(fn)});
    return id;
}

void Observers::remove(Id id) {
    for (auto it = _slots.begin(); it != _slots.end(); ++it)
        if (it->id == id) {
            if (_depth > 0)
                it->fn = nullptr; // notify() is walking the list
            else
                _slots.erase(it);
            return;
        }
}

void Observers::notify(const std::string &arg) {
    ++_depth;
    for (size_t i = 0; i < _slots.size(); ++i) // add() inside a callback appends
        if (_slots[i].fn) {
            const Fn fn = _slots[i].fn; // it may remove itself
            fn(arg);
        }
    if (--_depth == 0)
        std::erase_if(_slots, [](const Slot &s) { return !s.fn; });
}

bool Observers::empty() const {
    return std::none_of(_slots.begin(), _slots.end(), [](const Slot &s) { return bool(s.fn); });
}

} // namespace base

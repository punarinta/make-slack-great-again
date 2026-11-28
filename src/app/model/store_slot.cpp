#include "app/model/store_slot.h"

#include <algorithm>

namespace model {

StoreSlot::~StoreSlot() {
    for (const Slot &s : _slots)
        _s->unobserve(s.inner);
}

Store::ObserverId StoreSlot::observe(ConvRef conv, Store::Observer fn) {
    const Store::ObserverId id = _next++;
    _slots.push_back({id, _s->observe(conv, fn), conv, std::move(fn)});
    return id;
}

void StoreSlot::unobserve(Store::ObserverId id) {
    for (auto it = _slots.begin(); it != _slots.end(); ++it)
        if (it->id == id) {
            _s->unobserve(it->inner);
            _slots.erase(it);
            return;
        }
}

void StoreSlot::setTarget(Store &s) {
    if (&s == _s)
        return;
    for (Slot &x : _slots) {
        _s->unobserve(x.inner);
        x.inner = s.observe(x.conv, x.fn);
    }
    _s = &s;
    // What a clear() + reload would have told them. A callback may observe
    // or unobserve: walk the ids, call a copy.
    std::vector<Store::ObserverId> ids;
    for (const Slot &x : _slots)
        ids.push_back(x.id);
    for (const ChangeKind kind : {ChangeKind::Roster, ChangeKind::Users})
        for (const Store::ObserverId id : ids) {
            const auto it = std::find_if(_slots.begin(), _slots.end(), [id](const Slot &x) {
                return x.id == id;
            });
            if (it == _slots.end())
                continue;
            const Store::Observer fn = it->fn;
            fn({kind});
        }
}

} // namespace model

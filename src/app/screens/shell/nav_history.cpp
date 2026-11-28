#include "screens/shell/nav_history.h"

namespace shell {

void NavHistory::recordOpen(const NavLocation &loc) {
    if (loc == _current)
        return;
    if (_current.valid())
        push(_back, _current);
    _forward.clear();
    _current = loc;
}

void NavHistory::purge(const std::string &key) {
    const auto stale = [&](const NavLocation &l) { return l.key == key; };
    std::erase_if(_back, stale);
    std::erase_if(_forward, stale);
    if (_current.key == key)
        _current = {};
}

void NavHistory::push(std::vector<NavLocation> &d, const NavLocation &loc) {
    if (!d.empty() && d.back() == loc)
        return;
    d.push_back(loc);
    if (d.size() > kMaxDepth)
        d.erase(d.begin());
}

NavLocation NavHistory::go(
    std::vector<NavLocation> &from, std::vector<NavLocation> &to, const Validator &valid
) {
    while (!from.empty()) {
        const NavLocation loc = from.back();
        from.pop_back();
        if (!valid(loc) || loc == _current)
            continue; // stale: drop it and keep looking
        if (_current.valid())
            push(to, _current);
        _current = loc;
        return loc;
    }
    return {};
}

} // namespace shell

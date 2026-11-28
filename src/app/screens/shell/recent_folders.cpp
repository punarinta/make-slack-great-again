#include "screens/shell/recent_folders.h"

#include "base/time.h"

#include <algorithm>

namespace shell::recent_folders {

// Forward slashes, no "." or empty segments, ".." resolved: the stored form.
std::string normalized(std::string_view path) {
    if (path.empty())
        return {};
    std::string p(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    const bool               absolute = p.front() == '/';
    std::vector<std::string> parts;
    size_t                   i = 0;
    while (i <= p.size()) {
        const size_t j = std::min(p.find('/', i), p.size());
        std::string  seg(p, i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".")
            continue;
        if (seg == ".." && !parts.empty() && parts.back() != "..") {
            parts.pop_back();
            continue;
        }
        if (seg == ".." && absolute)
            continue; // above the root is the root
        parts.push_back(std::move(seg));
    }
    std::string out = absolute ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k)
        out += (k ? "/" : "") + parts[k];
    return out.empty() ? std::string(".") : out;
}

std::vector<Entry> bumped(std::vector<Entry> list, std::string_view path, int64_t now, int cap) {
    const std::string p = normalized(path);
    if (p.empty())
        return list;
    list.erase(
        std::remove_if(
            list.begin(), list.end(), [&](const Entry &e) { return normalized(e.path) == p; }
        ),
        list.end()
    );
    list.insert(list.begin(), Entry{p, now});
    if (cap >= 0 && int(list.size()) > cap)
        list.resize(size_t(cap));
    return list;
}

std::vector<Choice> rank(
    const std::vector<Entry>                       &remembered,
    const std::vector<SessionFolder>               &sessions,
    const std::function<bool(const std::string &)> &exists,
    int                                             cap
) {
    std::vector<Choice> out;
    // Index of `raw`'s row in out (added on first sight), -1 for no path.
    const auto          slot = [&](const std::string &raw) {
        const std::string p = normalized(raw);
        if (p.empty())
            return -1;
        for (size_t i = 0; i < out.size(); ++i)
            if (out[i].path == p)
                return int(i);
        out.push_back(Choice{p});
        return int(out.size()) - 1;
    };
    for (const Entry &e : remembered)
        if (const int i = slot(e.path); i >= 0)
            out[size_t(i)].lastUsed = std::max(out[size_t(i)].lastUsed, e.usedAt);
    for (const SessionFolder &f : sessions)
        if (const int i = slot(f.path); i >= 0) {
            out[size_t(i)].lastUsed = std::max(out[size_t(i)].lastUsed, f.activity);
            ++out[size_t(i)].sessions;
        }
    if (exists)
        out.erase(
            std::remove_if(
                out.begin(), out.end(), [&](const Choice &c) { return !exists(c.path); }
            ),
            out.end()
        );
    // Stable: equal times keep the remembered (MRU) order, then first-seen.
    std::stable_sort(out.begin(), out.end(), [](const Choice &a, const Choice &b) {
        return a.lastUsed > b.lastUsed;
    });
    if (cap >= 0 && int(out.size()) > cap)
        out.resize(size_t(cap));
    return out;
}

std::string teammateFolder(const Settings &s, const std::string &role, const std::string &home) {
    for (const auto &[id, dir] : s.claudeTeammateDirs)
        if (id == role && !dir.empty())
            return dir;
    return s.claudeLastDir.empty() ? home : s.claudeLastDir;
}

std::string existingFolder(
    std::string_view                                path,
    const std::string                              &home,
    const std::function<bool(const std::string &)> &exists
) {
    const std::string h = normalized(home);
    std::string       p = normalized(path);
    while (p != h) {
        if (exists(p))
            return p;
        const size_t cut = p.rfind('/');
        if (cut == std::string::npos || cut == 0 || p[cut - 1] == ':')
            break; // up next is a root ("/", "C:/"): no place to start in
        p.resize(cut);
    }
    return home;
}

void forgetFolder(Settings &s, const std::string &missing, const std::string &instead) {
    const std::string gone = normalized(missing);
    const auto        is   = [&](const std::string &p) { return normalized(p) == gone; };
    s.claudeRecentDirs.erase(
        std::remove_if(
            s.claudeRecentDirs.begin(),
            s.claudeRecentDirs.end(),
            [&](const Entry &e) { return is(e.path); }
        ),
        s.claudeRecentDirs.end()
    );
    for (auto &[id, d] : s.claudeTeammateDirs)
        if (is(d))
            d = instead;
    if (is(s.claudeLastDir))
        s.claudeLastDir = instead;
}

void pickTeammateFolder(Settings &s, const std::string &role, const std::string &dir) {
    bool found = false;
    for (auto &[id, d] : s.claudeTeammateDirs)
        if (id == role) {
            d     = dir;
            found = true;
        }
    if (!found)
        s.claudeTeammateDirs.emplace_back(role, dir);
    bump(s, dir);
}

void bump(Settings &s, const std::string &dir) {
    s.claudeRecentDirs = bumped(std::move(s.claudeRecentDirs), dir, base::nowSecs());
}

} // namespace shell::recent_folders

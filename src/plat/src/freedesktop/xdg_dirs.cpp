// XDG base and user directories (standardDir). Read from the environment and
// $XDG_CONFIG_HOME/user-dirs.dirs on every call: both are cheap, and a
// long-running app then sees the user's edits without a restart.
#include "freedesktop/files.h"
#include "freedesktop/instance.h"

#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

namespace plat::linux_instance {

namespace {

// Absolute, without trailing slashes ("/" stays "/"); "" for anything relative.
std::string clean(std::string p) {
    if (p.empty() || p[0] != '/')
        return {};
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return p;
}

using linux_files::isDir;

std::string home() {
    if (const char *h = std::getenv("HOME"); h && *h)
        if (std::string p = clean(h); !p.empty())
            return p;
    // No usable $HOME (daemons, some sandboxes): the passwd entry.
    if (const passwd *pw = getpwuid(getuid()); pw && pw->pw_dir)
        return clean(pw->pw_dir);
    return {};
}

// The base-dir spec: a relative value in the variable is invalid and ignored.
std::string xdgHome(const char *var, const char *fallbackUnderHome) {
    if (const char *v = std::getenv(var); v && *v)
        if (std::string p = clean(v); !p.empty())
            return p;
    const std::string h = home();
    return h.empty() ? std::string() : h + "/" + fallbackUnderHome;
}

// One entry of user-dirs.dirs (xdg-user-dirs format): lines of
// XDG_<NAME>_DIR="$HOME/sub" or XDG_<NAME>_DIR="/abs", shell-quoted with
// backslash escapes. nullopt when the file or the entry is missing.
std::optional<std::string> userDir(const char *name) {
    const std::string cfg = xdgHome("XDG_CONFIG_HOME", ".config");
    if (cfg.empty())
        return std::nullopt;
    const std::optional<std::string> file = linux_files::readFile(cfg + "/user-dirs.dirs");
    if (!file)
        return std::nullopt;
    const std::string &text = *file;
    const std::string  key  = std::string("XDG_") + name + "_DIR=";
    for (size_t at = 0, eol; at < text.size(); at = eol + 1) {
        eol = text.find('\n', at);
        if (eol == std::string::npos)
            eol = text.size();
        const std::string line = text.substr(at, eol - at);
        size_t            i    = line.find_first_not_of(" \t");
        if (i == std::string::npos || line.compare(i, key.size(), key) != 0)
            continue;
        i += key.size();
        if (i >= line.size() || line[i] != '"')
            continue;
        std::string v;
        for (++i; i < line.size() && line[i] != '"'; ++i) {
            if (line[i] == '\\' && i + 1 < line.size())
                ++i;
            v += line[i];
        }
        if (v.compare(0, 5, "$HOME") == 0 && (v.size() == 5 || v[5] == '/'))
            v = home() + v.substr(5);
        return clean(v);
    }
    return std::nullopt;
}

// A user dir: the configured one, "" if the user disabled it (xdg-user-dirs
// points a disabled dir at $HOME itself), else the English default only if
// it exists — never a made-up path the app would then create.
std::string special(const char *name, const char *english) {
    const std::string h = home();
    if (auto d = userDir(name))
        return (*d == h) ? std::string() : *d;
    const std::string def = h.empty() ? std::string() : h + "/" + english;
    return isDir(def) ? def : std::string();
}

} // namespace

std::string standardDir(StandardDir d) {
    switch (d) {
    case StandardDir::Config:
        return xdgHome("XDG_CONFIG_HOME", ".config");
    case StandardDir::Data:
        return xdgHome("XDG_DATA_HOME", ".local/share");
    case StandardDir::Cache:
        return xdgHome("XDG_CACHE_HOME", ".cache");
    case StandardDir::State:
        return xdgHome("XDG_STATE_HOME", ".local/state");
    case StandardDir::Temp: {
        const char *t = std::getenv("TMPDIR");
        std::string p = t ? clean(t) : std::string();
        return isDir(p) ? p : std::string("/tmp");
    }
    case StandardDir::Home:
        return home();
    case StandardDir::Desktop:
        return special("DESKTOP", "Desktop");
    case StandardDir::Documents:
        return special("DOCUMENTS", "Documents");
    case StandardDir::Downloads:
        return special("DOWNLOAD", "Downloads");
    case StandardDir::Pictures:
        return special("PICTURES", "Pictures");
    }
    return {};
}

} // namespace plat::linux_instance

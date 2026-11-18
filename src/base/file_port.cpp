// The OS half of base/file.h over the port contract (MSGA_PORT builds); the
// path-string half stays in file.cpp, and readRange / writeAtomic / isDir /
// makeDirs come from prim (prim/port_file.cpp).
#include "base/file.h"

#include "port/port.h"

namespace file {

namespace {
bool statPath(std::string_view path, bool follow, port_stat_info *st) {
    *st      = {};
    st->size = sizeof *st;
    return !path.empty() && port_stat(std::string(path).c_str(), follow ? 1 : 0, st) == PORT_OK;
}
} // namespace

bool overwrite(std::string_view path, std::string_view data) {
    port_file *f = nullptr;
    if (port_file_open(std::string(path).c_str(), PORT_O_WRITE | PORT_O_TRUNC, 0, &f) != PORT_OK)
        return false;
    size_t put = 0;
    while (put < data.size()) {
        const int64_t w = port_file_write(f, data.data() + put, data.size() - put);
        if (w <= 0)
            break;
        put += size_t(w);
    }
    return port_file_close(f) == PORT_OK && put == data.size();
}

bool exists(std::string_view path) {
    port_stat_info st;
    return statPath(path, true, &st);
}

int64_t size(std::string_view path) {
    port_stat_info st;
    return statPath(path, true, &st) ? st.bytes : -1;
}

bool stat(std::string_view path, Stat *out) {
    port_stat_info st;
    if (!statPath(path, true, &st))
        return false;
    out->isDir       = st.kind == PORT_KIND_DIR;
    out->size        = out->isDir ? 0 : st.bytes;
    out->mtimeMicros = st.mtime_us;
    out->birthMicros = st.btime_us ? st.btime_us : st.mtime_us;
    return true;
}

bool listDir(std::string_view dir, std::vector<DirEntry> *out) {
    out->clear();
    port_dir *d = nullptr;
    if (port_dir_open(std::string(dir).c_str(), &d) != PORT_OK)
        return false;
    port_dir_entry e{};
    e.size = sizeof e;
    while (port_dir_next(d, &e) == 1) {
        DirEntry de;
        de.name   = e.name;
        de.hidden = e.hidden != 0;
        de.isDir  = e.kind == PORT_KIND_DIR;
        de.size   = de.isDir ? 0 : e.bytes;
        de.mtime  = e.mtime_us / 1000000;
        out->push_back(std::move(de));
        e      = {};
        e.size = sizeof e;
    }
    port_dir_close(d);
    return true;
}

bool remove(std::string_view path) {
    return port_remove(std::string(path).c_str()) == PORT_OK;
}

bool removeTree(std::string_view path) {
    port_stat_info st;
    if (!statPath(path, false, &st))
        return true; // nothing there
    if (st.kind == PORT_KIND_DIR) {
        // Names first, then the recursion: the listing is not held open
        // while its entries go.
        std::vector<DirEntry> entries;
        listDir(path, &entries);
        for (const DirEntry &e : entries)
            removeTree(join(path, e.name));
    }
    return port_remove(std::string(path).c_str()) == PORT_OK;
}

int64_t treeBytes(std::string_view path) {
    port_stat_info st;
    if (!statPath(path, false, &st))
        return 0;
    if (st.kind != PORT_KIND_DIR)
        return st.kind == PORT_KIND_FILE ? st.bytes : 0; // a link counts as nothing
    std::vector<DirEntry> entries;
    listDir(path, &entries);
    int64_t n = 0;
    for (const DirEntry &e : entries)
        n += treeBytes(join(path, e.name));
    return n;
}

bool touch(std::string_view path) {
    return port_touch(std::string(path).c_str()) == PORT_OK;
}

bool isAbsolute(std::string_view p) {
    return !p.empty() && p[0] == '/';
}

bool isExecutable(std::string_view path) {
    port_stat_info st;
    return statPath(path, true, &st) && st.kind == PORT_KIND_FILE && (st.mode & 0111) != 0;
}

std::string realPath(std::string_view path) {
    port_stat_info st;
    if (!statPath(path, true, &st))
        return {};
    // No links to read: the absolute path with "." and ".." folded.
    const std::string        abs = absolute(path);
    std::vector<std::string> parts;
    size_t                   at = 0;
    while (at <= abs.size()) {
        size_t slash = abs.find('/', at);
        if (slash == std::string::npos)
            slash = abs.size();
        const std::string_view part(abs.data() + at, slash - at);
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.emplace_back(part);
        }
        at = slash + 1;
    }
    std::string out;
    for (const std::string &p : parts)
        out += "/" + p;
    return out.empty() ? std::string("/") : out;
}

bool makeDir(std::string_view path, int mode) {
    return port_mkdir(std::string(path).c_str(), uint32_t(mode)) == PORT_OK;
}

bool isRealDir(std::string_view path) {
    port_stat_info st;
    return statPath(path, false, &st) && st.kind == PORT_KIND_DIR;
}

std::string absolute(std::string_view path) {
    if (isAbsolute(path))
        return std::string(path);
    char      buf[4096];
    const int n = port_path_get(PORT_PATH_CWD, buf, sizeof buf);
    if (n <= 0 || size_t(n) >= sizeof buf)
        return std::string(path);
    return join(buf, path);
}

} // namespace file

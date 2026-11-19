// prim/file.h over the port contract (MSGA_PORT builds): the same bodies as
// posix_file.cpp, with port_file_* / port_stat / port_mkdir / port_rename
// in place of the POSIX calls.
#include "prim/file.h"

#include "port/port.h"

#include <string>

namespace prim::file {

namespace {
// Closes on every path out.
struct File {
    port_file *f = nullptr;
    ~File() {
        if (f)
            port_file_close(f);
    }
    int close() {
        const int rc = port_file_close(f);
        f            = nullptr;
        return rc;
    }
};
} // namespace

bool readRange(std::string_view path, int64_t offset, size_t maxBytes, std::string *out) {
    out->clear();
    const std::string p(path);
    File              file;
    port_stat_info    st{};
    st.size = sizeof st;
    if (offset < 0 || port_stat(p.c_str(), 1, &st) != PORT_OK || st.kind != PORT_KIND_FILE ||
        port_file_open(p.c_str(), PORT_O_READ, 0, &file.f) != PORT_OK)
        return false;
    const int64_t size = port_file_size(file.f);
    if (size > offset) {
        const auto left = uint64_t(size) - uint64_t(offset);
        out->resize(left < maxBytes ? size_t(left) : maxBytes);
        size_t got = 0;
        while (got < out->size()) {
            const int64_t r = port_file_pread(
                file.f, out->data() + got, out->size() - got, offset + int64_t(got)
            );
            if (r <= 0)
                break;
            got += size_t(r);
        }
        out->resize(got); // the file may have shrunk under us
    }
    return true;
}

bool writeAtomic(std::string_view path, std::string_view data, int mode, bool durable) {
    const std::string target(path);
    // The parent ("/a" has the root, which always exists).
    if (const size_t slash = path.rfind('/');
        slash != std::string_view::npos && slash > 0 && !makeDirs(path.substr(0, slash)))
        return false;
    // Same directory as the target so the rename stays within one file
    // system; a random suffix keeps two writers (threads or processes) apart
    // where a port has no process id to offer.
    uint8_t     rnd[6] = {};
    std::string tmp    = target + ".tmp";
    port_random(rnd, sizeof rnd);
    for (const uint8_t b : rnd) {
        tmp += "0123456789abcdef"[b >> 4];
        tmp += "0123456789abcdef"[b & 15];
    }
    File file;
    if (port_file_open(
            tmp.c_str(), PORT_O_WRITE | PORT_O_CREATE | PORT_O_TRUNC, uint32_t(mode), &file.f
        ) != PORT_OK)
        return false;
    size_t put = 0;
    bool   ok  = true;
    while (put < data.size()) {
        const int64_t w = port_file_write(file.f, data.data() + put, data.size() - put);
        if (w <= 0) {
            ok = false;
            break;
        }
        put += size_t(w);
    }
    // Synced before the rename: otherwise a crash can leave the renamed file
    // empty where the file system delays allocation.
    ok = ok && (!durable || port_file_sync(file.f) == PORT_OK);
    ok = file.close() == PORT_OK && ok;
    if (ok)
        ok = port_rename(tmp.c_str(), target.c_str()) == PORT_OK;
    if (!ok)
        port_remove(tmp.c_str());
    return ok;
}

bool isDir(std::string_view path) {
    port_stat_info st{};
    st.size = sizeof st;
    return port_stat(std::string(path).c_str(), 1, &st) == PORT_OK && st.kind == PORT_KIND_DIR;
}

bool makeDirs(std::string_view path) {
    if (path.empty())
        return false;
    if (isDir(path))
        return true;
    // Create each missing ancestor, shallowest first.
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string p(path.substr(0, i));
            if (p.back() != '/' && !isDir(p)) {
                const int rc = port_mkdir(p.c_str(), 0755);
                if (rc != PORT_OK && rc != PORT_E_EXIST)
                    return false;
            }
        }
    }
    return isDir(path);
}

std::string executablePath() {
    // The port returns the full length, so a too-small buffer is retried
    // once with room enough; a port without one (PEOS) answers an error.
    std::string out(256, '\0');
    int         n = port_path_get(PORT_PATH_EXECUTABLE, out.data(), out.size());
    if (n >= 0 && size_t(n) >= out.size()) {
        out.resize(size_t(n) + 1);
        n = port_path_get(PORT_PATH_EXECUTABLE, out.data(), out.size());
    }
    out.resize(n > 0 ? size_t(n) : 0);
    return out;
}

} // namespace prim::file

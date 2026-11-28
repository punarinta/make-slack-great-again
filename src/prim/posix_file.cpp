#include "prim/file.h"

#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <sys/stat.h>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

namespace prim::file {

bool readRange(std::string_view path, int64_t offset, size_t maxBytes, std::string *out) {
    out->clear();
    const std::string p(path);
    const int         fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    const bool  ok = offset >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    if (ok && offset < int64_t(st.st_size)) {
        const auto left = uint64_t(st.st_size) - uint64_t(offset);
        out->resize(left < maxBytes ? size_t(left) : maxBytes);
        size_t got = 0;
        while (got < out->size()) {
            const ssize_t r =
                ::pread(fd, out->data() + got, out->size() - got, off_t(offset + got));
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            got += size_t(r);
        }
        out->resize(got); // the file may have shrunk under us
    }
    ::close(fd);
    return ok;
}

bool writeAtomic(std::string_view path, std::string_view data, int mode, bool durable) {
    const std::string target(path);
    // The parent ("/a" has the root, which always exists).
    if (const size_t slash = path.rfind('/');
        slash != std::string_view::npos && slash > 0 && !makeDirs(path.substr(0, slash)))
        return false;
    // Same directory as the target so rename() stays within one filesystem;
    // the pid keeps two processes from sharing a temp name.
    char        pid[16];
    const auto  end = std::to_chars(pid, pid + sizeof pid, int(getpid())).ptr;
    std::string tmp = target + ".tmp";
    tmp.append(pid, end);
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return false;
    size_t put = 0;
    bool   ok  = true;
    while (put < data.size()) {
        const ssize_t w = ::write(fd, data.data() + put, data.size() - put);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            ok = false;
            break;
        }
        put += size_t(w);
    }
    // fsync before rename: otherwise a crash can leave the renamed file empty
    // on ext4/xfs with delayed allocation.
    ok = ok && (!durable || ::fsync(fd) == 0);
    ok = (::close(fd) == 0) && ok;
    if (ok)
        ok = ::rename(tmp.c_str(), target.c_str()) == 0;
    if (!ok)
        ::unlink(tmp.c_str());
    return ok;
}

bool isDir(std::string_view path) {
    struct stat st;
    return ::stat(std::string(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
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
            if (p.back() != '/' && !isDir(p) && ::mkdir(p.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    }
    return isDir(path);
}

namespace {

#if defined(__APPLE__) || defined(__OpenBSD__)
std::string realPath(const char *path) {
    char real[PATH_MAX];
    return ::realpath(path, real) ? std::string(real) : std::string();
}
#endif

#if defined(__OpenBSD__)
// argv[0] as the process was started with (unless it has rewritten its own
// argv since, which msga doesn't).
std::string argv0() {
    int    mib[4] = {CTL_KERN, KERN_PROC_ARGS, int(getpid()), KERN_PROC_ARGV};
    size_t size   = 0;
    if (::sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0 || size == 0)
        return {};
    std::string buf(size, '\0');
    if (::sysctl(mib, 4, buf.data(), &size, nullptr, 0) != 0)
        return {};
    char *const *argv = reinterpret_cast<char *const *>(buf.data());
    return argv[0] ? std::string(argv[0]) : std::string();
}

// What a shell would run for a bare `name`.
std::string searchPath(const std::string &name) {
    const char      *env  = std::getenv("PATH");
    std::string_view path = env && *env ? env : "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin";
    while (!path.empty()) {
        const size_t           colon = path.find(':');
        const std::string_view dir   = path.substr(0, colon);
        path = colon == std::string_view::npos ? std::string_view() : path.substr(colon + 1);
        std::string candidate(dir.empty() ? std::string_view(".") : dir); // "" is the cwd
        candidate += '/';
        candidate += name;
        struct stat st;
        if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            ::access(candidate.c_str(), X_OK) == 0)
            return realPath(candidate.c_str());
    }
    return {};
}
#endif

#if defined(__APPLE__) || defined(__OpenBSD__)
// What the system says the executable is: macOS gives the path it was
// started by and OpenBSD only argv[0], either of which may be relative to
// the directory the process started in.
std::string startPath() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string out(size, '\0');
    if (_NSGetExecutablePath(out.data(), &size) != 0)
        return {};
    out.resize(std::strlen(out.c_str()));
    const std::string real = realPath(out.c_str());
    return real.empty() ? out : real;
#else
    const std::string arg0 = argv0();
    if (arg0.empty())
        return {};
    return arg0.find('/') != std::string::npos ? realPath(arg0.c_str()) : searchPath(arg0);
#endif
}

const std::string &startupPath() {
    static const std::string path = startPath();
    return path;
}

// Resolved before main, while the working directory and $PATH are still the
// ones the process started with: one sysctl or dyld call and a realpath.
[[maybe_unused]] const bool g_startupPathTaken = !startupPath().empty();
#endif

} // namespace

std::string executablePath() {
#if defined(__APPLE__) || defined(__OpenBSD__)
    return startupPath();
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    int    mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    char   buf[PATH_MAX];
    size_t size = sizeof buf;
    if (::sysctl(mib, 4, buf, &size, nullptr, 0) != 0 || size <= 1)
        return {};
    return std::string(buf, size - 1); // size counts the NUL
#else
    char          buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return {};
    std::string                path(buf, size_t(n));
    // Once the binary is replaced on disk (an applied update renames the new
    // one over it), the kernel names the running image "<path> (deleted)":
    // the path itself is where the new binary now is.
    constexpr std::string_view deleted = " (deleted)";
    if (path.size() > deleted.size() &&
        path.compare(path.size() - deleted.size(), deleted.size(), deleted) == 0 &&
        ::access(path.c_str(), F_OK) != 0)
        path.resize(path.size() - deleted.size());
    return path;
#endif
}

} // namespace prim::file

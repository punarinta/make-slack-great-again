#include "base/file.h"

#include "base/str.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include "base/winstr.h"

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(MSGA_PORT) // a port build: file_port.cpp
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace file {

bool readAll(std::string_view path, std::string *out) {
    return readRange(path, 0, SIZE_MAX, out);
}

#ifdef _WIN32
namespace {
using base::widePath;

bool dotName(const wchar_t *n) {
    return n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]));
}

// FILETIME (100 ns ticks since 1601) → unix microseconds.
int64_t unixMicros(const FILETIME &t) {
    const uint64_t ft = (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    return int64_t(ft / 10) - 11644473600LL * 1000000;
}

bool removeTreeW(const std::wstring &path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return true;
    if (a & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path.c_str(), a & ~DWORD(FILE_ATTRIBUTE_READONLY));
    if (!(a & FILE_ATTRIBUTE_DIRECTORY))
        return DeleteFileW(path.c_str());
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) { // a junction or link: only itself goes
        WIN32_FIND_DATAW d;
        HANDLE           h = FindFirstFileW((path + L"\\*").c_str(), &d);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!dotName(d.cFileName))
                    removeTreeW(path + L"\\" + d.cFileName);
            } while (FindNextFileW(h, &d));
            FindClose(h);
        }
    }
    return RemoveDirectoryW(path.c_str());
}

int64_t treeBytesW(const std::wstring &dir) {
    int64_t          n = 0;
    WIN32_FIND_DATAW d;
    HANDLE           h = FindFirstFileExW(
        (dir + L"\\*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, 0
    );
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (dotName(d.cFileName) || (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue; // links are not followed, nor counted
        n += (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                 ? treeBytesW(dir + L"\\" + d.cFileName)
                 : int64_t((uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow);
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return n;
}
} // namespace

bool readRange(std::string_view path, int64_t offset, size_t maxBytes, std::string *out) {
    out->clear();
    HANDLE h = CreateFileW(
        widePath(path).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz, at;
    at.QuadPart = offset;
    bool ok = offset >= 0 && GetFileSizeEx(h, &sz) && SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    if (ok && offset < sz.QuadPart) {
        const int64_t left = sz.QuadPart - offset;
        const int64_t want = maxBytes < uint64_t(left) ? int64_t(maxBytes) : left;
        ok                 = want < (1ll << 31); // one ReadFile, and a sane amount to hold
        if (ok)
            out->resize(size_t(want));
        DWORD got = 0;
        ok        = ok && ReadFile(h, out->data(), DWORD(out->size()), &got, nullptr);
        out->resize(ok ? size_t(got) : 0);
    }
    CloseHandle(h);
    return ok;
}

bool overwrite(std::string_view path, std::string_view data) {
    HANDLE h = CreateFileW(
        widePath(path).c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        TRUNCATE_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD      put = 0;
    const bool ok = data.empty() || (WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr) &&
                                     put == data.size());
    CloseHandle(h);
    return ok;
}

bool writeAtomic(std::string_view path, std::string_view data, int mode, bool durable) {
    (void)mode; // NTFS: the user's profile ACLs apply
    makeDirs(dirName(path));
    const std::wstring target = widePath(path);
    const std::wstring tmp    = target + L".tmp~";
    HANDLE             h      = CreateFileW(
        tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    bool  ok = data.empty() ||
               (WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr) && put == data.size());
    ok       = ok && (!durable || FlushFileBuffers(h));
    CloseHandle(h);
    if (ok)
        ok = MoveFileExW(
            tmp.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)
        );
    if (!ok)
        DeleteFileW(tmp.c_str());
    return ok;
}

bool exists(std::string_view path) {
    return GetFileAttributesW(widePath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isDir(std::string_view path) {
    const DWORD a = GetFileAttributesW(widePath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t size(std::string_view path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widePath(path).c_str(), GetFileExInfoStandard, &d))
        return -1;
    return (int64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

bool stat(std::string_view path, Stat *out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (path.empty() || !GetFileAttributesExW(widePath(path).c_str(), GetFileExInfoStandard, &d))
        return false;
    out->isDir       = d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    out->size        = out->isDir ? 0 : (int64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    out->mtimeMicros = unixMicros(d.ftLastWriteTime);
    out->birthMicros = unixMicros(d.ftCreationTime);
    return true;
}

bool listDir(std::string_view dir, std::vector<DirEntry> *out) {
    out->clear();
    WIN32_FIND_DATAW d;
    HANDLE           h = FindFirstFileExW(
        widePath(join(dir, "*")).c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, 0
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do {
        const wchar_t *n = d.cFileName;
        if (dotName(n))
            continue;
        DirEntry e;
        e.name   = base::narrow(n);
        e.isDir  = d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        e.hidden = (d.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) || n[0] == L'.';
        e.size   = e.isDir ? 0 : (int64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        e.mtime  = unixMicros(d.ftLastWriteTime) / 1000000;
        out->push_back(std::move(e));
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return true;
}

static bool makeDir(std::string_view p) {
    return CreateDirectoryW(widePath(p).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool remove(std::string_view path) {
    const std::wstring w = widePath(path);
    return isDir(path) ? RemoveDirectoryW(w.c_str()) : DeleteFileW(w.c_str());
}

bool removeTree(std::string_view path) {
    return removeTreeW(widePath(path)) || !exists(path);
}

int64_t treeBytes(std::string_view path) {
    const std::wstring w = widePath(path);
    const DWORD        a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? treeBytesW(w) : std::max<int64_t>(size(path), 0);
}

bool touch(std::string_view path) {
    HANDLE h = CreateFileW(
        widePath(path).c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const bool ok = SetFileTime(h, nullptr, nullptr, &now) != 0;
    CloseHandle(h);
    return ok;
}

bool isAbsolute(std::string_view p) {
    return (p.size() >= 3 && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) ||
           (p.size() >= 2 && (p[0] == '/' || p[0] == '\\') && (p[1] == '/' || p[1] == '\\'));
}

std::string absolute(std::string_view path) {
    if (isAbsolute(path))
        return std::string(path);
    wchar_t     buf[MAX_PATH];
    const DWORD n = GetCurrentDirectoryW(MAX_PATH, buf);
    std::string cwd;
    if (n > 0 && n < MAX_PATH) {
        cwd = base::narrow(std::wstring_view(buf, n));
        for (auto &c : cwd)
            if (c == '\\')
                c = '/';
    }
    // "/x" is rooted on the current drive: "C:/x", never "<cwd>/x".
    if (!path.empty() && (path[0] == '/' || path[0] == '\\'))
        return cwd.size() >= 2 && cwd[1] == ':' ? cwd.substr(0, 2) + std::string(path)
                                                : std::string(path);
    return join(cwd, path);
}

bool isExecutable(std::string_view path) {
    return exists(path) && !isDir(path);
}
#elif !defined(MSGA_PORT)
namespace {
bool dotName(const char *n) {
    return n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]));
}
} // namespace

bool overwrite(std::string_view path, std::string_view data) {
    const std::string p(path);
    const int         fd = ::open(p.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0)
        return false;
    size_t put = 0;
    while (put < data.size()) {
        const ssize_t w = ::write(fd, data.data() + put, data.size() - put);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        put += size_t(w);
    }
    return ::close(fd) == 0 && put == data.size();
}

bool exists(std::string_view path) {
    struct stat st;
    return ::stat(std::string(path).c_str(), &st) == 0;
}

int64_t size(std::string_view path) {
    struct stat st;
    if (::stat(std::string(path).c_str(), &st) != 0)
        return -1;
    return int64_t(st.st_size);
}

bool stat(std::string_view path, Stat *out) {
    const std::string p(path);
    struct stat       st;
    if (p.empty() || ::stat(p.c_str(), &st) != 0)
        return false;
    out->isDir  = S_ISDIR(st.st_mode);
    out->size   = out->isDir ? 0 : int64_t(st.st_size);
    out->device = uint64_t(st.st_dev);
    out->inode  = uint64_t(st.st_ino);
#ifdef __APPLE__
    out->mtimeMicros = int64_t(st.st_mtimespec.tv_sec) * 1000000 + st.st_mtimespec.tv_nsec / 1000;
    out->birthMicros =
        int64_t(st.st_birthtimespec.tv_sec) * 1000000 + st.st_birthtimespec.tv_nsec / 1000;
#elif defined(__FreeBSD__)
    out->mtimeMicros = int64_t(st.st_mtim.tv_sec) * 1000000 + st.st_mtim.tv_nsec / 1000;
    out->birthMicros =
        st.st_birthtim.tv_sec > 0 // -1 where the file system keeps none
            ? int64_t(st.st_birthtim.tv_sec) * 1000000 + st.st_birthtim.tv_nsec / 1000
            : out->mtimeMicros;
#elif defined(__OpenBSD__)
    out->mtimeMicros = int64_t(st.st_mtim.tv_sec) * 1000000 + st.st_mtim.tv_nsec / 1000;
    out->birthMicros =
        st.__st_birthtim.tv_sec > 0 // FFS2 keeps one; FFS1 and others give 0
            ? int64_t(st.__st_birthtim.tv_sec) * 1000000 + st.__st_birthtim.tv_nsec / 1000
            : out->mtimeMicros;
#else
    out->mtimeMicros = int64_t(st.st_mtim.tv_sec) * 1000000 + st.st_mtim.tv_nsec / 1000;
    out->birthMicros = out->mtimeMicros;
#if defined(SYS_statx) && defined(STATX_BTIME)
    // The birth time, on file systems that keep one (a raw syscall: older C
    // libraries have no statx wrapper).
    struct statx sx;
    if (::syscall(SYS_statx, AT_FDCWD, p.c_str(), 0, STATX_BTIME, &sx) == 0 &&
        (sx.stx_mask & STATX_BTIME))
        out->birthMicros = int64_t(sx.stx_btime.tv_sec) * 1000000 + sx.stx_btime.tv_nsec / 1000;
#endif
#endif
    return true;
}

bool listDir(std::string_view dir, std::vector<DirEntry> *out) {
    out->clear();
    const std::string d(dir);
    DIR              *h = ::opendir(d.c_str());
    if (!h)
        return false;
    while (const dirent *e = ::readdir(h)) {
        const char *n = e->d_name;
        if (dotName(n))
            continue;
        DirEntry de;
        de.name   = n;
        de.hidden = n[0] == '.';
        struct stat st;
        // stat, not lstat: a link to a folder is browsed like one. Relative
        // to the open directory: no joined path to build and walk per entry.
        if (::fstatat(::dirfd(h), n, &st, 0) == 0) {
            de.isDir = S_ISDIR(st.st_mode);
            de.size  = de.isDir ? 0 : int64_t(st.st_size);
            de.mtime = int64_t(st.st_mtime);
        }
        out->push_back(std::move(de));
    }
    ::closedir(h);
    return true;
}

bool remove(std::string_view path) {
    return ::remove(std::string(path).c_str()) == 0;
}

bool removeTree(std::string_view path) {
    const std::string p(path);
    struct stat       st;
    if (::lstat(p.c_str(), &st) != 0)
        return true; // nothing there
    if (!S_ISDIR(st.st_mode))
        return ::unlink(p.c_str()) == 0;
    if (DIR *d = ::opendir(p.c_str())) {
        while (const dirent *e = ::readdir(d))
            if (!dotName(e->d_name))
                removeTree(join(p, e->d_name));
        ::closedir(d);
    }
    return ::rmdir(p.c_str()) == 0;
}

// The entry `name` of the directory `at`, links not followed; entries are
// looked up relative to their open directory, not by a full path each.
static int64_t treeBytesAt(int at, const char *name) {
    struct stat st;
    if (::fstatat(at, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return S_ISREG(st.st_mode) ? int64_t(st.st_size) : 0; // a link counts as nothing
    const int fd = ::openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return 0;
    DIR *d = ::fdopendir(fd);
    if (!d) {
        ::close(fd);
        return 0;
    }
    int64_t n = 0;
    while (const dirent *e = ::readdir(d))
        if (!dotName(e->d_name))
            n += treeBytesAt(fd, e->d_name);
    ::closedir(d); // closes fd too
    return n;
}

int64_t treeBytes(std::string_view path) {
    return treeBytesAt(AT_FDCWD, std::string(path).c_str());
}

bool touch(std::string_view path) {
    return ::utimensat(AT_FDCWD, std::string(path).c_str(), nullptr, 0) == 0;
}

bool isAbsolute(std::string_view p) {
    return !p.empty() && p[0] == '/';
}

std::string absolute(std::string_view path) {
    if (isAbsolute(path))
        return std::string(path);
    char buf[4096];
    if (!::getcwd(buf, sizeof buf))
        return std::string(path);
    return join(buf, path);
}

bool isExecutable(std::string_view path) {
    const std::string p(path);
    struct stat       st;
    return ::stat(p.c_str(), &st) == 0 && !S_ISDIR(st.st_mode) && ::access(p.c_str(), X_OK) == 0;
}

std::string realPath(std::string_view path) {
    if (path.empty())
        return {};
    char *real = ::realpath(std::string(path).c_str(), nullptr);
    if (!real)
        return {};
    std::string out(real);
    std::free(real);
    return out;
}

bool makeDir(std::string_view path, int mode) {
    return ::mkdir(std::string(path).c_str(), mode_t(mode)) == 0;
}

bool isRealDir(std::string_view path) {
    struct stat st;
    return ::lstat(std::string(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
#endif

bool copy(std::string_view from, std::string_view to) {
    // Attachments are small enough to hold once; atomic so a failed copy
    // never leaves half a file under the chosen name.
    std::string data;
    return readAll(from, &data) && writeAtomic(to, data);
}

static bool isSep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

#ifdef _WIN32
bool makeDirs(std::string_view path) {
    if (path.empty())
        return false;
    if (isDir(path))
        return true;
    // Create each missing ancestor, shallowest first.
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || isSep(path[i])) {
            const std::string_view prefix = path.substr(0, i);
            if (!prefix.empty() && !isSep(prefix.back()) &&
                !(prefix.size() == 2 && prefix[1] == ':'))
                if (!isDir(prefix) && !makeDir(prefix))
                    return false;
        }
    }
    return isDir(path);
}
#endif

std::string_view dirName(std::string_view path) {
    size_t i = path.size();
    while (i > 0 && !isSep(path[i - 1]))
        --i;
    if (i == 0)
        return {};
    // Keep the root: dirName("/a") is "/".
    return i == 1 ? path.substr(0, 1) : path.substr(0, i - 1);
}

std::string_view baseName(std::string_view path) {
    size_t i = path.size();
    while (i > 0 && !isSep(path[i - 1]))
        --i;
    return path.substr(i);
}

std::string_view extension(std::string_view path) {
    const std::string_view b   = baseName(path);
    const size_t           dot = b.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    return b.substr(dot + 1);
}

std::string join(std::string_view dir, std::string_view rel) {
    if (dir.empty())
        return std::string(rel);
    std::string out(dir);
    if (!isSep(out.back()))
        out += '/';
    while (rel.size() >= 2 && rel[0] == '.' && isSep(rel[1]))
        rel.remove_prefix(2); // "./assets" → "assets"
    out.append(rel);
    return out;
}

std::string resolve(std::string_view base, std::string_view rel) {
    return isAbsolute(rel) ? std::string(rel) : join(base, rel);
}

// ── file:// URLs ────────────────────────────────────────────────────────────

std::string toFileUrl(std::string_view path) {
    std::string p(path);
#ifdef _WIN32
    for (char &c : p)
        if (c == '\\')
            c = '/';
#endif
    std::string out = "file://";
    size_t      i   = 0;
    if (p.size() >= 2 && p[0] == '/' && p[1] == '/')
        i = 2; // UNC "//server/share/x": the server is the authority
    else if (p.empty() || p[0] != '/')
        out += '/'; // "C:/x" → "file:///C:/x"
    // RFC 3986 pchar: unreserved, sub-delims, ':',
    // '@' and '/' as they are, every other byte (UTF-8 too) as %XX.
    out += str::percentEncode(std::string_view(p).substr(i), "!$&'()*+,;=:@/");
    return out;
}

std::string fromFileUrl(std::string_view url) {
    if (url.substr(0, 7) != "file://")
        return {};
    std::string_view rest = url.substr(7);
    std::string      out;
    if (!rest.empty() && rest[0] != '/') { // an authority
        const size_t     slash = rest.find('/');
        std::string_view host  = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
#ifdef _WIN32
        if (host != "localhost")
            out = "//" + std::string(host); // UNC
#else
        (void)host; // another machine's file: its path here, as before
#endif
    }
    out += str::percentDecode(rest);
#ifdef _WIN32
    if (out.size() > 2 && out[0] == '/' && out[2] == ':')
        out.erase(0, 1); // "/C:/x"
#endif
    return out;
}

} // namespace file

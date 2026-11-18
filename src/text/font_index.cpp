#include "text/font_index.h"

#include "base/file.h"
#include "base/process.h"
#include "base/str.h"
#include "base/utf8.h"
#include "prim/bytes.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include "base/winstr.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace text::fonts {

namespace {

constexpr uint32_t kMagic   = 0x49464E4D; // "MNFI"
constexpr uint32_t kVersion = 4;          // bump when FaceRec/parsing changes

// ── OS layer: stat, list ─────────────────────────────────────────────────────

struct PathInfo {
    bool     isDir  = false;
    bool     isFile = false;
    int64_t  mtime  = 0; // ns since the epoch
    uint64_t size   = 0;
};

#ifdef _WIN32
using base::wide;

bool pathInfo(const std::string &path, PathInfo *out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &a))
        return false;
    const uint64_t t =
        uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime;
    out->isDir  = a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    out->isFile = !out->isDir;
    out->mtime  = int64_t(t - 116444736000000000ull) * 100; // 100 ns ticks since 1601
    out->size   = uint64_t(a.nFileSizeHigh) << 32 | a.nFileSizeLow;
    return true;
}

std::vector<std::string> listNames(const std::string &dir) {
    std::vector<std::string> names;
    WIN32_FIND_DATAW         e;
    const HANDLE             h = FindFirstFileExW(
        wide(dir + "/*").c_str(), FindExInfoBasic, &e, FindExSearchNameMatch, nullptr, 0
    );
    if (h == INVALID_HANDLE_VALUE)
        return names;
    do
        names.push_back(base::narrow(e.cFileName));
    while (FindNextFileW(h, &e));
    FindClose(h);
    return names;
}
#else
bool pathInfo(const std::string &path, PathInfo *out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
    out->isDir  = S_ISDIR(st.st_mode);
    out->isFile = S_ISREG(st.st_mode);
#ifdef __APPLE__
    out->mtime = int64_t(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
    out->mtime = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
    out->size = uint64_t(st.st_size);
    return true;
}

std::vector<std::string> listNames(const std::string &dir) {
    std::vector<std::string> names;
    DIR                     *d = opendir(dir.c_str());
    if (!d)
        return names;
    while (dirent *e = readdir(d))
        names.push_back(e->d_name);
    closedir(d);
    return names;
}
#endif

// ── sfnt reading ─────────────────────────────────────────────────────────────

struct Buf {
    const uint8_t *p = nullptr;
    size_t         n = 0;
    uint32_t       u16(size_t o) const { return o + 2 <= n ? prim::be16(p + o) : 0; }
    uint32_t       u32(size_t o) const { return o + 4 <= n ? prim::be32(p + o) : 0; }
    Buf            sub(size_t o, size_t len) const {
        if (o > n)
            return {};
        return {p + o, std::min(len, n - o)};
    }
};

constexpr uint32_t tag(const char *t) {
    return uint32_t(uint8_t(t[0])) << 24 | uint32_t(uint8_t(t[1])) << 16 |
           uint32_t(uint8_t(t[2])) << 8 | uint8_t(t[3]);
}

struct Tables {
    Buf  name, os2, head, post, cmap, fvar;
    bool color = false, bitmaps = false, outlines = false;
};

Tables readTables(const Buf &file, size_t faceOff) {
    Tables     t;
    const auto count = file.u16(faceOff + 4);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t   r   = faceOff + 12 + 16 * i;
        const uint32_t tg  = file.u32(r);
        const Buf      tab = file.sub(file.u32(r + 8), file.u32(r + 12));
        switch (tg) {
        case tag("name"):
            t.name = tab;
            break;
        case tag("OS/2"):
            t.os2 = tab;
            break;
        case tag("head"):
            t.head = tab;
            break;
        case tag("post"):
            t.post = tab;
            break;
        case tag("cmap"):
            t.cmap = tab;
            break;
        case tag("fvar"):
            t.fvar = tab;
            break;
        case tag("CBDT"):
        case tag("sbix"):
            t.bitmaps = true;
            t.color   = true;
            break;
        case tag("COLR"):
            t.color = true;
            break;
        case tag("glyf"):
        case tag("CFF "):
        case tag("CFF2"):
            t.outlines = true;
            break;
        default:
            break;
        }
    }
    return t;
}

// The typographic family (name ID 16) if present, else the legacy family (1);
// Windows-platform English first, then any Windows language, then Mac Roman.
std::string familyName(const Buf &name) {
    const uint32_t count = name.u16(2), strOff = name.u16(4);
    int            best = -1, bestScore = -1;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t   r    = 6 + 12 * i;
        const uint32_t plat = name.u16(r), enc = name.u16(r + 2), lang = name.u16(r + 4),
                       id = name.u16(r + 6);
        if (id != 1 && id != 16)
            continue;
        int score = id == 16 ? 8 : 0;
        if (plat == 3 && (enc == 1 || enc == 10 || enc == 0))
            score += lang == 0x409 ? 4 : 2;
        else if (plat == 1 && enc == 0)
            score += 1;
        else if (plat == 0)
            score += 2;
        else
            continue;
        if (score > bestScore)
            bestScore = score, best = int(i);
    }
    std::string out;
    if (best < 0)
        return out;
    const size_t   r   = 6 + 12 * size_t(best);
    const uint32_t len = name.u16(r + 8), off = strOff + name.u16(r + 10);
    const Buf      s = name.sub(off, len);
    if (name.u16(r) == 1) { // Mac Roman: ASCII is all we care about
        for (size_t k = 0; k < s.n; ++k)
            out += char(s.p[k] < 0x80 ? s.p[k] : '?');
    } else {
        for (size_t k = 0; k + 1 < s.n; k += 2) {
            uint32_t c = s.u16(k);
            if (c >= 0xD800 && c < 0xDC00 && k + 3 < s.n) {
                c = 0x10000 + ((c - 0xD800) << 10) + (s.u16(k + 2) - 0xDC00);
                k += 2;
            }
            utf8::append(out, c);
        }
    }
    for (auto &c : out)
        c = str::asciiLower(c);
    return out;
}

using Ranges = std::vector<std::pair<uint32_t, uint32_t>>;

void cmap4(const Buf &t, Ranges &out) {
    const uint32_t segX2 = t.u16(6);
    const size_t   ends = 14, starts = 16 + segX2, deltas = starts + segX2, ro = deltas + segX2;
    for (uint32_t s = 0; s < segX2; s += 2) {
        const uint32_t end = t.u16(ends + s), start = t.u16(starts + s);
        const uint32_t delta = t.u16(deltas + s), rangeOff = t.u16(ro + s);
        if (start > end || start == 0xFFFF)
            continue;
        if (rangeOff == 0) {
            out.push_back({start, std::min(end, 0xFFFEu)});
            continue;
        }
        // Glyph ids come from the array: drop code points that map to .notdef.
        for (uint32_t c = start; c <= end && c != 0xFFFF; ++c) {
            const size_t gi = ro + s + rangeOff + 2 * (c - start);
            uint32_t     g  = t.u16(gi);
            if (g && ((g + delta) & 0xFFFF)) {
                if (!out.empty() && out.back().second + 1 == c)
                    out.back().second = c;
                else
                    out.push_back({c, c});
            }
        }
    }
}

void cmap12(const Buf &t, Ranges &out) {
    const uint32_t groups = t.u32(12);
    for (uint32_t g = 0; g < groups && 16 + 12 * size_t(g) + 12 <= t.n; ++g) {
        const size_t   r     = 16 + 12 * size_t(g);
        const uint32_t start = t.u32(r), end = std::min(t.u32(r + 4), 0x10FFFFu);
        if (start <= end)
            out.push_back({start, end});
    }
}

// Unicode coverage from the best cmap subtable (full-repertoire format 12
// preferred over BMP format 4).
Ranges coverage(const Buf &cmap) {
    Ranges         out;
    const uint32_t n         = cmap.u16(2);
    size_t         best      = 0;
    int            bestScore = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const size_t   r    = 4 + 8 * i;
        const uint32_t plat = cmap.u16(r), enc = cmap.u16(r + 2), off = cmap.u32(r + 4);
        const uint32_t fmt   = cmap.u16(off);
        int            score = 0;
        if (fmt == 12 && ((plat == 3 && enc == 10) || plat == 0))
            score = 3;
        else if (fmt == 4 && ((plat == 3 && enc == 1) || plat == 0))
            score = 2;
        else if (fmt == 4 && plat == 3 && enc == 0)
            score = 1; // symbol font
        if (score > bestScore)
            bestScore = score, best = off;
    }
    if (!bestScore)
        return out;
    const Buf t = cmap.sub(best, cmap.n - best);
    if (t.u16(0) == 12)
        cmap12(t, out);
    else
        cmap4(t, out);
    std::sort(out.begin(), out.end());
    Ranges merged;
    for (auto &r : out) {
        if (!merged.empty() && r.first <= merged.back().second + 1)
            merged.back().second = std::max(merged.back().second, r.second);
        else
            merged.push_back(r);
    }
    return merged;
}

void putVarint(std::vector<uint8_t> &v, uint32_t x) {
    while (x >= 0x80) {
        v.push_back(uint8_t(x | 0x80));
        x >>= 7;
    }
    v.push_back(uint8_t(x));
}

uint32_t getVarint(const uint8_t *&p, const uint8_t *end) {
    uint32_t x = 0;
    for (int shift = 0; p < end && shift < 35; shift += 7) {
        const uint8_t b = *p++;
        x |= uint32_t(b & 0x7F) << shift;
        if (!(b & 0x80))
            break;
    }
    return x;
}

struct Pool {
    std::string *s;
    uint32_t     add(const std::string &v) {
        const auto off = uint32_t(s->size());
        s->append(v);
        s->push_back('\0');
        return off;
    }
};

void parseFace(
    const Buf &file, size_t faceOff, uint16_t ttc, uint32_t fileIdx, Index &ix, Pool &pool
) {
    const Tables t = readTables(file, faceOff);
    if (!t.cmap.n || !t.name.n || (!t.outlines && !t.color))
        return;
    const std::string fam = familyName(t.name);
    if (fam.empty())
        return;
    FaceRec f;
    f.file = fileIdx;
    f.ttc  = ttc;
    if (t.os2.n >= 64) {
        f.weight           = uint16_t(std::clamp<uint32_t>(t.os2.u16(4), 1, 1000));
        f.width            = uint16_t(std::clamp<uint32_t>(t.os2.u16(6), 1, 9));
        const uint32_t sel = t.os2.u16(62);
        if (sel & (1 | 1 << 9))
            f.flags |= kItalic;
        const uint8_t famType = t.os2.p[32], serif = t.os2.p[33], prop = t.os2.p[35];
        if (famType == 2 && prop == 9)
            f.flags |= kMono;
        if (famType == 2 && serif >= 2 && serif <= 10)
            f.flags |= kSerif;
    } else if (t.head.n >= 46 && (t.head.u16(44) & 2)) {
        f.flags |= kItalic;
    }
    if (t.post.n >= 16 && t.post.u32(12))
        f.flags |= kMono;
    if (t.color)
        f.flags |= kColor;
    // Colour bitmap strikes win over outlines: Apple Color Emoji's glyf holds
    // empty placeholders, and FreeType calls such a face fixed-size only, so
    // FT_Set_Char_Size fails at any size that isn't one of its strikes.
    if (!t.outlines || t.bitmaps)
        f.flags |= kBitmapOnly;
    if (fam.find("serif") != std::string::npos && fam.find("sans") == std::string::npos)
        f.flags |= kSerif;
    if (t.fvar.n >= 16) {
        const uint32_t axesOff = t.fvar.u16(4), count = t.fvar.u16(8), size = t.fvar.u16(10);
        for (uint32_t a = 0; a < count; ++a) {
            const size_t r = axesOff + size * a;
            if (t.fvar.u32(r) == tag("wght")) {
                f.wghtMin = uint16_t(std::clamp<uint32_t>(t.fvar.u32(r + 4) >> 16, 1, 1000));
                f.wghtMax = uint16_t(std::clamp<uint32_t>(t.fvar.u32(r + 12) >> 16, 1, 1000));
            }
        }
    }
    const Ranges cov = coverage(t.cmap);
    if (cov.empty())
        return;
    f.covOff         = uint32_t(ix.cov.size());
    uint32_t prevEnd = 0;
    for (auto &r : cov) {
        putVarint(ix.cov, r.first - prevEnd);
        putVarint(ix.cov, r.second - r.first);
        prevEnd = r.second;
        for (uint32_t b = r.first >> 8; b <= (r.second >> 8) && b < 768; ++b)
            f.blocks[b >> 3] |= uint8_t(1 << (b & 7));
    }
    f.covLen = uint32_t(ix.cov.size()) - f.covOff;
    f.family = pool.add(fam);
    ix.faces.push_back(f);
}

void parseFile(const char *path, uint32_t fileIdx, Index &ix, Pool &pool) {
    size_t         n = 0;
    const uint8_t *m = mapFile(path, &n);
    if (!m)
        return;
    const Buf file{m, n};
    if (file.u32(0) == tag("ttcf")) {
        const uint32_t n = std::min<uint32_t>(file.u32(8), 256);
        for (uint32_t i = 0; i < n; ++i)
            parseFace(file, file.u32(12 + 4 * i), uint16_t(i), fileIdx, ix, pool);
    } else {
        const uint32_t v = file.u32(0);
        if (v == 0x00010000 || v == tag("OTTO") || v == tag("true"))
            parseFace(file, 0, 0, fileIdx, ix, pool);
    }
    unmapFile(m, n);
}

// ── Directory walk + cache ───────────────────────────────────────────────────

std::vector<std::string> fontRoots() {
    std::vector<std::string> roots;
#ifdef _WIN32
    const char sep = ';'; // "C:\…" has a colon of its own
#else
    const char sep = ':';
#endif
    if (const char *o = std::getenv("MSGA_NEXT_FONT_DIRS")) { // tests / packaging
        str::Splitter dirs(o, sep);
        for (std::string_view d; dirs.next(&d);)
            if (!d.empty())
                roots.emplace_back(d);
        return roots;
    }
#ifdef _WIN32
    const std::string win = base::env("WINDIR");
    roots.push_back((win.empty() ? std::string("C:/Windows") : win) + "/Fonts");
    // Per-user installs ("Install for me", Windows 10 1809+).
    if (const std::string l = base::env("LOCALAPPDATA"); !l.empty())
        roots.push_back(l + "/Microsoft/Windows/Fonts");
    return roots;
#endif
    const std::string h = base::homeDir();
#ifdef __APPLE__
    if (!h.empty())
        roots.push_back(h + "/Library/Fonts");
    roots.push_back("/Library/Fonts");
    roots.push_back("/System/Library/Fonts");
    return roots;
#endif
    if (const char *x = std::getenv("XDG_DATA_HOME"); x && *x)
        roots.push_back(std::string(x) + "/fonts");
    else if (!h.empty())
        roots.push_back(h + "/.local/share/fonts");
    if (!h.empty())
        roots.push_back(h + "/.fonts");
    std::string dataDirs = "/usr/local/share:/usr/share";
    if (const char *x = std::getenv("XDG_DATA_DIRS"); x && *x)
        dataDirs = std::string(x) + ":" + dataDirs;
    str::Splitter dirs(dataDirs, ':');
    for (std::string_view part; dirs.next(&part);) {
        if (part.empty())
            continue;
        std::string d(part);
        while (d.size() > 1 && d.back() == '/')
            d.pop_back();
        d += "/fonts";
        if (std::find(roots.begin(), roots.end(), d) == roots.end())
            roots.push_back(d);
    }
    // The X server's own fonts: on OpenBSD the only ones a base install has
    // (DejaVu, Spleen, …), outside every XDG data dir.
    if (const char *x11 = "/usr/X11R6/lib/X11/fonts";
        std::find(roots.begin(), roots.end(), x11) == roots.end())
        roots.push_back(x11);
    return roots;
}

bool fontExt(const char *name) {
    const char *dot = std::strrchr(name, '.');
    if (!dot)
        return false;
    char e[5] = {};
    for (int i = 0; i < 4 && dot[1 + i]; ++i)
        e[i] = char(dot[1 + i] | 0x20);
    if (dot[1] && std::strlen(dot + 1) > 3)
        return false;
    return !std::strcmp(e, "ttf") || !std::strcmp(e, "otf") || !std::strcmp(e, "ttc") ||
           !std::strcmp(e, "otc");
}

struct Found {
    std::vector<std::pair<std::string, int64_t>> dirs;
    struct F {
        std::string path;
        int64_t     mtime;
        uint64_t    size;
    };
    std::vector<F> files;
};

void walk(const std::string &dir, int depth, Found &out) {
    PathInfo pi;
    if (depth > 8 || !pathInfo(dir, &pi) || !pi.isDir)
        return;
    out.dirs.push_back({dir, pi.mtime});
    std::vector<std::string> subdirs;
    for (const std::string &name : listNames(dir)) {
        if (name[0] == '.')
            continue;
        std::string p = dir + "/" + name;
        if (!pathInfo(p, &pi))
            continue;
        if (pi.isDir)
            subdirs.push_back(std::move(p));
        else if (pi.isFile && fontExt(name.c_str()))
            out.files.push_back({std::move(p), pi.mtime, pi.size});
    }
    for (auto &s : subdirs)
        walk(s, depth + 1, out);
}

// In the app's cache dir (app/identity.h's cacheDir: organization "msga",
// application "MSGA"), under a name earlier versions never used.
std::string cachePath() {
#ifdef _WIN32
    const std::string l = base::env("LOCALAPPDATA");
    return (l.empty() ? base::env("TEMP") : l) + "/msga/MSGA/cache/fonts.idx";
#elif defined(__APPLE__)
    return base::homeDir() + "/Library/Caches/msga/MSGA/fonts.idx";
#endif
    if (const char *x = std::getenv("XDG_CACHE_HOME"); x && *x)
        return std::string(x) + "/msga/MSGA/fonts.idx";
    const std::string home = base::homeDir();
    return (home.empty() ? std::string("/tmp") : home) + "/.cache/msga/MSGA/fonts.idx";
}

template <class T>
bool readVec(const std::string &b, size_t &o, uint32_t n, std::vector<T> &v) {
    if (o + size_t(n) * sizeof(T) > b.size())
        return false;
    v.resize(n);
    if (n)
        std::memcpy(v.data(), b.data() + o, size_t(n) * sizeof(T));
    o += size_t(n) * sizeof(T);
    return true;
}

bool readCache(const std::string &path, Index &ix) {
    std::string b;
    if (!file::readAll(path, &b))
        return false;
    uint32_t h[7];
    if (b.size() < sizeof h)
        return false;
    std::memcpy(h, b.data(), sizeof h);
    if (h[0] != kMagic || h[1] != kVersion)
        return false;
    size_t            o = sizeof h;
    std::vector<char> pool;
    if (!readVec(b, o, h[2], ix.dirs) || !readVec(b, o, h[3], ix.files) ||
        !readVec(b, o, h[4], ix.faces) || !readVec(b, o, h[5], pool) ||
        !readVec(b, o, h[6], ix.cov))
        return false;
    ix.pool.assign(pool.begin(), pool.end());
    for (auto &fr : ix.faces)
        if (fr.file >= ix.files.size() || fr.family >= ix.pool.size() ||
            size_t(fr.covOff) + fr.covLen > ix.cov.size())
            return false;
    return true;
}

void writeCache(const std::string &path, const Index &ix) {
    const uint32_t h[7] = {
        kMagic,
        kVersion,
        uint32_t(ix.dirs.size()),
        uint32_t(ix.files.size()),
        uint32_t(ix.faces.size()),
        uint32_t(ix.pool.size()),
        uint32_t(ix.cov.size())
    };
    std::string b;
    auto        put = [&](const void *p, size_t n) { b.append(static_cast<const char *>(p), n); };
    put(h, sizeof h);
    put(ix.dirs.data(), ix.dirs.size() * sizeof(DirRec));
    put(ix.files.data(), ix.files.size() * sizeof(FileRec));
    put(ix.faces.data(), ix.faces.size() * sizeof(FaceRec));
    put(ix.pool.data(), ix.pool.size());
    put(ix.cov.data(), ix.cov.size());
    file::writeAtomic(path, b); // creates the directory; replaces an old cache on Windows too
}

// The cache is valid while every directory it saw has the same mtime and the
// set of existing roots is unchanged (adding/removing a font touches its
// directory's mtime; a new subdirectory touches its parent's).
bool cacheValid(const Index &ix, const std::vector<std::string> &roots) {
    PathInfo pi;
    for (auto &d : ix.dirs)
        if (!pathInfo(ix.str(d.path), &pi) || pi.mtime != d.mtime)
            return false;
    for (auto &r : roots) {
        const bool exists = pathInfo(r, &pi) && pi.isDir;
        bool       known  = false;
        for (auto &d : ix.dirs)
            known = known || r == ix.str(d.path);
        if (exists != known)
            return false;
    }
    return true;
}

} // namespace

#ifdef _WIN32
const uint8_t *mapFile(const char *path, size_t *size) {
    const HANDLE f = CreateFileW(
        wide(path).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (f == INVALID_HANDLE_VALUE)
        return nullptr;
    LARGE_INTEGER n;
    void         *p = nullptr;
    if (GetFileSizeEx(f, &n) && n.QuadPart > 0) {
        if (HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr)) {
            p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0); // the view keeps the mapping alive
            CloseHandle(m);
        }
    }
    CloseHandle(f);
    if (p)
        *size = size_t(n.QuadPart);
    return static_cast<const uint8_t *>(p);
}

void unmapFile(const uint8_t *p, size_t) {
    UnmapViewOfFile(p);
}
#else
const uint8_t *mapFile(const char *path, size_t *size) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    struct stat st;
    void       *p = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size > 0)
        p = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED)
        return nullptr;
    *size = size_t(st.st_size);
    return static_cast<const uint8_t *>(p);
}

void unmapFile(const uint8_t *p, size_t size) {
    munmap(const_cast<uint8_t *>(p), size);
}
#endif

bool Index::covers(const FaceRec &f, uint32_t cp) const {
    if (cp < 0x30000 && !(f.blocks[cp >> 11] & (1 << ((cp >> 8) & 7))))
        return false;
    const uint8_t *p = cov.data() + f.covOff, *end = p + f.covLen;
    uint32_t       prevEnd = 0;
    while (p < end) {
        const uint32_t start = prevEnd + getVarint(p, end);
        const uint32_t last  = start + getVarint(p, end);
        if (cp < start)
            return false;
        if (cp <= last)
            return true;
        prevEnd = last;
    }
    return false;
}

bool loadIndex(Index *out, std::string *error) {
    const auto        roots = fontRoots();
    const std::string path  = cachePath();
    Index             old;
    const bool        haveOld = readCache(path, old);
    if (haveOld && cacheValid(old, roots) && !old.faces.empty()) {
        *out = std::move(old);
        return true;
    }
    Found found;
    for (auto &r : roots)
        walk(r, 0, found);

    Index ix;
    Pool  pool{&ix.pool};
    for (auto &[d, mt] : found.dirs)
        ix.dirs.push_back({pool.add(d), 0, mt});
    // The old faces grouped by file (facesOf[file] .. facesOf[file + 1] in
    // byFile), so a reused file copies its faces without a scan.
    std::vector<uint32_t> facesOf, byFile;
    if (haveOld) {
        facesOf.assign(old.files.size() + 1, 0);
        for (const FaceRec &ofr : old.faces)
            ++facesOf[ofr.file + 1];
        for (size_t i = 1; i < facesOf.size(); ++i)
            facesOf[i] += facesOf[i - 1];
        byFile.resize(old.faces.size());
        std::vector<uint32_t> at(facesOf.begin(), facesOf.end() - 1);
        for (uint32_t fi = 0; fi < old.faces.size(); ++fi)
            byFile[at[old.faces[fi].file]++] = fi;
    }
    uint32_t next = 0; // the walk is in the same order as last time: look there first
    for (auto &f : found.files) {
        const auto fileIdx = uint32_t(ix.files.size());
        ix.files.push_back({pool.add(f.path), 0, f.mtime, f.size});
        // Reuse the old entry when the file is unchanged: parsing CJK cmaps is
        // most of the scan time.
        bool reused = false;
        if (haveOld) {
            const auto same = [&](uint32_t oi) {
                const FileRec &of = old.files[oi];
                return of.mtime == f.mtime && of.size == f.size && f.path == old.str(of.path);
            };
            uint32_t oi = next;
            if (oi >= old.files.size() || !same(oi))
                for (oi = 0; oi < old.files.size() && !same(oi);)
                    ++oi;
            if (oi < old.files.size()) {
                reused = true;
                next   = oi + 1;
                for (uint32_t k = facesOf[oi]; k < facesOf[oi + 1]; ++k) {
                    const FaceRec &ofr = old.faces[byFile[k]];
                    FaceRec        nf  = ofr;
                    nf.file            = fileIdx;
                    nf.family          = pool.add(old.str(ofr.family));
                    nf.covOff          = uint32_t(ix.cov.size());
                    ix.cov.insert(
                        ix.cov.end(),
                        old.cov.begin() + ofr.covOff,
                        old.cov.begin() + ofr.covOff + ofr.covLen
                    );
                    ix.faces.push_back(nf);
                }
            }
        }
        if (!reused)
            parseFile(f.path.c_str(), fileIdx, ix, pool);
    }
    if (ix.faces.size() > 0xFFFE)
        ix.faces.resize(0xFFFE); // FontKey carries 16 bits of face index
    writeCache(path, ix);
    if (ix.faces.empty()) {
        if (error) {
            *error = "no usable fonts found (looked in ";
            for (size_t i = 0; i < roots.size(); ++i)
                *error += (i ? ", " : "") + roots[i];
            *error += ")";
        }
        return false;
    }
    *out = std::move(ix);
    return true;
}

} // namespace text::fonts

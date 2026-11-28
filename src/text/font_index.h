// The installed-font index: which faces exist, their family/weight/italic/
// mono flags and cmap coverage. Built by reading sfnt tables directly (no
// fontconfig), cached in the app's cache dir (msga/MSGA/fonts.idx) and
// revalidated by directory mtimes. Internal to text/.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace text::fonts {

enum FaceFlags : uint8_t {
    kItalic     = 1,
    kMono       = 2,
    kColor      = 4,  // CBDT/sbix/COLR
    kSerif      = 8,  // panose says serif (fallback prefers sans)
    kBitmapOnly = 16, // CBDT/sbix strikes (or no outlines): select a strike and scale
};

// POD, written to the cache file as-is (the version number guards layout).
struct FaceRec {
    uint32_t file   = 0;             // index into Index::files
    uint32_t family = 0;             // offset of the lowercase family name in Index::pool
    uint32_t covOff = 0, covLen = 0; // coverage bytes in Index::cov
    uint16_t ttc    = 0;
    uint16_t weight = 400, width = 5;
    uint16_t wghtMin = 0, wghtMax = 0; // variable 'wght' axis range, 0 = none
    uint8_t  flags      = 0;
    uint8_t  pad        = 0;
    // One bit per 256-code-point block below U+30000: a quick reject before
    // decoding the exact coverage.
    uint8_t  blocks[96] = {};
};

struct FileRec {
    uint32_t path  = 0; // pool offset
    uint32_t pad   = 0;
    int64_t  mtime = 0;
    uint64_t size  = 0;
};

struct DirRec {
    uint32_t path  = 0;
    uint32_t pad   = 0;
    int64_t  mtime = 0;
};

struct Index {
    std::vector<DirRec>  dirs;
    std::vector<FileRec> files;
    std::vector<FaceRec> faces;
    std::string          pool; // NUL-terminated strings
    std::vector<uint8_t> cov;  // varint-coded ranges (gap, length-1) per face

    const char *str(uint32_t off) const { return pool.c_str() + off; }
    bool        covers(const FaceRec &f, uint32_t cp) const;
};

// Loads the cached index if still valid, else rescans (reusing entries of
// unchanged files) and rewrites the cache. False when no font was found.
bool loadIndex(Index *out, std::string *error);

// The whole file mapped read-only; null when it cannot be opened or is empty.
// A port's system faces (PORT_CAP2_FONTS) are "port-font:<name>" paths.
const uint8_t *mapFile(const char *path, size_t *size);
void           unmapFile(const char *path, const uint8_t *p, size_t size);

} // namespace text::fonts

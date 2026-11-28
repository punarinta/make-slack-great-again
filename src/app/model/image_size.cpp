#include "app/model/image_size.h"

#include "prim/bytes.h"

#include <cstdio>
#include <cstring>

namespace model {

namespace {

using prim::be16;
using prim::be32;
using prim::le16;
using prim::le24;
using prim::le32;

// A small window onto the file. The header formats need its first bytes
// only; a JPEG's frame header may sit past big EXIF / ICC segments, which
// are skipped by seeking, never read.
class Window {
public:
    explicit Window(FILE *f) : _f(f) {}
    // The file's first bytes (up to the window's size); *n = how many.
    const unsigned char *head(size_t *n) {
        fill(0);
        *n = _n;
        return _buf;
    }
    // [at, at+len) when the file has it, else null.
    const unsigned char *at(size_t at, size_t len) {
        if (at < _start || at + len > _start + _n)
            fill(at);
        return at + len <= _start + _n ? _buf + (at - _start) : nullptr;
    }

private:
    void fill(size_t at) {
        _start = at;
        _n = std::fseek(_f, long(at), SEEK_SET) == 0 ? std::fread(_buf, 1, sizeof(_buf), _f) : 0;
    }

    FILE         *_f;
    size_t        _start = 0, _n = 0;
    unsigned char _buf[4096];
};

} // namespace

bool imageSize(const std::string &path, int32_t *w, int32_t *h) {
    *w = *h = 0;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    Window               win(f);
    size_t               n = 0;
    const unsigned char *p = win.head(&n);
    if (n >= 24 && std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0 &&
        std::memcmp(p + 12, "IHDR", 4) == 0) {
        *w = int32_t(be32(p + 16));
        *h = int32_t(be32(p + 20));
    } else if (n >= 10 && std::memcmp(p, "GIF8", 4) == 0) {
        *w = int32_t(le16(p + 6));
        *h = int32_t(le16(p + 8));
    } else if (n >= 30 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0) {
        if (std::memcmp(p + 12, "VP8X", 4) == 0) {
            *w = int32_t(le24(p + 24) + 1);
            *h = int32_t(le24(p + 27) + 1);
        } else if (std::memcmp(p + 12, "VP8L", 4) == 0 && n >= 25) {
            const uint32_t b = le32(p + 21);
            *w               = int32_t((b & 0x3FFF) + 1);
            *h               = int32_t(((b >> 14) & 0x3FFF) + 1);
        } else if (std::memcmp(p + 12, "VP8 ", 4) == 0) {
            *w = int32_t(le16(p + 26) & 0x3FFF);
            *h = int32_t(le16(p + 28) & 0x3FFF);
        }
    } else if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        // Walk the JPEG segments to the first start-of-frame, seeking over
        // each one's payload.
        size_t i = 2;
        for (int steps = 0; steps < (1 << 20); ++steps) {
            const unsigned char *q = win.at(i, 10);
            if (!q)
                break;
            if (q[0] != 0xFF) {
                ++i;
                continue;
            }
            const unsigned marker = q[1];
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) ||
                marker == 0xFF) {
                i += marker == 0xFF ? 1 : 2;
                continue;
            }
            const uint32_t len = be16(q + 2);
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                marker != 0xCC) {
                *h = int32_t(be16(q + 5));
                *w = int32_t(be16(q + 7));
                break;
            }
            i += 2 + len;
        }
    }
    std::fclose(f);
    return *w > 0 && *h > 0;
}

} // namespace model

#include "text/glyph_cache.h"

#include <cstring>
#include <vector>

namespace text::cache {

namespace {

constexpr int kPageSize = 256; // A8 page = 64 KB, colour page = 256 KB

struct Page {
    std::vector<uint8_t>  a8;
    std::vector<uint32_t> argb;
    int                   w = 0, h = 0;
    int                   shelfY = 0, shelfH = 0, cursorX = 0;
    uint64_t              lastUse = 0;
    bool                  color = false, alive = false, full = false;
    size_t                bytes() const { return a8.size() + argb.size() * 4; }
};

struct Entry {
    uint32_t font = 0, glyph = 0, ppem = 0;
    uint8_t  phase = 0;
    bool     used  = false;
    Glyph    g;
};

struct Cache {
    std::vector<Page>  pages;
    std::vector<Entry> table; // open addressing, power-of-two size
    size_t             count = 0, bytes = 0, budget = 8u << 20;
    uint64_t           epoch  = 1;
    int                openA8 = -1, openColor = -1;
};
Cache c;

uint32_t hashKey(uint32_t font, uint32_t glyph, uint32_t ppem, int phase) {
    uint32_t h = font * 0x9E3779B1u;
    h ^= glyph + 0x7F4A7C15u + (h << 6) + (h >> 2);
    h ^= ppem * 0x85EBCA6Bu + uint32_t(phase) + (h << 6) + (h >> 2);
    return h ^ (h >> 15);
}

size_t homeOf(const Entry &e, size_t mask) {
    return hashKey(e.font, e.glyph, e.ppem, e.phase) & mask;
}

Entry *insertEntry(const Entry &e) {
    const size_t mask = c.table.size() - 1;
    size_t       i    = homeOf(e, mask);
    while (c.table[i].used)
        i = (i + 1) & mask;
    c.table[i] = e;
    ++c.count;
    return &c.table[i];
}

void grow() {
    std::vector<Entry> old;
    old.swap(c.table);
    c.table.assign(old.size() * 2, Entry{});
    c.count = 0;
    for (auto &e : old)
        if (e.used)
            insertEntry(e);
}

// Drops every entry on a dead page in one sweep, in place: backward-shift
// deletion keeps each probe chain intact, so no tombstones and no rehash.
// The sweep starts just after an empty slot (load stays below 1/2), so no
// chain wraps past its start and an entry only ever moves to a slot the
// sweep has not passed yet.
void purgeDeadPages() {
    const size_t n = c.table.size();
    if (!n)
        return;
    const size_t mask  = n - 1;
    size_t       start = 0;
    while (c.table[start].used)
        ++start;
    for (size_t step = 1; step <= n;) {
        const size_t i = (start + step) & mask;
        Entry       &e = c.table[i];
        if (!e.used || e.g.page == 0xFFFF || c.pages[e.g.page].alive) {
            ++step;
            continue;
        }
        size_t hole = i;
        for (size_t j = (hole + 1) & mask; c.table[j].used; j = (j + 1) & mask) {
            const size_t home = homeOf(c.table[j], mask);
            // Movable when its home is not cyclically in (hole, j].
            if (((j - home) & mask) >= ((j - hole) & mask)) {
                c.table[hole] = c.table[j];
                hole          = j;
            }
        }
        c.table[hole] = Entry{};
        --c.count;
        // Slot i now holds a shifted entry (or is empty): look at it again.
    }
}

// Frees the least recently used page other than `keep`; its entries stay in
// the table until purgeDeadPages(). False when nothing could be freed.
bool evictOne(int keep) {
    int victim = -1;
    for (int i = 0; i < int(c.pages.size()); ++i)
        if (c.pages[i].alive && i != keep &&
            (victim < 0 || c.pages[i].lastUse < c.pages[victim].lastUse))
            victim = i;
    if (victim < 0)
        return false;
    Page &p = c.pages[victim];
    c.bytes -= p.bytes();
    p = Page{};
    if (c.openA8 == victim)
        c.openA8 = -1;
    if (c.openColor == victim)
        c.openColor = -1;
    return true;
}

int newPage(bool color, int w, int h) {
    int idx = -1;
    for (int i = 0; i < int(c.pages.size()); ++i)
        if (!c.pages[i].alive) {
            idx = i;
            break;
        }
    if (idx < 0) {
        if (c.pages.size() >= 0xFFFE)
            return -1;
        c.pages.emplace_back();
        idx = int(c.pages.size() - 1);
    }
    Page &p = c.pages[idx];
    p       = Page{};
    p.w = w, p.h = h, p.color = color, p.alive = true;
    if (color)
        p.argb.assign(size_t(w) * h, 0);
    else
        p.a8.assign(size_t(w) * h, 0);
    c.bytes += p.bytes();
    bool evicted = false;
    while (c.bytes > c.budget && c.pages.size() > 1 && evictOne(idx))
        evicted = true;
    if (evicted)
        purgeDeadPages();
    return idx;
}

// Shelf packing: glyphs of similar height share a row; a full page is left
// for eviction and a new one opened.
bool place(bool color, int w, int h, int *page, int *x, int *y) {
    if (w > kPageSize || h > kPageSize) { // huge glyph (big emoji): its own page
        const int p = newPage(color, w, h);
        if (p < 0)
            return false;
        c.pages[p].full = true;
        *page = p, *x = 0, *y = 0;
        return true;
    }
    int &open = color ? c.openColor : c.openA8;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (open < 0 || !c.pages[open].alive) {
            open = newPage(color, kPageSize, kPageSize);
            if (open < 0)
                return false;
        }
        Page &p = c.pages[open];
        if (p.cursorX + w > p.w) {
            p.shelfY += p.shelfH;
            p.cursorX = 0;
            p.shelfH  = 0;
        }
        if (p.shelfY + h <= p.h) {
            *page = open, *x = p.cursorX, *y = p.shelfY;
            p.cursorX += w;
            if (h > p.shelfH)
                p.shelfH = h;
            return true;
        }
        p.full = true;
        open   = -1;
    }
    return false;
}

} // namespace

const Glyph *get(fonts::FontKey font, uint32_t glyph, uint32_t ppem64, int phase) {
    if (c.table.empty())
        c.table.assign(1024, Entry{});
    const size_t mask = c.table.size() - 1;
    for (size_t i = hashKey(font, glyph, ppem64, phase) & mask; c.table[i].used;
         i        = (i + 1) & mask) {
        Entry &e = c.table[i];
        if (e.font == font && e.glyph == glyph && e.ppem == ppem64 && e.phase == phase) {
            if (e.g.page != 0xFFFF)
                c.pages[e.g.page].lastUse = c.epoch;
            return &e.g;
        }
    }
    Entry e;
    e.font = font, e.glyph = glyph, e.ppem = ppem64, e.phase = uint8_t(phase), e.used = true;
    fonts::Raster r;
    int           page, x, y;
    if (fonts::rasterize(font, glyph, ppem64, phase, &r) && r.w > 0 && r.h > 0 && r.w < 4096 &&
        r.h < 4096 && place(r.color, r.w, r.h, &page, &x, &y)) {
        Page &p = c.pages[page];
        for (int row = 0; row < r.h; ++row) {
            if (r.color)
                std::memcpy(
                    &p.argb[size_t(y + row) * p.w + x],
                    r.argb + size_t(row) * r.pitch,
                    size_t(r.w) * 4
                );
            else { // the gamma applied on the way in: one pass, no scratch copy
                const uint8_t *src = r.a8 + ptrdiff_t(row) * r.pitch;
                uint8_t       *dst = &p.a8[size_t(y + row) * p.w + x];
                for (int k = 0; k < r.w; ++k)
                    dst[k] = r.gamma[src[k]];
            }
        }
        p.lastUse = c.epoch;
        e.g       = {
            int16_t(r.left),
            int16_t(r.top),
            uint16_t(r.w),
            uint16_t(r.h),
            uint16_t(page),
            uint16_t(x),
            uint16_t(y),
            r.color
        };
    } else {
        e.g.left = int16_t(r.left);
    }
    if ((c.count + 1) * 2 > c.table.size())
        grow();
    return &insertEntry(e)->g;
}

gfx::Mask8 mask(const Glyph &g) {
    const Page &p = c.pages[g.page];
    return {p.a8.data() + size_t(g.y) * p.w + g.x, g.w, g.h, p.w};
}

gfx::BitmapView colorView(const Glyph &g) {
    Page &p = c.pages[g.page];
    return {p.argb.data() + size_t(g.y) * p.w + g.x, g.w, g.h, p.w};
}

void tick() {
    ++c.epoch;
}
void setBudget(size_t bytes) {
    c.budget = bytes;
}
size_t bytesUsed() {
    return c.bytes;
}
size_t glyphCount() {
    return c.count;
}

bool consistent() {
    const size_t n = c.table.size(), mask = n ? n - 1 : 0;
    size_t       used = 0;
    for (size_t i = 0; i < n; ++i) {
        const Entry &e = c.table[i];
        if (!e.used)
            continue;
        ++used;
        if (e.g.page != 0xFFFF && (e.g.page >= c.pages.size() || !c.pages[e.g.page].alive))
            return false;
        for (size_t j = homeOf(e, mask); j != i; j = (j + 1) & mask) {
            const Entry &o = c.table[j];
            if (!o.used ||
                (o.font == e.font && o.glyph == e.glyph && o.ppem == e.ppem && o.phase == e.phase))
                return false;
        }
    }
    return used == c.count;
}

} // namespace text::cache

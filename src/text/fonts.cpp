#include "text/fonts.h"

#include "text/font_index.h"

#include "base/str.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H

#include <hb-ot.h>
#include <hb.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace text::fonts {

namespace {

// The fixed preference order (no fontconfig): first installed family wins.
// An app setting can override these later. The OS's own first: Segoe UI
// on Windows (the system message font), San Francisco on macOS (the system
// UI font); and for code the CSS "monospace": Courier New on Windows, Menlo on macOS.
constexpr const char *kUiFamilies[] = {
#if defined(_WIN32)
    "segoe ui",
#elif defined(__APPLE__)
    "system font", // SFNS.ttf, San Francisco's variable face
    "helvetica neue",
#endif
    "inter",
    "noto sans",
    "ubuntu",
    "cantarell",
    "dejavu sans",
    "liberation sans"
};
constexpr const char *kMonoFamilies[] = {
#if defined(_WIN32)
    "courier new",
#elif defined(__APPLE__)
    "menlo",
#endif
    "noto sans mono",
    "dejavu sans mono",
    "liberation mono"
};
// The OS's own colour emoji font first: Segoe UI
// Emoji (COLR) on Windows, Apple Color Emoji (sbix) on macOS, Noto Color
// Emoji (CBDT) on Linux.
constexpr const char *kEmojiFamilies[] = {
#if defined(_WIN32)
    "segoe ui emoji",
#elif defined(__APPLE__)
    "apple color emoji",
#endif
    "noto color emoji",
    "twemoji",
    "twitter color emoji",
    "joypixels",
    "emojione color"
};

struct Family {
    uint32_t              name = 0; // pool offset
    std::vector<uint16_t> faces;
    int                   rank = 50;
};

struct Loaded {
    bool                                          tried = false, ok = false;
    hb_face_t                                    *hbFace    = nullptr;
    hb_font_t                                    *hbDefault = nullptr;
    FT_Face                                       ft        = nullptr;
    float                                         upem      = 1000;
    FaceMetrics                                   m;
    std::vector<std::pair<uint16_t, hb_font_t *>> var; // per requested wght
    int                                           wghtAxis = -1;
    uint16_t              ftWght = 0; // variation currently set on ft (0 = default)
    std::vector<FT_Fixed> defCoords;
};

struct FileMap {
    const uint8_t *p     = nullptr;
    size_t         n     = 0;
    bool           tried = false;
};

struct Resolved {
    uint32_t fam;
    uint16_t weight;
    bool     italic;
    FontKey  key;
};

struct State {
    Index                 ix;
    std::vector<Family>   fams;
    std::vector<uint16_t> famOfFace;
    std::vector<uint16_t> fallbackOrder; // family indices by rank
    std::vector<Loaded>   loaded;
    std::vector<FileMap>  maps;
    FT_Library            ft = nullptr;
    int                   ui = -1, mono = -1, emoji = -1;
    // fallback()'s answers, open-addressed: (cp + 1) << 16 | family
    // (0xFFFF = none); 0 is an empty slot. At most half full.
    std::vector<uint64_t> fbCache;
    size_t                fbCount = 0;
    std::vector<Resolved> resolved;
    gfx::Bitmap           scratchColor;
    uint8_t               gamma[256];
};
State *g = nullptr;

using str::startsWith;

int findFamily(const char *name) {
    for (size_t i = 0; i < g->fams.size(); ++i)
        if (!std::strcmp(g->ix.str(g->fams[i].name), name))
            return int(i);
    return -1;
}

// "noto sans cjk jp" etc. from the locale, so Han characters get the
// regional glyph forms the reader expects. Japanese forms when unknown.
const char *cjkFamily() {
    const char *vars[] = {"LC_ALL", "LC_CTYPE", "LANG", "LANGUAGE"};
    const char *lang   = "";
    for (auto v : vars)
        if (const char *s = std::getenv(v); s && *s) {
            lang = s;
            break;
        }
    if (startsWith(lang, "ko"))
        return "noto sans cjk kr";
    if (startsWith(lang, "zh_TW") || startsWith(lang, "zh_Hant"))
        return "noto sans cjk tc";
    if (startsWith(lang, "zh_HK"))
        return "noto sans cjk hk";
    if (startsWith(lang, "zh"))
        return "noto sans cjk sc";
    return "noto sans cjk jp";
}

int rankFamily(const Family &f, const char *cjk) {
    const char    *n    = g->ix.str(f.name);
    const FaceRec &face = g->ix.faces[f.faces[0]];
    if (face.flags & kColor)
        return 90; // emoji fonts are chosen explicitly, not as text fallback
    if (!std::strcmp(n, cjk))
        return 0;
    if (face.flags & kMono)
        return 40;
    if (startsWith(n, "noto sans"))
        return startsWith(n, "noto sans cjk") ? 12 : 10;
    if (startsWith(n, "dejavu sans"))
        return 15;
    if (!(face.flags & kSerif))
        return 20;
    return startsWith(n, "noto serif") ? 29 : 30;
}

void buildFamilies() {
    std::vector<uint16_t> order(g->ix.faces.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = uint16_t(i);
    auto name = [](uint16_t f) { return g->ix.str(g->ix.faces[f].family); };
    // Index as tie-break keeps the order stable without std::stable_sort's
    // extra merge-sort instantiations.
    std::sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) {
        const int c = std::strcmp(name(a), name(b));
        return c ? c < 0 : a < b;
    });
    g->famOfFace.assign(order.size(), 0);
    for (uint16_t f : order) {
        if (g->fams.empty() || std::strcmp(g->ix.str(g->fams.back().name), name(f)) != 0)
            g->fams.push_back({g->ix.faces[f].family, {}, 50});
        g->fams.back().faces.push_back(f);
        g->famOfFace[f] = uint16_t(g->fams.size() - 1);
    }
    const char *cjk = cjkFamily();
    for (auto &f : g->fams)
        f.rank = rankFamily(f, cjk);
    // By rank, then index: packed as rank << 16 | index, a plain number sort.
    std::vector<uint32_t> byRank(g->fams.size());
    for (size_t i = 0; i < byRank.size(); ++i)
        byRank[i] = uint32_t(g->fams[i].rank) << 16 | uint32_t(i);
    std::sort(byRank.begin(), byRank.end());
    for (uint32_t k : byRank)
        g->fallbackOrder.push_back(uint16_t(k));
}

template <size_t N>
int firstFamily(const char *const (&names)[N]) {
    for (auto n : names)
        if (int f = findFamily(n); f >= 0)
            return f;
    return -1;
}

const FileMap &fileMap(uint32_t file) {
    FileMap &m = g->maps[file];
    if (m.tried)
        return m;
    m.tried = true;
    m.p     = mapFile(g->ix.str(g->ix.files[file].path), &m.n);
    return m;
}

Loaded &load(uint32_t face) {
    Loaded &L = g->loaded[face];
    if (L.tried)
        return L;
    L.tried            = true;
    const FaceRec &rec = g->ix.faces[face];
    const FileMap &m   = fileMap(rec.file);
    if (!m.p)
        return L;
    if (!g->ft && FT_Init_FreeType(&g->ft) != 0)
        return L;
    if (FT_New_Memory_Face(g->ft, m.p, FT_Long(m.n), rec.ttc, &L.ft) != 0) {
        L.ft = nullptr;
        return L;
    }
    // The blob borrows the mapping, which lives as long as the process.
    hb_blob_t *blob = hb_blob_create(
        reinterpret_cast<const char *>(m.p),
        unsigned(m.n),
        HB_MEMORY_MODE_READONLY,
        nullptr,
        nullptr
    );
    L.hbFace = hb_face_create(blob, rec.ttc);
    hb_blob_destroy(blob);
    L.hbDefault = hb_font_create(L.hbFace);
    L.upem      = float(hb_face_get_upem(L.hbFace));
    hb_font_set_scale(L.hbDefault, int(L.upem), int(L.upem));

    hb_font_extents_t e{};
    if (hb_font_get_h_extents(L.hbDefault, &e)) {
        L.m.ascent  = float(e.ascender) / L.upem;
        L.m.descent = float(-e.descender) / L.upem;
        L.m.lineGap = float(e.line_gap) / L.upem;
    }
    hb_position_t v;
    if (hb_ot_metrics_get_position(L.hbDefault, HB_OT_METRICS_TAG_CAP_HEIGHT, &v) && v > 0)
        L.m.capHeight = float(v) / L.upem;
    if (hb_ot_metrics_get_position(L.hbDefault, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &v))
        L.m.underlinePos = float(-v) / L.upem;
    if (hb_ot_metrics_get_position(L.hbDefault, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &v) && v > 0)
        L.m.underlineThick = float(v) / L.upem;
    if (hb_ot_metrics_get_position(L.hbDefault, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &v) && v > 0)
        L.m.strikePos = float(v) / L.upem;
    if (hb_ot_metrics_get_position(L.hbDefault, HB_OT_METRICS_TAG_STRIKEOUT_SIZE, &v) && v > 0)
        L.m.strikeThick = float(v) / L.upem;

    if (rec.wghtMax) {
        FT_MM_Var *mm = nullptr;
        if (FT_Get_MM_Var(L.ft, &mm) == 0) {
            for (FT_UInt a = 0; a < mm->num_axis; ++a) {
                L.defCoords.push_back(mm->axis[a].def);
                if (mm->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't'))
                    L.wghtAxis = int(a);
            }
            FT_Done_MM_Var(g->ft, mm);
        }
    }
    L.ok = true;
    return L;
}

// CSS-like matching inside one family: normal width first, then italic
// match, then the closest weight (variable fonts match any weight in range).
FontKey resolveIn(uint32_t fam, uint16_t weight, bool italic) {
    for (auto &r : g->resolved)
        if (r.fam == fam && r.weight == weight && r.italic == italic)
            return r.key;
    int      best      = -1;
    uint32_t bestScore = ~0u;
    for (uint16_t f : g->fams[fam].faces) {
        const FaceRec &r     = g->ix.faces[f];
        uint32_t       score = uint32_t(std::abs(int(r.width) - 5)) * 10000;
        if (bool(r.flags & kItalic) != italic)
            score += 5000;
        int d;
        if (r.wghtMax)
            d = weight < r.wghtMin   ? r.wghtMin - weight
                : weight > r.wghtMax ? weight - r.wghtMax
                                     : 0;
        else
            d = std::abs(int(r.weight) - int(weight)) * 2 + (r.weight < weight ? 1 : 0);
        score += uint32_t(d);
        if (score < bestScore)
            bestScore = score, best = f;
    }
    const FaceRec &r    = g->ix.faces[best];
    uint16_t       wght = 0, eff = r.weight;
    if (r.wghtMax) {
        eff = uint16_t(std::clamp<int>(weight, r.wghtMin, r.wghtMax));
        if (eff != r.weight)
            wght = eff;
    }
    const bool    bold = weight >= 600 && eff < 550 && !(r.flags & kBitmapOnly);
    const bool    obl  = italic && !(r.flags & kItalic) && !(r.flags & kBitmapOnly);
    const FontKey key  = makeKey(uint32_t(best), wght, bold, obl);
    g->resolved.push_back({fam, weight, italic, key});
    return key;
}

size_t fbSlot(uint32_t cp, size_t mask) {
    return size_t((cp * 0x9E3779B97F4A7C15ull) >> 32) & mask;
}

// The cached fallback family for cp, or -1 when not asked yet.
int fbFind(uint32_t cp) {
    const std::vector<uint64_t> &t = g->fbCache;
    if (t.empty())
        return -1;
    const size_t mask = t.size() - 1;
    for (size_t i = fbSlot(cp, mask); t[i]; i = (i + 1) & mask)
        if (t[i] >> 16 == uint64_t(cp) + 1)
            return int(t[i] & 0xFFFF);
    return -1;
}

void fbPut(std::vector<uint64_t> &t, uint64_t e) {
    const size_t mask = t.size() - 1;
    size_t       i    = fbSlot(uint32_t((e >> 16) - 1), mask);
    while (t[i])
        i = (i + 1) & mask;
    t[i] = e;
}

void fbInsert(uint32_t cp, uint16_t fam) {
    std::vector<uint64_t> &t = g->fbCache;
    if ((g->fbCount + 1) * 2 > t.size()) { // grow (a power of two), at most half full
        std::vector<uint64_t> grown(std::max<size_t>(256, t.size() * 2), 0);
        for (uint64_t e : t)
            if (e)
                fbPut(grown, e);
        t.swap(grown);
    }
    fbPut(t, (uint64_t(cp) + 1) << 16 | fam);
    ++g->fbCount;
}

} // namespace

bool init(std::string *error) {
    if (g)
        return true;
    auto *s = new State;
    if (!loadIndex(&s->ix, error)) {
        delete s;
        return false;
    }
    g = s;
    g->loaded.resize(g->ix.faces.size());
    g->maps.resize(g->ix.files.size());
    buildFamilies();
    g->ui    = firstFamily(kUiFamilies);
    g->mono  = firstFamily(kMonoFamilies);
    g->emoji = firstFamily(kEmojiFamilies);
    // No preferred family installed: the best-ranked family that has Latin.
    for (uint16_t fam : g->fallbackOrder) {
        const FaceRec &r = g->ix.faces[g->fams[fam].faces[0]];
        if (!g->ix.covers(r, 'a'))
            continue;
        if (g->ui < 0 && !(r.flags & (kMono | kColor)))
            g->ui = fam;
        if (g->mono < 0 && (r.flags & kMono))
            g->mono = fam;
    }
    if (g->ui < 0)
        g->ui = g->fallbackOrder[0];
    if (g->mono < 0)
        g->mono = g->ui;
    // Linear coverage looks thin when blended in sRGB; a mild boost (gamma
    // 1/1.2) brings dark-on-light text close to what users know from
    // FreeType+Cairo without colour-dependent caches.
    for (int i = 0; i < 256; ++i)
        g->gamma[i] = uint8_t(std::lround(255.0 * std::pow(i / 255.0, 1 / 1.2)));
    return true;
}

void shutdown() {
    if (!g)
        return;
    for (Loaded &L : g->loaded) {
        for (auto &[w, f] : L.var)
            hb_font_destroy(f);
        hb_font_destroy(L.hbDefault);
        hb_face_destroy(L.hbFace); // before the mapping its blob borrows
        if (L.ft)
            FT_Done_Face(L.ft);
    }
    if (g->ft)
        FT_Done_FreeType(g->ft);
    for (const FileMap &m : g->maps)
        if (m.p)
            unmapFile(m.p, m.n);
    delete g;
    g = nullptr;
}

FontKey primary(const Style &s) {
    return resolveIn(uint32_t(s.mono ? g->mono : g->ui), uint16_t(s.weight), s.italic);
}

FontKey emoji() {
    return g->emoji < 0 ? kNoFont : resolveIn(uint32_t(g->emoji), 400, false);
}

FontKey fallback(uint32_t cp, const Style &s) {
    const int found = fbFind(cp);
    uint16_t  fam;
    if (found >= 0) {
        fam = uint16_t(found);
    } else {
        fam = 0xFFFF;
        for (uint16_t f : g->fallbackOrder) {
            if (int(f) == g->emoji)
                continue;
            // Faces of a family share coverage in practice: test the first.
            if (g->ix.covers(g->ix.faces[g->fams[f].faces[0]], cp)) {
                fam = f;
                break;
            }
        }
        if (fam == 0xFFFF && g->emoji >= 0 &&
            g->ix.covers(g->ix.faces[g->fams[g->emoji].faces[0]], cp))
            fam = uint16_t(g->emoji);
        fbInsert(cp, fam);
    }
    if (fam == 0xFFFF)
        return kNoFont;
    FontKey k = resolveIn(fam, s.mono ? 400 : uint16_t(s.weight), s.italic);
    if (!hasGlyph(k, cp))
        k = makeKey(g->fams[fam].faces[0], 0, false, false);
    return k;
}

bool hasGlyph(FontKey k, uint32_t cp) {
    Loaded &L = load(faceOf(k));
    if (!L.ok)
        return false;
    hb_codepoint_t gid;
    return hb_font_get_nominal_glyph(L.hbDefault, cp, &gid);
}

hb_font_t *hbFont(FontKey k) {
    Loaded &L = load(faceOf(k));
    if (!L.ok)
        return nullptr;
    const auto w = uint16_t(wghtOf(k));
    if (!w)
        return L.hbDefault;
    for (auto &[vw, f] : L.var)
        if (vw == w)
            return f;
    hb_font_t *f = hb_font_create(L.hbFace);
    hb_font_set_scale(f, int(L.upem), int(L.upem));
    hb_variation_t v{HB_TAG('w', 'g', 'h', 't'), float(w)};
    hb_font_set_variations(f, &v, 1);
    L.var.push_back({w, f});
    return f;
}

float upem(FontKey k) {
    return load(faceOf(k)).upem;
}

bool isColor(FontKey k) {
    return g->ix.faces[faceOf(k)].flags & kColor;
}

const FaceMetrics &metrics(FontKey k) {
    return load(faceOf(k)).m;
}

bool rasterize(FontKey k, uint32_t glyph, uint32_t ppem64, int phase, Raster *out) {
    Loaded &L = load(faceOf(k));
    *out      = {};
    if (!L.ok)
        return false;
    const FaceRec &rec  = g->ix.faces[faceOf(k)];
    FT_Face        f    = L.ft;
    const float    ppem = float(ppem64) / 64.f;

    if (rec.flags & kBitmapOnly) {
        // Colour bitmap strikes (CBDT/sbix): pick the smallest strike at least
        // as big as needed (else the biggest) and resample to the pixel size.
        // A strike may lack the glyph (Apple Color Emoji's 40/48/52 have no
        // Unicode 14+ faces): then the next one up, then the biggest below.
        if (f->num_fixed_sizes <= 0)
            return false;
        auto better = [&](int i, int b) {
            const float y = float(f->available_sizes[i].y_ppem) / 64.f;
            const float s = float(f->available_sizes[b].y_ppem) / 64.f;
            return (y >= ppem && (s < ppem || y < s)) || (s < ppem && y > s);
        };
        const int        n     = std::min(f->num_fixed_sizes, 32);
        const FT_Bitmap &bm    = f->glyph->bitmap; // the slot outlives each load
        uint32_t         tried = 0;
        int              best;
        for (;;) {
            best = -1;
            for (int i = 0; i < n; ++i)
                if (!(tried >> i & 1) && (best < 0 || better(i, best)))
                    best = i;
            if (best < 0)
                return true; // empty glyph in every strike
            tried |= 1u << best;
            if (FT_Select_Size(f, best) == 0 && FT_Load_Glyph(f, glyph, FT_LOAD_COLOR) == 0 &&
                bm.pixel_mode == FT_PIXEL_MODE_BGRA && bm.width && bm.rows)
                break;
        }
        const float           s  = ppem / (float(f->available_sizes[best].y_ppem) / 64.f);
        const int             dw = std::max(1, int(std::lround(bm.width * s)));
        const int             dh = std::max(1, int(std::lround(bm.rows * s)));
        // FreeType's BGRA is premultiplied B,G,R,A bytes = our 0xAARRGGBB on
        // little-endian hosts.
        const gfx::BitmapView src{
            reinterpret_cast<uint32_t *>(bm.buffer), int(bm.width), int(bm.rows), bm.pitch / 4
        };
        // Apple Color Emoji's sbix images stand on the baseline (origin 0,0,
        // one em tall); CoreText draws them 1/8 em lower, centred 3/8 em up
        // like the digits beside them. As stored they sit visibly high.
        const float drop = FT_HAS_SBIX(f) ? ppem / 8 : 0;

        g->scratchColor = gfx::resize(src, dw, dh);
        out->w          = dw;
        out->h          = dh;
        out->pitch      = dw;
        out->left       = int(std::lround(f->glyph->bitmap_left * s));
        out->top        = int(std::lround(f->glyph->bitmap_top * s - drop));
        out->color      = true;
        out->argb       = g->scratchColor.pixels();
        return true;
    }

    if (L.wghtAxis >= 0 && L.ftWght != wghtOf(k)) {
        std::vector<FT_Fixed> c = L.defCoords;
        if (wghtOf(k))
            c[size_t(L.wghtAxis)] = FT_Fixed(wghtOf(k)) << 16;
        FT_Set_Var_Design_Coordinates(f, FT_UInt(c.size()), c.data());
        L.ftWght = uint16_t(wghtOf(k));
    }
    if (FT_Set_Char_Size(f, 0, FT_F26Dot6(ppem64), 72, 72) != 0)
        return false;
    FT_Matrix oblique{0x10000, 0x0366A, 0, 0x10000}; // ~12° slant
    FT_Vector delta{FT_Pos(phase * 16), 0};
    FT_Set_Transform(f, synthOblique(k) ? &oblique : nullptr, &delta);
    FT_Int32 flags = FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP;
    if (rec.flags & kColor)
        flags |= FT_LOAD_COLOR; // COLRv0 layers are composited by FT_Render_Glyph
    if (FT_Load_Glyph(f, glyph, flags) != 0)
        return false;
    FT_GlyphSlot slot = f->glyph;
    if (synthBold(k) && slot->format == FT_GLYPH_FORMAT_OUTLINE)
        FT_Outline_EmboldenXY(&slot->outline, FT_Pos(ppem64 / 24), 0);
    if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, FT_RENDER_MODE_LIGHT) != 0)
        return false;
    const FT_Bitmap &bm = slot->bitmap;
    out->w              = int(bm.width);
    out->h              = int(bm.rows);
    out->left           = slot->bitmap_left;
    out->top            = slot->bitmap_top;
    if (!bm.width || !bm.rows)
        return true;
    if (bm.pixel_mode == FT_PIXEL_MODE_BGRA) {
        out->color = true;
        out->argb  = reinterpret_cast<const uint32_t *>(bm.buffer);
        out->pitch = bm.pitch / 4;
        return true;
    }
    if (bm.pixel_mode != FT_PIXEL_MODE_GRAY)
        return false;
    out->a8    = bm.buffer; // FreeType's own: the reader applies the gamma
    out->pitch = bm.pitch;
    out->gamma = g->gamma;
    return true;
}

std::string familyName(FontKey k) {
    if (k == kNoFont)
        return {};
    return g->ix.str(g->ix.faces[faceOf(k)].family);
}

size_t faceCount() {
    return g ? g->ix.faces.size() : 0;
}

} // namespace text::fonts

// gfx unit tests: pixel-exact painter expectations, decoders, icons.
// Run one group: gfx_tests <group> (ctest registers each group separately).
#include "gfx/gfx.h"
#include "gfx/icons_generated.h"
#include "gfx/internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace gfx;

// Live heap bytes, for the allocation checks (svglayers). The replacement
// operator new serves the whole test binary.
namespace {
constexpr size_t kHeapHeader = alignof(std::max_align_t);
size_t           g_heapLive = 0, g_heapPeak = 0, g_heapLargest = 0;
bool             g_heapTrack = false;
} // namespace

void *operator new(size_t n) {
    char *p = static_cast<char *>(std::malloc(n + kHeapHeader));
    if (!p)
        std::abort(); // no exceptions in this build
    std::memcpy(p, &n, sizeof n);
    g_heapLive += n;
    if (g_heapTrack)
        g_heapPeak = std::max(g_heapPeak, g_heapLive), g_heapLargest = std::max(g_heapLargest, n);
    return p + kHeapHeader;
}
void operator delete(void *q) noexcept {
    if (!q)
        return;
    char  *p = static_cast<char *>(q) - kHeapHeader;
    size_t n;
    std::memcpy(&n, p, sizeof n);
    g_heapLive -= n;
    std::free(p);
}
void operator delete(void *q, size_t) noexcept {
    operator delete(q);
}

namespace {

int g_fail = 0;

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);             \
            ++g_fail;                                                                              \
        }                                                                                          \
    } while (0)
#define CHECK_PX(a, b)                                                                             \
    do {                                                                                           \
        const uint32_t va_ = (a), vb_ = (b);                                                       \
        if (va_ != vb_) {                                                                          \
            std::fprintf(                                                                          \
                stderr, "%s:%d: %s = %08x, expected %08x\n", __FILE__, __LINE__, #a, va_, vb_      \
            );                                                                                     \
            ++g_fail;                                                                              \
        }                                                                                          \
    } while (0)

uint32_t px(const Bitmap &b, int x, int y) {
    return b.pixels()[y * b.width() + x];
}
int alphaAt(const Bitmap &b, int x, int y) {
    return int(px(b, x, y) >> 24);
}
double alphaSum(const Bitmap &b) {
    double s = 0;
    for (int i = 0; i < b.width() * b.height(); ++i)
        s += (b.pixels()[i] >> 24) / 255.0;
    return s;
}
int countNonZero(const Bitmap &b) {
    int n = 0;
    for (int i = 0; i < b.width() * b.height(); ++i)
        n += b.pixels()[i] != 0;
    return n;
}
void fillBitmap(Bitmap &b, uint32_t v) {
    for (int i = 0; i < b.width() * b.height(); ++i)
        b.pixels()[i] = v;
}
bool readFile(const std::string &path, std::string *out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char   buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out->append(buf, n);
    std::fclose(f);
    return true;
}
bool premulValid(const Bitmap &b) {
    for (int i = 0; i < b.width() * b.height(); ++i) {
        const uint32_t p = b.pixels()[i], a = p >> 24;
        if (((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a)
            return false;
    }
    return true;
}
// Mirror symmetries of the alpha channel, pixel-exact.
bool symmetricX(const Bitmap &b) {
    for (int y = 0; y < b.height(); ++y)
        for (int x = 0; x < b.width(); ++x)
            if (alphaAt(b, x, y) != alphaAt(b, b.width() - 1 - x, y))
                return false;
    return true;
}
bool symmetricY(const Bitmap &b) {
    for (int y = 0; y < b.height(); ++y)
        for (int x = 0; x < b.width(); ++x)
            if (alphaAt(b, x, y) != alphaAt(b, x, b.height() - 1 - y))
                return false;
    return true;
}
bool symmetricDiag(const Bitmap &b) {
    for (int y = 0; y < b.height(); ++y)
        for (int x = 0; x < b.width(); ++x)
            if (alphaAt(b, x, y) != alphaAt(b, y, x))
                return false;
    return true;
}

const Color kRed = rgb(0xff0000), kWhite = rgb(0xffffff), kBlack = rgb(0);

// ── groups ──────────────────────────────────────────────────────────────────
void testBlend() {
    // mulPx is exactly round(c·a/255) for every channel value and factor.
    bool exact = true;
    for (uint32_t c = 0; c < 256; ++c)
        for (uint32_t a = 0; a < 256; ++a) {
            const uint32_t want = uint32_t(std::floor(c * a / 255.0 + 0.5));
            const uint32_t got  = mulPx(c | (c << 8) | (c << 16) | (c << 24), a);
            exact &= got == (want | (want << 8) | (want << 16) | (want << 24));
        }
    CHECK(exact);
    CHECK_PX(premultiply(rgba(0xff0000, 0x80), 1), 0x80800000u);
    CHECK_PX(premultiply(kWhite, 0.5f), 0x80808080u);
    CHECK_PX(premultiply(rgba(0xffffff, 0), 1), 0u);
    CHECK_PX(over(0xff123456u, 0xffabcdefu), 0xff123456u);
    CHECK_PX(over(0, 0xffabcdefu), 0xffabcdefu);
    CHECK_PX(over(0x80000000u, 0xffffffffu), 0xff7f7f7fu);
    CHECK_PX(withAlpha(rgb(0x102030), 0.5f), 0x80102030u);

    // No darkening drift: translucent white over white, many times.
    Bitmap b(4, 1);
    fillBitmap(b, 0xffffffffu);
    Painter p(b.view(), 1);
    for (int i = 0; i < 200; ++i)
        p.fillRect({0, 0, 4, 1}, rgba(0xffffff, 0x40));
    CHECK_PX(px(b, 0, 0), 0xffffffffu);
    // ...and on transparent: alpha settles at 8-bit precision (254 is the
    // fixed point of 64 + a·191/255) and the colour stays exactly white.
    Bitmap  t(1, 1);
    Painter q(t.view(), 1);
    for (int i = 0; i < 200; ++i)
        q.fillRect({0, 0, 1, 1}, rgba(0xffffff, 0x40));
    CHECK_PX(px(t, 0, 0), 0xfefefefeu);
}

void testFill() {
    {
        Bitmap  b(10, 10);
        Painter p(b.view(), 1);
        p.fillRect({2, 3, 4, 5}, kRed);
        CHECK(countNonZero(b) == 20);
        CHECK_PX(px(b, 2, 3), 0xffff0000u);
        CHECK_PX(px(b, 5, 7), 0xffff0000u);
        CHECK_PX(px(b, 6, 7), 0u);
        CHECK_PX(px(b, 5, 8), 0u);
    }
    { // scale 2: logical 1..3 → physical 2..6
        Bitmap  b(8, 8);
        Painter p(b.view(), 2);
        p.fillRect({1, 1, 2, 2}, kRed);
        CHECK(countNonZero(b) == 16);
        CHECK_PX(px(b, 2, 2), 0xffff0000u);
        CHECK_PX(px(b, 6, 6), 0u);
    }
    { // fractional edges: half coverage each side, premultiplied
        Bitmap  b(3, 1);
        Painter p(b.view(), 1);
        p.fillRect({0.5f, 0, 1, 1}, kRed);
        CHECK_PX(px(b, 0, 0), 0x80800000u);
        CHECK_PX(px(b, 1, 0), 0x80800000u);
        CHECK_PX(px(b, 2, 0), 0u);
    }
    { // fractional scale 1.5: 2 logical px = 3 physical, on the grid
        Bitmap  b(6, 1);
        Painter p(b.view(), 1.5f);
        p.fillRect({0, 0, 2, 1}, kRed);
        CHECK_PX(px(b, 2, 0), 0xffff0000u);
        CHECK_PX(px(b, 3, 0), 0u);
        CHECK(alphaAt(b, 0, 0) == 255);
    }
    { // translate + opacity + save/restore
        Bitmap  b(6, 1);
        Painter p(b.view(), 1);
        p.save();
        p.translate(3, 0);
        p.setOpacity(0.5f);
        p.fillRect({0, 0, 1, 1}, kWhite);
        p.restore();
        p.fillRect({0, 0, 1, 1}, kWhite);
        CHECK_PX(px(b, 3, 0), 0x80808080u);
        CHECK_PX(px(b, 0, 0), 0xffffffffu);
        CHECK(countNonZero(b) == 2);
    }
    { // out of bounds is clipped, never written
        Bitmap  b(4, 4);
        Painter p(b.view(), 1);
        p.fillRect({-100, -100, 1000, 1000}, kRed);
        CHECK(countNonZero(b) == 16);
        p.fillRect({1e9f, 1e9f, 5, 5}, kBlack);
        p.fillRoundRect({-1e9f, -1e9f, 5, 5}, 2, kBlack);
        CHECK_PX(px(b, 3, 3), 0xffff0000u);
    }
}

void testClip() {
    {
        Bitmap  b(10, 10);
        Painter p(b.view(), 1);
        p.save();
        p.clipRect({2, 2, 4, 4});
        const RectF cb = p.clipBounds();
        CHECK(cb.x == 2 && cb.y == 2 && cb.w == 4 && cb.h == 4);
        p.fillRect({0, 0, 10, 10}, kRed);
        CHECK(countNonZero(b) == 16);
        p.restore();
        p.fillRect({0, 0, 1, 1}, kRed);
        CHECK(countNonZero(b) == 17);
    }
    { // translation moves clipBounds into local coordinates
        Bitmap  b(10, 10);
        Painter p(b.view(), 2);
        p.clipRect({1, 1, 2, 2});
        p.translate(1, 1);
        const RectF cb = p.clipBounds();
        CHECK(cb.x == 0 && cb.y == 0 && cb.w == 2 && cb.h == 2);
    }
    { // round clip = circle: symmetric, AA, nothing outside
        Bitmap  b(20, 20);
        Painter p(b.view(), 1);
        p.clipRoundRect({0, 0, 20, 20}, 10);
        p.fillRect({0, 0, 20, 20}, kRed);
        CHECK(alphaAt(b, 0, 0) == 0);
        CHECK(alphaAt(b, 10, 10) == 255);
        CHECK(alphaAt(b, 0, 10) > 0 && alphaAt(b, 0, 10) < 255);
        CHECK(symmetricX(b) && symmetricY(b) && symmetricDiag(b));
        CHECK(std::fabs(alphaSum(b) - 3.14159265 * 100) < 3);
        CHECK(premulValid(b));
    }
    { // nested round clips multiply; restore pops the inner one only
        Bitmap  b(20, 20);
        Painter p(b.view(), 1);
        p.clipRoundRect({0, 0, 20, 20}, 10);
        p.save();
        p.clipRoundRect({10, 0, 10, 20}, 5);
        p.fillRect({0, 0, 20, 20}, kRed);
        CHECK(alphaAt(b, 5, 10) == 0);    // outside the inner clip
        CHECK(alphaAt(b, 14, 10) == 255); // inside both
        CHECK(alphaAt(b, 19, 10) < 255);  // circle edge
        p.restore();
        p.fillRect({0, 0, 20, 20}, kRed);
        CHECK(alphaAt(b, 5, 10) == 255);
        CHECK(alphaAt(b, 0, 0) == 0); // outer circle still clips
    }
}

void testRoundRect() {
    {
        Bitmap  b(20, 20);
        Painter p(b.view(), 1);
        p.fillRoundRect({0, 0, 20, 20}, 6, kRed);
        CHECK(symmetricX(b) && symmetricY(b) && symmetricDiag(b));
        CHECK(alphaAt(b, 0, 0) == 0);
        CHECK(alphaAt(b, 0, 10) == 255);
        CHECK(alphaAt(b, 10, 10) == 255);
        CHECK(alphaAt(b, 1, 1) > 0 && alphaAt(b, 1, 1) < 255);
        const double area = 400 - (4 - 3.14159265) * 36;
        CHECK(std::fabs(alphaSum(b) - area) < 1.5);
    }
    { // fractional placement keeps coverage == area
        Bitmap  b(24, 24);
        Painter p(b.view(), 1);
        p.fillRoundRect({2.3f, 1.7f, 15.5f, 11.2f}, 4, kWhite);
        const double area = 15.5 * 11.2 - (4 - 3.14159265) * 16;
        CHECK(std::fabs(alphaSum(b) - area) < 1.5);
        CHECK(premulValid(b));
    }
    {
        Bitmap  b(21, 21);
        Painter p(b.view(), 1);
        p.fillCircle({10.5f, 10.5f}, 8, kRed);
        CHECK(symmetricX(b) && symmetricY(b) && symmetricDiag(b));
        CHECK(std::fabs(alphaSum(b) - 3.14159265 * 64) < 1.5);
    }
    { // strokes stay inside the outline
        Bitmap  b(12, 12);
        Painter p(b.view(), 1);
        p.strokeRoundRect({1, 1, 10, 10}, 0, 1, kRed);
        CHECK(alphaAt(b, 0, 0) == 0 && alphaAt(b, 11, 5) == 0);
        CHECK_PX(px(b, 1, 1), 0xffff0000u);
        CHECK_PX(px(b, 10, 5), 0xffff0000u);
        CHECK_PX(px(b, 5, 5), 0u);
        CHECK(countNonZero(b) == 36);
    }
    {
        Bitmap  b(40, 40);
        Painter p(b.view(), 1);
        p.strokeCircle({20, 20}, 15, 3, kRed);
        CHECK(alphaAt(b, 20, 20) == 0);
        CHECK(alphaAt(b, 20, 6) == 255);
        CHECK(symmetricX(b) && symmetricY(b) && symmetricDiag(b));
        CHECK(std::fabs(alphaSum(b) - 3.14159265 * (225 - 144)) < 2);
    }
    { // a fractional rounded border: coverage == ring area, the hole untouched
        Bitmap  b(40, 30);
        Painter p(b.view(), 1);
        p.strokeRoundRect({2.3f, 1.7f, 30, 20}, 6, 2.5f, kWhite);
        const double outer = 30 * 20 - (4 - 3.14159265) * 36;
        const double inner = 25 * 15 - (4 - 3.14159265) * 3.5 * 3.5;
        CHECK(std::fabs(alphaSum(b) - (outer - inner)) < 2);
        CHECK(premulValid(b));
        Bitmap  bg(40, 30);
        Painter q(bg.view(), 1);
        fillBitmap(bg, 0xff0000ffu);
        q.clipRoundRect({0, 0, 40, 30}, 8);
        q.strokeRoundRect({2.3f, 1.7f, 30, 20}, 6, 2.5f, kRed);
        for (int y = 6; y < 17; ++y)
            for (int x = 6; x < 27; ++x)
                CHECK_PX(px(bg, x, y), 0xff0000ffu);
        CHECK_PX(px(bg, 1, 10), 0xff0000ffu);  // left of the border
        CHECK_PX(px(bg, 3, 10), 0xffff0000u);  // in it
        CHECK_PX(px(bg, 17, 2), 0xffff0000u);  // the top edge
        CHECK_PX(px(bg, 17, 23), 0xff0000ffu); // below it
    }
}

Path square(float x0, float y0, float x1, float y1, bool cw) {
    Path p;
    p.moveTo(x0, y0);
    if (cw) {
        p.lineTo(x1, y0);
        p.lineTo(x1, y1);
        p.lineTo(x0, y1);
    } else {
        p.lineTo(x0, y1);
        p.lineTo(x1, y1);
        p.lineTo(x1, y0);
    }
    p.close();
    return p;
}

void append(Path &dst, const Path &src) {
    const float *pt = src.pts().data();
    for (uint8_t c : src.cmds()) {
        switch (c) {
        case Path::Move:
            dst.moveTo(pt[0], pt[1]), pt += 2;
            break;
        case Path::Line:
            dst.lineTo(pt[0], pt[1]), pt += 2;
            break;
        case Path::Cubic:
            dst.cubicTo(pt[0], pt[1], pt[2], pt[3], pt[4], pt[5]), pt += 6;
            break;
        case Path::Close:
            dst.close();
            break;
        }
    }
}

void testPath() {
    { // pixel-aligned polygon: exact
        Bitmap  b(10, 10);
        Painter p(b.view(), 1);
        p.fillPath(square(2, 2, 8, 8, true), kRed);
        CHECK(countNonZero(b) == 36);
        CHECK_PX(px(b, 2, 2), 0xffff0000u);
        CHECK_PX(px(b, 7, 7), 0xffff0000u);
    }
    { // non-zero: same direction nested = filled, opposite = hole
        Bitmap b(10, 10), h(10, 10);
        Path   same = square(0, 0, 10, 10, true), hole = square(0, 0, 10, 10, true);
        append(same, square(3, 3, 7, 7, true));
        append(hole, square(3, 3, 7, 7, false));
        Painter p(b.view(), 1), q(h.view(), 1);
        p.fillPath(same, kRed);
        q.fillPath(hole, kRed);
        CHECK(countNonZero(b) == 100);
        CHECK(countNonZero(h) == 84);
        CHECK(alphaAt(h, 5, 5) == 0);
    }
    { // coverage sums equal the true areas
        Bitmap  b(20, 20);
        Painter p(b.view(), 1);
        Path    t;
        t.moveTo(1, 1);
        t.lineTo(19, 1);
        t.lineTo(1, 15);
        t.close();
        p.fillPath(t, kWhite);
        CHECK(std::fabs(alphaSum(b) - 18 * 14 / 2.0) < 0.5);
        CHECK(premulValid(b));
        Bitmap  c(40, 40);
        Painter q(c.view(), 2);
        Path    circ;
        circ.addCircle(10, 10, 8);
        q.fillPath(circ, kWhite);
        std::printf("  circle area %.2f (expect %.2f)\n", alphaSum(c), 3.14159265 * 256);
        // Flattened chords sit inside the curve: ~0.07 px on average.
        CHECK(std::fabs(alphaSum(c) - 3.14159265 * 256) < 8);
        Bitmap  d(20, 20);
        Painter r(d.view(), 1);
        Path    qd;
        qd.moveTo(0, 20); // the quadratic through (10, -20), as its cubic
        qd.cubicTo(20 / 3.f, -20 / 3.f, 40 / 3.f, -20 / 3.f, 20, 20);
        qd.close();
        r.fillPath(qd, kWhite);
        std::printf("  parabola area %.2f (expect %.2f)\n", alphaSum(d), 2 / 3.0 * 400);
        CHECK(std::fabs(alphaSum(d) - 2 / 3.0 * 20 * 20) < 2); // parabolic segment: ⅔·base·height
    }
    { // half-pixel vertical edge: exact 50%
        Bitmap  b(4, 4);
        Painter p(b.view(), 1);
        p.fillPath(square(0.5f, 0, 2, 4, true), kRed);
        CHECK(alphaAt(b, 0, 2) == 128);
        CHECK(alphaAt(b, 1, 2) == 255);
    }
    { // clipped path equals the unclipped one inside the clip
        Bitmap a(20, 20), b(20, 20);
        Path   t;
        t.moveTo(-5, 3);
        t.lineTo(25, 7);
        t.lineTo(12, 30);
        t.close();
        Painter pa(a.view(), 1), pb(b.view(), 1);
        pa.fillPath(t, kRed);
        pb.clipRect({5, 5, 10, 10});
        pb.fillPath(t, kRed);
        bool same = true;
        for (int y = 5; y < 15; ++y)
            for (int x = 5; x < 15; ++x)
                same &= px(a, x, y) == px(b, x, y);
        CHECK(same);
        CHECK(alphaAt(b, 4, 10) == 0 && alphaAt(b, 16, 10) == 0);
    }
}

void testStroke() {
    { // a 2px horizontal stroke on the grid with round caps
        Bitmap  b(12, 10);
        Painter p(b.view(), 1);
        Path    l;
        l.moveTo(3, 5);
        l.lineTo(9, 5);
        p.strokePath(l, 2, kRed);
        for (int x = 3; x < 9; ++x)
            CHECK(alphaAt(b, x, 4) == 255 && alphaAt(b, x, 5) == 255);
        CHECK(alphaAt(b, 5, 3) == 0 && alphaAt(b, 5, 6) == 0);
        CHECK(alphaAt(b, 2, 4) > 0 && alphaAt(b, 2, 4) < 255); // cap
        CHECK(std::fabs(alphaSum(b) - (12 + 3.14159265)) < 0.4);
    }
    { // crossing strokes union: the crossing is not denser than the arms
        Bitmap  b(20, 20);
        Painter p(b.view(), 1);
        Path    x;
        x.moveTo(2, 2);
        x.lineTo(18, 18);
        x.moveTo(18, 2);
        x.lineTo(2, 18);
        p.strokePath(x, 2, kRed);
        CHECK(alphaAt(b, 10, 10) == 255 && alphaAt(b, 9, 9) == 255);
        CHECK(symmetricX(b) && symmetricY(b));
        // A polyline turning back on itself: overlapping segments and joins
        // still union (the area of one capsule, not two).
        Bitmap  c(20, 20);
        Painter q(c.view(), 1);
        Path    v;
        v.moveTo(4, 10);
        v.lineTo(16, 10);
        v.lineTo(4, 10);
        q.strokePath(v, 2, kRed);
        CHECK(std::fabs(alphaSum(c) - (24 + 3.14159265)) < 0.5);
    }
    {
        Bitmap  b(10, 10);
        Painter p(b.view(), 1);
        p.drawLine({1, 5}, {9, 5}, 2, kRed);
        CHECK(countNonZero(b) == 16);
        CHECK_PX(px(b, 1, 4), 0xffff0000u);
        Bitmap  d(20, 20);
        Painter q(d.view(), 1);
        q.drawLine({2, 2}, {18, 14}, 2, kWhite);
        CHECK(std::fabs(alphaSum(d) - 20 * 2) < 0.5);
    }
}

void testBitmap() {
    Bitmap src(2, 2);
    src.pixels()[0] = 0xffff0000u;
    src.pixels()[1] = 0xff00ff00u;
    src.pixels()[2] = 0xff0000ffu;
    src.pixels()[3] = 0x80808080u;
    { // 1:1 at an integer position = exact copy
        Bitmap  b(4, 4);
        Painter p(b.view(), 1);
        p.drawBitmap(src.view(), {1, 1, 2, 2});
        CHECK_PX(px(b, 1, 1), 0xffff0000u);
        CHECK_PX(px(b, 2, 1), 0xff00ff00u);
        CHECK_PX(px(b, 1, 2), 0xff0000ffu);
        CHECK_PX(px(b, 2, 2), 0x80808080u);
        CHECK(countNonZero(b) == 4);
    }
    { // nearest 2×
        Bitmap  b(4, 4);
        Painter p(b.view(), 1);
        p.drawBitmap(src.view(), {0, 0, 4, 4}, Sampling::Nearest);
        CHECK_PX(px(b, 0, 0), 0xffff0000u);
        CHECK_PX(px(b, 1, 1), 0xffff0000u);
        CHECK_PX(px(b, 2, 1), 0xff00ff00u);
        CHECK_PX(px(b, 3, 3), 0x80808080u);
    }
    { // flat colour stays flat under bilinear and smooth scaling
        Bitmap flat(3, 3);
        fillBitmap(flat, 0xff336699u);
        for (Sampling s : {Sampling::Bilinear, Sampling::Smooth}) {
            Bitmap  up(7, 7), down(2, 2);
            Painter p(up.view(), 1), q(down.view(), 1);
            p.drawBitmap(flat.view(), {0, 0, 7, 7}, s);
            q.drawBitmap(flat.view(), {0, 0, 2, 2}, s);
            bool ok = true;
            for (int i = 0; i < 49; ++i)
                ok &= up.pixels()[i] == 0xff336699u;
            for (int i = 0; i < 4; ++i)
                ok &= down.pixels()[i] == 0xff336699u;
            CHECK(ok);
        }
    }
    { // smooth shrink = area average: half white / half black → mid grey
        Bitmap s(4, 4);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                s.pixels()[y * 4 + x] = x < 2 ? 0xffffffffu : 0xff000000u;
        Bitmap  b(1, 1);
        Painter p(b.view(), 1);
        p.drawBitmap(s.view(), {0, 0, 1, 1}, Sampling::Smooth);
        CHECK_PX(px(b, 0, 0), 0xff808080u);
        const Bitmap r = resize(s.view(), 2, 1);
        CHECK_PX(r.pixels()[0], 0xffffffffu);
        CHECK_PX(r.pixels()[1], 0xff000000u);
        const Bitmap r3 = resize(s.view(), 3, 3); // non-integer ratio
        CHECK(premulValid(r3));
        CHECK_PX(r3.pixels()[0], 0xffffffffu);
        CHECK_PX(r3.pixels()[2], 0xff000000u);
        CHECK(((r3.pixels()[1] >> 8) & 255) >= 126 && ((r3.pixels()[1] >> 8) & 255) <= 129);
    }
    { // opacity and round clip apply to images
        Bitmap w(8, 8);
        fillBitmap(w, 0xffffffffu);
        Bitmap  b(8, 8);
        Painter p(b.view(), 1);
        p.save();
        p.clipRoundRect({0, 0, 8, 8}, 4);
        p.drawBitmap(w.view(), {0, 0, 8, 8}, Sampling::Smooth, 0.5f);
        p.restore();
        CHECK_PX(px(b, 4, 4), 0x80808080u);
        CHECK(alphaAt(b, 0, 0) == 0);
        CHECK(symmetricX(b) && symmetricDiag(b));
    }
}

void testBlit() {
    const uint8_t mask[6] = {0, 128, 255, 255, 128, 0};
    const Mask8   m{mask, 3, 2, 3};
    {
        Bitmap  b(5, 4);
        Painter p(b.view(), 2);
        p.translate(0.5f, 0.5f); // → physical origin (1, 1)
        p.blitMask(m, 1, 0, kRed);
        CHECK_PX(px(b, 2, 1), 0u);
        CHECK_PX(px(b, 3, 1), 0x80800000u);
        CHECK_PX(px(b, 4, 1), 0xffff0000u);
        CHECK_PX(px(b, 2, 2), 0xffff0000u);
        CHECK_PX(px(b, 3, 2), 0x80800000u);
        CHECK(countNonZero(b) == 4);
        const PointF o = p.toPhysical({1, 0});
        CHECK(o.x == 3 && o.y == 1);
    }
    { // clipped + partially outside the target
        Bitmap  b(3, 3);
        Painter p(b.view(), 1);
        p.clipRect({0, 0, 3, 1});
        p.blitMask(m, -1, 0, kRed);
        CHECK_PX(px(b, 0, 0), 0x80800000u);
        CHECK_PX(px(b, 1, 0), 0xffff0000u);
        CHECK(countNonZero(b) == 2);
    }
    {
        Bitmap s(2, 1);
        s.pixels()[0] = 0xffffffffu;
        s.pixels()[1] = 0x80000000u;
        Bitmap d(3, 1);
        fillBitmap(d, 0xffffffffu);
        Painter p(d.view(), 1);
        p.blitColor(s.view(), 1, 0);
        CHECK_PX(px(d, 0, 0), 0xffffffffu);
        CHECK_PX(px(d, 1, 0), 0xffffffffu);
        CHECK_PX(px(d, 2, 0), 0xff7f7f7fu);
        Bitmap  e(2, 1);
        Painter q(e.view(), 1);
        q.blitColor(s.view(), 0, 0, 0.5f);
        CHECK_PX(px(e, 0, 0), 0x80808080u);
        CHECK_PX(px(e, 1, 0), 0x40000000u);
    }
}

void testGradient() {
    Bitmap  b(256, 2);
    Painter p(b.view(), 1);
    p.fillRectGradient({0, 0, 256, 2}, {0, 0}, kBlack, {256, 0}, kWhite);
    CHECK(alphaAt(b, 0, 0) == 255 && alphaAt(b, 255, 1) == 255);
    CHECK((px(b, 0, 0) & 255) <= 1);
    CHECK((px(b, 255, 0) & 255) >= 254);
    bool mono = true;
    for (int x = 1; x < 256; ++x)
        mono &= (px(b, x, 0) & 255) >= (px(b, x - 1, 0) & 255);
    CHECK(mono);
    CHECK(std::abs(int(px(b, 128, 0) & 255) - 128) <= 1);
    // Translucent ends interpolate in premultiplied space.
    Bitmap  t(10, 1);
    Painter q(t.view(), 1);
    q.fillRectGradient({0, 0, 10, 1}, {0, 0}, rgba(0xff0000, 0), {10, 0}, kRed);
    CHECK(premulValid(t));
    CHECK(alphaAt(t, 0, 0) < 20 && alphaAt(t, 9, 0) > 235);
}

void testShadow() {
    Bitmap  b(60, 60);
    Painter p(b.view(), 1);
    p.dropShadow({15, 15, 30, 30}, 4, 8, rgba(0, 0x80));
    CHECK(alphaAt(b, 30, 30) == 0);                                // not under the box
    CHECK(alphaAt(b, 14, 30) > 0x30 && alphaAt(b, 14, 30) < 0x60); // ~half at the edge
    CHECK(alphaAt(b, 0, 0) == 0);
    // Horizontal-then-vertical box passes round differently: ±1 at most.
    bool diag = true;
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 60; ++x)
            diag &= std::abs(alphaAt(b, x, y) - alphaAt(b, y, x)) <= 1;
    CHECK(diag);
    Bitmap  c(60, 60);
    Painter q(c.view(), 1);
    q.dropShadow({15, 15, 30, 30}, 4, 8, rgba(0, 0x80)); // cached mask
    CHECK(std::memcmp(b.pixels(), c.pixels(), 60 * 60 * 4) == 0);
    // An offset shadow shows its solid part where the box does not cover it.
    Bitmap  o(60, 60);
    Painter po(o.view(), 1);
    po.dropShadow({15, 15, 30, 30}, 4, 8, {0, 10}, rgba(0, 0x80));
    CHECK(alphaAt(o, 30, 46) >= 0x78 && alphaAt(o, 30, 30) == 0);
    // A large shadow is stretched from a corner-sized mask: no seams (smooth
    // between neighbours), mirror-symmetric, the same edge profile throughout.
    Bitmap  big(400, 200);
    Painter r(big.view(), 1);
    r.dropShadow({20, 20, 360, 160}, 10, 12, rgb(0));
    auto inBox  = [](int x, int y) { return x >= 20 && x < 380 && y >= 20 && y < 180; };
    bool smooth = true, mirror = true;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 400; ++x) {
            if (x > 0 && !inBox(x, y) && !inBox(x - 1, y))
                smooth &= std::abs(alphaAt(big, x, y) - alphaAt(big, x - 1, y)) <= 24;
            if (y > 0 && !inBox(x, y) && !inBox(x, y - 1))
                smooth &= std::abs(alphaAt(big, x, y) - alphaAt(big, x, y - 1)) <= 24;
            mirror &= std::abs(alphaAt(big, x, y) - alphaAt(big, 399 - x, y)) <= 1;
        }
    CHECK(smooth && mirror);
    CHECK(alphaAt(big, 200, 100) == 0 && alphaAt(big, 0, 100) == 0);
    for (int y = 0; y < 20; ++y)
        CHECK(
            alphaAt(big, 120, y) == alphaAt(big, 280, y) &&
            alphaAt(big, 120, y) == alphaAt(big, 200, y)
        );
    CHECK(alphaAt(big, 19, 100) > 90 && alphaAt(big, 19, 100) < 165); // ~50% at the edge
}

// 32×8 JPEG: left half red, right half blue (ImageMagick, quality 95, 4:4:4).
const unsigned char kJpeg[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
    0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
    0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
    0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08, 0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08,
    0x0b, 0x0c, 0x0b, 0x0a, 0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0a, 0x07, 0x06, 0x07, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0xff, 0xc0,
    0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x20, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
    0x01, 0xff, 0xc4, 0x00, 0x15, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xc4,
    0x00, 0x17, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0a, 0x08, 0x09, 0xff, 0xc4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00,
    0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x8b, 0xd9, 0x4d, 0xdf, 0xc0,
    0x12, 0x1a, 0xaa, 0x13, 0x5e, 0x03, 0xff, 0xd9,
};

void testDecode() {
    const std::string dir = MSGA_TEST_ASSETS;
    for (const char *name : {"alex", "mira", "deploybot"}) {
        std::string bytes;
        CHECK(readFile(dir + "/avatars/" + name + ".png", &bytes));
        Bitmap b;
        CHECK(decodeImage(bytes, &b));
        CHECK(b.width() == 256 && b.height() == 256);
        CHECK(alphaAt(b, 128, 128) == 255);
        CHECK(premulValid(b));
        // Truncated data fails cleanly (stb may also return a partial image).
        Bitmap     t;
        const bool ok = decodeImage(std::string_view(bytes).substr(0, bytes.size() / 3), &t);
        CHECK(!ok || (t.width() == 256 && t.height() == 256));
    }
    {
        std::string bytes;
        CHECK(readFile(dir + "/gifs/bouncing-ball.gif", &bytes));
        Bitmap b;
        CHECK(decodeImage(bytes, &b));
        CHECK(b.width() == 240 && b.height() == 160);
    }
    {
        Bitmap b;
        CHECK(
            decodeImage(std::string_view(reinterpret_cast<const char *>(kJpeg), sizeof kJpeg), &b)
        );
        CHECK(b.width() == 32 && b.height() == 8);
        if (b.width() == 32 && b.height() == 8) {
            const uint32_t l = px(b, 4, 4), r = px(b, 28, 4);
            CHECK((l >> 24) == 255 && ((l >> 16) & 255) > 240 && (l & 255) < 16);
            CHECK(((r >> 16) & 255) < 16 && (r & 255) > 240);
        }
    }
    Bitmap junk;
    CHECK(!decodeImage("not an image at all", &junk));
    CHECK(!decodeImage("", &junk));
    CHECK(!decodeImage(std::string_view("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16), &junk));

    // What callers show as a picture: only formats this build decodes.
    CHECK(canDecodeImage(std::string_view("\x89PNG\r\n\x1a\n", 8)));
    CHECK(canDecodeImage("GIF89a"));
    CHECK(canDecodeImage(std::string_view(reinterpret_cast<const char *>(kJpeg), 16)));
    CHECK(!canDecodeImage(std::string_view("BM\x36\0\0\0", 6))); // BMP: not in stb's subset
    CHECK(!canDecodeImage(""));
    CHECK(!canDecodeImage("<svg xmlns='http://www.w3.org/2000/svg'/>"));
    // A still lossless WebP header (VP8L, 4x4) and an animated one (VP8X with
    // the animation flag): neither decodes without libwebp, the animated one
    // never (decodeImage has no frames for it).
    const std::string_view webpStill("RIFF\x1a\0\0\0WEBPVP8L\x0d\0\0\0\x2f\x03\xc0\0\0", 25);
    const std::string_view webpAnim(
        "RIFF\x1e\0\0\0WEBPVP8X\x0a\0\0\0\x02\0\0\0\x03\0\0\x03\0\0", 30
    );
#ifdef MSGA_GFX_WEBP
    CHECK(canDecodeImage(webpStill));
#else
    CHECK(!canDecodeImage(webpStill));
#endif
    CHECK(!canDecodeImage(webpAnim));
}

void testAnim() {
    const std::string dir = MSGA_TEST_ASSETS;
    for (const char *name : {"bouncing-ball", "party-confetti", "pulse-rings", "spinning-square"}) {
        std::string bytes;
        CHECK(readFile(dir + "/gifs/" + name + ".gif", &bytes));
        std::vector<AnimFrame> frames;
        CHECK(decodeAnimation(bytes, &frames));
        CHECK(frames.size() > 1);
        bool ok = true, differ = false;
        for (const auto &f : frames) {
            ok &= f.frame.width() == 240 && f.frame.height() == 160 && f.delayMs >= 20;
            ok &= premulValid(f.frame);
        }
        for (size_t i = 1; i < frames.size(); ++i)
            differ |=
                std::memcmp(frames[i].frame.pixels(), frames[0].frame.pixels(), 240 * 160 * 4) != 0;
        CHECK(ok && differ);
        std::printf(
            "  %s: %zu frames, first delay %d ms\n",
            name,
            frames.size(),
            frames.empty() ? 0 : frames[0].delayMs
        );
        // Truncated and bit-flipped files must fail or decode, never crash.
        for (size_t cut = 13; cut < bytes.size(); cut += bytes.size() / 17) {
            std::vector<AnimFrame> part;
            decodeAnimation(std::string_view(bytes).substr(0, cut), &part);
            std::string flipped = bytes;
            flipped[cut]        = char(flipped[cut] ^ 0x5a);
            decodeAnimation(flipped, &part);
        }
    }
    std::string png;
    CHECK(readFile(dir + "/avatars/sam.png", &png));
    std::vector<AnimFrame> one;
    CHECK(decodeAnimation(png, &one) && one.size() == 1);
}

// The frame budget and the decode-time fit (P11): frames shrunk as they are
// decoded are the frames coverResize makes of the full-size ones, and an
// animation past the budget keeps its first frame only.
void testAnimBudget() {
    std::string bytes;
    CHECK(readFile(std::string(MSGA_TEST_ASSETS) + "/gifs/party-confetti.gif", &bytes));
    std::vector<AnimFrame> full, fit;
    CHECK(decodeAnimation(bytes, &full) && full.size() > 2);
    if (full.size() < 3)
        return;
    AnimOptions o;
    o.width  = 80;
    o.height = 60;
    CHECK(decodeAnimation(bytes, &fit, o));
    CHECK(fit.size() == full.size());
    bool same = fit.size() == full.size();
    for (size_t i = 0; same && i < fit.size(); ++i) {
        const Bitmap want = coverResize(full[i].frame.view(), 80, 60);
        same &= fit[i].frame.width() == 80 && fit[i].frame.height() == 60 &&
                fit[i].delayMs == full[i].delayMs &&
                std::memcmp(fit[i].frame.pixels(), want.pixels(), 80 * 60 * 4) == 0;
    }
    CHECK(same);
    // Exactly the budget: every frame stays.
    std::vector<AnimFrame> capped;
    o.maxPixels = int64_t(80) * 60 * int64_t(full.size());
    CHECK(decodeAnimation(bytes, &capped, o) && capped.size() == full.size());
    // One pixel short: the first frame alone, still.
    o.maxPixels -= 1;
    CHECK(decodeAnimation(bytes, &capped, o) && capped.size() == 1);
    CHECK(
        capped.size() == 1 &&
        std::memcmp(capped[0].frame.pixels(), fit[0].frame.pixels(), 80 * 60 * 4) == 0
    );
    // The default budget is about frames × canvas: a 240×160 animation fits.
    CHECK(AnimOptions{}.maxPixels >= int64_t(240) * 160 * int64_t(full.size()));
}

// Regression: a real GIF stopped animating partway through because its LZW
// stream never cleared and overflowed the dictionary. Built synthetically
// here (a single-color GIF whose stream never clears) since the real file
// isn't ours to redistribute.
void testGifLzwDictionaryLimit() {
    constexpr int lzwCs = 2, clear = 1 << lzwCs, eos = clear + 1;
    constexpr int w = 100, h = 100, n = w * h; // enough pixels to pass the old 8192-entry ceiling

    // Variable-width LZW bit packer, LSB-first per the GIF spec.
    struct BitWriter {
        std::vector<uint8_t> bytes;
        int                  bitPos = 0;
        void                 write(int value, int bits) {
            for (int i = 0; i < bits; ++i) {
                const int bit = (value >> i) & 1;
                if (bitPos % 8 == 0)
                    bytes.push_back(0);
                bytes.back() |= uint8_t(bit << (bitPos % 8));
                ++bitPos;
            }
        }
    } bw;

    // Encode every pixel as the literal color index 0, with no clear code
    // in between -- the dictionary just keeps growing, same as the real
    // file that triggered this bug.
    int codesize = lzwCs + 1, avail = clear + 2, oldcode = -1;
    bw.write(clear, codesize);
    for (int i = 0; i < n; ++i) {
        bw.write(0, codesize);
        if (oldcode >= 0 && avail < 4096)
            ++avail;
        if ((avail & ((1 << codesize) - 1)) == 0 && avail <= 0x0FFF)
            ++codesize;
        oldcode = 0;
    }
    bw.write(eos, codesize);

    // Wrap it in a minimal, otherwise-ordinary single-frame GIF.
    std::string gif = "GIF89a";
    auto        u16 = [&](int v) {
        gif.push_back(char(v & 0xff));
        gif.push_back(char((v >> 8) & 0xff));
    };
    u16(w);
    u16(h);
    gif.push_back(char(0x80));                         // global color table, 2 entries
    gif.push_back(0);                                  // background color index
    gif.push_back(0);                                  // pixel aspect ratio
    gif += std::string("\x00\x00\x00\xff\xff\xff", 6); // black, white

    gif.push_back(char(0x2C)); // image descriptor
    u16(0);
    u16(0);
    u16(w);
    u16(h);
    gif.push_back(0); // no local color table, not interlaced

    gif.push_back(char(lzwCs));
    for (size_t i = 0; i < bw.bytes.size(); i += 255) { // GIF data sub-blocks, 255 bytes max each
        const size_t n2 = std::min(size_t(255), bw.bytes.size() - i);
        gif.push_back(char(n2));
        gif.append(reinterpret_cast<const char *>(bw.bytes.data() + i), n2);
    }
    gif.push_back(0);          // sub-block terminator
    gif.push_back(char(0x3B)); // trailer

    std::vector<AnimFrame> frames;
    CHECK(decodeAnimation(gif, &frames) && frames.size() == 1);
    CHECK(frames.size() == 1 && frames[0].frame.width() == w && frames[0].frame.height() == h);
}

// M17: a bitmap shrunk once to snappedSize() paints exactly what a Smooth
// drawBitmap of the original paints (which shrinks on every call).
void testSnapped() {
    Bitmap src(97, 61);
    for (int y = 0; y < 61; ++y)
        for (int x = 0; x < 97; ++x)
            src.pixels()[y * 97 + x] =
                0xff000000u | uint32_t(x * 2) << 16 | uint32_t(y * 4) << 8 | uint32_t(x ^ y);
    int checked = 0;
    for (float scale : {1.f, 1.25f, 1.5f, 2.f})
        for (float off : {0.f, 0.3f, 0.5f, 0.77f})
            for (RectF dst : {RectF{3, 4, 40, 25}, RectF{1.5f, 2.25f, 33.3f, 20.1f}}) {
                Bitmap  a(120, 90), b(120, 90);
                Painter pa(a.view(), scale), pb(b.view(), scale);
                pa.translate(off, off);
                pb.translate(off, off);
                pa.drawBitmap(src.view(), dst, Sampling::Smooth);
                int dw = 0, dh = 0;
                pb.snappedSize(dst, &dw, &dh);
                CHECK(dw > 0 && dh > 0 && dw < 97 && dh < 61);
                const Bitmap shrunk = resize(src.view(), dw, dh);
                pb.drawBitmap(shrunk.view(), dst, Sampling::Smooth);
                CHECK(std::memcmp(a.pixels(), b.pixels(), 120 * 90 * 4) == 0);
                ++checked;
            }
    CHECK(checked == 32);
}

void testIcons() {
    CHECK(kIconCount == 91); // gfx/ui/*.svg + the tray plane + the logo
    for (int size : {16, 20, 24, 48}) {
        int bad = 0;
        for (int i = 0; i < kIconCount; ++i) {
            Bitmap  b(size + 4, size + 4);
            Painter p(b.view(), 1);
            p.translate(2, 2);
            drawIcon(p, Icon(i), {0, 0, float(size), float(size)}, kBlack);
            const double cov    = alphaSum(b) / double(size * size);
            // Nothing reaches the outer pixel of the 2px margin.
            bool         margin = true;
            for (int k = 0; k < size + 4; ++k)
                margin &= alphaAt(b, k, 0) == 0 && alphaAt(b, 0, k) == 0 &&
                          alphaAt(b, k, size + 3) == 0 && alphaAt(b, size + 3, k) == 0;
            if (cov < 0.03 || cov > 0.85 || !margin || !premulValid(b)) {
                ++bad;
                std::fprintf(
                    stderr, "icon %d at %d px: coverage %.3f margin %d\n", i, size, cov, margin
                );
            }
        }
        CHECK(bad == 0);
    }
    { // plus at 24 px: the arms are 2 px wide on the grid, crisp
        Bitmap  b(24, 24);
        Painter p(b.view(), 1);
        drawIcon(p, Icon::Plus, {0, 0, 24, 24}, kRed);
        CHECK_PX(px(b, 11, 11), 0xffff0000u);
        CHECK_PX(px(b, 12, 8), 0xffff0000u);
        CHECK_PX(px(b, 7, 12), 0xffff0000u);
        CHECK_PX(px(b, 2, 2), 0u);
        CHECK_PX(px(b, 12, 2), 0u);
        CHECK(symmetricX(b) && symmetricY(b) && symmetricDiag(b));
        // Painter opacity applies.
        Bitmap  c(24, 24);
        Painter q(c.view(), 1);
        q.setOpacity(0.5f);
        drawIcon(q, Icon::Plus, {0, 0, 24, 24}, kRed);
        CHECK_PX(px(c, 11, 11), 0x80800000u);
    }
    { // turned about the centre: the plus at 90° is itself, at 45° an ×
        Bitmap  a(24, 24), b(24, 24), c(24, 24);
        Painter pa(a.view(), 1), pb(b.view(), 1), pc(c.view(), 1);
        drawIcon(pa, Icon::Plus, {0, 0, 24, 24}, kRed);
        drawIconRotated(pb, Icon::Plus, {0, 0, 24, 24}, kRed, 90);
        drawIconRotated(pc, Icon::Plus, {0, 0, 24, 24}, kRed, 45);
        CHECK(std::abs(alphaSum(a) - alphaSum(b)) < alphaSum(a) * 0.02);
        CHECK(std::abs(alphaSum(a) - alphaSum(c)) < alphaSum(a) * 0.1);
        CHECK_PX(px(b, 11, 11), 0xffff0000u);
        CHECK(alphaAt(c, 12, 7) < 0x40);  // the upright arm is gone
        CHECK(alphaAt(c, 8, 8) > 0xc0);   // a diagonal one is there
        CHECK(alphaAt(c, 15, 15) > 0xc0); // and its other half
        CHECK(premulValid(c));
    }
    { // fixed-colour parts keep their colour (the Slack mark is 4 colours)
        Bitmap  b(48, 48);
        Painter p(b.view(), 1);
        drawIcon(p, Icon::SlackMark, {0, 0, 48, 48}, kBlack);
        int colours = 0;
        for (uint32_t c : {0xffe01e5au, 0xff36c5f0u, 0xff2eb67du, 0xffecb22eu}) {
            bool found = false;
            for (int i = 0; i < 48 * 48; ++i)
                found |= b.pixels()[i] == c;
            colours += found;
        }
        CHECK(colours == 4);
    }
}

// ── Painter: gradients, even-odd, stroke styles ─────────────────────────────
uint32_t unpremul(uint32_t p) {
    const uint32_t a = p >> 24;
    if (!a)
        return 0;
    auto ch = [a](uint32_t c) { return (c * 255 + a / 2) / a; };
    return (a << 24) | (ch((p >> 16) & 255) << 16) | (ch((p >> 8) & 255) << 8) | ch(p & 255);
}
// Straight-colour comparison with a per-channel tolerance; alpha 0 expects
// transparency only.
bool near(uint32_t got, uint32_t want, int tol) {
    got = unpremul(got);
    if ((want >> 24) == 0)
        return (got >> 24) <= uint32_t(tol);
    for (int sh = 0; sh < 32; sh += 8)
        if (std::abs(int((got >> sh) & 255) - int((want >> sh) & 255)) > tol)
            return false;
    return true;
}

void testPaint2() {
    { // linear gradient fill, horizontal, premultiplied interpolation
        Bitmap   b(100, 10);
        Painter  p(b.view(), 1);
        Gradient g;
        g.a     = {0, 0};
        g.b     = {100, 0};
        g.stops = {{0, rgb(0xff0000)}, {1, rgb(0x0000ff)}};
        Path r;
        r.moveTo(0, 0), r.lineTo(100, 0), r.lineTo(100, 10), r.lineTo(0, 10), r.close();
        p.fillPath(r, g);
        CHECK(near(px(b, 0, 5), 0xfffe0001u, 3));
        CHECK(near(px(b, 99, 5), 0xff0100feu, 3));
        CHECK(near(px(b, 50, 5), 0xff7f0080u, 3));
        // Pad beyond the ends; a scale-2 painter maps logical → gradient space.
        Bitmap  c(40, 4);
        Painter q(c.view(), 2);
        g.a = {5, 0}, g.b = {15, 0};
        Path r2;
        r2.moveTo(0, 0), r2.lineTo(20, 0), r2.lineTo(20, 2), r2.lineTo(0, 2), r2.close();
        q.fillPath(r2, g);
        CHECK(near(px(c, 2, 1), 0xffff0000u, 1) && near(px(c, 38, 1), 0xff0000ffu, 1));
        CHECK(near(px(c, 20, 1), 0xff800080u, 8));
    }
    { // radial with repeat, and a transformed gradient
        Bitmap   b(40, 40);
        Painter  p(b.view(), 1);
        Gradient g;
        g.kind   = Gradient::Kind::Radial;
        g.a      = {20, 20};
        g.radius = 10;
        g.spread = Gradient::Spread::Repeat;
        g.stops  = {{0, rgb(0xffffff)}, {1, rgb(0x000000)}};
        Path r;
        r.moveTo(0, 0), r.lineTo(40, 0), r.lineTo(40, 40), r.lineTo(0, 40), r.close();
        p.fillPath(r, g);
        CHECK(near(px(b, 20, 20), 0xffffffffu, 24));
        CHECK(near(px(b, 20, 29), 0xff1a1a1au, 16)); // t ≈ 0.95
        CHECK(near(px(b, 20, 31), 0xffe6e6e6u, 16)); // repeated: t ≈ 0.05
    }
    { // even-odd vs non-zero on a self-overlapping path
        Path t;
        t.moveTo(0, 0), t.lineTo(20, 0), t.lineTo(20, 20), t.lineTo(0, 20), t.close();
        t.moveTo(5, 5), t.lineTo(15, 5), t.lineTo(15, 15), t.lineTo(5, 15),
            t.close(); // same direction
        Bitmap  a(20, 20), e(20, 20);
        Painter pa(a.view(), 1), pe(e.view(), 1);
        pa.fillPath(t, kRed, FillRule::NonZero);
        pe.fillPath(t, kRed, FillRule::EvenOdd);
        CHECK(alphaAt(a, 10, 10) == 255 && alphaAt(e, 10, 10) == 0 && alphaAt(e, 2, 2) == 255);
    }
    { // caps: butt stops at the end, square/round extend by half the width
        auto run = [](Stroke::Cap cap, int x) {
            Bitmap  b(40, 20);
            Painter p(b.view(), 1);
            Path    l;
            l.moveTo(10, 10), l.lineTo(30, 10);
            Stroke st;
            st.width = 6, st.cap = cap;
            p.strokePath(l, st, kRed);
            return alphaAt(b, x, 10);
        };
        CHECK(run(Stroke::Cap::Butt, 9) == 0 && run(Stroke::Cap::Butt, 10) == 255);
        CHECK(run(Stroke::Cap::Square, 7) == 255 && run(Stroke::Cap::Square, 6) == 0);
        CHECK(run(Stroke::Cap::Round, 7) > 128 && run(Stroke::Cap::Round, 6) == 0);
    }
    { // joins at a right angle: miter fills the corner, bevel cuts it, round in between
        auto corner = [](Stroke::Join j) {
            Bitmap  b(40, 40);
            Painter p(b.view(), 1);
            Path    l;
            l.moveTo(10, 30), l.lineTo(10, 10), l.lineTo(30, 10);
            Stroke st;
            st.width = 8, st.cap = Stroke::Cap::Butt, st.join = j;
            p.strokePath(l, st, kRed);
            return alphaAt(b, 6, 6); // the outer corner pixel
        };
        const int m = corner(Stroke::Join::Miter), r = corner(Stroke::Join::Round),
                  bv = corner(Stroke::Join::Bevel);
        CHECK(m == 255 && bv == 0 && r < m);
    }
    { // dashes, in logical units (scale 2)
        Bitmap  b(80, 10);
        Painter p(b.view(), 2);
        Path    l;
        l.moveTo(0, 2.5f), l.lineTo(40, 2.5f);
        const float dash[] = {5, 5};
        Stroke      st;
        st.width = 2, st.cap = Stroke::Cap::Butt, st.dashes = dash, st.dashCount = 2;
        p.strokePath(l, st, kRed);
        CHECK(alphaAt(b, 5, 5) == 255 && alphaAt(b, 15, 5) == 0 && alphaAt(b, 25, 5) == 255);
        // A "0 4" pattern with round caps draws dots.
        Bitmap      d(40, 10);
        Painter     q(d.view(), 1);
        const float dots[] = {0, 10};
        st.width = 4, st.cap = Stroke::Cap::Round, st.dashes = dots, st.dashCount = 2;
        Path l2;
        l2.moveTo(5, 5), l2.lineTo(35, 5);
        q.strokePath(l2, st, kRed);
        CHECK(alphaAt(d, 5, 5) > 200 && alphaAt(d, 15, 5) > 200 && alphaAt(d, 10, 5) == 0);
    }
}

// ── SVG ─────────────────────────────────────────────────────────────────────
std::string fixture(const char *name) {
    std::string s;
    CHECK(readFile(std::string(MSGA_TEST_ASSETS "/svg") + "/" + name, &s));
    return s;
}

struct Probe {
    int      x, y;
    uint32_t want; // straight 0xAARRGGBB
    int      tol;
};

void checkSvg(const char *name, std::initializer_list<Probe> probes, int size = 100) {
    const std::string svg = fixture(name);
    Bitmap            b;
    const bool        ok = renderSvg(svg, size, size, &b);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(premulValid(b));
    for (const Probe &pr : probes) {
        if (!near(px(b, pr.x, pr.y), pr.want, pr.tol)) {
            std::fprintf(
                stderr,
                "  %s at %d,%d: %08x, expected ~%08x\n",
                name,
                pr.x,
                pr.y,
                unpremul(px(b, pr.x, pr.y)),
                pr.want
            );
            ++g_fail;
        }
    }
}

void testSvg() {
    // Hex colours (SVG, theme files, attachment bars): every length, either
    // case, with or without '#'.
    Color c = 0;
    CHECK(parseHexColor("#1d1C1d", &c) && c == 0xff1d1c1du);
    CHECK(parseHexColor("abc", &c) && c == 0xffaabbccu);
    CHECK(parseHexColor("#abcd", &c) && c == 0xddaabbccu);
    CHECK(parseHexColor("#11223380", &c) && c == 0x80112233u);
    for (const char *bad : {"", "#", "#12", "#12345", "#1234567", "#123456789", "#12g", "red"}) {
        c = 7;
        CHECK(!parseHexColor(bad, &c) && c == 7);
    }
    const uint32_t T = 0; // transparent
    checkSvg(
        "shapes.svg",
        {{25, 25, 0xffff0000, 2},
         {75, 25, 0xff00ff00, 2},
         {56, 6, T, 8},
         {25, 75, 0xff0000ff, 2},
         {75, 75, 0xffffff00, 2},
         {75, 92, 0xffff00ff, 8},
         {10, 50, 0xff000000, 8},
         {50, 25, 0xff00ffff, 2},
         {2, 2, T, 2}}
    );
    checkSvg(
        "path.svg",
        {{15, 15, 0xffff0000, 2},
         {45, 15, 0xff00ff00, 2},
         {75, 15, 0xff0000ff, 2},
         {15, 45, 0xffffff00, 2},
         {45, 45, 0xffff00ff, 2},
         {75, 45, 0xff00ffff, 2},
         {7, 67, 0xff000000, 2},
         {15, 75, T, 2},
         {37, 67, 0xff808080, 2},
         {45, 75, T, 2},
         {80, 60, 0xff800000, 2},
         {80, 80, 0xff800000, 2},
         {66, 52, T, 2}}
    );
    checkSvg(
        "stroke.svg",
        {{15, 10, T, 2},
         {22, 10, 0xffff0000, 2},
         {50, 10, 0xffff0000, 2},
         {16, 30, 0xff00ff00, 40},
         {13, 30, T, 2},
         {16, 50, 0xff0000ff, 2},
         {13, 50, T, 2},
         {15, 70, 0xff000000, 2},
         {25, 70, T, 2},
         {35, 70, 0xff000000, 2},
         {50, 78, 0xffff00ff, 2}}
    );
    checkSvg(
        "transform.svg",
        {{60, 10, 0xffff0000, 2},
         {85, 5, 0xff00ff00, 2},
         {25, 75, 0xff0000ff, 2},
         {25, 63, 0xff0000ff, 2},
         {14, 64, T, 2},
         {70, 70, 0xffffff00, 2},
         {10, 45, 0xffff00ff, 2},
         {85, 15, 0xff00ffff, 2}}
    );
    checkSvg(
        "gradients.svg",
        {{1, 10, 0xfffb0004, 8},
         {1, 10, 0xffff0000, 48},
         {98, 10, 0xff0400fb, 8},
         {98, 10, 0xff0000ff, 56},
         {50, 10, 0xff800080, 10},
         {10, 26, 0xfff8000a, 10},
         {10, 64, 0xff0a00f8, 10},
         {50, 30, 0xff818181, 8},
         {50, 30, 0xff818181, 24},
         {65, 65, 0xffffffff, 12},
         {65, 47, 0xff1aff1a, 24},
         {25, 95, 0xffff8000, 12},
         {50, 95, 0xffffff00, 12},
         {75, 95, 0xffff8000, 12},
         {35, 45, 0xffff00ff, 2}}
    );
    checkSvg(
        "opacity.svg",
        {{20, 20, 0x80ff0000, 3},
         {40, 40, 0x80ff0000, 3},
         {60, 60, 0x80ff0000, 3},
         {20, 85, 0x800000ff, 3},
         {75, 85, 0x800000ff, 3},
         {59, 85, 0x800000ff, 3}}
    );
    checkSvg(
        "clip.svg",
        {{50, 50, 0xffff0000, 2},
         {50, 15, T, 2},
         {50, 23, 0xffff0000, 2},
         {2, 2, 0xff0000ff, 2},
         {10, 10, T, 2}}
    );
    checkSvg(
        "use.svg",
        {{20, 20, 0xffff0000, 2},
         {70, 20, 0xff0000ff, 2},
         {25, 75, 0xff00ff00, 2},
         {90, 90, 0xff000000, 2},
         {50, 50, T, 2}}
    );
    checkSvg(
        "colors.svg",
        {{10, 10, 0xffff0000, 2},
         {30, 10, 0xff0000ff, 2},
         {50, 10, 0xffffff00, 2},
         {70, 10, 0xff000080, 2},
         {90, 10, 0xff00ff00, 2},
         {10, 30, 0xffff00ff, 2},
         {30, 30, 0x80ff0000, 3},
         {50, 30, 0x800000ff, 3},
         {70, 30, 0x8800ff00, 3},
         {90, 30, 0xffffa500, 2},
         {10, 50, 0xff00ffff, 2},
         {30, 50, 0xff00ffff, 2},
         {50, 50, 0xffff0000, 2},
         {70, 50, T, 2},
         {90, 50, T, 2}, // display="none"
         {10, 70, T, 2}, // visibility="hidden"
         {30, 70, 0xff000000, 2}}
    );
    checkSvg(
        "aspect.svg",
        {{25, 50, 0xffff0000, 2}, {75, 50, 0xff0000ff, 2}, {50, 10, T, 2}, {50, 90, T, 2}}
    );
    // Text, filters, masks and CSS are skipped; the rest renders.
    checkSvg(
        "unsupported.svg",
        {{20, 20, 0xff00ff00, 2}, {70, 20, 0xffff0000, 2}, {20, 70, 0xff0000ff, 2}, {70, 70, T, 2}}
    );
    checkSvg(
        "inkscape-badge.svg", {{32, 10, 0xff923bbd, 24}, {1, 1, T, 2}, {22, 37, 0xffffffff, 2}}, 64
    );
    checkSvg("logo-orbit.svg", {{50, 50, 0xffecb22e, 2}, {1, 1, T, 2}}, 100);
    checkSvg("logo-signal.svg", {{50, 50, 0xff2eb67d, 2}, {50, 30, T, 2}}, 100);

    // The repo's own artwork renders too.
    for (const char *f :
         {"icon.svg",
          "icon_tray.svg",
          "claude_code_avatar.svg",
          "roles/designer.svg",
          "roles/engineer.svg",
          "roles/marketer.svg",
          "roles/researcher.svg",
          "ui/slack-mark.svg"}) {
        std::string svg;
        CHECK(readFile(std::string(GFX_REPO_ART) + "/" + f, &svg));
        Bitmap b;
        CHECK(renderSvg(svg, 64, 64, &b) && alphaSum(b) > 64 * 64 * 0.2);
    }
    { // engineer.svg: blue tile with a white code glyph
        std::string svg;
        readFile(std::string(GFX_REPO_ART) + "/roles/engineer.svg", &svg);
        Bitmap b;
        CHECK(renderSvg(svg, 128, 128, &b));
        CHECK(near(px(b, 10, 64), 0xff2f6fdb, 2) && near(px(b, 0, 0), 0, 2));
    }
}

void testSvgSize() {
    float w = 0, h = 0;
    CHECK(svgSize(fixture("aspect.svg"), &w, &h) && w == 192 && h == 96);
    CHECK(svgSize(fixture("shapes.svg"), &w, &h) && w == 100 && h == 100);
    CHECK(svgSize(fixture("logo-signal.svg"), &w, &h) && w == 96 && h == 96);
    CHECK(svgSize(fixture("inkscape-badge.svg"), &w, &h) && std::fabs(w - 241.9f) < 0.5f && w == h);
    CHECK(svgSize("<svg width='30' viewBox='0 0 60 20'/>", &w, &h) && w == 30 && h == 10);
    CHECK(svgSize("<svg width='100%' viewBox='0 0 60 20'/>", &w, &h) && w == 60 && h == 20);
    CHECK(!svgSize("<svg/>", &w, &h));
    CHECK(!svgSize("<html><body/></html>", &w, &h));
    CHECK(!svgSize("", &w, &h));
    CHECK(!svgSize("\x89PNG\r\n", &w, &h));
    Bitmap b;
    CHECK(!renderSvg("not svg", 10, 10, &b));
    CHECK(!renderSvg(fixture("shapes.svg"), 0, 10, &b));
    CHECK(!renderSvg(fixture("shapes.svg"), 100000, 100000, &b));
    // No viewBox, width/height only: that is the coordinate system.
    CHECK(renderSvg(
        "<svg width='10' height='10'><rect width='5' height='10' fill='red'/></svg>", 20, 20, &b
    ));
    CHECK(near(px(b, 4, 10), 0xffff0000u, 2) && near(px(b, 15, 10), 0, 2));
}

void testSvgFuzz() {
    // Truncated, bit-flipped and hostile input: fail or render, never crash
    // or hang (ASan clean).
    const char *names[] = {
        "shapes.svg",
        "path.svg",
        "stroke.svg",
        "gradients.svg",
        "clip.svg",
        "use.svg",
        "colors.svg",
        "inkscape-badge.svg",
        "logo-orbit.svg",
        "transform.svg",
        "unsupported.svg"
    };
    uint32_t rng = 12345;
    for (const char *n : names) {
        const std::string svg = fixture(n);
        Bitmap            b;
        for (size_t cut = 0; cut < svg.size(); cut += svg.size() / 60 + 1)
            renderSvg(std::string_view(svg).substr(0, cut), 24, 24, &b);
        for (int i = 0; i < 200; ++i) {
            std::string f = svg;
            for (int k = 0; k < 3; ++k) {
                rng                      = rng * 1103515245 + 12345;
                f[(rng >> 8) % f.size()] = char(rng >> 24);
            }
            renderSvg(f, 24, 24, &b);
        }
    }
    Bitmap b;
    CHECK(!renderSvg("<<<<>>>>&&&&;;;", 16, 16, &b));
    CHECK(!renderSvg("<svg", 16, 16, &b) || true);
    renderSvg(
        "<svg viewBox='0 0 1 1'><path d='M0 0L1e30 1e30L-1e30 0z M 0 0 A 1e30 1e-30 0 1 1 1 "
        "1'/></svg>",
        16,
        16,
        &b
    );
    renderSvg(
        "<svg viewBox='0 0 0 0' width='-5' height='nan'><rect width='1e39' height='1'/></svg>",
        16,
        16,
        &b
    );
    renderSvg(
        "<svg><path d='M0 0 c' stroke='red' stroke-dasharray='0.0000001 0'/></svg>", 16, 16, &b
    );
    // Exponential <use> fan-out ("billion laughs") stops at the shape budget.
    std::string bomb = "<svg viewBox='0 0 10 10'><defs><rect id='l0' width='1' height='1'/>";
    for (int i = 1; i < 10; ++i) {
        bomb += "<g id='l" + std::to_string(i) + "'>";
        for (int k = 0; k < 10; ++k)
            bomb += "<use href='#l" + std::to_string(i - 1) + "'/>";
        bomb += "</g>";
    }
    bomb += "</defs><use href='#l9'/></svg>";
    CHECK(renderSvg(bomb, 16, 16, &b));
    // Deep nesting beyond the depth limit.
    std::string deep = "<svg viewBox='0 0 10 10'>";
    for (int i = 0; i < 20000; ++i)
        deep += "<g opacity='0.99' clip-path='url(#c)'>";
    deep += "<rect width='10' height='10'/>";
    CHECK(renderSvg(deep, 16, 16, &b));
    // Too many elements.
    std::string many = "<svg>";
    for (int i = 0; i < 210000; ++i)
        many += "<g/>";
    CHECK(!renderSvg(many + "</svg>", 16, 16, &b));
}

uint32_t g_rnd = 1;
uint32_t rnd() {
    g_rnd = g_rnd * 1664525u + 1013904223u;
    return g_rnd >> 8;
}
Bitmap randomPremul(int w, int h) {
    Bitmap b(w, h);
    for (int i = 0; i < w * h; ++i) {
        const uint32_t a = (rnd() & 1) ? 255 : rnd() & 255;
        b.pixels()[i]    = (a << 24) | ((rnd() & 255) * a / 255 << 16) |
                           ((rnd() & 255) * a / 255 << 8) | ((rnd() & 255) * a / 255);
    }
    return b;
}
Bitmap transposed(const Bitmap &b) {
    Bitmap t(b.height(), b.width());
    for (int y = 0; y < b.height(); ++y)
        for (int x = 0; x < b.width(); ++x)
            t.pixels()[x * b.height() + y] = px(b, x, y);
    return t;
}
bool samePixels(const Bitmap &a, const Bitmap &b) {
    return a.width() == b.width() && a.height() == b.height() &&
           std::memcmp(a.pixels(), b.pixels(), size_t(a.width()) * size_t(a.height()) * 4) == 0;
}

// The vertical pass runs a row at a time, the horizontal one a pixel at a
// time: resizing one axis must give the same pixels either way round.
void testResizeAxes() {
    g_rnd = 7;
    for (int c = 0; c < 120; ++c) {
        const int    w = 1 + int(rnd() % 90), h = 1 + int(rnd() % (c % 8 == 0 ? 700 : 90));
        const int    nh  = 1 + int(rnd() % 120);
        const Bitmap src = randomPremul(w, h);
        // src w×h → w×nh is all vertical pass; its transpose all horizontal.
        const Bitmap v   = resize(src.view(), w, nh);
        const Bitmap t   = transposed(src);
        const Bitmap hz  = resize(t.view(), nh, w);
        CHECK(samePixels(transposed(hz), v));
        CHECK(premulValid(v));
    }
}

// A PaintScratch lent to painters one after another (different widths,
// scales and contents) paints exactly what each painter's own scratch does.
void paintScene(Painter &p, uint32_t seed) {
    g_rnd  = seed;
    auto f = [](float lo, float hi) { return lo + (hi - lo) * float(rnd() % 1000) / 1000.f; };
    p.fillRect({0, 0, 400, 300}, rgb(0xf0f0f0));
    for (int i = 0; i < 12; ++i) {
        p.save();
        if (i % 3 == 0)
            p.clipRoundRect({f(0, 40), f(0, 40), f(60, 200), f(60, 160)}, f(2, 20));
        Path path;
        path.moveTo(f(0, 150), f(0, 120));
        path.cubicTo(f(0, 150), f(0, 120), f(0, 150), f(0, 120), f(0, 150), f(0, 120));
        path.lineTo(f(0, 150), f(0, 120));
        path.close();
        const Color c = rgba(rnd() & 0xffffff, uint8_t(120 + rnd() % 136));
        if (i % 2)
            p.fillPath(path, c, i % 4 == 1 ? FillRule::EvenOdd : FillRule::NonZero);
        else
            p.strokePath(path, f(0.5f, 5), c);
        const Bitmap img = randomPremul(1 + int(rnd() % 40), 1 + int(rnd() % 40));
        p.drawBitmap(img.view(), {f(0, 120), f(0, 100), f(1, 90), f(1, 70)}, Sampling(i % 3));
        drawIconRotated(p, Icon(i % kIconCount), {f(0, 120), f(0, 100), 18, 18}, kBlack, f(0, 360));
        p.restore();
    }
}

void testScratch() {
    PaintScratch shared;
    const int    widths[] = {300, 120, 401, 64};
    const float  scales[] = {1, 1.5f, 2, 1.25f};
    for (int i = 0; i < 8; ++i) {
        const int w = widths[i % 4];
        Bitmap    own(w, 200), lent(w, 200);
        {
            Painter p(own.view(), scales[i % 4]);
            paintScene(p, uint32_t(100 + i));
        }
        {
            Painter p(lent.view(), scales[i % 4], &shared);
            paintScene(p, uint32_t(100 + i));
        }
        CHECK(samePixels(own, lent));
        CHECK(countNonZero(own) > w * 100);
    }
}

void testCover() {
    // 6×2, a column per value: covering 2×2 keeps the middle two columns.
    Bitmap wide(6, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 6; ++x)
            wide.pixels()[y * 6 + x] = 0xff000000u | uint32_t(x);
    Bitmap sq = coverResize(wide.view(), 2, 2);
    CHECK(sq.width() == 2 && sq.height() == 2);
    CHECK_PX(px(sq, 0, 0), 0xff000002u);
    CHECK_PX(px(sq, 1, 1), 0xff000003u);
    // Tall to wide: the middle rows, then a resize (2×6 → crop 2×1 → 4×2).
    Bitmap tall(2, 6);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 2; ++x)
            tall.pixels()[y * 2 + x] = y == 2 || y == 3 ? 0xffffffffu : 0xff000000u;
    Bitmap band = coverResize(tall.view(), 2, 2);
    CHECK(band.width() == 2 && band.height() == 2);
    CHECK_PX(px(band, 0, 0), 0xffffffffu);
    CHECK_PX(px(band, 1, 1), 0xffffffffu);
    CHECK(coverResize(tall.view(), 8, 4).width() == 8);
    CHECK(coverResize(BitmapView{}, 4, 4).empty());
    CHECK(coverResize(tall.view(), 0, 4).empty());

    // Masks: a circle clears the corners and keeps the centre.
    Bitmap disc(20, 20);
    fillBitmap(disc, 0xffffffffu);
    maskRoundedRect(disc, 1e9f);
    CHECK_PX(px(disc, 0, 0), 0u);
    CHECK_PX(px(disc, 19, 19), 0u);
    CHECK_PX(px(disc, 10, 10), 0xffffffffu);
    CHECK_PX(px(disc, 10, 1), 0xffffffffu); // just inside the top edge
    // Premultiplied: a partly covered pixel scales every channel alike.
    const uint32_t rim = px(disc, 2, 3);
    CHECK((rim >> 24) > 0 && (rim >> 24) < 255 && (rim & 0xff) == (rim >> 24));
    Bitmap rounded(20, 20);
    fillBitmap(rounded, 0xffffffffu);
    maskRoundedRect(rounded, 4);
    CHECK_PX(px(rounded, 0, 0), 0u);
    CHECK_PX(px(rounded, 4, 0), 0xffffffffu);
    CHECK_PX(px(rounded, 0, 10), 0xffffffffu);
    Bitmap holed(20, 20);
    fillBitmap(holed, 0xffffffffu);
    clearDisc(holed, 10, 10, 4);
    CHECK_PX(px(holed, 10, 10), 0u);
    CHECK_PX(px(holed, 0, 0), 0xffffffffu);
    CHECK(alphaSum(holed) < 400 - 40 && alphaSum(holed) > 400 - 60); // ~π·4² cleared
    // Only the disc's pixels are visited, also one cut by the edges: every
    // pixel matches the coverage formula.
    const struct {
        float x, y, r;
    } discs[] = {{10, 10, 4}, {18.3f, 1.6f, 5.5f}, {-2, 12, 6}, {40, 40, 3}};
    for (const auto &d : discs) {
        const float c[3] = {d.x, d.y, d.r};
        Bitmap      b(20, 20);
        fillBitmap(b, 0xffffffffu);
        clearDisc(b, c[0], c[1], c[2]);
        int bad = 0;
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x) {
                const float k = std::clamp(
                    std::hypot(float(x) + 0.5f - c[0], float(y) + 0.5f - c[1]) - c[2] + 0.5f,
                    0.f,
                    1.f
                );
                bad += std::abs(alphaAt(b, x, y) - int(std::lround(k * 255))) > 1;
            }
        CHECK(bad == 0);
    }

    // An SVG renders covering the box, at least that big.
    const std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10'>"
                            "<rect width='20' height='10' fill='#f00'/></svg>";
    Bitmap            r;
    CHECK(renderSvgCover(svg, 8, 8, &r));
    CHECK(r.width() == 16 && r.height() == 8);
    CHECK(renderSvgCover(svg, 0, 0, &r));
    CHECK(r.width() == 20 && r.height() == 10);
    CHECK(!renderSvgCover("not svg", 8, 8, &r));
}

void testSvgLayers() {
    // Group opacity and clip-path render through a layer sized to what the
    // group paints. Opacity 0.999 and a clip covering everything composite
    // unchanged, so the result must equal the bare content, bit for bit: a
    // layer cut short (miter tips, square caps, nesting) would show.
    const char *shapes[] = {
        "<path d='M3540 2610 L3600 2500 L3660 2610' fill='none' stroke='#2060c0' "
        "stroke-width='40' stroke-miterlimit='10'/>",
        "<line x1='3420' y1='2420' x2='3480' y2='2440' stroke='#c02020' stroke-width='40' "
        "stroke-linecap='square'/>",
        "<circle cx='3700' cy='2700' r='37.3' fill='url(#g)' stroke='#208020' stroke-width='9'/>",
    };
    const std::string head =
        "<svg xmlns='http://www.w3.org/2000/svg' width='4000' height='3000'>"
        "<defs><linearGradient id='g'><stop offset='0' stop-color='#f80'/>"
        "<stop offset='1' stop-color='#08f' stop-opacity='0.5'/></linearGradient>"
        "<clipPath id='all'><rect width='4000' height='3000'/></clipPath></defs>";
    std::string bareSvg = head, layeredSvg = head; // each shape in its own layers
    for (const char *sh : shapes) {
        bareSvg += sh;
        layeredSvg +=
            std::string("<g opacity='0.999'><g clip-path='url(#all)'><g opacity='0.999'>") + sh +
            "</g></g></g>";
    }
    const int W = 1000, H = 750; // drawn at a quarter: the shapes sit near the far corner
    Bitmap    bare, layered;
    CHECK(renderSvg(bareSvg + "</svg>", W, H, &bare));
    CHECK(renderSvg(layeredSvg + "</svg>", W, H, &layered));
    CHECK(countNonZero(bare) > 1000);
    CHECK(bare.width() == layered.width() && bare.height() == layered.height());
    int diff = 0;
    for (int i = 0; i < W * H && bare.width() == layered.width(); ++i)
        diff += bare.pixels()[i] != layered.pixels()[i];
    CHECK(diff == 0);

    // Memory: a small clipped, translucent group on a large canvas allocates
    // in proportion to the group, not the canvas (a canvas-sized layer and
    // mask would be 48 MB each here).
    const std::string small =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 4000 3000'>"
        "<clipPath id='c'><circle cx='3900' cy='2900' r='40'/></clipPath>"
        "<g opacity='0.5' clip-path='url(#c)'><rect x='3700' y='2700' width='300' height='300' "
        "fill='red'/><rect x='3900' y='2800' width='100' height='200' fill='blue'/></g></svg>";
    Bitmap big;
    g_heapPeak = g_heapLive, g_heapLargest = 0, g_heapTrack = true;
    const size_t before = g_heapLive;
    CHECK(renderSvg(small, 4000, 3000, &big));
    g_heapTrack         = false;
    const size_t canvas = size_t(4000) * 3000 * 4; // the output itself
    CHECK(g_heapLargest == canvas);
    std::printf(
        "  layered 4000x3000 render: %zu bytes beyond the output\n", g_heapPeak - before - canvas
    );
    CHECK(g_heapPeak - before < canvas + 256 * 1024);
    // Inside the circle: red, and blue covering it (not blended with it) at
    // half opacity; outside: nothing.
    CHECK(near(px(big, 3870, 2900), 0x80ff0000u, 3));
    CHECK(near(px(big, 3920, 2900), 0x800000ffu, 3));
    CHECK(px(big, 3800, 2800) == 0 && px(big, 3920, 2950) == 0);
}

struct Group {
    const char *name;
    void (*fn)();
};
const Group kGroups[] = {
    {"blend", testBlend},
    {"fill", testFill},
    {"clip", testClip},
    {"roundrect", testRoundRect},
    {"path", testPath},
    {"stroke", testStroke},
    {"bitmap", testBitmap},
    {"blit", testBlit},
    {"gradient", testGradient},
    {"shadow", testShadow},
    {"decode", testDecode},
    {"anim", testAnim},
    {"animbudget", testAnimBudget},
    {"giflzwlimit", testGifLzwDictionaryLimit},
    {"snapped", testSnapped},
    {"icons", testIcons},
    {"paint2", testPaint2},
    {"svg", testSvg},
    {"svgsize", testSvgSize},
    {"svgfuzz", testSvgFuzz},
    {"cover", testCover},
    {"resizeaxes", testResizeAxes},
    {"scratch", testScratch},
    {"svglayers", testSvgLayers},
};

} // namespace

int main(int argc, char **argv) {
    int ran = 0;
    for (const Group &g : kGroups) {
        if (argc > 1 && std::strcmp(argv[1], g.name) != 0)
            continue;
        const int before = g_fail;
        g.fn();
        std::printf("%-10s %s\n", g.name, g_fail == before ? "ok" : "FAILED");
        ++ran;
    }
    if (!ran) {
        std::fprintf(stderr, "no test group '%s'\n", argc > 1 ? argv[1] : "");
        return 2;
    }
    return g_fail ? 1 : 0;
}

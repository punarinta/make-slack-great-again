// gfx_gallery — a plat window showing what gfx draws: shapes, clipped
// avatars, an animated GIF, shadows and every icon at 16/20/24 px.
//   gfx_gallery [--quit-after <ms>] [--bench]   (--bench: offscreen paint timing)
#include "gfx/gfx.h"
#include "gfx/icons_generated.h"

#include <plat/plat.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace gfx;

namespace {

std::string readFile(const std::string &path) {
    std::string out;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char   buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

struct Assets {
    Bitmap                   avatars[6];
    Bitmap                   small[6]; // avatars resized once for the current scale
    float                    smallScale = 0;
    std::vector<AnimFrame>   gif, gif2;
    int                      gifTotal = 0, gif2Total = 0;
    std::vector<std::string> svgs;                 // sources, see kSvgFiles
    std::vector<Bitmap>      svg32, svg64, svg128; // rendered once per scale
    float                    svgScale = 0;
};

// Fixtures (tests/assets/svg) then the repo's own artwork (../gfx).
const char *const kSvgFiles[] = {
    "S:logo-orbit.svg",
    "R:icon.svg",
    "S:inkscape-badge.svg",
    "S:gradients.svg",
    "R:roles/engineer.svg",
    "S:logo-signal.svg",
    "R:roles/designer.svg",
    "R:roles/marketer.svg",
    "R:roles/researcher.svg",
    "R:claude_code_avatar.svg",
    "R:ui/slack-mark.svg",
    "R:icon_tray.svg",
    "S:shapes.svg",
    "S:path.svg",
    "S:stroke.svg",
    "S:transform.svg",
    "S:opacity.svg",
    "S:clip.svg",
    "S:use.svg",
    "S:colors.svg",
    "S:aspect.svg",
    "S:unsupported.svg",
};
constexpr int kSvgCount = int(sizeof(kSvgFiles) / sizeof(kSvgFiles[0]));

const AnimFrame *frameAt(const std::vector<AnimFrame> &frames, int total, int ms) {
    if (frames.empty())
        return nullptr;
    int t = total > 0 ? ms % total : 0;
    for (const AnimFrame &f : frames) {
        if (t < f.delayMs)
            return &f;
        t -= f.delayMs;
    }
    return &frames.back();
}

void star(Path &p, float cx, float cy, float ro, float ri) {
    for (int i = 0; i < 10; ++i) {
        const float a = -1.5707963f + float(i) * 0.31415927f * 2;
        const float r = (i & 1) ? ri : ro;
        if (i == 0)
            p.moveTo(cx + r * std::cos(a), cy + r * std::sin(a));
        else
            p.lineTo(cx + r * std::cos(a), cy + r * std::sin(a));
    }
    p.close();
}

void paint(Painter &p, float w, float h, Assets &a, int ms) {
    if (a.smallScale != p.scale()) {
        const int px = int(std::lround(64 * p.scale()));
        for (int i = 0; i < 6; ++i)
            a.small[i] = resize(a.avatars[i].view(), px, px);
        a.smallScale = p.scale();
    }
    if (a.svgScale != p.scale()) { // rasterise the SVGs for this scale
        a.svg32.assign(kSvgCount, {}), a.svg64.assign(kSvgCount, {}),
            a.svg128.assign(kSvgCount, {});
        for (int i = 0; i < kSvgCount; ++i)
            for (auto [v, sz] : {std::pair{&a.svg32, 32}, {&a.svg64, 64}, {&a.svg128, 128}}) {
                const int px = int(std::lround(float(sz) * p.scale()));
                if (!renderSvg(a.svgs[size_t(i)], px, px, &(*v)[size_t(i)]))
                    std::fprintf(stderr, "gallery: cannot render %s\n", kSvgFiles[i]);
            }
        a.svgScale = p.scale();
    }
    p.fillRectGradient({0, 0, w, h}, {0, 0}, rgb(0xf4f5f8), {0, h}, rgb(0xe3e6ee));

    // ── Shapes ──────────────────────────────────────────────────────────────
    p.dropShadow({24, 24, 300, 200}, 12, 18, {0, 4}, rgba(0x1b2440, 0x50));
    p.fillRoundRect({24, 24, 300, 200}, 12, rgb(0xffffff));
    p.fillRect({44, 44, 60, 40}, rgb(0x3b5bdb));
    p.fillRect({114.5f, 44.5f, 60, 40}, rgb(0x3b5bdb)); // fractional: AA edges
    p.fillRoundRect({184, 44, 120, 40}, 20, rgb(0x2eb67d));
    p.strokeRoundRect({44, 100, 120, 44}, 8, 1, rgb(0x868e96));
    p.strokeRoundRect({174, 100, 130, 44}, 8, 2.5f, rgb(0xe01e5a));
    p.fillCircle({70, 180}, 22, rgb(0xecb22e));
    p.strokeCircle({126, 180}, 22, 3, rgb(0x36c5f0));
    Path s;
    star(s, 184, 180, 24, 10);
    p.fillPath(s, rgb(0x7048e8));
    Path c;
    c.moveTo(220, 196);
    c.cubicTo(240, 150, 270, 210, 300, 164);
    p.strokePath(c, 4, rgb(0x1d1c1d));
    for (int i = 0; i < 6; ++i)
        p.drawLine({44.0f + float(i) * 44, 214}, {80.0f + float(i) * 44, 204}, 1.5f, rgb(0x495057));

    // ── Avatars: round clips, round-rect clips, opacity ─────────────────────
    p.dropShadow({344, 24, 280, 200}, 12, 18, {0, 4}, rgba(0x1b2440, 0x50));
    p.fillRoundRect({344, 24, 280, 200}, 12, rgb(0xffffff));
    for (int i = 0; i < 6; ++i) {
        const float x = 364 + float(i % 3) * 84, y = 44 + float(i / 3) * 90;
        p.save();
        if (i < 3)
            p.clipRoundRect({x, y, 64, 64}, 32);
        else
            p.clipRoundRect({x, y, 64, 64}, 12);
        // Resized once per scale, as the app's image cache will: an area-
        // averaging shrink every frame would dominate the paint time.
        p.drawBitmap(a.small[i].view(), {x, y, 64, 64}, Sampling::Smooth, i == 5 ? 0.45f : 1.0f);
        p.restore();
        if (i < 3) { // presence dot with a ring
            p.fillCircle({x + 54, y + 54}, 9, rgb(0xffffff));
            p.fillCircle({x + 54, y + 54}, 6, i == 1 ? rgb(0xadb5bd) : rgb(0x2eb67d));
        }
    }

    // ── Animated GIFs ───────────────────────────────────────────────────────
    p.dropShadow({644, 24, 280, 200}, 12, 18, {0, 4}, rgba(0x1b2440, 0x50));
    p.fillRoundRect({644, 24, 280, 200}, 12, rgb(0xffffff));
    if (const AnimFrame *f = frameAt(a.gif, a.gifTotal, ms)) {
        p.save();
        p.clipRoundRect({656, 36, 126, 84}, 8);
        p.drawBitmap(f->frame.view(), {656, 36, 126, 84}, Sampling::Smooth);
        p.restore();
    }
    if (const AnimFrame *f = frameAt(a.gif2, a.gif2Total, ms)) {
        p.save();
        p.clipRoundRect({788, 36, 126, 84}, 8);
        p.drawBitmap(f->frame.view(), {788, 36, 126, 84}, Sampling::Smooth);
        p.restore();
    }
    // A spinner built from strokes, rotating with time.
    for (int i = 0; i < 12; ++i) {
        const float ang = float(i) * 0.5235988f + float(ms) * 0.004f;
        const float cx = 720, cy = 172;
        p.drawLine(
            {cx + 10 * std::cos(ang), cy + 10 * std::sin(ang)},
            {cx + 22 * std::cos(ang), cy + 22 * std::sin(ang)},
            3,
            withAlpha(rgb(0x3b5bdb), float(i + 1) / 12.0f)
        );
    }
    // Gradient pill + opacity stack.
    p.save();
    p.clipRoundRect({770, 150, 140, 44}, 22);
    p.fillRectGradient({770, 150, 140, 44}, {770, 0}, rgb(0x7048e8), {910, 0}, rgb(0xe01e5a));
    p.restore();

    // ── Icons: every icon at 16, 20, 24 px, light and dark ──────────────────
    const float top      = 248;
    const float half     = 276; // one band of icons: 3 sizes × 3 rows
    const int   cols     = 29;
    const float sizes[3] = {16, 20, 24};
    p.dropShadow({24, top, w - 48, 2 * half}, 12, 18, {0, 4}, rgba(0x1b2440, 0x50));
    p.fillRoundRect({24, top, w - 48, 2 * half}, 12, rgb(0xffffff));
    p.save();
    p.clipRoundRect({24, top, w - 48, 2 * half}, 12);
    p.fillRect({24, top + half, w - 48, half}, rgb(0x1a1d21));
    p.restore();
    for (int band = 0; band < 2; ++band) {
        float y = top + 12 + float(band) * half;
        for (float sz : sizes) {
            const float step = sz + 8;
            for (int i = 0; i < kIconCount; ++i) {
                const int   row = i / cols, col = i % cols;
                const float x = 40 + float(col) * 30;
                drawIcon(
                    p,
                    Icon(i),
                    {x + (30 - step) / 2, y + float(row) * step, sz, sz},
                    band ? rgb(0xd1d2d3) : rgb(0x1d1c1d)
                );
            }
            y += step * float((kIconCount + cols - 1) / cols) + 4;
        }
    }

    // ── Runtime SVG: every fixture and the repo artwork at 32 px, the first
    // twelve at 64 px, the first six at 128 px ─────────────────────────────
    const float stop = 824;
    p.dropShadow({24, stop, w - 48, 272}, 12, 18, {0, 4}, rgba(0x1b2440, 0x50));
    p.fillRoundRect({24, stop, w - 48, 272}, 12, rgb(0xffffff));
    auto put = [&](const Bitmap &b, float x, float y, float sz) {
        p.drawBitmap(b.view(), {x, y, sz, sz}, Sampling::Smooth);
    };
    for (int i = 0; i < kSvgCount; ++i)
        put(a.svg32[size_t(i)], 44 + float(i) * 40, stop + 12, 32);
    for (int i = 0; i < 12; ++i)
        put(a.svg64[size_t(i)], 44 + float(i) * 74, stop + 56, 64);
    for (int i = 0; i < 6; ++i)
        put(a.svg128[size_t(i)], 44 + float(i) * 146, stop + 132, 128);
}

} // namespace

int main(int argc, char **argv) {
    int  quitAfter = 0;
    bool bench     = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--quit-after") && i + 1 < argc)
            quitAfter = std::atoi(argv[i + 1]);
        bench |= !std::strcmp(argv[i], "--bench");
    }

    Assets            a;
    const std::string dir      = MSGA_TEST_ASSETS;
    const char       *names[6] = {"alex", "mira", "jonas", "priya", "yuki", "sam"};
    for (int i = 0; i < 6; ++i)
        if (!decodeImage(readFile(dir + "/avatars/" + names[i] + ".png"), &a.avatars[i]))
            std::fprintf(stderr, "gallery: cannot decode avatar %s\n", names[i]);
    decodeAnimation(readFile(dir + "/gifs/bouncing-ball.gif"), &a.gif);
    decodeAnimation(readFile(dir + "/gifs/party-confetti.gif"), &a.gif2);
    for (const auto &f : a.gif)
        a.gifTotal += f.delayMs;
    for (const auto &f : a.gif2)
        a.gif2Total += f.delayMs;

    for (const char *f : kSvgFiles)
        a.svgs.push_back(readFile(
            std::string(f[0] == 'S' ? MSGA_TEST_ASSETS "/svg" : GFX_REPO_ART) + "/" + (f + 2)
        ));

    if (bench) { // offscreen paint timing, no window
        for (float sc : {1.0f, 2.0f}) {
            Bitmap     target(int(948 * sc), int(1120 * sc));
            const int  n  = 60;
            const auto t0 = std::chrono::steady_clock::now();
            for (int f = 0; f < n; ++f) {
                Painter p(target.view(), sc);
                paint(p, 948, 1120, a, f * 20);
            }
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            std::printf("scale %.0f: %.2f ms per full-window paint\n", sc, ms / n);
        }
        return 0;
    }

    std::string err;
    auto        app = plat::App::create(&err);
    if (!app) {
        std::fprintf(stderr, "gfx_gallery: %s\n", err.c_str());
        return 1;
    }
    auto win = app->createWindow(
        {.title = "gfx gallery", .appId = "org.nisdos.gfx-gallery", .size = {948, 1120}}
    );
    const auto start   = std::chrono::steady_clock::now();
    double     paintMs = 0;
    int        frames  = 0;
    app->setEventHandler([&](const plat::Event &e) {
        switch (e.type) {
        case plat::EventType::CloseRequested:
        case plat::EventType::QuitRequested:
            app->quit();
            break;
        case plat::EventType::Frame: {
            plat::Canvas c = win->beginPaint();
            if (!c.pixels)
                break;
            const auto t0 = std::chrono::steady_clock::now();
            const int  ms =
                int(std::chrono::duration_cast<std::chrono::milliseconds>(t0 - start).count());
            Painter p({c.pixels, c.width, c.height, c.stride}, float(c.scale));
            paint(p, float(c.width / c.scale), float(c.height / c.scale), a, ms);
            paintMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            ++frames;
            win->endPaint({});
            break;
        }
        default:
            break;
        }
    });
    // The GIFs and spinner animate at ~50 fps.
    app->addTimer(20, true, [&] { win->requestFrame(); });
    if (quitAfter > 0)
        app->addTimer(quitAfter, false, [&] { app->quit(); });
    win->requestFrame();
    app->run();
    if (frames)
        std::printf("gfx_gallery: %d frames, %.2f ms average paint\n", frames, paintMs / frames);
    return 0;
}

// text_gallery — a plat window showing what src/text renders: paragraph
// sizes, rich styles, emoji, scripts, wrapping, selection and caret.
//
//   text_gallery [--quit-after ms]      (PLAT_SCALE=1.5 to try other scales)
#include "text/text.h"

#include "plat/plat.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace text;

namespace {

constexpr gfx::Color kInk = gfx::rgb(0x1d1c1d), kMuted = gfx::rgb(0x616061),
                     kLink = gfx::rgb(0x1264a3), kCodeBg = gfx::rgb(0xf2f2f2),
                     kCodeInk = gfx::rgb(0xc01343), kMentionBg = gfx::rgb(0xe8f2fa),
                     kSelect = gfx::rgb(0xb4d5fe);

Style st(float size = 15, Weight w = Weight::Regular) {
    Style s;
    s.size   = size;
    s.weight = w;
    return s;
}

struct Block {
    std::unique_ptr<Layout> layout;
    float                   x, y;
};

struct Gallery {
    std::vector<Block> blocks;
    float              scale    = 0;
    double             buildUs  = 0;
    int                layouts  = 0;
    // Selection / caret demo
    int                selBlock = -1;
    uint32_t           selFrom = 0, selTo = 0, caret = 0;
    int                boxBlock = -1;
};

float add(
    Gallery              &g,
    const AttributedText &t,
    float                 x,
    float                 y,
    float                 maxW,
    LayoutOptions         o = LayoutOptions{}
) {
    o.maxWidth    = maxW;
    const auto t0 = std::chrono::steady_clock::now();
    auto       l  = Layout::build(t, o, g.scale);
    g.buildUs +=
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    ++g.layouts;
    const float h = l->height();
    g.blocks.push_back({std::move(l), x, y});
    return y + h;
}

float label(Gallery &g, const char *s, float x, float y) {
    AttributedText t;
    Style          l = st(11, Weight::Semibold);
    l.color          = kMuted;
    t.append(s, l);
    return add(g, t, x, y, 1e9f) + 2;
}

void build(Gallery &g, float width) {
    g.blocks.clear();
    g.buildUs        = 0;
    g.layouts        = 0;
    const float colW = (width - 72) / 2, x0 = 24, x1 = 24 + colW + 24;
    float       y = 16;
    {
        AttributedText t;
        t.append("src/text gallery", st(22, Weight::Bold));
        Style sub = st(13);
        sub.color = kMuted;
        char buf[96];
        std::snprintf(
            buf, sizeof buf, "   scale %.2f · HarfBuzz + FreeType · own font index", g.scale
        );
        t.append(buf, sub);
        y = add(g, t, x0, y, 1e9f) + 12;
    }

    // ── Left column ──────────────────────────────────────────────────────────
    float       yl   = y;
    const char *para = "Slack-style messages wrap at spaces and never inside words. Kerning "
                       "stays intact across style boundaries: AVA Tyo Wo. Numbers 0123456789.";
    for (float size : {13.f, 15.f, 17.f}) {
        char lab[32];
        std::snprintf(lab, sizeof lab, "PARAGRAPH %.0f PX", size);
        yl = label(g, lab, x0, yl);
        AttributedText t;
        t.append(para, st(size));
        yl = add(g, t, x0, yl, colW) + 12;
    }

    yl = label(g, "RICH STYLES", x0, yl);
    {
        AttributedText t;
        t.append("Regular, ", st());
        t.append("medium, ", st(15, Weight::Medium));
        t.append("semibold, ", st(15, Weight::Semibold));
        t.append("bold", st(15, Weight::Bold));
        t.append(", ", st());
        Style it  = st();
        it.italic = true;
        t.append("italic", it);
        t.append(", ", st());
        Style bi  = st(15, Weight::Bold);
        bi.italic = true;
        t.append("bold italic", bi);
        t.append(", ", st());
        Style strike  = st();
        strike.strike = true;
        t.append("struck", strike);
        t.append(". Code: ", st());
        Style code      = st(13.5f);
        code.mono       = true;
        code.color      = kCodeInk;
        code.background = kCodeBg;
        t.append("layout.cpp:42", code);
        t.append(" and ", st());
        Style mention      = st(15, Weight::Medium);
        mention.color      = kLink;
        mention.background = kMentionBg;
        mention.linkId     = 1;
        t.append("@robin", mention);
        t.append(" in ", st());
        Style link     = st();
        link.color     = kLink;
        link.underline = true;
        link.linkId    = 2;
        t.append("#general", link);
        t.append(" — see ", st());
        t.append("the docs", link);
        t.append(".", st());
        yl = add(g, t, x0, yl, colW) + 12;
    }

    yl = label(g, "EMOJI", x0, yl);
    {
        AttributedText t;
        t.append(
            "Party \xF0\x9F\x8E\x89 family \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
            "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6 dev "
            "\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\x92"
            "\xBB flags "
            "\xF0\x9F\x87\xB8\xF0\x9F\x87\xAA\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5\xF0\x9F\x87"
            "\xBA\xF0\x9F\x87\xA6 \xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88 tones "
            "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD\xF0\x9F\x91\x8D\xF0"
            "\x9F"
            "\x8F\xBF heart \xE2\x9D\xA4\xEF\xB8\x8F keycap 1\xEF\xB8\x8F\xE2\x83\xA3 text-style "
            "\xE2\x9C\x94 \xE2\x98\x85",
            st()
        );
        yl = add(g, t, x0, yl, colW) + 4;
        AttributedText big;
        big.append(
            "\xF0\x9F\x98\x80\xF0\x9F\x9A\x80\xF0\x9F\x90\xB1\xF0\x9F\x8D\x95 at 28 px", st(28)
        );
        yl = add(g, big, x0, yl, colW) + 12;
    }

    yl = label(g, "INLINE BOX (CUSTOM EMOJI) + ELLIPSIS", x0, yl);
    {
        AttributedText t;
        t.append("Deploy went fine ", st());
        Style box       = st();
        box.inlineBoxId = 1;
        box.boxWidth    = 20;
        box.boxHeight   = 20;
        t.append(":shipit:", box);
        t.append(" and the build is green.", st());
        g.boxBlock = int(g.blocks.size());
        yl         = add(g, t, x0, yl, colW) + 6;
        AttributedText e;
        e.append(
            "A preview line that is much too long for its box gets cut with an ellipsis "
            "after two lines, the way the channel list and search results show it.",
            st(13)
        );
        LayoutOptions o;
        o.maxLines = 2;
        o.ellipsis = true;
        yl         = add(g, e, x0, yl, colW * 0.8f, o) + 12;
    }

    // ── Right column ─────────────────────────────────────────────────────────
    float yr = y;
    yr       = label(g, "SCRIPTS (FALLBACK + BIDI)", x1, yr);
    {
        const char *lines[] = {
            "Cyrillic: Съешь же ещё этих мягких французских булок",
            "Greek: Ξεσκεπάζω την ψυχοφθόρα βδελυγμία",
            "日本語: 吾輩は猫である。名前はまだ無い。",
            "中文：我能吞下玻璃而不伤身体。",
            "한국어: 다람쥐 헌 쳇바퀴에 타고파",
            ("\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9: "
             "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8"
             "\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85 2024"),
            "Mixed: the word \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D means peace",
            ("\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA: \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D "
             "\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D (hello world)"),
            "Devanagari: नमस्ते दुनिया · Thai: สวัสดีชาวโลก",
            "Math & symbols: ∑ ∞ ≠ → ⌘ ⇧ ✓ ♥ € £ ¥",
        };
        for (auto s : lines) {
            AttributedText t;
            t.append(s, st());
            yr = add(g, t, x1, yr, colW) + 3;
        }
        yr += 9;
    }

    yr = label(g, "WRAPPED LONG URL (240 PX)", x1, yr);
    {
        AttributedText t;
        t.append("Link: ", st());
        Style link  = st();
        link.color  = kLink;
        link.linkId = 3;
        t.append(
            "https://github.com/punarinta/make-slack-great-again/blob/master/src/text/"
            "layout.cpp#L120-L184",
            link
        );
        t.append(" (breaks anywhere when it overflows)", st());
        yr = add(g, t, x1, yr, 240) + 12;
    }

    yr = label(g, "SELECTION + CARET", x1, yr);
    {
        AttributedText t;
        t.append("Select across styles: plain, ", st());
        t.append("bold", st(15, Weight::Bold));
        t.append(
            ", emoji \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD and \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D, then "
            "keep going onto a second line.",
            st()
        );
        g.selBlock = int(g.blocks.size());
        yr         = add(g, t, x1, yr, colW) + 12;
        g.selFrom  = 14;
        g.selTo    = uint32_t(t.text.find("second"));
        g.caret    = uint32_t(t.text.find("going") + 5);
    }

    yr = label(g, "MONO + ALIGNMENT", x1, yr);
    {
        AttributedText t;
        Style          m = st(13);
        m.mono           = true;
        t.append("for (auto &run : runs)\n    shape(run, font);  // -> glyphs", m);
        yr = add(g, t, x1, yr, colW) + 6;
        for (auto a : {LayoutOptions::Align::Center, LayoutOptions::Align::Right}) {
            AttributedText c;
            c.append(
                a == LayoutOptions::Align::Center ? "Centred line" : "Right-aligned line", st(13)
            );
            LayoutOptions o;
            o.align = a;
            yr      = add(g, c, x1, yr, colW, o) + 2;
        }
    }
    (void)yl;
}

void paint(Gallery &g, gfx::Painter &p, float w, float h) {
    p.fillRect({0, 0, w, h}, gfx::rgb(0xffffff));
    for (size_t i = 0; i < g.blocks.size(); ++i) {
        const Block &b = g.blocks[i];
        if (int(i) == g.selBlock) {
            for (auto r : b.layout->selectionRects(g.selFrom, g.selTo))
                p.fillRect({b.x + r.x, b.y + r.y, r.w, r.h}, kSelect);
        }
        b.layout->paint(p, {b.x, b.y});
        if (int(i) == g.selBlock) {
            const gfx::RectF c = b.layout->caretRect(g.caret);
            p.fillRect({b.x + c.x, b.y + c.y, c.w, c.h}, kInk);
        }
        if (int(i) == g.boxBlock)
            for (auto &bx : b.layout->boxes()) {
                const gfx::RectF r{b.x + bx.rect.x, b.y + bx.rect.y, bx.rect.w, bx.rect.h};
                p.fillRoundRect(r, 4, gfx::rgb(0x2eb67d));
                p.fillCircle({r.x + r.w / 2, r.y + r.h / 2}, r.w / 4, gfx::rgb(0xffffff));
            }
    }
}

} // namespace

int main(int argc, char **argv) {
    int quitAfter = 0;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--quit-after") && i + 1 < argc)
            quitAfter = std::atoi(argv[++i]);
    std::string err;
    if (!text::init(&err)) {
        std::fprintf(stderr, "text_gallery: %s\n", err.c_str());
        return 1;
    }
    auto app = plat::App::create(&err);
    if (!app) {
        std::fprintf(stderr, "text_gallery: %s\n", err.c_str());
        return 1;
    }
    auto win = app->createWindow(
        {.title = "text gallery", .appId = "org.nisdos.text-gallery", .size = {1060, 760}}
    );
    Gallery g;
    float   builtW  = 0;
    double  paintMs = 0;
    int     frames  = 0;
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
            const float w = float(c.width / c.scale), h = float(c.height / c.scale);
            if (g.scale != float(c.scale) || builtW != w) {
                g.scale = float(c.scale);
                builtW  = w;
                build(g, w);
                std::printf(
                    "text_gallery: %d layouts built in %.0f us (%.1f us each) at scale %.2f\n",
                    g.layouts,
                    g.buildUs,
                    g.buildUs / g.layouts,
                    g.scale
                );
            }
            const auto   t0 = std::chrono::steady_clock::now();
            gfx::Painter p({c.pixels, c.width, c.height, c.stride}, float(c.scale));
            paint(g, p, w, h);
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
    if (quitAfter > 0)
        app->addTimer(quitAfter, false, [&] { app->quit(); });
    win->requestFrame();
    app->run();
    if (frames)
        std::printf("text_gallery: %d frames, %.2f ms average paint\n", frames, paintMs / frames);
    return 0;
}

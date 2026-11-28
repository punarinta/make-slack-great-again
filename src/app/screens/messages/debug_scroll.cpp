#include "app/screens/messages/debug_scroll.h"

#include "ui/debug.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace screens {

namespace {

struct Run {
    Context                           &ctx;
    ui::Window                        &w;
    DebugScrollHooks                   hooks;
    int                                events;
    std::function<void(int)>           done;
    std::vector<std::function<void()>> stages;
    size_t                             next = 0;
    std::string                        label;
    std::vector<double>                frameMs, paintMs;
    int                                lastFrames = 0, stageMismatch0 = 0;
    bool                               sampling = false;
};

// The list to scroll: the message list (leftmost) or the thread panel's (rightmost).
ui::RectF listRect(ui::Window &w, bool thread) {
    std::vector<ui::View *> lists = ui::debug::findByRole(w.root(), ui::Role::List);
    ui::View               *pick  = nullptr;
    for (ui::View *v : lists) {
        const ui::RectF r = v->windowRect();
        if (r.w < 100 || r.h < 100)
            continue;
        if (!pick || (thread ? r.x > pick->windowRect().x : r.x < pick->windowRect().x))
            pick = v;
    }
    return pick ? pick->windowRect() : ui::RectF{};
}

void sample(const std::shared_ptr<Run> &r) {
    if (!r->sampling)
        return;
    const ui::Window::Stats &s = r->w.stats();
    if (s.frames != r->lastFrames) {
        r->lastFrames = s.frames;
        r->frameMs.push_back(s.lastFrameMs);
        r->paintMs.push_back(s.lastPaintMs);
    }
    r->ctx.app.addTimer(2, false, [r] { sample(r); });
}

void runNext(const std::shared_ptr<Run> &r) {
    const ui::Window::Stats &s = r->w.stats();
    if (!r->label.empty())
        std::fprintf(
            stderr,
            "debug-scroll: %-28s %d mismatching frames\n",
            r->label.c_str(),
            s.verifyMismatches - r->stageMismatch0
        );
    if (r->next >= r->stages.size()) {
        r->sampling = false;
        auto stat   = [](std::vector<double> v, const char *name) {
            if (v.empty())
                return;
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (double x : v)
                sum += x;
            std::fprintf(
                stderr,
                "debug-scroll: %s avg %.2f ms, p50 %.2f, p95 %.2f, max %.2f (%zu frames)\n",
                name,
                sum / double(v.size()),
                v[v.size() / 2],
                v[v.size() * 95 / 100],
                v.back(),
                v.size()
            );
        };
        stat(r->frameMs, "frame");
        stat(r->paintMs, "paint");
        std::fprintf(
            stderr,
            "debug-scroll: %d frames verified, %d mismatching, %d with only ±1 AA noise\n",
            s.verifiedFrames,
            s.verifyMismatches,
            s.verifyNoiseFrames
        );
        // Hold still, so a screenshot of what the compositor shows can be
        // compared with a from-scratch repaint ($UI_FINAL_DUMP).
        if (const char *dump = std::getenv("UI_FINAL_DUMP"); dump && *dump) {
            r->ctx.app.addTimer(400, false, [r, dump = std::string(dump)] {
                r->w.dumpFullRepaint(dump);
                std::fprintf(stderr, "debug-scroll: idle, repaint written to %s\n", dump.c_str());
                r->ctx.app.addTimer(2500, false, [r] {
                    auto cb = std::move(r->done);
                    if (cb)
                        cb(r->w.stats().verifyMismatches);
                });
            });
            return;
        }
        auto cb = std::move(r->done);
        if (cb)
            cb(s.verifyMismatches);
        return;
    }
    r->stageMismatch0 = s.verifyMismatches;
    r->stages[r->next++]();
}

// A stage: set up, let it settle, tour the chosen list, go on.
std::function<void()>
stage(const std::shared_ptr<Run> &r, std::string label, std::function<void()> setup, bool thread) {
    return [r, label, setup, thread] {
        r->label = label;
        setup();
        r->ctx.app.addTimer(300, false, [r, thread] {
            const ui::RectF area = listRect(r->w, thread);
            if (area.w <= 0 || !ui::debug::scrollTour(r->w, area, r->events, [r] { runNext(r); }))
                runNext(r);
        });
    };
}

} // namespace

void debugScrollTour(
    Context &ctx, ui::Window &w, DebugScrollHooks hooks, int events, std::function<void(int)> done
) {
    auto r = std::make_shared<Run>(Run{ctx, w, std::move(hooks), events, std::move(done)});
    w.setVerify(true);
    const Store &st         = ctx.store;
    ConvRef      threadConv = model::kNoConv;
    Ts           threadRoot = 0;
    for (ConvRef c = 0; c < st.conversationCount(); ++c) {
        std::string name = st.displayName(c);
        r->stages.push_back(stage(r, name, [r, c] { r->hooks.open(c); }, false));
        if (!threadRoot)
            for (const model::Message &m : st.conversation(c).messages)
                if (m.replyCount > 1) {
                    threadConv = c;
                    threadRoot = m.ts;
                    break;
                }
    }
    if (threadRoot) {
        r->stages.push_back(stage(
            r,
            "thread open: channel",
            [r, threadConv, threadRoot] {
                r->hooks.open(threadConv);
                r->hooks.openThread(threadConv, threadRoot);
            },
            false
        ));
        r->stages.push_back(stage(r, "thread open: panel", [] {}, true));
        r->stages.push_back(stage(r, "thread closed", [r] { r->hooks.closeThread(); }, false));
    }
    const bool dark = ctx.app.dark();
    // The other theme, on the busiest conversation and a thread.
    for (int pass = 0; pass < 2 && threadRoot; ++pass)
        r->stages.push_back(stage(
            r,
            pass ? "other theme: panel" : "other theme: channel",
            [r, dark, threadConv, threadRoot, pass] {
                if (!pass) {
                    r->ctx.app.setThemeMode(dark ? ui::ThemeMode::Light : ui::ThemeMode::Dark);
                    r->hooks.open(threadConv);
                    r->hooks.openThread(threadConv, threadRoot);
                }
            },
            pass == 1
        ));
    r->sampling = true;
    sample(r);
    runNext(r);
}

} // namespace screens

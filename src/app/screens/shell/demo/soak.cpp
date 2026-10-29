#include "screens/shell/demo/soak.h"

#include "app/diag/mem_stats.h"
#include "screens/common/context.h"
#include "screens/shell/message_search.h"
#include "screens/shell/profile_card.h"
#include "screens/shell/quick_switcher.h"
#include "screens/settings/settings_dialog.h"
#include "screens/shell/shell.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace demo {

namespace {

// Long enough for a frame or two and the list's first image requests.
constexpr int kStepMs   = 150;
// After a round: the decode workers and the downloads settle before sampling.
constexpr int kSettleMs = 1500;

struct Soak {
    screens::Context                  &ctx;
    int                                rounds;
    std::function<std::string()>       appStats;
    std::function<void()>              done;
    std::vector<std::function<void()>> steps;
    size_t                             next  = 0;
    int                                round = 0;
};

// Each pending timer holds the run; none is left once it is done.
void advance(const std::shared_ptr<Soak> &s) {
    if (s->next < s->steps.size()) {
        s->steps[s->next++]();
        s->ctx.app.addTimer(kStepMs, false, [s] { advance(s); });
        return;
    }
    s->ctx.app.addTimer(kSettleMs, false, [s] {
        ++s->round;
        std::fprintf(
            stderr,
            "msga soak: round %d/%d: %s; %s\n",
            s->round,
            s->rounds,
            diag::formatMem(diag::sampleMem()).c_str(),
            s->appStats().c_str()
        );
        std::fflush(stderr);
        if (s->round >= s->rounds) {
            s->done();
            return;
        }
        s->next = 0;
        advance(s);
    });
}

} // namespace

void runSoak(
    screens::Context            &ctx,
    shell::Shell                &sh,
    model::ConvRef               start,
    int                          rounds,
    std::function<std::string()> appStats,
    std::function<void()>        done
) {
    auto  s     = std::make_shared<Soak>(Soak{ctx, rounds, std::move(appStats), std::move(done)});
    auto &steps = s->steps;
    shell::Shell *shp = &sh;

    const model::Store &st = ctx.store;
    for (model::ConvRef c = 0; c < st.conversationCount(); ++c) {
        steps.push_back([shp, c] { shp->open(c); });
        for (const model::Message &m : st.conversation(c).messages)
            if (m.replyCount > 0) {
                const model::Ts root = m.ts;
                steps.push_back([shp, c, root] { shp->openThread(c, root); });
                steps.push_back([shp] { shp->closeThread(); });
                break;
            }
    }
    steps.push_back([shp] { shp->showPage(shell::Shell::Page::Activity); });
    steps.push_back([shp] { shp->showPage(shell::Shell::Page::Threads); });
    steps.push_back([shp] { shp->showPage(shell::Shell::Page::Saved); });
    steps.push_back([shp] { shp->showPage(shell::Shell::Page::Scheduled); });
    steps.push_back([shp, start] { shp->open(start); });
    steps.push_back([shp] { shp->openSearch(); });
    steps.push_back([shp] {
        if (shp->messageSearch())
            shp->messageSearch()->hideNow();
    });
    steps.push_back([shp] { shp->showQuickSwitcher(); });
    steps.push_back([shp] {
        if (shp->quickSwitcher())
            shp->quickSwitcher()->close();
    });
    steps.push_back([shp] { shp->openSettings(); });
    steps.push_back([shp] {
        if (shp->settingsDialog())
            shp->settingsDialog()->close();
    });
    model::UserRef someone = model::kNoUser;
    for (const model::Message &m : st.conversation(start).messages)
        if (m.user != model::kNoUser)
            someone = m.user;
    if (someone != model::kNoUser) {
        steps.push_back([shp, someone] { shp->showProfile(someone); });
        steps.push_back([shp] { shp->profiles().hideNow(); });
    }
    // The other theme and back: every view re-themes twice, and each round
    // ends where it started.
    const ui::ThemeMode here  = ctx.app.dark() ? ui::ThemeMode::Dark : ui::ThemeMode::Light;
    const ui::ThemeMode other = ctx.app.dark() ? ui::ThemeMode::Light : ui::ThemeMode::Dark;
    ui::App            *app   = &ctx.app;
    steps.push_back([app, other] { app->setThemeMode(other); });
    steps.push_back([app, here] { app->setThemeMode(here); });

    advance(s);
}

} // namespace demo

// The shell parts: the sidebar's sections, filters and footer, the conversation header and tabs,
// the thread panel, and the composer's plain-text formatting, pickers,
// pills, undo send and outgoing conversion.
#include "app/fake/fake_backend.h"
#include "app/i18n/languages.h"
#include "base/i18n.h"
#include "app/model/jobs.h"
#include "app/mrkdwn/emoji.h"
#include "app/spell/spell.h"
#include "base/file.h"
#include "base/str.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/testing.h"
#include "screens/common/user_search.h"
#include "screens/shell/canvas_page.h"
#include "screens/shell/composer.h"
#include "screens/shell/composer_popups.h"
#include "screens/shell/gif_search.h"
#include "screens/shell/header.h"
#include "screens/shell/nav_chrome.h"
#include "app/mrkdwn/markdown.h"
#include "screens/shell/scheduled_page.h"
#include "screens/shell/shell.h"
#include "screens/shell/sidebar_footer.h"
#include "screens/shell/status_dialog.h"
#include "screens/shell/typing_indicator.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/emoji_picker.h"
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/thread_panel.h"
#include "ui/datetime.h"
#endif

#include <cmath>
#include <memory>

using namespace model;
using shell::shortcuts::Id;

namespace {

ui::App &app() {
    static std::unique_ptr<ui::App> a = [] {
        std::string err;
        auto        p = ui::App::create(&err);
        if (!p)
            std::fprintf(stderr, "ui::App::create: %s\n", err.c_str());
        return p;
    }();
    return *a;
}

void pump(int n = 10) {
    for (int i = 0; i < n; ++i)
        app().pump(2);
}

template <class F>
bool until(F done, int ms = 3000) {
    for (int t = 0; t < ms && !done(); t += 5)
        app().pump(5);
    return done();
}

// The fake backend, with the capabilities a Slack workspace adds switchable.
struct Fake : fake::FakeBackend {
    using fake::FakeBackend::FakeBackend;
    Capabilities capabilities() const override {
        Capabilities c   = fake::FakeBackend::capabilities();
        c.replyBroadcast = broadcast;
        c.huddles        = huddles;
        c.scheduledSend  = schedule;
        return c;
    }
    void sendBroadcast(ConvRef conv, std::string text, Ts thread, Done done) override {
        ++broadcasts;
        send(conv, std::move(text), thread, std::move(done));
    }
    void setPresence(bool away, Done done) override {
        if (presenceError.empty())
            return fake::FakeBackend::setPresence(away, std::move(done));
        done(false, presenceError);
    }
    void setStatus(std::string emoji, std::string text, int64_t expiry, Done done) override {
        if (statusError.empty())
            return fake::FakeBackend::setStatus(
                std::move(emoji), std::move(text), expiry, std::move(done)
            );
        done(false, statusError);
    }
    void loadMembers(ConvRef conv, MembersDone done) override {
        if (membersError.empty())
            return fake::FakeBackend::loadMembers(conv, std::move(done));
        done({}, membersError);
    }
    void sendBlocks(
        ConvRef conv, std::string text, std::string blocks, Ts thread, bool bc, Done done
    ) override {
        lastBlocks = blocks;
        fake::FakeBackend::sendBlocks(
            conv, std::move(text), std::move(blocks), thread, bc, std::move(done)
        );
    }
    void
    scheduleMessage(ConvRef conv, std::string text, Ts thread, int64_t at, Done done) override {
        scheduled     = {conv, thread, at};
        scheduledText = std::move(text);
        if (done)
            done(scheduleError.empty(), scheduleError);
    }
    struct Scheduled {
        ConvRef conv   = kNoConv;
        Ts      thread = 0;
        int64_t at     = 0;
    } scheduled;
    std::string scheduledText;
    bool        gifSearchAvailable() const override { return gifs; }
    std::string promptSuggestion(ConvRef c) const override {
        return c == suggestFor ? std::string("run the  tests\nagain") : std::string();
    }
    bool        broadcast = false, huddles = false, schedule = false, gifs = true;
    int         broadcasts = 0;
    ConvRef     suggestFor = kNoConv;
    std::string membersError, lastBlocks;
    std::string scheduleError; // scheduleMessage fails with it
    void        refreshScheduled() override { ++scheduledRefreshes; }
    void        cancelScheduled(const std::string &id, Done done) override {
        cancelled.push_back(id);
        if (cancelError.empty())
            store().removeScheduled(id);
        if (done)
            done(cancelError.empty(), cancelError);
    }
    int                      scheduledRefreshes = 0;
    std::vector<std::string> cancelled;
    std::string              cancelError;

    std::string presenceError; // setPresence fails with it
    std::string statusError;   // setStatus too

    // The presence link, as a Slack session workspace reports it.
    PresenceLink presenceLink() const override { return link; }
    void         setPresenceMode(PresenceMode m) override { mode = m; }
    void         noteUserActivity() override { ++activity; }
    PresenceLink link     = PresenceLink::Off;
    PresenceMode mode     = PresenceMode::Native;
    int          activity = 0;
};

struct Harness {
    Store store;
    Fake  backend{store, app().platform()};
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache images{app().platform()};
    screens::Context    ctx{app(), store, backend, images, {}, {}, {}, {}, {}};
#else
    alignas(16) char noCache[16]{};
    screens::Context ctx{
        app(), store, backend, *reinterpret_cast<screens::ImageCache *>(noCache), {}, {}, {}, {}, {}
    };
#endif
    shell::Settings               settings;
    std::unique_ptr<ui::Window>   win;
    std::unique_ptr<shell::Shell> sh;

    explicit Harness(bool open = true) {
        backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
        bool done = false;
        backend.connect([&](bool ok, const std::string &) { done = ok; });
        for (int i = 0; i < 200 && !done; ++i)
            app().pump(5);
        plat::WindowDesc d;
        d.size        = {1200, 800};
        d.decorations = plat::Decorations::Custom;
        win           = std::make_unique<ui::Window>(d);
        sh            = std::make_unique<shell::Shell>(ctx, *win, settings, std::string());
        if (open)
            sh->open(store.findConversation("C0DESIGN"));
        pump();
    }
    ConvRef          conv(const char *id) const { return store.findConversation(id); }
    shell::Sidebar  &sidebar() { return sh->sidebar(); }
    shell::Composer &composer() { return sh->composer(); }
};

std::string joined(const std::vector<std::string> &v) {
    std::string out;
    for (const auto &s : v)
        out += (out.empty() ? "" : "|") + s;
    return out;
}

} // namespace

// ── Sidebar ─────────────────────────────────────────────────────────────────

TEST("sidebar: the entries and sections, and Saved messages while something is saved") {
    Harness h;
    CHECK_STR(
        joined(h.sidebar().sectionTitles()), "Activity|Threads|Starred|Channels|Direct messages"
    );
    const ConvRef design = h.conv("C0DESIGN");
    const Ts      ts     = h.store.conversation(design).messages.back().ts;
    h.backend.setSaved(design, ts, true);
    pump();
    CHECK_STR(
        joined(h.sidebar().sectionTitles()),
        "Activity|Threads|Saved messages|Starred|Channels|Direct messages"
    );
    h.backend.setSaved(design, ts, false);
    pump();
    CHECK_STR(
        joined(h.sidebar().sectionTitles()), "Activity|Threads|Starred|Channels|Direct messages"
    );
}

TEST("sidebar: a collapsed section lists nothing; opening one of its chats expands it") {
    Harness       h;
    const ConvRef eng = h.conv("C0ENG");
    REQUIRE(h.sidebar().toggleSection("Channels"));
    pump();
    CHECK_FALSE(h.sidebar().rowState(eng).visible); // unread, still hidden
    h.sh->open(eng);
    pump();
    CHECK(h.sidebar().rowState(eng).visible);
    CHECK(h.sidebar().rowState(eng).selected);
}

TEST("sidebar: channels outside the relevant days go under \"N more channels\"") {
    Harness h;
    CHECK(h.sidebar().hiddenChannels() == 0);
    shell::Sidebar::Filters f;
    f.relevantDays = 1;
    h.sidebar().setFilters(f);
    // random (last active yesterday morning) falls out; the open, the unread,
    // the recent and the starred ones stay.
    CHECK(h.sidebar().hiddenChannels() == 1);
    CHECK_FALSE(h.sidebar().rowState(h.conv("C0RANDOM")).exists);
    CHECK(h.sidebar().rowState(h.conv("G0LAUNCH")).exists);
    CHECK(h.sidebar().rowState(h.conv("C0GENERAL")).exists);
    CHECK(h.sidebar().rowState(h.conv("C0ENG")).exists);
    CHECK(h.sidebar().rowState(h.conv("C0DESIGN")).exists);
    const int hidden = h.sidebar().hiddenChannels();
    h.sidebar().showAllChannels();
    pump();
    CHECK(h.sidebar().hiddenChannels() == 0);
    CHECK(hidden > 0);
    // Live from Settings: the shell passes the value through applySettings.
    h.settings.relevantDays = 14;
    h.sh->applySettings();
    CHECK(h.sidebar().hiddenChannels() == 0);
}

TEST("sidebar: \"Show only unread conversations\" keeps the unread, the open and the starred") {
    Harness h;
    h.settings.unreadsOnly = true;
    h.sh->applySettings();
    pump();
    CHECK(h.sidebar().rowState(h.conv("C0ENG")).exists);     // unread
    CHECK(h.sidebar().rowState(h.conv("D0MIRA")).exists);    // unread DM
    CHECK(h.sidebar().rowState(h.conv("C0DESIGN")).exists);  // open (and starred)
    CHECK(h.sidebar().rowState(h.conv("C0GENERAL")).exists); // starred
    CHECK_FALSE(h.sidebar().rowState(h.conv("C0RANDOM")).exists);
    CHECK_FALSE(h.sidebar().rowState(h.conv("D0JONAS")).exists);
    h.settings.unreadsOnly = false;
    h.sh->applySettings();
    CHECK(h.sidebar().rowState(h.conv("C0RANDOM")).exists);
}

TEST("sidebar: bot DMs live under Agents & apps, which Settings can hide") {
    Harness       h;
    const UserRef bot = h.store.findUser("U0DEPLOY"); // the fixture's Deploybot
    REQUIRE(bot != kNoUser);
    CHECK(h.store.user(bot).bot);
    Conversation c;
    c.id     = "D0BOT";
    c.kind   = ConvKind::Dm;
    c.dmUser = bot;
    h.store.addConversation(std::move(c));
    h.sidebar().rebuild();
    CHECK_STR(
        joined(h.sidebar().sectionTitles()),
        "Activity|Threads|Starred|Channels|Direct messages|Agents & apps"
    );
    h.settings.showAgentsApps = false;
    h.sh->applySettings();
    CHECK_STR(
        joined(h.sidebar().sectionTitles()), "Activity|Threads|Starred|Channels|Direct messages"
    );
}

TEST("sidebar: a DM whose peer was deactivated is not listed") {
    Harness    h;
    // A reinstalled app leaves its old, deactivated bot users behind, each
    // with a DM the service keeps returning.
    const auto dm = [&](const char *id, const char *user, bool deleted, bool placeholder) {
        User u;
        u.id            = user;
        u.name          = "roomote";
        u.displayName   = "Roomote";
        u.bot           = true;
        u.deleted       = deleted;
        u.placeholder   = placeholder;
        const UserRef r = h.store.addUser(std::move(u));
        Conversation  c;
        c.id     = id;
        c.kind   = ConvKind::Dm;
        c.dmUser = r;
        c.unread = 1; // unread keeps a DM listed, unless its peer is gone
        return h.store.addConversation(std::move(c));
    };
    const ConvRef live = dm("D0LIVE", "U0ROOMOTE", false, false);
    const ConvRef dead = dm("D0DEAD", "U0ROOMOTEX", true, false);
    h.sidebar().rebuild();
    CHECK(h.sidebar().rowState(live).exists);
    CHECK_FALSE(h.sidebar().rowState(dead).exists);
    // A peer known only by id is let through until its record arrives.
    const ConvRef pending = h.store.addConversation([&] {
        Conversation c;
        c.id     = "D0PENDING";
        c.kind   = ConvKind::Dm;
        c.dmUser = h.store.internUser("U0NOTLOADED");
        c.unread = 1;
        return c;
    }());
    h.sidebar().rebuild();
    CHECK(h.sidebar().rowState(pending).exists);
}

TEST("sidebar footer: the presence toggle flips at once and settles on the service's answer") {
    Harness               h;
    shell::SidebarFooter &f = h.sidebar().footer();
    CHECK_FALSE(f.showsHidden());
    CHECK(f.toggle()->tooltip().find("no official Slack app is connected") != std::string::npos);
    f.togglePresence();
    CHECK(f.showsHidden()); // optimistic
    // The fake service keeps reporting automatic presence: back it goes.
    REQUIRE(until([&] { return !f.showsHidden(); }));
    CHECK_STR(f.avatarButton()->tooltip(), "Profile & status");
}

TEST("sidebar footer: a presence change the service refuses says why and settles back") {
    Harness h;
    h.backend.presenceError = "missing_scope";
    shell::SidebarFooter &f = h.sidebar().footer();
    f.togglePresence();
    CHECK_FALSE(f.showsHidden());
    CHECK(h.sh->errorBanner()->visible());
    CHECK_STR(
        h.sh->errorBanner()->text(),
        "Could not change presence: missing_scope \xE2\x80\x94 sign in to this workspace again "
        "to grant the new permission"
    );
}

TEST("sidebar footer: the presence tooltip says what the presence link is doing") {
    using PresenceLink = Backend::PresenceLink;
    Harness               h;
    shell::SidebarFooter &f   = h.sidebar().footer();
    // The fake workspace's self presence is "away for want of a connected client".
    const auto            tip = [&](PresenceLink l) {
        h.backend.link = l;
        f.refresh();
        return f.toggle()->tooltip();
    };
    CHECK_STR(
        tip(PresenceLink::Off),
        "Visible \xE2\x80\x94 but you appear away while no official Slack app is connected. "
        "MSGA can keep you active: Settings \xE2\x86\x92 System \xE2\x86\x92 Presence."
    );
    CHECK_STR(
        tip(PresenceLink::Idle),
        "Away \xE2\x80\x94 you haven't used MSGA for a while. Any click or keystroke makes you "
        "active again (Settings \xE2\x86\x92 System \xE2\x86\x92 Presence)."
    );
    const std::string connecting = "Visible \xE2\x80\x94 connecting so you appear active "
                                   "without the official Slack app\xE2\x80\xA6";
    CHECK_STR(tip(PresenceLink::Connecting), connecting);
    CHECK_STR(tip(PresenceLink::Active), connecting); // Slack registers it a beat later
    CHECK_STR(
        tip(PresenceLink::Unavailable),
        "Visible \xE2\x80\x94 but you appear away while no official Slack app is connected. "
        "MSGA can't hold your presence on this workspace."
    );
}

TEST("presence: Settings picks the link's mode, and input reaches it (throttled)") {
    Harness h;
    // Settings → System → Presence: 0 = while running (the default).
    CHECK(h.backend.mode == Backend::PresenceMode::WhileRunning);
    h.settings.presence = 1;
    h.sh->applySettings();
    CHECK(h.backend.mode == Backend::PresenceMode::WhileUsing);
    h.settings.presence = 2;
    h.sh->applySettings();
    CHECK(h.backend.mode == Backend::PresenceMode::Native);
    // Real input (a click, a key) is noted once per 20 s.
    auto     *hooks  = app().platform().testHooks();
    const int before = h.backend.activity;
    hooks->injectPointerMove(h.win->native(), {600, 400});
    hooks->injectButton(h.win->native(), plat::Button::Left, true);
    hooks->injectButton(h.win->native(), plat::Button::Left, false);
    pump();
    CHECK(h.backend.activity == before + 1);
    hooks->injectKey(h.win->native(), plat::Key::F, true);
    hooks->injectKey(h.win->native(), plat::Key::F, false);
    pump();
    CHECK(h.backend.activity == before + 1);
}

TEST("parallel-usage banner: shown on contention, stays until closed") {
    Harness   h;
    ui::View *b = h.sh->parallelUsageBanner();
    REQUIRE(b != nullptr);
    CHECK_FALSE(b->visible());
    h.sh->showParallelUsage();
    pump();
    CHECK(b->visible());
    REQUIRE(b->childCount() == 2);
    CHECK_STR(
        static_cast<ui::Label *>(b->child(0))->text(),
        "The same app keys are running on another device and keep interrupting your Slack "
        "connection. How to solve this?"
    );
    // No timeout, unlike the error banner.
    pump(200);
    CHECK(b->visible());
    auto *close = static_cast<ui::Clickable *>(b->child(1));
    REQUIRE(close->onClick);
    close->onClick();
    CHECK_FALSE(b->visible());
}

TEST("sidebar footer: the background-task cog turns while jobs run and lists them on hover") {
    Harness               h;
    shell::SidebarFooter &f   = h.sidebar().footer();
    ui::View             *cog = f.tasksIndicator();
    pump();
    CHECK_FALSE(cog->visible());
    CHECK_FALSE(f.tasksTurning());
    const int a = model::jobs().begin("Downloading report.pdf");
    pump();
    REQUIRE(cog->visible());
    CHECK(f.tasksTurning());
    // Left of the presence toggle, 8 px apart, the same 40 px square.
    const ui::RectF c = cog->windowRect(), t = f.toggle()->windowRect();
    CHECK(std::abs(c.x + c.w + 8 - t.x) < 0.5f);
    CHECK(c.w == 40 && c.h == 40 && c.y == t.y);
    // Hovering it: the count, then the jobs; it follows the list.
    auto *hooks = app().platform().testHooks();
    hooks->injectPointerMove(h.win->native(), {c.x + 20, c.y + 20});
    pump();
    REQUIRE(f.tasksPopup() != nullptr);
    CHECK_STR(
        f.tasksPopup()->accessibleName(), "1 background task running\nDownloading report.pdf"
    );
    CHECK(f.tasksPopup()->windowRect().y + f.tasksPopup()->height() <= c.y); // above it
    const int b = model::jobs().begin("Copying photo.png");
    pump();
    CHECK_STR(
        f.tasksPopup()->accessibleName(),
        "2 background tasks running\nDownloading report.pdf\nCopying photo.png"
    );
    model::jobs().end(a);
    pump();
    CHECK_STR(f.tasksPopup()->accessibleName(), "1 background task running\nCopying photo.png");
    // Leaving it hides the list.
    hooks->injectPointerMove(h.win->native(), {600, 300});
    pump();
    CHECK(f.tasksPopup() == nullptr);
    hooks->injectPointerMove(h.win->native(), {c.x + 20, c.y + 20});
    pump();
    REQUIRE(f.tasksPopup() != nullptr);
    // The last job ends: the cog, its list and the animation go.
    model::jobs().end(b);
    pump();
    CHECK_FALSE(cog->visible());
    CHECK(f.tasksPopup() == nullptr);
    CHECK_FALSE(f.tasksTurning());
    hooks->injectPointerMove(h.win->native(), {600, 300});
    pump();
}

TEST("status dialog: a suggestion fills it; Save sets the status, Clear status clears it") {
    Harness h;
    auto   *d = shell::StatusDialog::show(*h.win, h.ctx);
    pump();
    d->applyPreset(3); // Vacationing, don't clear
    CHECK_STR(d->emoji(), "palm_tree");
    CHECK_STR(d->text().text(), "Vacationing");
    CHECK(d->expiry(1000) == 0);
    d->clearAfter().setSelected(2); // 1 hour
    CHECK(d->expiry(1000) == 1000 + 3600);
    d->save();
    REQUIRE(until([&] { return h.store.user(h.store.me).statusEmoji == "palm_tree"; }));
    CHECK_STR(h.store.user(h.store.me).statusText, "Vacationing");
    auto *d2 = shell::StatusDialog::show(*h.win, h.ctx); // prefilled with what is set
    pump();
    CHECK_STR(d2->text().text(), "Vacationing");
    d2->clearStatus();
    REQUIRE(until([&] { return h.store.user(h.store.me).statusEmoji.empty(); }));
    // A refusal says why, with a re-sign-in hint for a missing scope.
    h.backend.statusError = "missing_scope";
    auto       *d3        = shell::StatusDialog::show(*h.win, h.ctx);
    std::string banner;
    d3->onError = [&](const std::string &m) { banner = m; };
    pump();
    d3->applyPreset(3);
    d3->save();
    REQUIRE(until([&] { return !banner.empty(); }));
    CHECK_STR(
        banner,
        "Could not set status: missing_scope \xE2\x80\x94 sign in to this workspace again to "
        "grant the new permission"
    );
}

TEST("profile dialog: shows my profile and saves only a change") {
    Harness h;
    auto   *d = shell::ProfileDialog::show(*h.win, h.ctx, h.sh->avatars());
    REQUIRE(until([&] { return !d->email().text().empty(); }));
    CHECK_STR(d->name().text(), "Alex Kim");
    d->name().setText("Alex K.");
    d->save();
    REQUIRE(until([&] { return h.store.user(h.store.me).displayName == "Alex K."; }));
}

// ── Header and tabs ─────────────────────────────────────────────────────────

TEST("header: channel members and star, a DM's avatar, huddles only where the service has them") {
    Harness            h;
    shell::ConvHeader &hd = h.sh->header();
    CHECK_STR(hd.title()->text(), "#design");
    CHECK(hd.members()->visible());
    CHECK_STR(hd.memberCount()->text(), "9");
    CHECK_STR(hd.members()->tooltip(), "View members");
    CHECK_STR(hd.star()->tooltip(), "Unstar conversation");
    CHECK_STR(hd.search()->tooltip(), "Search messages");
    CHECK_FALSE(hd.huddle()->visible());
    h.backend.huddles = true;
    hd.refresh();
    CHECK(hd.huddle()->visible());
    h.sh->open(h.conv("D0JONAS"));
    pump();
    CHECK_STR(hd.title()->text(), "Jonas Weber");
    CHECK_FALSE(hd.members()->visible());
    CHECK_STR(hd.star()->tooltip(), "Star conversation");
}

TEST("tabs: the canvas's title, or \"Add canvas\"") {
    Harness h;
    CHECK_STR(h.sh->tabs().tabText(0), "Messages");
    CHECK_STR(h.sh->tabs().tabText(1), "Design crit \xE2\x80\x94 week 38");
    h.sh->open(h.conv("C0ENG"));
    pump();
    CHECK_STR(h.sh->tabs().tabText(1), "Add canvas");
    CHECK(h.sh->tabs().tab(1)->visible());
}

// The canvas tab: the fixture's canvas as editable
// markdown text, created on the first save, saved whole, deleted.
TEST("canvas: the channel's canvas loads as a title over its markdown body") {
    Harness h;
    h.sh->showCanvas(true);
    shell::CanvasPage &cp = h.sh->canvasPage();
    REQUIRE(until([&] { return !cp.body().empty(); }));
    CHECK_STR(cp.title().text(), "Design crit \xE2\x80\x94 week 38");
    const std::string &t = cp.body().text();
    CHECK(t.find("## Decisions\n- Proposal B ships.") != std::string::npos);
    CHECK(t.find("1. Mira \xE2\x80\x94 add the skip link") != std::string::npos);
    CHECK(t.find("| Step | Drop-off today | Target |\n| --- | --- | --- |") != std::string::npos);
    CHECK(t.find("> Do we keep the illustration") != std::string::npos);
    CHECK(cp.menuButton()->visible());
    // Formats survive into canvas markdown.
    const std::string md = shell::canvas::markdown(cp.body());
    CHECK(md.find("- **Proposal B ships.** Three onboarding") != std::string::npos);
    CHECK(md.find("[#design](https://lumen.studio/design)") != std::string::npos);
    CHECK(md.find("## Decisions\n\n- **Proposal B") != std::string::npos);
}

TEST("canvas: \"Add canvas\" creates it on the first save, then the tab names it") {
    Harness       h;
    const ConvRef eng = h.conv("C0ENG");
    h.sh->open(eng);
    pump();
    h.sh->showCanvas(true);
    shell::CanvasPage &cp = h.sh->canvasPage();
    CHECK(cp.body().empty());
    CHECK_FALSE(cp.menuButton()->visible());
    cp.title().setText("Launch notes");
    cp.body().insertHtml("<p><b>Ship</b> on Friday</p><p>- check the docs</p>");
    cp.flushPendingSave();
    REQUIRE(until([&] { return h.store.conversation(eng).canvasTitle == "Launch notes"; }));
    CHECK_FALSE(h.store.conversation(eng).canvasId.empty());
    CHECK(cp.menuButton()->visible());
    pump();
    CHECK_STR(h.sh->tabs().tabText(1), "Launch notes");
    // Read back from the backend, as Slack would serve it.
    cp.clear();
    h.sh->showCanvas(true);
    REQUIRE(until([&] { return !cp.body().empty(); }));
    CHECK_STR(cp.title().text(), "Launch notes");
    CHECK_STR(cp.body().text(), "Ship on Friday\n- check the docs");
    CHECK_STR(shell::canvas::markdown(cp.body()), "**Ship** on Friday\n\n- check the docs");
}

TEST("canvas: an edit replaces the document; \"Delete canvas\" goes back to \"Add canvas\"") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    h.sh->showCanvas(true);
    shell::CanvasPage &cp = h.sh->canvasPage();
    REQUIRE(until([&] { return !cp.body().empty(); }));
    cp.body().setText("Just this");
    h.sh->showCanvas(false); // a tab switch saves
    std::string html;
    REQUIRE(until([&] {
        h.backend.loadCanvasContent(
            h.store.conversation(design).canvasId, [&](std::string s, std::string) { html = s; }
        );
        pump(3);
        return html.find(">Just this</p>") != std::string::npos;
    }));
    h.sh->showCanvas(true);
    REQUIRE(until([&] { return cp.body().text() == "Just this"; }));
    cp.confirmDelete();
    auto *dlg = static_cast<ui::Dialog *>(h.win->topPopup());
    REQUIRE(dlg != nullptr);
    dlg->accept();
    REQUIRE(until([&] { return h.store.conversation(design).canvasId.empty(); }));
    pump();
    CHECK_STR(h.sh->tabs().tabText(1), "Add canvas");
    CHECK(cp.body().empty());
    CHECK_FALSE(cp.menuButton()->visible());
}

TEST("header: the members popup lists the channel's people, sorted, me marked") {
    Harness h;
    h.sh->header().openMembers({900, 40, 40, 28});
    pump();
    ui::Popup *p = h.win->topPopup();
    REQUIRE(p != nullptr);
    std::function<bool(ui::View *, std::string_view)> has = [&](ui::View *v, std::string_view s) {
        if (v->accessibleName() == s)
            return true;
        for (size_t i = 0; i < v->childCount(); ++i)
            if (has(v->child(i), s))
                return true;
        return false;
    };
    REQUIRE(until([&] { return has(p, "Alex Kim (you)"); }));
    CHECK(has(p, "Jonas Weber"));
}

namespace {
bool hasText(ui::View *v, std::string_view s) {
    if (v->accessibleName() == s)
        return true;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (hasText(v->child(i), s))
            return true;
    return false;
}
} // namespace

TEST("header: the clickable parts — members button, group avatars, star, huddle, search") {
    Harness            h;
    shell::ConvHeader &hd = h.sh->header();
    // The members button opens the member list under it.
    REQUIRE(hd.members()->onClick != nullptr);
    hd.members()->onClick();
    pump();
    REQUIRE(h.win->topPopup() != nullptr);
    REQUIRE(until([&] { return hasText(h.win->topPopup(), "9 members"); }));
    h.win->topPopup()->close();
    pump();
    // The star toggles the conversation's star.
    const ConvRef design = h.conv("C0DESIGN");
    CHECK(h.store.conversation(design).starred);
    hd.star()->onClick();
    REQUIRE(until([&] { return !h.store.conversation(design).starred; }));
    CHECK_STR(hd.star()->tooltip(), "Star conversation");
    // The huddle button hands a channel off to its /huddle/ link, a DM to the
    // conversation.
    std::string opened;
    h.ctx.openUrl     = [&](const std::string &u) { opened = u; };
    h.backend.huddles = true;
    hd.refresh();
    hd.huddle()->onClick();
    CHECK_STR(opened, "https://app.slack.com/huddle/T0LUMEN/C0DESIGN");
    // The search button is the hook Shell routes to its message search.
    bool searched = false;
    hd.onSearch   = [&] { searched = true; };
    hd.search()->onClick();
    CHECK(searched);
    // A group DM's stacked avatars are its members button, with a hand cursor.
    h.sh->open(h.conv("G0TRIO"));
    pump();
    CHECK_FALSE(hd.members()->visible());
    REQUIRE(hd.avatar()->visible());
    CHECK(hd.avatar()->cursorAt({1, 1}) == uint8_t(plat::Cursor::Hand));
    hd.avatar()->onClick();
    pump();
    REQUIRE(h.win->topPopup() != nullptr);
    REQUIRE(until([&] { return hasText(h.win->topPopup(), "3 members"); }));
    h.win->topPopup()->close();
    pump();
    hd.huddle()->onClick();
    CHECK_STR(opened, "https://app.slack.com/client/T0LUMEN/G0TRIO");
    // A DM's single avatar is not a button (the pointer stays an arrow).
    h.sh->open(h.conv("D0JONAS"));
    pump();
    CHECK(hd.avatar()->cursorAt({1, 1}) == uint8_t(plat::Cursor::Arrow));
    hd.avatar()->onClick();
    pump();
    CHECK(h.win->topPopup() == nullptr);
}

TEST("header: the members popup says why the list couldn't load") {
    Harness h;
    h.backend.membersError = "ratelimited";
    h.sh->header().openMembers({900, 40, 40, 28});
    pump();
    ui::Popup *p = h.win->topPopup();
    REQUIRE(p != nullptr);
    REQUIRE(until([&] { return hasText(p, "Couldn't load the members (ratelimited)."); }));
}

TEST("header: the members popup grows to its list when the count was unknown") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    h.store.updateConversation(design, [](Conversation &c) { c.memberCount = 0; });
    shell::ConvHeader &hd = h.sh->header();
    hd.refresh();
    CHECK_FALSE(hd.memberCount()->visible());
    const ui::RectF anchor{900, 40, 40, 28};
    hd.openMembers(anchor);
    pump();
    ui::Popup *p = h.win->topPopup();
    REQUIRE(p != nullptr);
    const float loading = p->frame().h; // one row while the count is unknown
    REQUIRE(until([&] { return hasText(p, "9 members"); }));
    pump();
    // Six whole rows of 60 (the cap), still under the button and in the window.
    CHECK(std::fabs(p->frame().h - loading - 5 * 60) < 0.5f);
    CHECK(p->frame().y >= anchor.y + anchor.h);
    CHECK(p->frame().y + p->frame().h <= h.win->size().h);
    CHECK(p->frame().x + p->frame().w <= h.win->size().w);
}

TEST("header: the members popup matches titles; Up / Down stop at the ends, Enter opens") {
    Harness        h;
    model::UserRef opened = model::kNoUser;
    h.ctx.messageUser     = [&](model::UserRef u) { opened = u; };
    auto key              = [&](plat::Key k) {
        app().platform().testHooks()->injectKey(h.win->native(), k, true);
        app().platform().testHooks()->injectKey(h.win->native(), k, false);
        pump(2);
    };
    // "lead": Alex (Product lead) and Mira (Design lead), by name; typing
    // picks the first.
    auto open = [&] {
        h.sh->header().openMembers({900, 40, 40, 28});
        pump();
        REQUIRE(h.win->topPopup() != nullptr);
        REQUIRE(until([&] { return hasText(h.win->topPopup(), "9 members"); }));
        plat::Event te;
        te.type   = plat::EventType::TextInput;
        te.window = &h.win->native();
        te.text   = "lead";
        h.win->handle(te);
        pump(2);
        CHECK(hasText(h.win->topPopup(), "Mira Okafor"));
        CHECK_FALSE(hasText(h.win->topPopup(), "Jonas Weber"));
    };
    open();
    key(plat::Key::Up); // no wrap to the last match
    key(plat::Key::Enter);
    CHECK(opened == h.store.findUser("U0ALEX"));
    CHECK(h.win->topPopup() == nullptr);
    open();
    key(plat::Key::Down);
    key(plat::Key::Down); // stays on the last
    key(plat::Key::Enter);
    CHECK(opened == h.store.findUser("U0MIRA"));
}

TEST("people search key: label and handle, the title on request") {
    model::User u;
    u.id = "U1", u.name = "mira", u.displayName = "Mira Okafor", u.title = "Design Lead";
    CHECK_STR(screens::userSearchKey(u), "mira okafor mira");
    CHECK_STR(screens::userSearchKey(u, true), "mira okafor mira design lead");
    u.displayName.clear();
    CHECK_STR(screens::userSearchKey(u), "mira mira");
    // Both names, whichever shows (Settings → Names).
    u.realName    = "Mira Okafor";
    u.profileName = "Mimi";
    u.resolveName(false);
    CHECK_STR(screens::userSearchKey(u), "mimi mira okafor mira");
    u.resolveName(true);
    CHECK_STR(screens::userSearchKey(u), "mira okafor mimi mira");
}

// ── Thread panel ────────────────────────────────────────────────────────────

#ifdef MSGA_HAVE_MESSAGES
TEST("thread panel: the header buttons; the broadcast tick only where the service has it") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    const Ts      root   = h.backend.findTs(design, "Proposal B");
    h.sh->openThread(design, root);
    pump();
    auto *panel = static_cast<screens::ThreadPanel *>(h.sh->threadPanel());
    REQUIRE(panel);
    CHECK_STR(panel->muteButton()->tooltip(), "Mute thread");
    panel->toggleMuted();
    CHECK(h.store.threadMuted(design, root));
    CHECK_STR(panel->muteButton()->tooltip(), "Unmute thread");
    CHECK_FALSE(panel->broadcastShown()); // the fake backend can't broadcast replies
    shell::Composer *tc = h.sh->threadComposer();
    // Both composers' schedule chevrons follow the service.
    CHECK_FALSE(tc->scheduleVisible());
    CHECK_FALSE(h.composer().scheduleVisible());
    CHECK_STR(tc->edit().accessibleName(), "Reply in thread\xE2\x80\xA6");

    // A Slack workspace: ticked, the reply goes to the channel too, once.
    h.backend.broadcast = true;
    h.sh->closeThread();
    h.sh->openThread(design, root);
    pump();
    REQUIRE(panel->broadcastShown());
    CHECK_STR(panel->broadcast()->accessibleName(), "Also send to channel");
    panel->setBroadcastWanted(true);
    tc->edit().insertText("to all");
    CHECK(tc->send());
    CHECK(h.backend.broadcasts == 1);
    CHECK_FALSE(panel->broadcastWanted()); // one reply's worth
    // Attachments rule it out until they are gone.
    panel->setBroadcastWanted(true);
    REQUIRE(tc->addAttachments({MSGA_TEST_ASSETS "/images/blog-hero.png"}) == 1);
    CHECK_FALSE(panel->broadcastWanted());
    tc->removeAttachment(0);
    CHECK(panel->broadcastWanted());
}
#endif

#ifdef MSGA_HAVE_MESSAGES
namespace {
ui::View *findView(ui::View *v, const std::function<bool(ui::View *)> &match) {
    if (match(v))
        return v;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (ui::View *f = findView(v->child(i), match))
            return f;
    return nullptr;
}
} // namespace

TEST("schedule send: the date-time picker, an hour out; a thread reply stays in its thread") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    const Ts      root   = h.backend.findTs(design, "Proposal B");
    h.sh->openThread(design, root);
    pump();
    shell::Composer *tc = h.sh->threadComposer();
    tc->edit().insertText("later");
    tc->openSchedule();
    pump();
    ui::Popup *p = h.win->topPopup();
    REQUIRE(p != nullptr);
    CHECK(hasText(p, "Send at"));
    auto *when = static_cast<ui::DateTimeField *>(findView(p, [](ui::View *v) {
        return v->role() == ui::Role::TextInput;
    }));
    REQUIRE(when != nullptr);
    const int64_t now = base::nowSecs();
    CHECK(when->value() >= now + 3600 - 60 && when->value() <= now + 3600);
    // Never under a minute out: stepping the hour back far stops there.
    // (From 23:00 on, an hour out is past midnight: the hour, at 0, can't
    // step back at all: the picker's sections stop at their bounds. The
    // all-day sweep below pins both cases at fixed times.)
    const int64_t initial = when->value();
    const bool    atZero  = base::localTime(initial).hour == 0;
    when->setSection(3); // the hour ("MMM d, yyyy h:mm AP")
    when->step(-5);
    CHECK(when->value() >= now + 60);
    if (atZero)
        CHECK(when->value() == initial);
    else
        CHECK(when->value() < now + 3600 - 60);
    when->setValue(now + 7200);
    const int64_t at       = when->value();
    auto         *schedule = static_cast<ui::Clickable *>(findView(p, [](ui::View *v) {
        return v->accessibleName() == "Schedule";
    }));
    REQUIRE(schedule != nullptr);
    schedule->activate();
    pump();
    CHECK(h.backend.scheduled.conv == design);
    CHECK(h.backend.scheduled.thread == root);
    CHECK(h.backend.scheduled.at == at);
    CHECK_STR(h.backend.scheduledText, "later");
    // Japanese: "yyyy年M月d日 APh:mm" — year first, the day
    // period before the hour.
    // (The date language: it follows a language change at once.)
    base::setDateLanguage("ja");
    base::setUse24h(false);
    ui::DateTimeField ja(ui::DateTimeField::Kind::DateTime);
    ja.setValue(base::fromLocal(2026, 9, 30, 20, 45));
    ja.setSection(0);
    ja.step(1);
    CHECK(ja.year() == 2027);
    ja.setSection(3);
    ja.step(1);
    CHECK(ja.hour() == 8);
    base::setDateLanguage("en");
}

TEST(
    "schedule send: the chevron follows the open workspace; files are refused; a failure "
    "gives the text back"
) {
    // The shell is built before a workspace is behind the proxy: switching
    // to one that schedules shows the chevrons, switching away hides them.
    Harness h;
    h.sh->openThread(h.conv("C0DESIGN"), h.backend.findTs(h.conv("C0DESIGN"), "Proposal B"));
    pump();
    shell::Composer &c  = h.composer();
    shell::Composer *tc = h.sh->threadComposer();
    CHECK_FALSE(c.scheduleVisible());
    h.backend.schedule = true;
    h.sh->workspaceChanged();
    CHECK(c.scheduleVisible());
    CHECK(tc->scheduleVisible());
    h.backend.schedule = false;
    h.sh->workspaceChanged();
    CHECK_FALSE(c.scheduleVisible());
    CHECK_FALSE(tc->scheduleVisible());
    h.backend.schedule = true;
    h.sh->workspaceChanged();

    auto scheduleNow = [&] {
        c.openSchedule();
        pump();
        ui::Popup *p = h.win->topPopup();
        REQUIRE(p != nullptr);
        auto *schedule = static_cast<ui::Clickable *>(findView(p, [](ui::View *v) {
            return v->accessibleName() == "Schedule";
        }));
        REQUIRE(schedule != nullptr);
        schedule->activate();
        pump();
    };

    // Attachments can't go with it: nothing is scheduled, nothing is lost.
    c.edit().insertText("with a file");
    REQUIRE(c.addAttachments({MSGA_TEST_ASSETS "/images/blog-hero.png"}) == 1);
    c.openSchedule();
    pump();
    CHECK(h.backend.scheduled.conv == kNoConv);
    CHECK(h.sh->errorBanner()->visible());
    CHECK_STR(
        h.sh->errorBanner()->text(), "Files can't be scheduled. Send them now or remove them first."
    );
    CHECK(c.attachments().size() == 1);
    CHECK_STR(c.edit().text(), "with a file");
    c.removeAttachment(0);

    // Scheduled: the composer empties.
    scheduleNow();
    CHECK(h.backend.scheduled.conv == h.conv("C0DESIGN"));
    CHECK(h.backend.scheduled.thread == 0);
    CHECK_STR(h.backend.scheduledText, "with a file");
    CHECK(c.edit().empty());

    // Refused by the service: the text comes back to be sent another way.
    h.backend.scheduleError = "time_in_past";
    c.edit().insertText("too late");
    scheduleNow();
    CHECK_STR(h.backend.scheduledText, "too late");
    CHECK_STR(c.edit().text(), "too late");
}

TEST(
    "scheduled messages: the sidebar entry shows only while something is scheduled; its "
    "page sends now or cancels"
) {
    Harness h;
    h.backend.schedule = true;
    h.sh->workspaceChanged();
    h.sidebar().rebuild();
    pump();
    const auto entries = [&] { return joined(h.sidebar().sectionTitles()); };
    CHECK(entries().find("Scheduled messages") == std::string::npos); // nothing scheduled

    const ConvRef               design = h.conv("C0DESIGN");
    const Ts                    root   = h.backend.findTs(design, "Proposal B");
    model::Store::ScheduledItem later, reply, unknown;
    later.id            = "S2";
    later.conv          = design;
    later.at            = base::nowSecs() + 7200;
    later.text          = "later";
    reply               = later;
    reply.id            = "S1";
    reply.at            = base::nowSecs() + 3600; // soonest: first
    reply.text          = "a reply";
    reply.thread        = root;
    unknown             = later;
    unknown.id          = "S3";
    unknown.at          = base::nowSecs() + 9000;
    unknown.threadKnown = false; // a token workspace's: no Send now
    h.store.setScheduled({later, reply, unknown});
    pump();
    CHECK(h.sidebar().scheduledShown());
    CHECK(entries().find("Scheduled messages") != std::string::npos);

    h.sh->showPage(shell::Shell::Page::Scheduled);
    pump();
    REQUIRE(h.sh->pageOpen(shell::Shell::Page::Scheduled));
    CHECK(h.sidebar().selectedNav() == shell::Sidebar::Nav::Scheduled);
    CHECK_FALSE(h.composer().visible());
    CHECK(h.backend.scheduledRefreshes == 1); // opening re-lists
    shell::ScheduledPage *p = h.sh->scheduledPage();
    REQUIRE(p->cardCount() == 3);
    CHECK_STR(p->whenText(0), "Sends " + base::formatDateTime(reply.at));
    CHECK(p->canSendNow(0));
    CHECK_FALSE(p->canSendNow(2));

    // Send now: unscheduled first, then posted to its thread.
    p->sendNow(0);
    pump();
    CHECK(h.backend.cancelled.size() == 1 && h.backend.cancelled[0] == "S1");
    const auto *replies = h.store.replies(design, root);
    REQUIRE(replies != nullptr);
    CHECK_STR(replies->back().text, "a reply");
    REQUIRE(p->cardCount() == 2);

    // A refused cancel keeps the card, clickable again.
    h.backend.cancelError = "not_allowed";
    p->cancel(0);
    pump();
    CHECK(p->cardCount() == 2);
    h.backend.cancelError.clear();
    p->cancel(0);
    pump();
    CHECK(h.backend.cancelled.size() == 3);
    REQUIRE(p->cardCount() == 1);
    p->cancel(0);
    pump();
    CHECK(p->cardCount() == 0);
    CHECK_STR(p->statusText(), "Messages you schedule will appear here until they're sent.");
    // Nothing left: the entry goes, even with its page still open.
    CHECK_FALSE(h.sidebar().scheduledShown());

    // Opening a conversation leaves the page.
    h.sh->open(design);
    pump();
    CHECK_FALSE(h.sh->pageOpen(shell::Shell::Page::Scheduled));
    CHECK(h.composer().visible());

    // A workspace that can't schedule never shows it.
    h.store.setScheduled({later});
    h.backend.schedule = false;
    h.sidebar().rebuild();
    pump();
    CHECK(entries().find("Scheduled messages") == std::string::npos);
}

// The picker's hour step at every time of day (the schedule popup's
// minimum a minute out, its value an hour out): it stops at the minimum, and
// at midnight, never wrapping into the day before.
TEST("schedule send: the picker's hour step, all day long") {
    base::setDateLanguage("en");
    const int64_t day = base::fromLocal(2026, 9, 30, 0, 0);
    for (int64_t now = day; now < day + 86400; now += 7 * 60 + 13) {
        ui::DateTimeField f(ui::DateTimeField::Kind::DateTime);
        f.setMinimumValue(now + 60);
        f.setValue(now + 3600);
        const int64_t initial = f.value();
        f.setSection(3); // the hour
        f.step(-5);
        const base::CivilTime t = base::localTime(initial);
        if (!CHECK(f.value() >= now + 60))
            std::fprintf(stderr, "  at %02d:%02d\n", t.hour, t.minute);
        if (t.hour == 0)
            CHECK(f.value() == initial); // past midnight: the hour is at its bound
        else if (!CHECK(f.value() < initial && f.value() >= initial - 5 * 3600))
            std::fprintf(stderr, "  at %02d:%02d\n", t.hour, t.minute);
    }
}

#endif

// ── Composer ────────────────────────────────────────────────────────────────

TEST("composer: the toolbar types the formatting markers; the send button lights with text") {
    Harness h;
    auto   &c = h.composer();
    auto   &e = c.edit();
    CHECK_STR(e.accessibleName(), "Message #design");
#ifdef __APPLE__
    CHECK_STR(
        c.button(Id::Bold)->tooltip(),
        "Bold (\xE2\x8C\x98"
        "B)"
    );
#else
    CHECK_STR(c.button(Id::Bold)->tooltip(), "Bold (Ctrl+B)");
#endif
    CHECK_STR(c.gifButton()->tooltip(), "Search GIFs"); // the fake backend's stand-in GIPHY
    h.backend.gifs = false;                             // no service, no key
    c.refreshTips();
    CHECK_STR(c.gifButton()->tooltip(), "Search GIFs \xE2\x80\x94 needs a GIPHY API key");
    CHECK_STR(c.mentionButton()->tooltip(), "Mention (@)");
    c.button(Id::Bold)->onClick();
    CHECK_STR(e.text(), "**");
    CHECK(e.caret() == 1);
    e.insertText("hi");
    CHECK_STR(c.mrkdwn(), "*hi*");
    CHECK(c.sendButton()->tint() == ui::C::AccentText);
    e.clear();
    c.button(Id::Quote)->onClick();
    CHECK_STR(e.text(), "> ");
    e.clear();
    CHECK(c.sendButton()->tint() == ui::C::DropArrow);
}

TEST("composer: the window's is disabled until a conversation opens; an embedded one is not") {
    Harness h(false);
    CHECK(!h.composer().enabled());
    h.sh->open(h.conv("C0DESIGN"));
    pump();
    CHECK(h.composer().enabled());
    // The forward dialog's: no target, a fixed placeholder, Send asks the host.
    shell::DraftStash drafts;
    shell::Composer   c(h.ctx, drafts);
    c.setPlaceholder("Add a message, if you'd like.");
    bool asked      = false;
    c.onSendRequest = [&] { return asked = true; };
    c.edit().insertText("note");
    CHECK(c.enabled());
    CHECK_STR(c.edit().accessibleName(), "Add a message, if you'd like.");
    CHECK(c.send());
    CHECK(asked);
    CHECK_STR(c.edit().text(), "note");
}

TEST("composer: a mention pill shows the name as Settings → Names does") {
    Harness      h;
    auto        &c    = h.composer();
    model::User &mira = h.store.user(h.store.findUser("U0MIRA"));
    mira.realName     = "Mira Okafor";
    mira.profileName  = "Mimi";
    for (const bool real : {true, false}) {
        h.store.setRealNames(real);
        const char *want = real ? "@Mira Okafor " : "@Mimi ";
        c.edit().clear();
        c.edit().focus();
        c.edit().insertText("@mi");
        pump();
        REQUIRE(c.pickList() != nullptr);
        c.pickList()->confirm();
        pump();
        CHECK_STR(c.edit().text(), want);
        CHECK_STR(c.mrkdwn(), "<@U0MIRA> ");
        // A draft or an edit coming back into the composer reads the same.
        c.edit().clear();
        shell::loadMrkdwn(c.edit(), h.store, "<@U0MIRA> ");
        CHECK_STR(c.edit().text(), want);
    }
}

TEST("composer: an edit reopens links as links, so saving sends them back as tokens") {
    Harness h;
    auto   &c = h.composer();
    // A bare link shows its URL; brackets left in the text would go out as
    // typed and Slack would escape them (issue #111).
    shell::loadMrkdwn(c.edit(), h.store, "<https://www.anthropic.com/research/x> ddd");
    CHECK_STR(c.edit().text(), "https://www.anthropic.com/research/x ddd");
    CHECK_STR(c.mrkdwn(), "<https://www.anthropic.com/research/x> ddd");
    const mrkdwn::Composed composed = mrkdwn::compose(c.mrkdwn());
    CHECK_STR(composed.mrkdwn, "<https://www.anthropic.com/research/x> ddd");
    CHECK(composed.blocks.empty());
    // A label shows alone; URL and label come back decoded (Slack escapes
    // what goes out).
    shell::loadMrkdwn(
        c.edit(), h.store, "see <https://x.test/a?b=1&amp;c=2|the &lt;docs&gt;> &amp; more"
    );
    CHECK_STR(c.edit().text(), "see the <docs> & more");
    CHECK_STR(c.mrkdwn(), "see <https://x.test/a?b=1&c=2|the <docs&gt;> & more");
    shell::loadMrkdwn(c.edit(), h.store, "<mailto:a@b.test|a@b.test>");
    CHECK_STR(c.edit().text(), "a@b.test");
    CHECK_STR(c.mrkdwn(), "<mailto:a@b.test|a@b.test>");
    // Inside a list the link travels as a link element too.
    shell::loadMrkdwn(c.edit(), h.store, "- <https://x.test>");
    CHECK(
        mrkdwn::compose(c.mrkdwn()).blocks.find(R"("type":"link","url":"https://x.test")") !=
        std::string::npos
    );
    // Anything else in brackets is not a link and stays as it was.
    shell::loadMrkdwn(c.edit(), h.store, "a <word> b");
    CHECK_STR(c.edit().text(), "a <word> b");
    CHECK_STR(c.mrkdwn(), "a <word> b");
}

TEST("composer: @ lists people and @channel & co.; a pick is a pill that sends the token") {
    Harness h;
    auto   &c = h.composer();
    c.edit().focus();
    c.edit().insertText("hey @mi");
    pump();
    shell::PickList *p = c.pickList();
    REQUIRE(p != nullptr);
    REQUIRE(p->count() >= 1);
    CHECK_STR(p->item(0).title, "@Mira Okafor");
    p->confirm();
    pump();
    CHECK_STR(c.edit().text(), "hey @Mira Okafor ");
    CHECK_STR(c.mrkdwn(), "hey <@U0MIRA> ");
    // "@" alone in a channel: the aliases first.
    c.edit().clear();
    c.edit().insertText("@");
    pump();
    REQUIRE(c.pickList() != nullptr);
    CHECK_STR(c.pickList()->item(0).title, "@channel");
    CHECK_STR(c.pickList()->item(0).subtitle, "Notify everyone in this channel");
}

TEST("composer: # lists channels, : completes emoji") {
    Harness h;
    auto   &c = h.composer();
    c.edit().focus();
    c.edit().insertText("see #eng");
    pump();
    REQUIRE(c.pickList() != nullptr);
    CHECK_STR(c.pickList()->item(0).title, "engineering");
    c.pickList()->confirm();
    pump();
    CHECK_STR(c.mrkdwn(), "see <#C0ENG|engineering> ");
    c.edit().clear();
    c.edit().insertText(":tad");
    pump();
    REQUIRE(c.pickList() != nullptr);
    CHECK_STR(c.pickList()->item(0).display, emoji::toUnicode("tada") + "  :tada:");
    // The ranking: the common emoji first, prefix → _-+ boundary →
    // anywhere, custom emoji as :name:, at most 8.
    auto r = shell::emojiCompletions(h.store, "ok");
    REQUIRE(!r.empty());
    CHECK_STR(r[0].name, "ok_hand");
    CHECK(shell::emojiCompletions(h.store, "a").size() == 8);
    h.store.setCustomEmoji("abcdq", "https://emoji.example/abcdq.png");
    h.store.setCustomEmoji("xq_bcdq", "https://emoji.example/xq.png");
    r = shell::emojiCompletions(h.store, "bcdq");
    REQUIRE(r.size() == 2);
    CHECK_STR(r[0].name, "xq_bcdq"); // the boundary match before the substring one
    CHECK(r[0].custom);
    CHECK_STR(r[1].name, "abcdq");
    c.edit().clear();
    c.edit().insertText(":bcdq");
    pump();
    REQUIRE(c.pickList() != nullptr);
    c.pickList()->confirm();
    pump();
    CHECK(c.edit().text().find(":xq_bcdq:") != std::string::npos);
}

TEST("composer: editing shows the banner; Escape and its cross leave it") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    auto         &c      = h.composer();
    h.backend.send(design, "mine *bold* & <@U0MIRA>", 0, nullptr);
    pump();
    const Ts ts = h.store.conversation(design).messages.back().ts;
    c.beginEdit(ts);
    CHECK(c.editBannerShown());
    // The mrkdwn stays literal, mentions are pills, entities decoded.
    CHECK_STR(c.edit().text(), "mine *bold* & @Mira Okafor");
    CHECK_STR(c.mrkdwn(), "mine *bold* & <@U0MIRA>");
    c.endEdit();
    CHECK_FALSE(c.editBannerShown());
}

TEST("composer: undo send takes the message back and returns the text") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    auto         &c      = h.composer();
    const size_t  before = h.store.conversation(design).messages.size();
    c.edit().insertText("oops");
    REQUIRE(c.send());
    CHECK(c.undoOffered());
    CHECK(h.store.conversation(design).messages.size() == before + 1);
    REQUIRE(c.undoSend());
    pump(40);
    CHECK_STR(c.edit().text(), "oops");
    CHECK(h.store.conversation(design).messages.size() == before);
    CHECK_FALSE(c.undoOffered());
}

TEST("composer: a message that never went out comes back, in front of what's typed") {
    Harness           h;
    const ConvRef     design = h.conv("C0DESIGN");
    const ConvRef     jonas  = h.conv("D0JONAS");
    const std::string png    = MSGA_TEST_ASSETS "/images/blog-hero.png";
    const std::string scope  = h.sh->drafts().scope();
    auto             &c      = h.composer();
    // Shown: the editor gets it, what was typed since on the next line.
    c.edit().insertText("typed since");
    h.sh->giveBackUnsent(scope, h.store, design, 0, "lost *bold* & <@U0MIRA>", {png});
    CHECK_STR(c.mrkdwn(), "lost *bold* & <@U0MIRA>\ntyped since");
    CHECK(c.attachments() == std::vector<std::string>{png});
    c.clearAttachments();
    c.edit().clear();
    // Moved on: it waits in front of that conversation's draft.
    h.sh->open(jonas);
    pump();
    c.edit().insertText("draft");
    h.sh->open(design);
    pump();
    h.sh->giveBackUnsent(scope, h.store, jonas, 0, "lost", {png});
    CHECK(c.edit().empty());
    h.sh->open(jonas);
    pump();
    CHECK_STR(c.mrkdwn(), "lost\ndraft");
    CHECK(c.attachments() == std::vector<std::string>{png});
    // Another workspace's: its own drafts, not the one shown.
    c.clearAttachments();
    c.edit().clear();
    h.sh->giveBackUnsent(scope + "-other", h.store, jonas, 0, "elsewhere", {});
    CHECK(c.edit().empty());
}

TEST("composer: a suggested reply is the placeholder, Tab takes it") {
    Harness h;
    h.backend.suggestFor = h.conv("D0JONAS");
    h.sh->open(h.conv("D0JONAS"));
    pump();
    auto &c = h.composer();
    CHECK_STR(c.suggestion(), "run the tests again");
    CHECK_STR(c.edit().accessibleName(), "run the tests again  \xE2\x86\x92");
    REQUIRE(c.acceptSuggestion());
    CHECK_STR(c.edit().text(), "run the tests again");
}

TEST("composer: CommonMark habits go out as mrkdwn") {
    CHECK_STR(mrkdwn::convertOutgoing("**bold** and ~~gone~~"), "*bold* and ~gone~");
    CHECK_STR(mrkdwn::convertOutgoing("***both***"), "*_both_*");
    CHECK_STR(mrkdwn::convertOutgoing("[site](https://x.test/a)"), "<https://x.test/a|site>");
    CHECK_STR(mrkdwn::convertOutgoing("5 ** 2 stays"), "5 ** 2 stays");
    CHECK_STR(mrkdwn::convertOutgoing("`**code**` <@U1|x>"), "`**code**` <@U1|x>");
    CHECK_STR(
        mrkdwn::convertOutgoing("- a\n- b\n  - c"),
        "\xE2\x80\xA2 a\n\xE2\x80\xA2 b\n    \xE2\x80\xA2 c"
    );
    CHECK_STR(mrkdwn::convertOutgoing("3. x\n4. y"), "3. x\n4. y");
    CHECK_STR(mrkdwn::convertOutgoing("```js\nlet a\n```"), "```\nlet a\n```");
    CHECK_STR(mrkdwn::convertOutgoing("```ls -la\n```"), "```\nls -la\n```");
    CHECK_STR(mrkdwn::convertOutgoing("> **q**"), "> *q*");
}

TEST("composer: the typing indicator names who types") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    h.store.setTyping(design, h.store.findUser("U0MIRA"), 0, true);
    CHECK_STR(h.sh->typing().text(), "Mira Okafor is typing\xE2\x80\xA6");
    h.store.setTyping(design, h.store.findUser("U0JONAS"), 0, true);
    CHECK_STR(h.sh->typing().text(), "Mira Okafor, Jonas Weber are typing\xE2\x80\xA6");
    h.store.setTyping(design, h.store.findUser("U0MIRA"), 0, false);
    h.store.setTyping(design, h.store.findUser("U0JONAS"), 0, false);
    CHECK(h.sh->typing().text().empty());
    CHECK_FALSE(h.sh->typing().visible());
}

#ifdef MSGA_HAVE_MESSAGES
TEST("pickers: the emoji categories; the fake backend's GIFs go in as a pill") {
    Harness h;
    auto   *ep = screens::EmojiPicker::show(*h.win, {400, 700, 20, 20}, h.ctx, nullptr);
    pump();
    CHECK_STR(
        joined(ep->sections()),
        "Smileys & people|Animals & nature|Food & drink|Travel & places|Activities|Objects|"
        "Symbols|Flags"
    );
    CHECK(ep->tabCount() == 8);
    ep->filter("tada");
    CHECK_STR(joined(ep->sections()), "Search results");
    CHECK(ep->selected() == 0);
    h.win->closeAllPopups();
    pump();

    auto &c = h.composer();
    c.openGif();
    pump();
    std::vector<Backend::Gif> gifs;
    h.backend.searchGifs({}, [&](std::vector<Backend::Gif> g) { gifs = std::move(g); });
    REQUIRE(until([&] { return !gifs.empty(); }));
    CHECK_STR(gifs[0].title, "Big check");
    CHECK(gifs[0].width > 0);
}
#endif

// ── GIPHY (no network here) ─────────────────────────────────────────────────

TEST("giphy: the request URL carries the key, the trimmed query, limit, rating, bundle") {
    CHECK_STR(
        shell::GifSearch::requestUrl("  happy cat ", 30, "k&y"),
        "https://api.giphy.com/v1/gifs/search?api_key=k%26y&q=happy%20cat&limit=30&rating=pg-13"
        "&bundle=messaging_non_clips"
    );
    CHECK_STR(
        shell::GifSearch::requestUrl(" ", 99, "k"),
        "https://api.giphy.com/v1/gifs/trending?api_key=k&limit=50&rating=pg-13"
        "&bundle=messaging_non_clips"
    );
    // GIPHY refuses a term past 50 characters.
    const std::string url = shell::GifSearch::requestUrl(std::string(80, 'a'), 30, "k");
    CHECK(url.find("&q=" + std::string(50, 'a') + "&") != std::string::npos);
}

TEST("giphy: a search body maps to preview, posted rendition, size and title") {
    const char *body = R"({"data":[
        {"title":"Cat GIF","alt_text":"A cat dancing","images":{
            "fixed_width_downsampled":{"url":"https://m.giphy.com/1/200w_d.gif","width":"200","height":"150"},
            "fixed_width":{"url":"https://m.giphy.com/1/200w.gif","width":"200","height":"150"}}},
        {"title":"Dog GIF","images":{
            "preview_gif":{"url":"https://m.giphy.com/2/p.gif","width":"100","height":"80"},
            "downsized":{"url":"https://m.giphy.com/2/d.gif"}}},
        {"title":"Bare","images":{"original":{"mp4":"https://m.giphy.com/3/o.mp4"}}},
        {"title":"Preview only","images":{"fixed_width_small":{"url":"https://m.giphy.com/4/s.gif"}}}
    ],"pagination":{"count":4}})";
    const auto  gifs = shell::GifSearch::parseResponse(body);
    REQUIRE(gifs.size() == 3); // the one without a usable rendition is skipped
    CHECK_STR(gifs[0].preview, "https://m.giphy.com/1/200w_d.gif");
    CHECK_STR(gifs[0].url, "https://m.giphy.com/1/200w.gif");
    CHECK(gifs[0].width == 200 && gifs[0].height == 150);
    CHECK_STR(gifs[0].title, "A cat dancing"); // alt text before the title
    CHECK_STR(gifs[1].url, "https://m.giphy.com/2/d.gif");
    CHECK_STR(gifs[1].title, "Dog GIF");
    CHECK_STR(gifs[2].url, gifs[2].preview); // nothing to post but the preview
    CHECK(gifs[2].width == 0);
    CHECK(shell::GifSearch::parseResponse("not json").empty());
    CHECK(shell::GifSearch::parseResponse(R"({"meta":{"status":401}})").empty());
}

TEST("giphy: errors name the status, never the URL") {
    CHECK_STR(shell::GifSearch::errorMessage(429), "GIPHY rate limit reached. Try again shortly.");
    CHECK_STR(shell::GifSearch::errorMessage(500), "GIPHY request failed (HTTP 500).");
    CHECK_STR(
        shell::GifSearch::errorMessage(0),
        "Could not reach GIPHY \xE2\x80\x94 check your connection."
    );
}

TEST("giphy: without a key the search answers later, asking for one") {
    shell::GifSearch         s(app().platform());
    shell::GifSearch::Result got;
    bool                     done = false;
    s.search("cats", "  ", [&](shell::GifSearch::Result r) {
        got  = std::move(r);
        done = true;
    });
    CHECK(!done); // never inside the call
    REQUIRE(until([&] { return done; }));
    CHECK(got.keyRejected);
    CHECK_STR(got.error, "No GIPHY API key configured.");
}

// ── Lists, clipboard, remembered picks ──────────────────────────────────────

TEST("composer: a list goes out as mrkdwn fallback plus a rich_text block") {
    Harness h;
    auto   &c = h.composer();
    c.edit().setText("plan:\n- **one**\n- two");
    REQUIRE(c.send());
    pump();
    CHECK(h.backend.lastBlocks.find("\"rich_text_list\"") != std::string::npos);
    const auto &msgs = h.store.conversation(h.conv("C0DESIGN")).messages;
    REQUIRE(!msgs.empty());
    CHECK_STR(msgs.back().text, "plan:\n\xE2\x80\xA2 *one*\n\xE2\x80\xA2 two");
    // No list, no block.
    h.backend.lastBlocks = "unset";
    c.edit().setText("just **text**");
    REQUIRE(c.send());
    CHECK_STR(h.backend.lastBlocks, "unset");
}

TEST("composer: a pasted picture becomes an attachment, pasted text stays text") {
    Harness           h;
    auto             &c   = h.composer();
    plat::App        &pa  = app().platform();
    const std::string png = "\x89PNG\r\n\x1a\n-fake-picture-bytes";
    pa.setClipboard({{"image/png", png}});
    c.edit().paste();
    REQUIRE(until([&] { return c.attachments().size() == 1; }));
    const std::string &path = c.attachments()[0];
    const std::string  name(file::baseName(path));
    CHECK(name.rfind("Pasted image ", 0) == 0);
    CHECK(file::extension(path) == "png");
    std::string bytes;
    CHECK(file::readAll(path, &bytes) && bytes == png);
    CHECK(c.edit().text().empty());
    file::remove(path);

    pa.setClipboardText("hello");
    c.edit().paste();
    REQUIRE(until([&] { return c.edit().text() == "hello"; }));
    CHECK(c.attachments().size() == 1);
}

namespace {

// "a b/c" → "a%20b/c": a file URI's path.
std::string uriPath(const std::string &p) {
    std::string out;
    for (char ch : p)
        out += ch == ' ' ? std::string("%20") : std::string(1, ch);
    return out;
}

} // namespace

TEST("composer: files copied in a file manager are attached, other URIs paste as text") {
    Harness           h;
    auto             &c    = h.composer();
    plat::App        &pa   = app().platform();
    const std::string dir  = pa.standardDir(plat::StandardDir::Temp);
    const std::string path = file::join(dir, "msga paste test.txt");
    REQUIRE(file::writeAtomic(path, "notes"));
    pa.setClipboard(
        {{"text/uri-list", "# copied\r\n" + base::test::fileUri(uriPath(path)) + "\r\n"},
         {"text/plain;charset=utf-8", path}}
    );
    c.edit().paste();
    REQUIRE(until([&] { return c.attachments().size() == 1; }));
    CHECK_STR(c.attachments()[0], path);
    CHECK(c.edit().text().empty());
    file::remove(path);

    pa.setClipboard(
        {{"text/uri-list", "https://example.com/a\r\n"},
         {"text/plain;charset=utf-8", "https://example.com/a"}}
    );
    c.edit().paste();
    REQUIRE(until([&] { return !c.edit().text().empty(); }));
    CHECK_STR(c.edit().text(), "https://example.com/a");
    CHECK(c.attachments().size() == 1);
}

#if defined(__linux__)
TEST("composer: a middle click attaches the primary selection's picture or files") {
    // A middle click inserts the primary selection like a paste.
    Harness           h;
    auto             &c           = h.composer();
    plat::App        &pa          = app().platform();
    auto             *hk          = pa.testHooks();
    const ui::RectF   r           = c.edit().windowRect();
    const std::string png         = "\x89PNG\r\n\x1a\n-primary-picture";
    const auto        middleClick = [&] {
        hk->injectPointerMove(h.win->native(), {r.x + 20.0, r.y + r.h / 2.0});
        hk->injectButton(h.win->native(), plat::Button::Middle, true);
        hk->injectButton(h.win->native(), plat::Button::Middle, false);
    };
    pa.setClipboardText("not this one"); // the Ctrl+V clipboard is not read
    pa.setClipboard({{"image/png", png}}, plat::Selection::Primary);
    middleClick();
    REQUIRE(until([&] { return c.attachments().size() == 1; }));
    std::string bytes;
    CHECK(file::readAll(c.attachments()[0], &bytes) && bytes == png);
    CHECK(c.edit().text().empty());
    file::remove(c.attachments()[0]);

    const std::string path =
        file::join(pa.standardDir(plat::StandardDir::Temp), "msga primary.txt");
    REQUIRE(file::writeAtomic(path, "notes"));
    pa.setClipboard(
        {{"text/uri-list", base::test::fileUri(uriPath(path)) + "\r\n"},
         {"text/plain;charset=utf-8", path}},
        plat::Selection::Primary
    );
    middleClick();
    REQUIRE(until([&] { return c.attachments().size() == 2; }));
    CHECK_STR(c.attachments()[1], path);
    CHECK(c.edit().text().empty());
    file::remove(path);

    // Text stays text, and plain.
    pa.setClipboard(
        {{"text/html", "<b>bold</b>"}, {"text/plain;charset=utf-8", "selected words"}},
        plat::Selection::Primary
    );
    middleClick();
    REQUIRE(until([&] { return !c.edit().text().empty(); }));
    CHECK_STR(c.edit().text(), "selected words");
    CHECK(c.attachments().size() == 2);
}
#endif

#ifdef MSGA_HAVE_MESSAGES
// The heap allocations counter (test_messages.cpp, same binary).
void   countAllocs(bool on);
size_t testAllocs();

// M11: a keystroke in the picker or the composer's ":" list neither copies
// nor sorts the workspace's custom emoji (a Grid workspace has thousands);
// a change of the set still shows up in both.
TEST("pickers: typing costs nothing per custom emoji; a changed set shows up") {
    Harness     h;
    auto       *ep   = screens::EmojiPicker::show(*h.win, {400, 700, 20, 20}, h.ctx, nullptr);
    const char *qs[] = {"qz", "qzx", "qzxv", "qzx", "qz", "zqx"};
    auto        type = [&] {
        countAllocs(true);
        const size_t before = testAllocs();
        for (int round = 0; round < 5; ++round)
            for (const char *q : qs) {
                ep->filter(q);
                CHECK(shell::emojiCompletions(h.store, q).empty());
            }
        countAllocs(false);
        return testAllocs() - before;
    };
    pump();
    ep->filter("x"); // anything built lazily on a first search
    const size_t few = type();
    // Long names and URLs: a copy of one would allocate.
    for (int i = 0; i < 5000; ++i) {
        const std::string n = "workspace_party_emoji_" + std::to_string(i);
        h.store.setCustomEmoji(n, "https://emoji.example/" + n + ".png");
        h.store.setCustomEmoji(n + "_alias", "alias:" + n);
    }
    const uint64_t rev = h.store.customEmojiRevision();
    ep->filter("x"); // the new names folded, once
    CHECK(type() == few);
    CHECK(h.store.customEmojiRevision() == rev);
    // Added, removed, renamed: the next keystroke sees the new set.
    ep->filter("party_emoji_4999");
    REQUIRE(ep->cellCount() == 1);
    CHECK_STR(ep->cellName(0), "workspace_party_emoji_4999");
    h.store.setCustomEmoji("qzxv_new", "https://emoji.example/new.png");
    ep->filter("qzxv");
    REQUIRE(ep->cellCount() == 1);
    CHECK_STR(ep->cellName(0), "qzxv_new");
    auto r = shell::emojiCompletions(h.store, "qzxv");
    REQUIRE(r.size() == 1);
    CHECK_STR(r[0].name, "qzxv_new");
    std::unordered_map<std::string, std::string> all = h.store.customEmoji();
    all.erase("qzxv_new");
    all["qzxv_renamed"] = "https://emoji.example/new.png";
    h.store.replaceCustomEmoji(std::move(all));
    ep->filter("qzxv");
    REQUIRE(ep->cellCount() == 1);
    CHECK_STR(ep->cellName(0), "qzxv_renamed");
    r = shell::emojiCompletions(h.store, "qzxv");
    REQUIRE(r.size() == 1);
    CHECK_STR(r[0].name, "qzxv_renamed");
    // The same set again (a reload) is no change.
    const uint64_t same = h.store.customEmojiRevision();
    h.store.replaceCustomEmoji(h.store.customEmoji());
    h.store.setCustomEmoji("qzxv_renamed", "https://emoji.example/new.png");
    CHECK(h.store.customEmojiRevision() == same);
    h.win->closeAllPopups();
    pump();
}
#endif

#ifdef MSGA_HAVE_MESSAGES
TEST("pickers: the emoji picks and skin tone are kept in Settings") {
    Harness h;
    auto   *ep = screens::EmojiPicker::show(*h.win, {400, 700, 20, 20}, h.ctx, nullptr);
    pump();
    ep->setSkinTone(4);
    CHECK(h.settings.emojiSkinTone == 4);
    ep->pick("wave::skin-tone-4");
    pump();
    REQUIRE(!h.settings.emojiRecent.empty());
    CHECK_STR(h.settings.emojiRecent.front(), "wave");
    // A new shell starts from what was saved.
    h.settings.emojiRecent = {"rocket"};
    h.sh.reset();
    h.sh      = std::make_unique<shell::Shell>(h.ctx, *h.win, h.settings, std::string());
    auto *ep2 = screens::EmojiPicker::show(*h.win, {400, 700, 20, 20}, h.ctx, nullptr);
    pump();
    CHECK(ep2->skinTone() == 4);
    CHECK(ep2->cellCount() > 0 && ep2->cellName(0) == "rocket");
    h.win->closeAllPopups();
    pump();
}
#endif

// ── Spelling (highlighting and the composer's menu) ─────────────────────────

namespace {

// Knows every word but "wrold" / "teh"; suggests "world", "would".
struct SpellFake : spell::Backend {
    bool load(const std::vector<std::string> &) override { return true; }
    bool threadSafe() const override { return true; }
    bool check(std::string_view w) override { return w != "wrold" && w != "teh"; }
    std::vector<std::string> suggest(std::string_view w, int) override {
        if (w == "wrold")
            return {"world", "would"};
        return {};
    }
    void addToDictionary(std::string_view) override {}
};

struct SpellOn {
    SpellOn() {
        spell::Checker::instance().setBackendForTesting(
            app().platform(), std::make_unique<SpellFake>()
        );
    }
    ~SpellOn() { spell::Checker::instance().setBackendForTesting(app().platform(), nullptr); }
};

std::string squiggled(ui::TextEdit &e) {
    std::string out;
    for (const auto &r : e.squiggles())
        out += (out.empty() ? "" : "|") + e.text().substr(r.from, r.to - r.from);
    return out;
}

ui::Menu *openSpellMenu(Harness &h, uint32_t offset) {
    auto &e = h.composer().edit();
    if (!CHECK(e.onContextMenu && e.onContextMenu(offset, {300, 600})))
        return nullptr;
    // The only popup: the menu opens once the suggestions are in.
    until([&] { return h.win->topPopup() != nullptr; });
    return static_cast<ui::Menu *>(h.win->topPopup());
}

} // namespace

TEST("spelling: nothing is underlined while checking is off") {
    Harness h;
    auto   &e = h.composer().edit();
    e.setText("hello wrold");
    pump(40);
    CHECK(e.squiggles().empty());
}

TEST("spelling: misspelled words are underlined, pills and code never") {
    SpellOn on;
    Harness h;
    auto   &c = h.composer();
    auto   &e = c.edit();
    e.setText("hello wrold `teh` and teh ");
    // A pill whose label is a misspelling: never checked.
    e.setSelection(uint32_t(e.text().size()), uint32_t(e.text().size()));
    e.insertText("@wrold");
    e.setSelection(uint32_t(e.text().size() - 6), uint32_t(e.text().size()));
    e.setLink("<@U0MIRA>");
    REQUIRE(until([&] { return !e.squiggles().empty(); }));
    pump(20);
    CHECK_STR(squiggled(e), "wrold|teh");
    // Sending still carries the pill's token.
    CHECK(c.mrkdwn().find("<@U0MIRA>") != std::string::npos);
}

TEST("spelling: the word being typed is underlined once the caret leaves it") {
    SpellOn on;
    Harness h;
    auto   &e = h.composer().edit();
    e.focus();
    e.insertText("so wrold");
    REQUIRE(until([&] { return e.squiggles().size() == 1; }));
    CHECK(!e.squiggleShown(e.squiggles()[0])); // the caret is at its end
    e.insertText(" ");
    REQUIRE(until([&] { return e.squiggles().size() == 1; }));
    CHECK(e.squiggleShown(e.squiggles()[0]));
    // Editing the word drops its squiggle until it is checked again.
    e.setSelection(3, 3);
    e.insertText("x");
    CHECK(e.squiggles().empty());
}

TEST("spelling: moving the caret onto a misspelled word keeps its squiggle") {
    SpellOn on;
    Harness h;
    auto   &e = h.composer().edit();
    e.focus();
    e.insertText("so wrold now");
    REQUIRE(until([&] { return e.squiggles().size() == 1; }));
    // Put at its end (a click, an arrow key): not typing it, so it shows.
    e.setSelection(8, 8);
    CHECK(e.squiggleShown(e.squiggles()[0]));
    // Typing there again (retyping its last letter) hides it until the
    // caret leaves.
    e.setSelection(7, 8);
    e.insertText("d");
    REQUIRE(until([&] { return e.squiggles().size() == 1; }));
    CHECK(!e.squiggleShown(e.squiggles()[0]));
    e.setSelection(9, 9);
    CHECK(e.squiggleShown(e.squiggles()[0]));
}

TEST("spelling: the context menu offers suggestions and replaces the word") {
    SpellOn on;
    Harness h;
    auto   &e = h.composer().edit();
    e.setText("hello wrold now");
    REQUIRE(until([&] { return e.squiggles().size() == 1; }));
    ui::Menu *m = openSpellMenu(h, 8);
    REQUIRE(m != nullptr);
    std::string labels;
    for (const auto &it : m->items())
        labels += (labels.empty() ? "" : "|") + (it.separator ? std::string("-") : it.label);
    CHECK_STR(labels, "world|would|-|Add to dictionary|Ignore|-|Cut|Copy|Paste|-|Select all");
    CHECK(m->items()[0].bold);
    m->setCurrent(0);
    ui::Event enter{ui::EventType::KeyDown};
    enter.key = plat::Key::Enter;
    m->onEvent(enter);
    pump();
    CHECK_STR(e.text(), "hello world now");
    CHECK(e.undo()); // one undo step brings the typo back
    CHECK_STR(e.text(), "hello wrold now");
}

TEST("spelling: a word without suggestions says so; Ignore stops underlining it") {
    SpellOn on;
    Harness h;
    auto   &e = h.composer().edit();
    e.setText("teh cat");
    REQUIRE(until([&] { return e.squiggles().size() == 1; }));
    ui::Menu *m = openSpellMenu(h, 1);
    REQUIRE(m != nullptr);
    REQUIRE(!m->items().empty());
    CHECK_STR(m->items()[0].label, "No spelling suggestions");
    CHECK(!m->items()[0].enabled);
    int ignore = -1;
    for (size_t i = 0; i < m->items().size(); ++i)
        if (m->items()[i].label == "Ignore")
            ignore = int(i);
    REQUIRE(ignore >= 0);
    m->setCurrent(ignore);
    ui::Event enter{ui::EventType::KeyDown};
    enter.key = plat::Key::Enter;
    m->onEvent(enter);
    REQUIRE(until([&] { return e.squiggles().empty(); }));
    // A right-click on a correct word is the standard menu.
    CHECK(!e.onContextMenu(5, {300, 600}));
}

TEST("spelling: the squiggle is painted in the danger colour just under the word") {
    SpellOn on;
    Harness h;
    auto   &e = h.composer().edit();
    e.setText("hello wrold now");
    REQUIRE(until([&] { return e.squiggles().size() == 1; }));
    pump(20);
    auto *hooks = app().platform().testHooks();
    REQUIRE(hooks != nullptr);
    const float     s    = h.win->scale();
    const ui::RectF r    = e.windowRect();
    const uint32_t  d    = ui::color(ui::C::Danger);
    const auto      near = [d](uint32_t px) {
        for (int sh : {0, 8, 16})
            if (std::abs(int((px >> sh) & 0xff) - int((d >> sh) & 0xff)) > 40)
                return false;
        return true;
    };
    int       minX = 1 << 30, maxX = -1, minY = 1 << 30, maxY = -1;
    const int x0 = int(r.x * s), x1 = int((r.x + r.w) * s), y0 = int(r.y * s),
              y1 = int((r.y + r.h) * s);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            uint32_t px = 0;
            hooks->readPixel(h.win->native(), x, y, &px);
            if (near(px)) {
                minX = std::min(minX, x), maxX = std::max(maxX, x);
                minY = std::min(minY, y), maxY = std::max(maxY, y);
            }
        }
    REQUIRE(maxX >= 0);
    // One short wavy band, about a word wide and a few pixels high.
    CHECK(maxY - minY <= int(4 * s));
    CHECK(maxX - minX > int(20 * s) && maxX - minX < int(80 * s));
}

TEST("scheduled: a new name of mine shows on the cards, the list itself unchanged") {
    Harness h;
    h.backend.schedule = true;
    h.sh->workspaceChanged();
    h.sidebar().rebuild();
    pump();
    model::Store::ScheduledItem s;
    s.id   = "S1";
    s.conv = h.conv("C0DESIGN");
    s.at   = base::nowSecs() + 3600;
    s.text = "later";
    h.store.setScheduled({s});
    pump();
    h.sh->showPage(shell::Shell::Page::Scheduled);
    pump();
    shell::ScheduledPage *p = h.sh->scheduledPage();
    REQUIRE(p->cardCount() == 1);
    CHECK_FALSE(hasText(p, "Alex Renamed"));
    h.store.user(h.store.me).displayName = "Alex Renamed";
    h.store.usersChanged();
    pump();
    CHECK(hasText(p, "Alex Renamed"));
}

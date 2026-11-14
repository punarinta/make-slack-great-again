// The Claude Code parts of the conversation UI: the error banner, the composer's lock, prompt
// history (↑ / ↓, Ctrl+R), slash commands msga runs itself, the zen toggle, "thinking (…)", the
// thread panel's "Open as session" and read-only threads, message buttons and the agent delete
// rule.
#include "app/fake/fake_backend.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/testing.h"
#include "screens/shell/composer.h"
#include "screens/shell/header.h"
#include "screens/shell/header_jobs.h"
#include "screens/shell/shell.h"
#include "screens/shell/sidebar_footer.h"
#include "screens/shell/typing_indicator.h"
#include "ui/controls.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/message_list.h"
#include "app/screens/messages/thread_panel.h"
#endif

#include <memory>

using namespace model;
using K = plat::Key;

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

// The test workspace as a Claude Code one: DMs are sessions with commands,
// a prompt history and buttons that can't be pressed.
struct AgentFake : fake::FakeBackend {
    using fake::FakeBackend::FakeBackend;
    Capabilities capabilities() const override {
        Capabilities c  = fake::FakeBackend::capabilities();
        c.agentSessions = true;
        c.zenMode       = true;
        c.slashCommands = true;
        c.selfStatus    = false;
        return c;
    }
    bool isAgentSession(ConvRef c) const override {
        return c < _store.conversationCount() && _store.conversation(c).kind == ConvKind::Dm;
    }
    std::vector<Command> commands(ConvRef) override {
        ++asked;
        return cmds;
    }
    uint64_t commandsRevision(ConvRef) override { return cmdsRev; }
    LocalResult
    runLocalCommand(ConvRef c, Ts thread, const std::string &name, const std::string &) override {
        ran.push_back(name);
        ranIn       = c;
        ranInThread = thread;
        LocalResult r;
        if (name == "status")
            r.status = {{"Version", "2.1.0"}, {"Folder", "~/src/msga"}};
        else if (name == "clear")
            r.open = clearTo;
        return r;
    }
    std::vector<std::string> promptHistory(ConvRef) override { return history; }
    bool    canDeleteMessage(ConvRef, Ts ts) const override { return ts != busyTs; }
    bool    threadAcceptsReplies(ConvRef, Ts root) const override { return root != closedRoot; }
    bool    threadOpensAsSession(ConvRef, Ts root) const override { return root == branchRoot; }
    ConvRef openThreadAsSession(ConvRef, Ts) override { return clearTo; }
    void    pressButton(ConvRef, Ts, const std::string &id, Done done) override {
        pressed = id;
        if (done)
            done(false, "nope");
    }
    void setZenMode(bool on) override { zen = on; }

    std::vector<Command> cmds{
        {"status", "Show the session's status", {}, true},
        {"btw", "Ask a side question", "<question>", false},
        {"clear", "Start a new session", {}, true},
        {"compact", "Compact the conversation", {}, false},
    };
    uint64_t                 cmdsRev = 1;
    int                      asked   = 0; // commands() calls
    std::vector<std::string> ran, history{"third prompt", "second\nprompt", "first prompt"};
    ConvRef                  clearTo = kNoConv, ranIn = kNoConv;
    Ts                       ranInThread = -1;
    Ts                       busyTs = 0, closedRoot = 0, branchRoot = 0;
    std::string              pressed;
    bool                     zen = false;
};

struct Harness {
    Store     store;
    AgentFake backend{store, app().platform()};
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
    ConvRef                       dm = kNoConv;

    Harness() {
        backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
        bool done = false;
        backend.connect([&](bool ok, const std::string &) { done = ok; });
        // By the clock: pump() returns early on every post from a worker
        // thread, so a count of pumps can run out before the connect.
        const int64_t end = base::monotonicMs() + 10000;
        while (!done && base::monotonicMs() < end)
            app().pump(5);
        plat::WindowDesc d;
        d.size          = {1200, 800};
        d.decorations   = plat::Decorations::Custom;
        win             = std::make_unique<ui::Window>(d);
        sh              = std::make_unique<shell::Shell>(ctx, *win, settings, std::string());
        dm              = store.findConversation("D0JONAS");
        backend.clearTo = store.findConversation("D0MIRA");
        sh->open(dm);
        pump();
    }
    // ctrl: the primary modifier (Cmd on macOS), as the shortcuts mean it.
    void key(K k, bool ctrl = false) {
        auto   *t    = app().platform().testHooks();
        const K held = plat::primaryMod() == plat::ModSuper ? K::SuperLeft : K::ControlLeft;
        if (ctrl)
            t->injectKey(win->native(), held, true);
        t->injectKey(win->native(), k, true);
        t->injectKey(win->native(), k, false);
        if (ctrl)
            t->injectKey(win->native(), held, false);
        pump();
    }
};

} // namespace

TEST("error banner: the message under the tabs, gone after 5 s") {
    Harness    h;
    ui::Label *b = h.sh->errorBanner();
    REQUIRE(b != nullptr);
    CHECK_FALSE(b->visible());
    h.backend.onError = [&](const std::string &m) { h.sh->showError(m); };
    h.backend.onError("Claude Code didn't take the queued message.");
    CHECK(b->visible());
    CHECK_STR(b->text(), "Claude Code didn't take the queued message.");
    const double end = app().nowMs() + 7000;
    while (b->visible() && app().nowMs() < end)
        app().pump(20);
    CHECK_FALSE(b->visible());
}

TEST("composer: a read-only conversation locks it and says why, and unlocks") {
    Harness h;
    auto   &c = h.sh->composer();
    CHECK(c.enabled());
    h.store.updateConversation(h.dm, [](Conversation &cv) {
        cv.readOnly = "This session is running in a terminal.";
    });
    pump();
    CHECK_FALSE(c.enabled());
    CHECK_STR(c.edit().accessibleName(), "This session is running in a terminal.");
    h.store.updateConversation(h.dm, [](Conversation &cv) { cv.readOnly.clear(); });
    pump();
    CHECK(c.enabled());
    CHECK(c.edit().accessibleName().rfind("Message ", 0) == 0);
}

TEST("prompt history: up from an empty editor steps back, down comes forward") {
    Harness h;
    auto   &c = h.sh->composer();
    c.edit().focus();
    h.key(K::Up);
    CHECK_STR(c.edit().text(), "third prompt");
    h.key(K::Up);
    CHECK_STR(c.edit().text(), "second\nprompt");
    CHECK(c.edit().caret() == 0); // going back from the first line
    h.key(K::Up);
    CHECK_STR(c.edit().text(), "first prompt");
    h.key(K::Up); // the oldest: stays
    CHECK_STR(c.edit().text(), "first prompt");
    h.key(K::Down);
    CHECK_STR(c.edit().text(), "second\nprompt");
    // Forward leaves the caret at the end: Down goes on at once. Inside a
    // multi-line prompt Up moves to its first line first.
    h.key(K::Up);
    CHECK_STR(c.edit().text(), "second\nprompt");
    h.key(K::Down);
    h.key(K::Down);
    CHECK_STR(c.edit().text(), "third prompt");
    h.key(K::Down);
    CHECK(c.edit().empty());
    // A draft is never replaced.
    c.edit().insertText("draft");
    h.key(K::Up);
    CHECK_STR(c.edit().text(), "draft");
}

TEST("prompt history: Ctrl+R searches it, Enter takes one into the editor") {
    CHECK(shell::historyFilter({"fix the build", "Fix tests", "docs"}, "fix").size() == 2);
    CHECK(shell::historyFilter({"fix the build", "docs"}, "").size() == 2);
    CHECK(shell::historyFilter({"fix the build", "docs"}, "build fix").size() == 1);
    const auto m = shell::historyMatches("Fix the fix", "fix");
    REQUIRE(m.size() == 2);
    CHECK(m[0].first == 0 && m[0].second == 3);
    CHECK(m[1].first == 8);

    Harness h;
    auto   &c = h.sh->composer();
    c.edit().focus();
    c.edit().insertText("prompt");
    h.key(K::R, true);
    shell::HistorySearch *hs = c.historySearch();
    REQUIRE(hs != nullptr);
    CHECK_STR(hs->query(), "prompt");
    CHECK(hs->matches().size() == 3);
    CHECK_STR(hs->selectedEntry(), "third prompt");
    h.key(K::Up); // older
    CHECK_STR(hs->selectedEntry(), "second\nprompt");
    hs->field().insertText(" fir"); // "prompt fir"
    pump();
    CHECK(hs->matches().size() == 1);
    h.key(K::Enter);
    CHECK(c.historySearch() == nullptr);
    CHECK_STR(c.edit().text(), "first prompt");
    // Esc leaves the draft alone.
    h.key(K::R, true);
    REQUIRE(c.historySearch() != nullptr);
    h.key(K::Escape);
    CHECK(c.historySearch() == nullptr);
    CHECK_STR(c.edit().text(), "first prompt");
}

// "/co" typed before Claude Code's commands came (msga's own only): the
// list shows /compact the moment they do, with nothing typed since.
TEST("slash commands: the list follows commands that arrive while typing") {
    Harness    h;
    auto      &c   = h.sh->composer();
    const auto all = h.backend.cmds;
    h.backend.cmds.pop_back(); // /compact
    ++h.backend.cmdsRev;
    c.edit().focus();
    c.edit().insertText("/co");
    pump();
    CHECK(c.pickList() == nullptr); // nothing by that name yet
    h.backend.cmds = all;
    ++h.backend.cmdsRev;
    h.sh->commandsChanged("other"); // another workspace's: not this composer's
    pump();
    CHECK(c.pickList() == nullptr);
    h.sh->commandsChanged(std::string());
    pump();
    REQUIRE(c.pickList() != nullptr);
    REQUIRE(c.pickList()->count() == 1);
    CHECK_STR(c.pickList()->item(0).title, "/compact");
    CHECK_STR(c.edit().text(), "/co");
}

TEST("slash commands: the list at the start, local ones run, the rest are sent") {
    Harness h;
    auto   &c = h.sh->composer();
    c.edit().focus();
    c.edit().insertText("/");
    pump();
    shell::PickList *p = c.pickList();
    REQUIRE(p != nullptr);
    REQUIRE(p->count() == 4);
    CHECK_STR(p->item(0).title, "/btw"); // by name
    CHECK_STR(p->item(0).usage, "<question>");
    CHECK(p->item(0).kind == shell::PickList::Item::Kind::Command);
    c.edit().insertText("st");
    pump();
    REQUIRE(c.pickList() != nullptr);
    REQUIRE(c.pickList()->count() == 1);
    c.pickList()->confirm();
    pump();
    CHECK_STR(c.edit().text(), "/status ");
    // Not at the start: no list.
    c.edit().clear();
    c.edit().insertText("see /");
    pump();
    CHECK(c.pickList() == nullptr);

    // /status: the status dialog, nothing sent.
    const size_t before = h.store.conversation(h.dm).messages.size();
    c.edit().clear();
    c.edit().insertText("/status");
    REQUIRE(c.send());
    pump();
    REQUIRE(h.backend.ran.size() == 1);
    CHECK_STR(h.backend.ran[0], "status");
    CHECK(h.store.conversation(h.dm).messages.size() == before);
    CHECK(c.edit().empty());
    CHECK(h.backend.ranInThread == 0); // the channel's composer: no thread
    ui::Popup *dlg = h.win->topPopup();
    REQUIRE(dlg != nullptr);
    CHECK(dlg->place() == ui::Popup::Place::Fill);
    dlg->close();
    pump();
    // /clear opens the conversation it answers.
    c.edit().insertText("/CLEAR");
    REQUIRE(c.send());
    pump();
    CHECK(h.sh->current() == h.backend.clearTo);
    // /btw is Claude Code's: a message.
    const ConvRef now = h.sh->current();
    const size_t  n   = h.store.conversation(now).messages.size();
    c.edit().insertText("/btw why");
    REQUIRE(c.send());
    pump();
    CHECK(h.backend.ran.size() == 2);
    CHECK(h.store.conversation(now).messages.size() == n + 1);
}

TEST("slash commands: the list is copied again only when the backend's revision moves") {
    Harness h;
    auto   &c = h.sh->composer();
    c.edit().focus();
    c.edit().insertText("/");
    pump();
    REQUIRE(c.pickList() != nullptr);
    const int asked = h.backend.asked;
    CHECK(asked >= 1);
    c.edit().insertText("c");
    pump();
    c.edit().insertText("o");
    pump();
    CHECK(h.backend.asked == asked); // keystrokes reuse what was fetched
    REQUIRE(c.pickList() != nullptr);
    REQUIRE(c.pickList()->count() == 1);
    CHECK_STR(c.pickList()->item(0).subtitle, "Compact the conversation");
    // Same names, another description: a new revision shows it.
    h.backend.cmds[3].desc = "Summarise the conversation";
    ++h.backend.cmdsRev;
    c.edit().insertText("m");
    pump();
    CHECK(h.backend.asked == asked + 1);
    REQUIRE(c.pickList() != nullptr);
    CHECK_STR(c.pickList()->item(0).subtitle, "Summarise the conversation");
}

TEST("slash commands: the thread composer runs them on the thread's conversation") {
    Harness     h;
    const auto &msgs = h.store.conversation(h.dm).messages;
    REQUIRE(!msgs.empty());
    const Ts root = msgs.front().ts;
    h.sh->openThread(h.dm, root);
    pump();
    shell::Composer *c = h.sh->threadComposer();
    REQUIRE(c != nullptr);
    const size_t before = h.store.conversation(h.dm).messages.size();
    c->edit().focus();
    c->edit().insertText("/status");
    REQUIRE(c->send());
    pump();
    REQUIRE(h.backend.ran.size() == 1);
    CHECK_STR(h.backend.ran[0], "status");
    CHECK(h.backend.ranIn == h.dm);
    CHECK(h.backend.ranInThread == root); // what it posts goes to the thread
    CHECK(c->edit().empty());
    CHECK(h.store.conversation(h.dm).messages.size() == before); // nothing posted
    ui::Popup *dlg = h.win->topPopup();
    REQUIRE(dlg != nullptr);
    dlg->close();
    pump();
}

TEST("footer: the zen toggle in the presence toggle's place, remembered") {
    Harness               h;
    shell::SidebarFooter &f = h.sh->sidebar().footer();
    f.refresh();
    REQUIRE(f.zenToggle() != nullptr);
    CHECK(f.zenToggle()->visible());
    CHECK_FALSE(f.toggle()->visible());
    CHECK_STR(
        f.zenToggle()->tooltip(), "Zen mode is off. Click to hide tool calls for easier reading."
    );
    f.zenToggle()->onClick();
    CHECK(f.zenOn());
    CHECK(h.settings.zenWorkspaces.size() == 1); // this workspace's own
    CHECK_FALSE(h.settings.zenMode("claude-code:other"));
    CHECK(h.backend.zen);
    CHECK_STR(
        f.zenToggle()->tooltip(), "Zen mode is on: tool calls are hidden. Click to show everything."
    );
}

TEST("typing: an agent at work is thinking, with its clock") {
    CHECK_STR(shell::TypingIndicator::formatElapsed(42'000), "42s");
    CHECK_STR(shell::TypingIndicator::formatElapsed(65'000), "1m 5s");
    CHECK_STR(shell::TypingIndicator::formatElapsed(7'380'000), "2h 3m");
    Harness     h;
    const auto &cv    = h.store.conversation(h.dm);
    const auto  agent = cv.dmUser;
    h.store.setTyping(h.dm, agent, 0, true, base::nowMicros() / 1000 - 65'000);
    pump();
    const std::string name(h.store.user(agent).label());
    CHECK_STR(h.sh->typing().text(), name + " is thinking (1m 5s)\xE2\x80\xA6");
    h.store.setTyping(h.dm, agent, 0, true, 0);
    pump();
    CHECK_STR(h.sh->typing().text(), name + " is typing\xE2\x80\xA6");
}

#ifdef MSGA_HAVE_MESSAGES
TEST("messages: any message deletes where the session allows; no edit or move") {
    Harness     h;
    const auto &msgs = h.store.conversation(h.dm).messages;
    REQUIRE(!msgs.empty());
    screens::MessageList ml(h.ctx);
    ml.showConversation(h.dm);
    const Ts   ts  = msgs.back().ts;
    const auto has = [&](int id) {
        for (const auto &it : ml.menuItems(ts))
            if (it.id == id)
                return true;
        return false;
    };
    CHECK(has(screens::MessageList::kDelete));
    CHECK_FALSE(has(screens::MessageList::kMoveToThread));
    CHECK_FALSE(has(screens::MessageList::kEdit));
    h.backend.busyTs = ts;
    CHECK_FALSE(has(screens::MessageList::kDelete));
}

TEST("thread panel: Open as session and a thread that takes no replies") {
    Harness     h;
    const auto &msgs = h.store.conversation(h.dm).messages;
    REQUIRE(!msgs.empty());
    const Ts root        = msgs.front().ts;
    h.backend.branchRoot = root;
    h.backend.closedRoot = root;
    h.sh->openThread(h.dm, root);
    pump();
    auto *tp = static_cast<screens::ThreadPanel *>(h.sh->threadPanel());
    REQUIRE(tp != nullptr);
    CHECK_FALSE(tp->composer()->visible());
    h.backend.closedRoot = 0;
    tp->show(h.dm, root);
    CHECK(tp->composer()->visible());
    REQUIRE(tp->onOpenAsSession);
    tp->onOpenAsSession(h.dm, root);
    pump();
    CHECK_FALSE(h.sh->threadOpen());
    CHECK(h.sh->current() == h.backend.clearTo);
}

TEST("message buttons: drawn under the message, a press goes to the backend") {
    Harness h;
    auto   &msgs = h.store.conversation(h.dm).messages;
    REQUIRE(!msgs.empty());
    const Ts ts = msgs.back().ts;
    h.store.updateMessage(h.dm, ts, [](Message &m) {
        m.extras().buttons.push_back({"option:1", "Yes", Button::Style::Primary});
        m.extras().buttons.push_back({"option:2", "No", Button::Style::Danger});
    });
    screens::MessageList ml(h.ctx);
    ml.showConversation(h.dm);
    ml.pressButton(ts, "option:2", {10, 10});
    pump();
    CHECK_STR(h.backend.pressed, "option:2");
}
#endif

namespace {

model::BackgroundJob
testJob(std::string id, std::string name, float progress, model::BackgroundJob::State state = {}) {
    model::BackgroundJob j;
    j.id        = std::move(id);
    j.name      = std::move(name);
    j.command   = "./scripts/" + j.name + ".sh";
    j.line      = "6200/10000 programs, 0 failures";
    j.ratio     = progress >= 0 ? "6200/10000" : "";
    j.progress  = progress;
    j.state     = state;
    j.startedMs = base::nowMicros() / 1000 - 850'000; // 14m 10s ago
    if (state != model::BackgroundJob::State::Running)
        j.endedMs = base::nowMicros() / 1000;
    if (state == model::BackgroundJob::State::Failed)
        j.exitCode = 1;
    return j;
}

} // namespace

TEST("header: an agent's background jobs show beside the name") {
    using Job  = model::BackgroundJob;
    using Form = shell::JobsButton::Form;
    Harness h;
    auto   *jobs = h.sh->header().jobs();
    REQUIRE(jobs != nullptr);
    CHECK_FALSE(jobs->visible()); // none yet

    // One job: its ring, name and how far along, with room to spare.
    h.store.setJobs(h.dm, {testJob("bq1", "fuzz-checkout", 0.62f)});
    pump();
    CHECK(jobs->visible());
    CHECK(jobs->form() == Form::Label);
    CHECK_STR(shell::JobsButton::shortValue(jobs->jobs()[0], base::nowMicros() / 1000), "62%");
    CHECK_STR(jobs->slotTooltip(0), "fuzz-checkout \xC2\xB7 62% (6200/10000) \xC2\xB7 14m 10s");
    CHECK(jobs->width() > 100);

    // Three: rings only; six: two rings and "+4", a failed one first.
    h.store.setJobs(
        h.dm,
        {testJob("a", "fuzz-checkout", 0.62f),
         testJob("b", "cmake --build", -1),
         testJob("c", "ctest", -1, Job::State::Failed)}
    );
    pump();
    CHECK(jobs->form() == Form::Rings);
    CHECK(jobs->slots().size() == 3);
    CHECK_STR(jobs->jobs()[0].id, "c");
    CHECK_STR(jobs->slotTooltip(0), "ctest \xC2\xB7 failed, exit code 1");
    CHECK_STR(jobs->summary(), "3 background jobs, 1 failed");
    std::vector<Job> six;
    for (int i = 0; i < 6; ++i)
        six.push_back(testJob("j" + std::to_string(i), "job" + std::to_string(i), 0.5f));
    h.store.setJobs(h.dm, six);
    pump();
    CHECK((jobs->slots() == std::vector<int>{0, 1, -1}));
    CHECK_STR(jobs->slotTooltip(2), "4 more background jobs");

    // No room: the name gives way first, then the jobs fold to a count.
    CHECK(jobs->fit(1000) == Form::Rings);
    CHECK(jobs->fit(40) == Form::Count);

    // A failed job stays until seen: the popover opened and closed.
    h.store.setJobs(
        h.dm,
        {testJob("ok", "fuzz-checkout", 0.3f), testJob("bad", "ctest", -1, Job::State::Failed)}
    );
    pump();
    jobs->openPopover();
    pump();
    REQUIRE(jobs->popover() != nullptr);
    CHECK(jobs->checked());
    h.win->closeAllPopups();
    pump();
    CHECK(jobs->popover() == nullptr);
    REQUIRE(h.store.jobs(h.dm).size() == 1);
    CHECK_STR(h.store.jobs(h.dm)[0].id, "ok");
    // …and set again by the backend, it stays out.
    h.store.setJobs(
        h.dm,
        {testJob("ok", "fuzz-checkout", 0.4f), testJob("bad", "ctest", -1, Job::State::Failed)}
    );
    CHECK(h.store.jobs(h.dm).size() == 1);

    // Gone: the element goes too.
    h.store.setJobs(h.dm, {});
    pump();
    CHECK_FALSE(jobs->visible());

    // A spinner turns every frame — also once the element was hidden and
    // shown again (hiding takes it off the window's tick list: it lagged,
    // redrawn only by the 1 s clock).
    h.store.setJobs(h.dm, {testJob("s1", "cd", -1)});
    pump();
    CHECK(jobs->spinning());
    h.store.setJobs(h.dm, {});
    pump();
    h.store.setJobs(h.dm, {testJob("s2", "make", -1)});
    pump();
    CHECK(jobs->visible());
    CHECK(jobs->spinning());
}

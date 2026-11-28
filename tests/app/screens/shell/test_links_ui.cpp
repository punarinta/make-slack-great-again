// Agent thread links in the shell: the robot in the hover toolbar of a
// Slack workspace's message (none in the Claude Code workspace, none without
// one), the session picker it opens, the link it makes — the branch asked
// with the message — and the robot active on the linked thread after it.
#include "app/claude/links.h"
#include "app/fake/fake_backend.h"
#include "app/model/jobs.h"
#include "app/model/null_backend.h"
#include "base/time.h"
#include "plat/testing.h"
#include "screens/shell/shell.h"
#include "support/test.h"
#include "ui/controls.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/common/icon_button.h"
#include "gfx/icons_generated.h"
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/message_list.h"

#include <memory>

using namespace model;

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

bool until(const std::function<bool()> &done, int ms = 3000) {
    const int64_t end = base::monotonicMs() + ms;
    while (!done()) {
        if (base::monotonicMs() > end)
            return false;
        app().pump(5);
    }
    return true;
}

// The Slack workspace: the fixture, its threads read from the Store, the
// agent's posts taken.
struct OwFake : fake::FakeBackend {
    OwFake(Store &st, plat::App &a) : fake::FakeBackend(st, a), _app(a) {}
    int  posts = 0;
    void postAgentReply(ConvRef, Ts, std::string, std::string, PostDone done) override {
        ++posts;
        const Ts ts = base::nowMicros();
        _app.post([done, ts] { done(ts, {}); });
    }
    void loadThreadReplies(ConvRef c, Ts root, Ts, RepliesDone done) override {
        std::vector<ThreadReply> out;
        if (const Message *m = _store.findMessage(c, root)) {
            out.emplace_back();
            out.back().message = m->clone();
        }
        auto shared = std::make_shared<std::vector<ThreadReply>>(std::move(out));
        _app.post([done, shared] { done(std::move(*shared), {}); });
    }
    plat::App &_app;
};

// The Claude Code workspace: one session to pick, the branch asked.
struct AgentsFake : NullBackend {
    using NullBackend::NullBackend;
    std::string asked, prompt;
    void        findAgentSessions(std::function<void(std::vector<FoundSession>)> done) override {
        FoundSession s;
        s.id           = "sid-1";
        s.title        = "Importer rewrite";
        s.folder       = "/tmp/importer";
        s.lastActiveMs = 1;
        done({s});
    }
    ConvRef addFoundSession(const std::string &id) override {
        return id == "sid-1" ? _store.findConversation("s1") : kNoConv;
    }
    void startAgentBranch(
        const std::string &session,
        std::string        p,
        std::vector<std::string>,
        std::vector<std::string>,
        AgentBranchDone,
        AgentTurnFn
    ) override {
        asked  = session;
        prompt = std::move(p);
    }
};

struct Harness {
    Store                          store, ccStore;
    OwFake                         backend{store, app().platform()};
    AgentsFake                     agents{ccStore};
    screens::ImageCache            images{app().platform()};
    screens::Context               ctx{app(), store, backend, images, {}, {}, {}, {}, {}};
    shell::Settings                settings;
    std::unique_ptr<ui::Window>    win;
    std::unique_ptr<shell::Shell>  sh;
    std::string                    dir = base::test::makeTempDir("links_ui_");
    std::unique_ptr<claude::Links> links;
    ConvRef                        design = kNoConv;

    Harness() {
        backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
        bool done = false;
        backend.connect([&](bool ok, const std::string &) { done = ok; });
        until([&] { return done; });
        Conversation s;
        s.id   = "s1";
        s.kind = ConvKind::Dm;
        ccStore.addConversation(std::move(s));
        plat::WindowDesc d;
        d.size        = {1200, 800};
        d.decorations = plat::Decorations::Custom;
        win           = std::make_unique<ui::Window>(d);
        sh            = std::make_unique<shell::Shell>(ctx, *win, settings, std::string());
        design        = store.findConversation("C0DESIGN");
        sh->open(design);
        until([&] { return !store.conversation(design).messages.empty(); });
        for (int i = 0; i < 10; ++i)
            app().pump(2);
    }
    ~Harness() {
        sh->setAgentLinks(nullptr);
        links.reset();
        sh.reset();
        win.reset();
        model::waitBackground();
        base::test::removeTree(dir);
    }
    void startLinks() {
        links = std::make_unique<claude::Links>(app().platform(), dir + "/links.json", dir);
        links->attach(store, backend);
        links->ready(store);
        links->setAgents(&ccStore, &agents);
        links->agentsReady();
        sh->setAgentLinks(links.get());
    }
    // Hovers the newest message: its toolbar's robot, or null.
    screens::IconButton *hoverNewest(Ts *ts) {
        ui::View   *robot = nullptr;
        const auto &msgs  = store.conversation(design).messages;
        *ts               = msgs.back().ts;
        // The newest row sits at the bottom of the message area.
        auto *h           = app().platform().testHooks();
        for (float y = 650; y > 200 && !robot; y -= 10) {
            h->injectPointerMove(win->native(), {700, y});
            for (int i = 0; i < 4; ++i)
                app().pump(2);
            robot = find(&win->root(), "Ask agent");
            if (!robot)
                robot = find(&win->root(), "Open agent thread");
            if (robot && !robot->visible())
                robot = nullptr;
        }
        return static_cast<screens::IconButton *>(robot);
    }
    static ui::View *find(ui::View *v, std::string_view tip, bool byName = false) {
        if ((byName ? v->accessibleName() : v->tooltip()) == tip)
            return v;
        for (size_t i = 0; i < v->childCount(); ++i)
            if (ui::View *f = find(v->child(i), tip, byName))
                return f;
        return nullptr;
    }
};

} // namespace

TEST("links ui: no Claude Code workspace, no robot") {
    Harness h;
    Ts      ts = 0;
    CHECK(h.hoverNewest(&ts) == nullptr);
    // The toolbar is there all the same.
    CHECK(Harness::find(&h.win->root(), "More actions") != nullptr);
}

TEST("links ui: the robot opens the session picker; a pick links the thread and asks") {
    Harness h;
    h.startLinks();
    Ts                   ts    = 0;
    screens::IconButton *robot = h.hoverNewest(&ts);
    REQUIRE(robot != nullptr);
    CHECK(robot->icon() == gfx::Icon::Bot);
    CHECK(robot->ink() != ui::C::Accent);
    robot->activate();
    for (int i = 0; i < 6; ++i)
        app().pump(2);
    // The picker: the session, no "Create a session"; Enter picks the first.
    ui::Popup *picker = h.win->topPopup();
    REQUIRE(picker != nullptr);
    CHECK(Harness::find(picker, "Importer rewrite", true) != nullptr);
    CHECK(Harness::find(picker, "Create a session", true) == nullptr);
    auto *t = app().platform().testHooks();
    t->injectKey(h.win->native(), plat::Key::Enter, true);
    t->injectKey(h.win->native(), plat::Key::Enter, false);
    REQUIRE(until([&] { return !h.agents.asked.empty(); }));
    CHECK(h.win->topPopup() == nullptr);
    CHECK_STR(h.agents.asked, "s1");
    REQUIRE(h.links->all().size() == 1);
    const model::Message *m    = h.store.findMessage(h.design, ts);
    const Ts              root = m && m->isReply() ? m->threadTs : ts;
    CHECK(h.links->find(h.store, h.design, root) != nullptr);
    CHECK(h.backend.posts == 1); // the status line
    CHECK(h.agents.prompt.find("<pasted_content>") != std::string::npos);
    // The robot shows the thread linked now.
    h.sh->setAgentLinks(h.links.get()); // a fresh look at the toolbar
    REQUIRE(until([&] { return robot->ink() == ui::C::Accent; }));
    CHECK_STR(robot->tooltip(), "Open agent thread");
}

TEST("links ui: none in the Claude Code workspace's own messages") {
    Harness h;
    h.startLinks();
    // The open Store as the agents' own: nothing offered.
    h.links->setAgents(&h.store, &h.agents);
    h.links->agentsReady();
    Ts ts = 0;
    CHECK(h.hoverNewest(&ts) == nullptr);
}

#endif // MSGA_HAVE_MESSAGES

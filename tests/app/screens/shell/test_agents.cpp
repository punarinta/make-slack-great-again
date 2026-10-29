// An agent workspace's sidebar and flows: the Sessions "+" menu, the Team section and its menus,
// presence (the yellow dot), Shift+Del, the teammate page and its composer, the session finder's
// rows, recent folders, SVG avatars.
#include "app/fake/fake_backend.h"
#include "base/file.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/testing.h"
#include "screens/shell/context_menus.h"
#include "screens/shell/recent_folders.h"
#include "screens/shell/session_dialogs.h"
#include "screens/shell/shell.h"
#include "screens/shell/teammate_page.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/image_cache.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <memory>

using namespace model;
using K      = plat::Key;
namespace rf = shell::recent_folders;

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

std::string home() {
    const char *h = std::getenv("HOME");
    return h ? h : "/tmp";
}

// The test workspace as a Claude Code one: its DMs are sessions, two of them
// working as a teammate (the teammate's picture on the session's user).
struct AgentFake : fake::FakeBackend {
    using fake::FakeBackend::FakeBackend;
    Capabilities capabilities() const override {
        Capabilities c  = fake::FakeBackend::capabilities();
        c.agentSessions = true;
        return c;
    }
    bool isAgentSession(ConvRef c) const override {
        return c < _store.conversationCount() && _store.conversation(c).kind == ConvKind::Dm;
    }
    bool                   canStopSession(ConvRef c) const override { return c == stoppable; }
    std::vector<AgentRole> agentRoles() const override { return team; }
    std::string            saveAgentRole(const AgentRole &r, std::string *) override {
        AgentRole a = r;
        if (a.id.empty())
            a.id = "added";
        team.push_back(a);
        return a.id;
    }
    std::string agentSessionBlocker(const std::string &dir) const override {
        return dir == "/nowhere" ? "Not a folder" : "";
    }
    void startAgentSession(
        const std::string                                &dir,
        bool                                              skip,
        const std::string                                &role,
        std::function<void(ConvRef, const std::string &)> done
    ) override {
        startedDir  = dir;
        startedRole = role;
        startedSkip = skip;
        app().platform().post([this, done] { done(newSession, ""); });
    }
    void send(ConvRef c, std::string text, Ts thread, Done d) override {
        sentTo   = c;
        sentText = text;
        fake::FakeBackend::send(c, std::move(text), thread, std::move(d));
    }
    void findAgentSessions(std::function<void(std::vector<FoundSession>)> done) override {
        done(found);
    }
    ConvRef addFoundSession(const std::string &id) override {
        return id == "gone" ? kNoConv : newSession;
    }
    std::string agentSessionFolder(ConvRef c) const override {
        return c == newSession ? home() : std::string();
    }
    // The fixture's sessions say their teammate by the peer's picture.
    std::string agentSessionRole(ConvRef c) const override {
        if (!isAgentSession(c) || _store.conversation(c).dmUser >= _store.userCount())
            return {};
        const std::string &pic = _store.user(_store.conversation(c).dmUser).avatar;
        for (const auto &r : team)
            if (!pic.empty() && r.avatar == pic)
                return r.id;
        return {};
    }

    mutable std::vector<AgentRole> team;
    std::vector<FoundSession>      found;
    ConvRef                        stoppable = kNoConv, newSession = kNoConv, sentTo = kNoConv;
    std::string                    startedDir, startedRole, sentText;
    bool                           startedSkip = false;
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

    Harness() {
        backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
        bool done = false;
        backend.connect([&](bool ok, const std::string &) { done = ok; });
        for (int i = 0; i < 200 && !done; ++i)
            app().pump(5);
        // The team: the Generalist (built in), a specialist edited, one added.
        Backend::AgentRole g;
        g.id = "generalist", g.name = "Generalist", g.builtIn = true, g.avatar = "/pics/g.svg";
        Backend::AgentRole e;
        e.id = "engineer", e.name = "Engineer", e.builtIn = true, e.edited = true;
        e.avatar = "/pics/e.svg";
        Backend::AgentRole c;
        c.id = "copy", c.name = "Copywriter", c.avatar = "/pics/c.svg";
        g.user                                                       = store.findUser("U0MIRA");
        e.user                                                       = store.findUser("U0JONAS");
        backend.team                                                 = {g, e, c};
        // Mira's DM works as the engineer.
        store.user(store.conversation(conv("D0MIRA")).dmUser).avatar = "/pics/e.svg";
        backend.newSession                                           = conv("D0JONAS");
        plat::WindowDesc d;
        d.size        = {1200, 800};
        d.decorations = plat::Decorations::Custom;
        win           = std::make_unique<ui::Window>(d);
        sh            = std::make_unique<shell::Shell>(ctx, *win, settings, std::string());
        pump();
    }
    ConvRef         conv(const char *id) const { return store.findConversation(id); }
    shell::Sidebar &sidebar() { return sh->sidebar(); }
    void            chord(bool shift, K k) {
        auto &hk = *app().platform().testHooks();
        if (shift)
            hk.injectKey(win->native(), K::ShiftLeft, true);
        hk.injectKey(win->native(), k, true);
        hk.injectKey(win->native(), k, false);
        if (shift)
            hk.injectKey(win->native(), K::ShiftLeft, false);
        pump();
    }
};

std::string labels(const std::vector<ui::MenuItem> &items) {
    std::string out;
    for (const auto &m : items)
        out += (out.empty() ? "" : "|") + (m.separator ? std::string("-") : m.label);
    return out;
}

} // namespace

// ── The sidebar ─────────────────────────────────────────────────────────────

TEST("agents: Sessions, then the Team section with every teammate") {
    Harness     h;
    const auto  titles = h.sidebar().sectionTitles();
    std::string joined;
    for (const auto &t : titles)
        joined += (joined.empty() ? "" : "|") + t;
    CHECK(joined.find("Sessions|Team") != std::string::npos);
    const auto mates = h.sidebar().teammates();
    REQUIRE(mates.size() == 3);
    CHECK_STR(mates[0], "generalist");
    CHECK_STR(mates[2], "copy");
}

TEST("agents: the yellow dot for an unavailable session and teammate, Shift+Del skips it") {
    Harness       h;
    const ConvRef dm   = h.conv("D0JONAS");
    const UserRef peer = h.store.conversation(dm).dmUser;
    User          u    = h.store.user(peer);
    u.active           = false;
    u.unavailable      = true;
    h.store.addUser(u);
    h.store.usersChanged();
    pump();
    using P = shell::Avatar::Presence;
    CHECK(h.sidebar().conversationPresence(dm) == int(P::Phantom));
    CHECK(h.sidebar().teammateState("engineer").presence == int(P::Phantom));
    h.sh->open(dm);
    pump();
    h.chord(true, K::Delete);
    CHECK(h.store.conversation(dm).member); // yellow: not idle
    u.unavailable = false;
    h.store.addUser(u);
    h.store.usersChanged();
    pump();
    CHECK(h.sidebar().conversationPresence(dm) == int(P::Away));
    h.chord(true, K::Delete);
    CHECK_FALSE(h.store.conversation(dm).member); // gray: removed
}

TEST("agents: a teammate is bright while one of its sessions is unread") {
    Harness h;
    // Mira's DM (unread in the fixture) works as the engineer.
    CHECK(h.store.conversation(h.conv("D0MIRA")).unread > 0);
    CHECK(h.sidebar().teammateState("engineer").bold);
    CHECK_FALSE(h.sidebar().teammateState("copy").bold);
}

// ── Menus ───────────────────────────────────────────────────────────────────

TEST("agents: the session menu, a session's and a teammate's, and their wording") {
    Harness h;
    CHECK_STR(
        labels(h.sh->menus().sessionItems()),
        "Find a session|Create a session|Create an unsafe session"
    );
    const ConvRef dm = h.conv("D0JONAS");
    CHECK_STR(
        labels(h.sh->menus().chatItems(dm)),
        "Star conversation|Move to section|-|Mute|Rename session…|-|Remove from msga"
    );
    h.backend.stoppable = dm;
    const auto items    = h.sh->menus().chatItems(dm);
    CHECK_STR(
        labels(items),
        "Star conversation|Move to section|-|Mute|Stop|Rename session…|-|Remove from msga"
    );
    CHECK(items.back().danger);
    const auto &t = h.backend.team;
    CHECK_STR(labels(h.sh->menus().teammateItems(t[0])), "Edit teammate…");
    CHECK_STR(labels(h.sh->menus().teammateItems(t[1])), "Edit teammate…|Restore default");
    const auto added = h.sh->menus().teammateItems(t[2]);
    CHECK_STR(labels(added), "Edit teammate…|-|Remove teammate…");
    CHECK(added.back().danger);
}

// ── The teammate page ───────────────────────────────────────────────────────

TEST("agents: a teammate's page lists its sessions; writing there starts one") {
    Harness h;
    h.settings.claudeTeammateDirs.emplace_back("engineer", home());
    h.sh->openTeammate("engineer");
    pump();
    REQUIRE(h.sh->teammateOpen());
    CHECK(h.sidebar().teammateState("engineer").selected);
    CHECK(h.sh->current() == kNoConv);
    shell::TeammatePage &page = *h.sh->teammatePage();
    REQUIRE(page.list().visibleCount() == 1);
    CHECK_STR(page.list().visibleItem(0).id, "D0MIRA");
    CHECK_STR(page.folder(), home());
    CHECK(page.folderLabel().text().find("Start new session in ~") == 0);
    // The folder menu: the current one ticked, then Browse….
    const auto menu = page.folderMenuItems(nullptr);
    REQUIRE(menu.size() >= 3);
    CHECK(menu.front().checked);
    CHECK_STR(menu.back().label, "Browse…");

    shell::Composer &c = h.sh->composer();
    CHECK(c.enabled());
    c.edit().insertText("fix the build");
    REQUIRE(c.send());
    pump(30);
    CHECK_STR(h.backend.startedRole, "engineer");
    CHECK_STR(h.backend.startedDir, home());
    CHECK_FALSE(h.backend.startedSkip);
    CHECK(h.backend.sentTo == h.conv("D0JONAS"));
    CHECK_STR(h.backend.sentText, "fix the build");
    CHECK_FALSE(h.sh->teammateOpen()); // the new session is open
    CHECK(h.sh->current() == h.conv("D0JONAS"));
    CHECK_FALSE(h.sidebar().teammateState("engineer").selected);
    CHECK(h.settings.claudeRecentDirs.size() == 1);
}

TEST("agents: a teammate's draft waits for the next visit; a blocked folder locks it") {
    Harness h;
    h.sh->openTeammate("copy");
    pump();
    h.sh->composer().edit().insertText("half a thought");
    h.sh->open(h.conv("D0JONAS"));
    pump();
    CHECK(h.sh->composer().edit().text().find("half") == std::string::npos);
    h.sh->openTeammate("copy");
    pump();
    CHECK(h.sh->composer().edit().text().find("half a thought") != std::string::npos);
    h.sh->teammatePage()->pickFolder("/nowhere");
    pump();
    CHECK_FALSE(h.sh->composer().enabled());
    CHECK_STR(h.sh->teammatePage()->blocker(), "Not a folder");
}

// The teammate page's list follows its sessions' changes (a status, a
// presence round) without jumping back to the top.
TEST("agents: the teammate page's list keeps its scroll through a change") {
    Harness          h;
    shell::Avatars   avatars(h.ctx.images);
    plat::WindowDesc d;
    d.size = {400, 200};
    ui::Window                           w(d);
    auto                                *list = w.root().add<shell::BrowseList>(avatars);
    std::vector<shell::BrowseList::Item> items(30);
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].id        = "S" + std::to_string(i);
        items[i].title     = "Session " + std::to_string(i);
        items[i].searchKey = "session " + std::to_string(i);
    }
    list->setItems(items);
    pump();
    list->scrollToAnchor({10, 0});
    pump();
    REQUIRE(list->anchor().index == 10);
    // Nothing changed: the rows stay as bound.
    const ui::View *row = list->viewFor(10);
    REQUIRE(row != nullptr);
    list->refreshItems(items);
    pump();
    CHECK(list->viewFor(10) == row);
    CHECK(list->anchor().index == 10);
    // A session's status: the new rows, where the list was.
    items[3].badge = "Working";
    list->refreshItems(items);
    pump();
    CHECK(list->anchor().index == 10);
    CHECK_STR(list->visibleItem(3).badge, "Working");
    // A new list (another teammate) starts at the top.
    list->setItems(items);
    pump();
    CHECK(list->anchor().index == 0);
}

TEST("agents: a teammate's folder that's gone starts in its parent, and leaves the menu") {
    Harness           h;
    const std::string proj = rf::normalized(file::join(home(), "proj"));
    const std::string gone = proj + "/robot-experiment/src";
    REQUIRE(file::makeDirs(proj));
    h.settings.claudeTeammateDirs.emplace_back("copy", gone);
    h.settings.claudeRecentDirs = {{gone, 9}, {proj, 5}};
    h.sh->openTeammate("copy");
    pump();
    CHECK_STR(h.sh->teammatePage()->folder(), proj);
    CHECK(h.sh->teammatePage()->blocker().empty());
    REQUIRE(h.settings.claudeRecentDirs.size() == 1);
    CHECK_STR(h.settings.claudeRecentDirs[0].path, proj);
    CHECK_STR(rf::teammateFolder(h.settings, "copy", home()), proj);
    std::vector<std::string> paths;
    h.sh->teammatePage()->folderMenuItems(&paths);
    CHECK(std::find(paths.begin(), paths.end(), gone) == paths.end());
}

TEST("agents: a message forwarded to a teammate waits in its page's composer, unsent") {
    Harness h;
    h.settings.claudeTeammateDirs.emplace_back("engineer", home());
    h.sh->openTeammate("engineer");
    pump();
    h.sh->composer().edit().insertText("look at this");
    h.sh->open(h.conv("D0JONAS")); // somewhere else meanwhile: the draft is kept
    pump();
    h.sh->prefillTeammate("engineer", "the *build* fails", {});
    pump(30);
    REQUIRE(h.sh->teammateOpen());
    CHECK_STR(h.sh->teammatePage()->teammate().id, "engineer");
    const std::string text = h.sh->composer().edit().text();
    CHECK(text.find("look at this") == 0);
    CHECK(text.find("the *build* fails") != std::string::npos);
    CHECK(h.backend.startedRole.empty()); // nothing started, nothing sent
    CHECK(h.backend.sentTo == kNoConv);
    // Sent from the page as typed there: the folder is the last one.
    REQUIRE(h.sh->composer().send());
    pump(30);
    CHECK_STR(h.backend.startedRole, "engineer");
    CHECK_STR(h.settings.claudeLastDir, home());
    CHECK(h.backend.sentText.find("the *build* fails") != std::string::npos);
}

// ── Session finder ──────────────────────────────────────────────────────────

TEST("agents: a found session's row: title or first prompt, ~ for home, In the list") {
    Backend::FoundSession s;
    s.id              = "abc";
    s.firstPrompt     = "make it fast";
    s.lastPrompt      = "and small";
    s.folder          = "/home/me/src/x";
    s.listed          = 3;
    const int64_t now = base::fromLocal(2026, 9, 21, 16, 0);
    s.lastActiveMs    = (now - 7200) * 1000;
    const auto it     = shell::foundSessionItem(s, "/home/me", now);
    CHECK_STR(it.title, "make it fast");
    CHECK_STR(it.subtitle, "~/src/x \xC2\xB7 2 hours ago \xC2\xB7 and small");
    CHECK_STR(it.badge, "In the list");
    s.title       = "Speed";
    s.listed      = kNoConv;
    const auto t2 = shell::foundSessionItem(s, "/home/me", now);
    CHECK_STR(t2.title, "Speed");
    CHECK(t2.badge.empty());
    CHECK(t2.searchKey.find("make it fast") != std::string::npos);
}

TEST("agents: picking a gone session says so; a found one opens") {
    Harness h;
    h.backend.found.resize(1);
    h.backend.found[0].id    = "gone";
    h.backend.found[0].title = "Old";
    h.sh->openSessionFinder();
    pump();
    // Enter in the search field opens the selected (first) row.
    h.chord(false, K::Enter);
    pump();
    CHECK(h.sh->current() == kNoConv); // "That session is gone…" (the banner)
    h.backend.found[0].id = "here";
    h.sh->openSessionFinder();
    pump();
    h.chord(false, K::Enter);
    pump(20);
    CHECK(h.sh->current() == h.conv("D0JONAS"));
}

// ── Recent folders, settings ────────────────────────────────────────────────

TEST("agents: recent folders are normalised, deduplicated, capped, ranked") {
    CHECK_STR(rf::normalized("/a/b/../c/./"), "/a/c");
    CHECK_STR(rf::normalized("C:\\x\\y"), "C:/x/y");
    std::vector<rf::Entry> l;
    for (int i = 0; i < 12; ++i)
        l = rf::bumped(std::move(l), "/d" + std::to_string(i), i);
    CHECK(l.size() == size_t(rf::kMax));
    CHECK_STR(l.front().path, "/d11");
    l = rf::bumped(std::move(l), "/d5/", 99);
    CHECK_STR(l.front().path, "/d5");
    CHECK(l.size() == size_t(rf::kMax));
    const auto r = rf::rank(
        {{"/a", 10}, {"/b", 30}, {"/gone", 50}},
        {{"/a/", 40}, {"/a", 5}, {"/c", 20}},
        [](const std::string &p) { return p != "/gone"; }
    );
    REQUIRE(r.size() == 3);
    CHECK_STR(r[0].path, "/a");
    CHECK(r[0].sessions == 2);
    CHECK(r[0].lastUsed == 40);
    CHECK_STR(r[1].path, "/b");
    CHECK_STR(r[2].path, "/c");
}

TEST("agents: a gone folder walks up to the nearest one there, stopping at home") {
    const auto there = [](const std::string &p) { return p == "/h/a" || p == "/srv"; };
    CHECK_STR(rf::existingFolder("/h/a/b/c/", "/h", there), "/h/a");
    CHECK_STR(rf::existingFolder("/h/a", "/h", there), "/h/a");
    CHECK_STR(rf::existingFolder("/h/x/y", "/h", there), "/h");
    CHECK_STR(rf::existingFolder("/srv/old", "/h", there), "/srv");
    CHECK_STR(rf::existingFolder("/mnt/usb", "/h", there), "/h"); // never the root
    CHECK_STR(rf::existingFolder("C:/gone", "/h", there), "/h");
    CHECK_STR(rf::existingFolder("", "/h", there), "/h");
    shell::Settings s;
    s.claudeLastDir      = "/h/x/y/";
    s.claudeRecentDirs   = {{"/h/a", 3}, {"/h/x/y", 2}};
    s.claudeTeammateDirs = {{"copy", "/h/x/y"}, {"engineer", "/h/a"}};
    rf::forgetFolder(s, "/h/x/y", "/h");
    REQUIRE(s.claudeRecentDirs.size() == 1);
    CHECK_STR(s.claudeRecentDirs[0].path, "/h/a");
    CHECK_STR(s.claudeLastDir, "/h");
    CHECK_STR(rf::teammateFolder(s, "copy", "/h"), "/h");
    CHECK_STR(rf::teammateFolder(s, "engineer", "/h"), "/h/a");
}

TEST("agents: the Claude Code folders survive a settings round trip") {
    shell::Settings s;
    s.claudeLastDir = "/w";
    s.claudeRecentDirs.push_back({"/w", 7});
    s.claudeTeammateDirs.emplace_back("copy", "/docs");
    const std::string path = file::join(home(), "agents-settings.json");
    REQUIRE(s.save(path));
    const shell::Settings t = shell::Settings::load(path);
    CHECK_STR(t.claudeLastDir, "/w");
    REQUIRE(t.claudeRecentDirs.size() == 1);
    CHECK(t.claudeRecentDirs[0].usedAt == 7);
    CHECK_STR(rf::teammateFolder(t, "copy", "/h"), "/docs");
    CHECK_STR(rf::teammateFolder(t, "other", "/h"), "/w");
}

// ── Avatars ─────────────────────────────────────────────────────────────────

TEST("agents: SVG avatars (the teammate tiles) decode at the size asked") {
    const std::string path = file::join(home(), "tile.svg");
    REQUIRE(
        file::writeAtomic(
            path,
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" width=\"128\" "
            "height=\"128\"><rect width=\"128\" height=\"128\" rx=\"28\" fill=\"#d97757\"/></svg>",
            0600
        )
    );
    screens::ImageCache images(app().platform());
    shell::Avatars      av(images);
    const auto          b = av.get(path, 40);
    REQUIRE(b != nullptr);
    // Rendered on the worker; the bitmap handed out fills in place.
    for (int i = 0; i < 400 && b->empty(); ++i)
        app().pump(5);
    CHECK(b->width() == 40);
    CHECK(b->height() == 40);
    // The centre is the tile's orange, opaque.
    CHECK((b->pixels()[20 * 40 + 20] >> 24) == 0xff);
}

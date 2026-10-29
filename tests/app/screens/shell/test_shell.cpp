// Shell behaviour on plat's headless backend: the sidebar follows the Store,
// the composer's mrkdwn round-trips through the parser, drafts survive every
// leave path, the quick switcher filters, the first load waits for the
// conversations, and URL avatars fill in when they land.
#include "app/fake/fake_backend.h"
#include "app/llm/service.h"
#include "support/fake_llm_server.h"
#include "app/llm/voice_input.h"
#include "app/model/backend_proxy.h"
#include "app/model/jobs.h"
#ifdef MSGA_SELF_UPDATE
#include "app/update/updater.h"
#endif
#include "net/net.h"
#include "app/mrkdwn/mrkdwn.h"
#include "app/mrkdwn/markdown.h"
#include "support/test.h"
#include "base/file.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "plat/audio.h"
#include "plat/testing.h"
#include "base/json.h"
#include "screens/common/file_dialogs.h"
#include "screens/common/remote_images.h"
#include "screens/shell/channel_dialogs.h"
#include "screens/shell/composer.h"
#include "screens/shell/context_menus.h"
#include "screens/shell/header.h"
#include "screens/shell/huddle_banner.h"
#include "screens/shell/sidebar_footer.h"
#include "screens/shell/profile_card.h"
#include "screens/shell/shell_dialogs.h"
#include "ui/controls.h"
#include "screens/shell/quick_switcher.h"
#include "screens/shell/shortcuts.h"
#include "screens/shell/shell.h"
#include "screens/settings/settings_dialog.h"
#include "screens/shell/threads_page.h"
#include "screens/shell/nav_chrome.h"
#include "screens/shell/voice_strip.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/image_cache.h"
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <unistd.h>

using namespace model;
using ui::TextEdit;

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

// A whole shell over the test fixture, loaded synchronously.
struct Harness {
    Store             store;
    fake::FakeBackend backend{store, app().platform()};
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache images{app().platform()};
    screens::Context    ctx{app(), store, backend, images, {}, {}, {}, {}, {}};
#else
    alignas(16) char noCache[16]{}; // nothing uses the cache without the messages screens
    screens::Context ctx{
        app(), store, backend, *reinterpret_cast<screens::ImageCache *>(noCache), {}, {}, {}, {}, {}
    };
#endif
    shell::Settings               settings;
    std::unique_ptr<ui::Window>   win;
    std::unique_ptr<shell::Shell> sh;

    // ai: the LLM layer the shell's composers see from the start (main's).
    explicit Harness(llm::Service *ai = nullptr) {
        ctx.ai = ai;
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
        sh->open(store.findConversation("C0DESIGN"));
        pump();
    }
    ConvRef conv(const char *id) const { return store.findConversation(id); }
};

std::vector<TextEdit::Run> run(uint32_t a, uint32_t b, uint8_t f, std::string_view link = {}) {
    return {TextEdit::Run{a, b, f, link}};
}

} // namespace

// ── mrkdwn serialiser ───────────────────────────────────────────────────────

TEST("composer: runs serialise to mrkdwn the parser reads back") {
    const std::string          text = "bold and italic code link";
    std::vector<TextEdit::Run> runs = {
        {0, 4, TextEdit::Bold, {}},
        {4, 9, 0, {}},
        {9, 15, TextEdit::Italic, {}},
        {15, 16, 0, {}},
        {16, 20, TextEdit::Code, {}},
        {20, 21, 0, {}},
        {21, 25, 0, "https://example.com/a?b=1&c=2"},
    };
    const std::string m = shell::toMrkdwn(text, runs);
    CHECK_STR(m, "*bold* and _italic_ `code` <https://example.com/a?b=1&c=2|link>");
    const auto r = mrkdwn::parse(m);
    CHECK_STR(r.text, text);
    REQUIRE(r.entities.size() == 4);
    CHECK(r.entities[0].kind == mrkdwn::Kind::Bold);
    CHECK(r.entities[1].kind == mrkdwn::Kind::Italic);
    CHECK(r.entities[2].kind == mrkdwn::Kind::Code);
    CHECK(r.entities[3].kind == mrkdwn::Kind::Link);
    CHECK_STR(r.entities[3].data, "https://example.com/a?b=1&c=2");
}

TEST("composer: marks hug words, nest across runs and stop at line breaks") {
    // Trailing/leading blanks move outside the marks.
    CHECK_STR(shell::toMrkdwn(" hi  there", run(0, 10, TextEdit::Bold)), " *hi  there*");
    // Bold over "a b", bold+italic over "b": no "*a **b*" collision. (Inside
    // one word "_" can't open — Slack's rule, so we don't test "ab".)
    std::vector<TextEdit::Run> nested = {
        {0, 2, TextEdit::Bold, {}}, {2, 3, TextEdit::Bold | TextEdit::Italic, {}}
    };
    const std::string m = shell::toMrkdwn("a b", nested);
    CHECK_STR(m, "*a _b_*");
    CHECK(mrkdwn::parse(m).entities.size() == 2);
    // A mark never spans a newline.
    CHECK_STR(shell::toMrkdwn("one\ntwo", run(0, 7, TextEdit::Strike)), "~one~\n~two~");
    // & < > go out bare (Slack escapes them itself), a typed "> " stays a
    // quote, and a URL that is its own label.
    CHECK_STR(shell::toMrkdwn("a < b & c > d", {}), "a < b & c > d");
    CHECK_STR(mrkdwn::compose(shell::toMrkdwn("> quoted\nplain", {})).mrkdwn, "> quoted\nplain");
    CHECK_STR(shell::toMrkdwn("a>b", run(0, 3, 0, "https://x.test")), "<https://x.test|a&gt;b>");
    CHECK_STR(
        shell::toMrkdwn("https://x.test", run(0, 14, 0, "https://x.test")), "<https://x.test>"
    );
    // Code keeps its inner spaces and takes no other marks inside it.
    CHECK_STR(shell::toMrkdwn("x y", run(0, 3, TextEdit::Code | TextEdit::Bold)), "*`x y`*");
    // Round trip through the parser for all of it.
    const auto r = mrkdwn::parse(shell::toMrkdwn("one\ntwo", run(0, 7, TextEdit::Strike)));
    CHECK_STR(r.text, "one\ntwo");
    CHECK(r.entities.size() == 2);
}

TEST("composer: a TextEdit's formatting survives the trip") {
    Harness h;
    auto   &edit = h.sh->composer().edit();
    edit.insertHtml("<b>Ship</b> it <i>today</i>, see <a href=\"https://x.test/p\">the plan</a>");
    const std::string m = h.sh->composer().mrkdwn();
    CHECK_STR(m, "*Ship* it _today_, see <https://x.test/p|the plan>");
    const auto r = mrkdwn::parse(m);
    CHECK_STR(r.text, "Ship it today, see the plan");
}

// ── Sidebar ─────────────────────────────────────────────────────────────────

TEST("sidebar: unread is bold with a badge, and markRead clears it") {
    Harness       h;
    const ConvRef eng = h.conv("C0ENG"), rel = h.conv("C0RELEASES"), mira = h.conv("D0MIRA");
    auto          s = h.sh->sidebar().rowState(eng);
    REQUIRE(s.exists);
    CHECK(s.bold);
    CHECK(s.badge == 1); // one mention
    s = h.sh->sidebar().rowState(rel);
    CHECK(s.bold);
    CHECK(s.dot); // unread, no mention: a dot
    CHECK(s.badge == 0);
    CHECK(h.sh->sidebar().rowState(mira).badge == 2); // DMs count every unread message
    CHECK(h.sh->sidebar().rowState(h.conv("C0DESIGN")).selected);
    CHECK_FALSE(h.sh->sidebar().rowState(h.conv("C0GENERAL")).bold);
    CHECK(h.sh->sidebar().attentionCount() == 3);

    h.backend.markRead(eng, h.store.conversation(eng).latest);
    pump();
    s = h.sh->sidebar().rowState(eng);
    CHECK_FALSE(s.bold);
    CHECK(s.badge == 0);
    CHECK(h.sh->sidebar().attentionCount() == 2);

    // Opening a conversation reads it (the message list does once its newest
    // message is on screen: give it frames).
    h.sh->open(mira);
    for (int i = 0; i < 100 && h.store.conversation(mira).unread; ++i)
        app().pump(5);
    CHECK(h.sh->sidebar().rowState(mira).badge == 0);
    CHECK(h.sh->sidebar().rowState(mira).selected);

    // A message from someone else lights it up again.
    h.backend.postAs(eng, h.store.findUser("U0LENA"), "hey <@U0ALEX>");
    pump();
    s = h.sh->sidebar().rowState(eng);
    CHECK(s.bold);
    CHECK(s.badge == 1);
}

TEST("sidebar: the open chat isn't read while the window is in the background") {
    Harness       h;
    const ConvRef mira = h.conv("D0MIRA");
    h.sh->open(mira);
    for (int i = 0; i < 100 && h.store.conversation(mira).unread; ++i)
        app().pump(5);
    REQUIRE(h.store.conversation(mira).unread == 0);
    auto focus = [&](bool in) {
        plat::Event e;
        e.type   = in ? plat::EventType::FocusIn : plat::EventType::FocusOut;
        e.window = &h.win->native();
        h.win->handle(e);
    };
    // Nothing is being read while the window is inactive: it builds up unreads.
    focus(false);
    h.backend.postAs(mira, h.store.conversation(mira).dmUser, "still there?");
    for (int i = 0; i < 60; ++i) // well past the list's edge check
        app().pump(5);
    CHECK(h.store.conversation(mira).unread == 1);
    CHECK(h.sh->sidebar().rowState(mira).badge == 1);
    // Back in front: read.
    focus(true);
    for (int i = 0; i < 100 && h.store.conversation(mira).unread; ++i)
        app().pump(5);
    CHECK(h.store.conversation(mira).unread == 0);
    CHECK(h.store.conversation(mira).lastRead == h.store.conversation(mira).messages.back().ts);
    // Focused: a new message is read as it lands.
    h.backend.postAs(mira, h.store.conversation(mira).dmUser, "ok");
    for (int i = 0; i < 100 && h.store.conversation(mira).unread; ++i)
        app().pump(5);
    CHECK(h.store.conversation(mira).unread == 0);
}

TEST("window: hidden to the tray or minimised, the backend polls as hidden; shown, as shown") {
    Harness h;
    REQUIRE(h.backend.windowVisible);
    // Minimised (the OS says so: StateChanged), then raised again.
    h.win->native().minimize();
    pump();
    CHECK_FALSE(h.backend.windowVisible);
    CHECK_FALSE(h.sh->windowVisible());
    h.sh->restore();
    pump();
    CHECK(h.backend.windowVisible);
    // To the tray (the window's close while Settings → Use tray is on), then back.
    h.settings.useTray = true;
    REQUIRE(h.sh->hideToTray());
    pump();
    CHECK_FALSE(h.backend.windowVisible);
    h.sh->restore();
    pump();
    CHECK(h.backend.windowVisible);
    // Another state change (maximised) is no news.
    h.backend.windowVisible = false;
    h.win->native().setMaximized(true);
    pump();
    CHECK_FALSE(h.backend.windowVisible); // not told again: nothing changed for it
}

TEST("window: Use tray off takes the icon away; close and minimize no longer hide") {
    Harness h;
    REQUIRE(h.sh->tray() != nullptr);
    // Turned off while hidden to the tray: the window comes back first.
    REQUIRE(h.sh->hideToTray());
    pump();
    h.settings.useTray        = false;
    h.settings.minimizeToTray = true;
    h.sh->applySettings();
    pump();
    CHECK(h.sh->tray() == nullptr);
    CHECK(h.backend.windowVisible);
    CHECK_FALSE(h.sh->hideToTray()); // the caller quits instead
    h.sh->minimize();                // to the taskbar, not hidden
    pump();
    CHECK(h.sh->tray() == nullptr);
    h.sh->restore();
    pump();
    // Back on: the icon returns.
    h.settings.useTray = true;
    h.sh->applySettings();
    pump();
    REQUIRE(h.sh->tray() != nullptr);
    CHECK(h.sh->hideToTray());
    h.sh->restore();
    pump();
}

TEST("sidebar: a presence flip restyles rows in place; a roster-shape change rebuilds") {
    Harness         h;
    auto           &sb   = h.sh->sidebar();
    const ConvRef   mira = h.conv("D0MIRA");
    const UserRef   peer = h.store.conversation(mira).dmUser;
    const ui::View *row  = sb.rowView(mira);
    REQUIRE(row != nullptr);
    const int rebuilds = sb.rebuildCount();
    const int dot      = sb.conversationPresence(mira);

    // A burst of presence flips: the dot follows, no row is recreated.
    for (int i = 0; i < 3; ++i) {
        h.store.user(peer).active = !h.store.user(peer).active;
        h.store.usersChanged();
    }
    pump();
    CHECK(sb.rebuildCount() == rebuilds);
    CHECK(sb.rowView(mira) == row);
    CHECK(sb.conversationPresence(mira) != dot);
    // DND, and someone off the list changing: in place too.
    h.store.user(peer).dnd                              = !h.store.user(peer).dnd;
    h.store.user(h.store.findUser("U0LENA")).statusText = "lunch";
    h.store.usersChanged();
    pump();
    CHECK(sb.rebuildCount() == rebuilds);
    CHECK(sb.rowView(mira) == row);

    // What a row is built with (its initial, a status emoji) rebuilds once.
    h.store.user(peer).displayName = "Mira (away)";
    h.store.user(peer).statusEmoji = "palm_tree";
    h.store.usersChanged();
    pump();
    CHECK(sb.rebuildCount() == rebuilds + 1);
    // A deactivated peer leaves the list.
    h.store.user(peer).deleted = true;
    h.store.usersChanged();
    pump();
    CHECK(sb.rebuildCount() == rebuilds + 2);
    CHECK_FALSE(sb.rowState(mira).exists);
}

TEST("sidebar: under unreads-only a Meta rebuilds only when a row's unread state flips") {
    Harness       h;
    auto         &sb  = h.sh->sidebar();
    const ConvRef eng = h.conv("C0ENG");
    const ConvRef rel = h.conv("C0RELEASES");
    auto          f   = shell::Sidebar::Filters{};
    f.unreadsOnly     = true;
    sb.setFilters(f);
    pump();
    REQUIRE(sb.rowState(eng).exists && sb.rowState(rel).exists);
    const int rebuilds = sb.rebuildCount();
    // A topic: no unread change, no rebuild.
    h.store.updateConversation(eng, [](Conversation &c) { c.topic = "new topic"; });
    pump();
    CHECK(sb.rebuildCount() == rebuilds);
    // Read: it drops out of the list (once, after the burst).
    h.store.markRead(eng, h.store.conversation(eng).latest);
    h.store.markRead(rel, h.store.conversation(rel).latest);
    pump();
    CHECK(sb.rebuildCount() == rebuilds + 1);
    CHECK_FALSE(sb.rowState(eng).exists);
    CHECK_FALSE(sb.rowState(rel).exists);
}

TEST("sidebar: starring moves a row") {
    Harness       h;
    const ConvRef eng   = h.conv("C0ENG");
    auto          order = h.sh->sidebar().order();
    // Starred (general, design) first, then channels, then DMs.
    REQUIRE(order.size() == h.store.conversationCount());
    CHECK(order[0] == h.conv("C0GENERAL"));
    CHECK(order[1] == h.conv("C0DESIGN"));
    CHECK(order[2] == eng);

    h.backend.setStarred(eng, true);
    pump();
    order = h.sh->sidebar().order();
    CHECK(order[2] == eng); // now the last starred row
    CHECK(h.store.conversation(eng).starred);
}

// ── Drafts ──────────────────────────────────────────────────────────────────

TEST("drafts: stashed per conversation and thread on every leave path") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN"), eng = h.conv("C0ENG");
    auto         &edit = h.sh->composer().edit();
    edit.insertText("half a thought");
    edit.selectAll();
    edit.toggleFormat(TextEdit::Bold);
    h.sh->open(eng); // leave: switch conversation
    CHECK(edit.empty());
    edit.insertText("eng draft");
    h.sh->open(design);
    CHECK_STR(edit.text(), "half a thought");
    CHECK_STR(h.sh->composer().mrkdwn(), "*half a thought*"); // formats survive the stash
    h.sh->open(eng);
    CHECK_STR(edit.text(), "eng draft");

    // Threads have their own drafts; closing the panel is a leave path too.
    const Ts root = h.backend.findTs(design, "Proposal B");
    REQUIRE(root != 0);
    h.sh->openThread(design, root);
    REQUIRE(h.sh->threadOpen());
    shell::Composer *tc = h.sh->threadComposer();
    CHECK(tc->edit().empty());
    tc->edit().insertText("thread reply draft");
    h.sh->closeThread();
    CHECK_FALSE(h.sh->threadOpen());
    CHECK_STR(edit.text(), "half a thought"); // the channel composer kept its own
    h.sh->openThread(design, root);
    CHECK_STR(tc->edit().text(), "thread reply draft");

    // Sending clears the draft for good.
    const size_t before = h.store.replies(design, root)->size();
    CHECK(tc->send());
    CHECK(tc->edit().empty());
    CHECK(h.store.replies(design, root)->size() == before + 1);
    h.sh->closeThread();
    h.sh->openThread(design, root);
    CHECK(tc->edit().empty());
    CHECK(h.sh->drafts().has({design, 0}));
    CHECK_FALSE(h.sh->drafts().has({design, root}));
}

// ── Quick switcher ──────────────────────────────────────────────────────────

TEST("quick switcher: a DM matches its peer's full name, display name and handle") {
    Harness      h;
    model::User &mira = h.store.user(h.store.findUser("U0MIRA"));
    mira.realName     = "Mira Okafor";
    mira.profileName  = "Mimi";
    h.store.setRealNames(false); // shown as "Mimi"
    const auto order = h.sh->sidebar().order();
    CHECK_STR(h.store.displayName(h.conv("D0MIRA")), "Mimi");
    for (const char *q : {"mimi", "okafor", "mira"}) {
        const auto r = shell::quickSwitchFilter(h.store, q, order);
        REQUIRE(!r.empty());
        CHECK(r[0] == h.conv("D0MIRA"));
    }
    h.store.setRealNames(true);
    const auto r = shell::quickSwitchFilter(h.store, "mimi", order);
    REQUIRE(!r.empty());
    CHECK(r[0] == h.conv("D0MIRA"));
}

TEST("quick switcher: filters by name, best match first") {
    Harness    h;
    const auto order = h.sh->sidebar().order();
    auto       r     = shell::quickSwitchFilter(h.store, "", order);
    CHECK(r == order); // all of them, no cap
    r = shell::quickSwitchFilter(h.store, "des", order);
    REQUIRE(r.size() == 1);
    CHECK(r[0] == h.conv("C0DESIGN"));
    r = shell::quickSwitchFilter(h.store, "MIRA", order);
    REQUIRE(r.size() >= 2); // the DM, the group DM (and scattered matches)
    CHECK(r[0] == h.conv("D0MIRA"));
    CHECK(std::find(r.begin(), r.end(), h.conv("G0TRIO")) != r.end());
    r = shell::quickSwitchFilter(h.store, "atlas", order); // a word inside "launch-atlas"
    REQUIRE(r.size() == 1);
    CHECK(r[0] == h.conv("G0LAUNCH"));
    r = shell::quickSwitchFilter(h.store, "e", order); // prefix matches rank first
    REQUIRE(!r.empty());
    CHECK(r[0] == h.conv("C0ENG"));
    CHECK(shell::quickSwitchFilter(h.store, "zzz", order).empty());

    // Through the popup: typing filters, Enter opens.
    h.sh->showQuickSwitcher();
    pump();
    shell::QuickSwitcher *qs = h.sh->quickSwitcher();
    REQUIRE(qs);
    qs->field().insertText("rand");
    REQUIRE(qs->results().size() == 1);
    qs->choose(0);
    pump();
    CHECK(h.sh->current() == h.conv("C0RANDOM"));
    CHECK(h.sh->quickSwitcher() == nullptr);
}

TEST("quick switcher: named conversations — members, live peers, most recent first") {
    model::Store store;
    User         me, bob, gone, raw;
    me.id = "U0ME00001", me.name = "me";
    bob.id = "U0BOB0001", bob.name = "bob";
    gone.id = "U0GONE001", gone.name = "gone", gone.deleted = true;
    raw.id = "U0RAW0001"; // never resolved: labelled with its id
    store.addUser(std::move(me));
    const UserRef b = store.addUser(std::move(bob));
    const UserRef g = store.addUser(std::move(gone));
    const UserRef r = store.addUser(std::move(raw));
    auto conv = [&](const char *id, const char *name, ConvKind k, model::Ts latest, UserRef peer) {
        Conversation c;
        c.id = id, c.name = name, c.kind = k, c.latest = latest, c.dmUser = peer;
        return store.addConversation(std::move(c));
    };
    const ConvRef zed   = conv("C1", "zed", ConvKind::Channel, 100'000'000, model::kNoUser);
    const ConvRef alpha = conv("C2", "alpha", ConvKind::Channel, 100'000'000, model::kNoUser);
    const ConvRef left  = conv("C3", "left", ConvKind::Channel, 900'000'000, model::kNoUser);
    const ConvRef dm    = conv("D1", "", ConvKind::Dm, 50'000'000, b);
    conv("D2", "", ConvKind::Dm, 900'000'000, g);
    conv("D3", "", ConvKind::Dm, 900'000'000, r);
    store.updateConversation(left, [](Conversation &c) { c.member = false; });
    // Visited here outranks both channels' latest message; a tie goes by name.
    auto order = shell::quickSwitchOrder(store, {{"D1", 500}});
    REQUIRE(order.size() == 3);
    CHECK(order[0] == dm);
    CHECK(order[1] == alpha);
    CHECK(order[2] == zed);
    store.updateConversation(zed, [](Conversation &c) { c.lastRead = 800'000'000; });
    order = shell::quickSwitchOrder(store, {});
    REQUIRE(order.size() == 3);
    CHECK(order[0] == zed);
}

TEST("quick switcher: a DM whose peer is still loading is listed, as in the sidebar") {
    model::Store store;
    Conversation c;
    c.id = "D9", c.kind = ConvKind::Dm, c.latest = 1'000'000;
    c.dmUser            = store.internUser("U0LATE001"); // a placeholder: not loaded yet
    const ConvRef dm    = store.addConversation(std::move(c));
    const auto    order = shell::quickSwitchOrder(store, {});
    REQUIRE(order.size() == 1);
    CHECK(order[0] == dm);
    CHECK_FALSE(shell::deadDm(store, store.conversation(dm)));
    // Once it resolves to nothing but its id, it is dead in both.
    User u;
    u.id = "U0LATE001";
    store.addUser(std::move(u));
    CHECK(shell::deadDm(store, store.conversation(dm)));
    CHECK(shell::quickSwitchOrder(store, {}).empty());
}

TEST("settings: round trip through the file") {
    shell::Settings s;
    s.width = 1300, s.height = 900, s.hasPosition = true, s.x = 40, s.y = 60, s.maximized = true;
    s.theme       = ui::ThemeMode::Dark;
    s.threadWidth = 500;
    s.setZenMode("claude-code:local", true);
    const std::string d = base::test::makeTempDir("msga_shell_");
    REQUIRE(!d.empty());
    const std::string path = d + "/msga/settings.json";
    REQUIRE(s.save(path));
    const shell::Settings l = shell::Settings::load(path);
    CHECK((l.width == 1300 && l.height == 900 && l.hasPosition && l.x == 40 && l.y == 60));
    CHECK(l.maximized);
    CHECK(l.theme == ui::ThemeMode::Dark);
    CHECK(l.threadWidth == 500);
    CHECK(l.zenMode("claude-code:local")); // per workspace
    CHECK_FALSE(l.zenMode("slack:T0OTHER"));
    // Earlier builds' one global toggle goes to the Claude Code workspace.
    REQUIRE(file::writeAtomic(d + "/old.json", R"({"zenMode":true})"));
    CHECK(shell::Settings::load(d + "/old.json").zenMode("claude-code:local"));
    const shell::Settings missing = shell::Settings::load(std::string(d) + "/nope.json");
    CHECK(missing.width == 1200);
}

// ── Context menus ───────────────────────────────────────────────────────────

namespace {

std::string labels(const std::vector<ui::MenuItem> &items) {
    std::string out;
    for (const ui::MenuItem &it : items) {
        if (!out.empty())
            out += '|';
        out += it.separator ? std::string("-") : it.label;
        if (!it.enabled)
            out += "(off)";
        if (it.checked)
            out += "(on)";
    }
    return out;
}

} // namespace

// ── Custom sections ─────────────────────────────────────────────────────────

namespace {
std::string titles(shell::Sidebar &sb) {
    std::string out;
    for (const std::string &t : sb.sectionTitles())
        out += (out.empty() ? "" : "|") + t;
    return out;
}
} // namespace

TEST("sections: a new section sits under Starred and takes its rows out of the others") {
    Harness         h;
    shell::Sidebar &sb  = h.sh->sidebar();
    const ConvRef   eng = h.conv("C0ENG"), mira = h.conv("D0MIRA");
    CHECK_STR(titles(sb), "Threads|Starred|Channels|Direct messages");
    h.backend.createSection("Projects", "rocket", eng);
    pump();
    CHECK_STR(titles(sb), "Threads|Starred|Projects|Channels|Direct messages");
    REQUIRE(h.store.sections().size() == 1);
    const std::string id = h.store.sections()[0].id;
    CHECK_STR(sb.rowSection(eng), id);
    // A DM joins it too, after the channel (the order they were put there).
    h.backend.moveToSection(mira, id);
    pump();
    auto order = sb.order();
    CHECK(order[2] == eng);
    CHECK(order[3] == mira);
    // A second section goes on top; moving one down swaps them back.
    h.backend.createSection("Later", "", kNoConv);
    pump();
    CHECK_STR(titles(sb), "Threads|Starred|Later|Projects|Channels|Direct messages");
    h.backend.moveSection(h.store.sections()[0].id, 1);
    pump();
    CHECK_STR(titles(sb), "Threads|Starred|Projects|Later|Channels|Direct messages");
    // Renamed in place; deleted, its rows go back where they came from.
    h.backend.updateSection(id, "Work", "");
    pump();
    CHECK_STR(titles(sb), "Threads|Starred|Work|Later|Channels|Direct messages");
    h.backend.deleteSection(id);
    pump();
    CHECK_STR(titles(sb), "Threads|Starred|Later|Channels|Direct messages");
    CHECK_STR(sb.rowSection(eng), "");
    CHECK(sb.rowState(eng).exists);
    CHECK(sb.rowState(mira).exists);
}

TEST("sections: a conversation is in one place: starring and moving trade it") {
    Harness         h;
    shell::Sidebar &sb      = h.sh->sidebar();
    const ConvRef   general = h.conv("C0GENERAL"); // starred in the fixture
    h.backend.createSection("Projects", "", general);
    pump();
    CHECK_FALSE(h.store.conversation(general).starred); // moved out of Starred
    CHECK_STR(sb.rowSection(general), h.store.sections()[0].id);
    h.backend.setStarred(general, true); // starring takes it out again
    pump();
    CHECK(h.store.conversation(general).starred);
    CHECK_STR(sb.rowSection(general), "");
    CHECK(h.store.sections()[0].convs.empty());
    // An empty section still shows its header.
    CHECK_STR(titles(sb), "Threads|Starred|Projects|Channels|Direct messages");
}

TEST("sections: a dropped row lands in the section; Channels takes it back") {
    Harness         h;
    shell::Sidebar &sb  = h.sh->sidebar();
    const ConvRef   eng = h.conv("C0ENG");
    h.backend.createSection("Projects", "", kNoConv);
    pump();
    CHECK_FALSE(sb.dropOnSection(eng, "Channels")); // already there
    CHECK_FALSE(sb.dropOnSection(eng, "Direct messages"));
    REQUIRE(sb.dropOnSection(eng, "Projects"));
    pump();
    CHECK_STR(sb.rowSection(eng), h.store.sections()[0].id);
    REQUIRE(sb.dropOnSection(eng, "Channels"));
    pump();
    CHECK_STR(sb.rowSection(eng), "");
    REQUIRE(sb.dropOnSection(eng, "Starred"));
    pump();
    CHECK(h.store.conversation(eng).starred);
}

TEST("sections: under unreads-only a section keeps its read rows") {
    Harness       h;
    const ConvRef random = h.conv("C0RANDOM"); // read
    h.backend.createSection("Projects", "", random);
    h.settings.unreadsOnly = true;
    h.sh->applySettings();
    pump();
    CHECK(h.sh->sidebar().rowState(random).exists);
}

TEST("sections: the conversation and section header menus") {
    Harness       h;
    shell::Menus &m   = h.sh->menus();
    const ConvRef eng = h.conv("C0ENG");
    CHECK(labels(m.chatItems(eng)).rfind("Star channel|Move to section|-|", 0) == 0);
    CHECK_STR(labels(m.chatItems(eng)[1].sub), "New section…");
    CHECK_STR(labels(m.sectionItems("")), "Create a section…");
    h.backend.createSection("Projects", "", eng);
    h.backend.createSection("Later", "", kNoConv);
    pump();
    const auto items = m.chatItems(eng);
    CHECK(labels(items).rfind("Star channel|Move to section|Remove from Projects|-|", 0) == 0);
    CHECK_STR(labels(items[1].sub), "Later|Projects(on)|-|New section…");
    const std::string later = h.store.sections()[0].id, projects = h.store.sections()[1].id;
    CHECK_STR(
        labels(m.sectionItems(later)),
        "Create a section…|Rename section…|-|Move section up(off)|Move section down|-|"
        "Delete section…"
    );
    CHECK(m.sectionItems(later).back().danger);
    // The submenu's items move it; "Remove from" takes it out.
    m.run(items[1].sub[0].id, eng);
    pump();
    CHECK_STR(h.sh->sidebar().rowSection(eng), later);
    m.run(shell::Menus::kRemoveFromSection, eng);
    pump();
    CHECK_STR(h.sh->sidebar().rowSection(eng), "");
    m.runSection(shell::Menus::kSectionDown, later);
    pump();
    CHECK_STR(h.store.sections()[0].id, projects);
}

// ── Files: the chooser helper, attachments, drops, Save as ──────────────────

namespace {

plat::TestHooks &hooks() {
    return *app().platform().testHooks();
}

// A throwaway folder with a few files to attach.
struct Files {
    std::string dir;
    Files() {
        dir = base::test::makeTempDir("msga_shell_files_");
        if (dir.empty())
            dir = "/tmp";
        file::writeAtomic(at("notes.txt"), "some notes");
        file::writeAtomic(at("chart.png"), std::string(64, 'x'));
        file::makeDirs(at("sub"));
    }
    ~Files() {
        for (const char *f : {"notes.txt", "chart.png", "copy.txt", "sub"})
            file::remove(at(f));
        file::remove(dir);
    }
    std::string at(const char *rel) const { return file::join(dir, rel); }
};

const Message *lastMessage(Store &st, ConvRef c) {
    const auto &ms = st.conversation(c).messages;
    return ms.empty() ? nullptr : &ms.back();
}

// A plat drop event over a window point, carrying file:// URIs.
plat::Event
dropEvent(ui::Window &w, plat::EventType t, ui::PointF at, std::vector<std::string> uris) {
    plat::Event e;
    e.type       = t;
    e.window     = &w.native();
    e.pos        = {at.x, at.y};
    e.dropAction = plat::DropAction::Copy;
    e.items      = {{"text/uri-list", {}}};
    e.uris       = std::move(uris);
    return e;
}

} // namespace

TEST("menus: channel, DM, group DM and workspace menus, item by item") {
    Harness       h;
    shell::Menus &m = h.sh->menus();
    // conv_list_widget.cpp showChannelContextMenu
    CHECK_STR(
        labels(m.chatItems(h.conv("C0ENG"))),
        "Star channel|Move to section|-|Notify you about…|All new posts(on)|Just mentions|"
        "Mute and hide|-|"
        "Leave channel"
    );
    const auto eng = m.chatItems(h.conv("C0ENG"));
    CHECK(eng[3].header);
    CHECK(eng[4].icon == uint16_t(gfx::Icon::Bell) && eng[6].icon == uint16_t(gfx::Icon::BellOff));
    CHECK(eng[0].icon == ui::Button::kNoIcon);
    CHECK(eng.back().danger);
    CHECK(
        labels(m.chatItems(h.conv("C0GENERAL"))).rfind("Unstar channel|Move to section|-|", 0) == 0
    );
    // showDmContextMenu (not an agent session)
    CHECK_STR(labels(m.chatItems(h.conv("D0MIRA"))), "Star conversation|Move to section|-|Mute");
    // showMpdmContextMenu
    CHECK_STR(
        labels(m.chatItems(h.conv("G0TRIO"))),
        "Name conversation…|-|Star conversation|Move to section|-|Notify you about…|"
        "All new posts(on)|"
        "Just mentions|Mute and hide|-|Leave conversation"
    );
    // main_window.cpp showWorkspaceMenu (me is not an admin: no "Workspace admin").
    // No sign-in yet: "Log out" shows, disabled.
    CHECK_STR(labels(m.workspaceItems()), "Change icon…|Mute|Log out from Lumen Studio(off)");
    CHECK(m.workspaceItems().back().danger);
    h.store.user(h.store.me).admin = true;
    CHECK(labels(m.workspaceItems()).rfind("Change icon…", 0) == 0); // no team URL, no item
    h.store.workspaceUrl = "https://lumen.slack.com/";
    CHECK(labels(m.workspaceItems()).rfind("Workspace admin|Change icon…", 0) == 0);
    // Muted: "Mute and hide" is the checked level; a muted DM offers Unmute.
    h.backend.setMuted(h.conv("C0ENG"), true);
    CHECK(m.chatItems(h.conv("C0ENG"))[6].checked);
    h.backend.setMuted(h.conv("D0MIRA"), true);
    CHECK_STR(labels(m.chatItems(h.conv("D0MIRA"))), "Star conversation|Move to section|-|Unmute");
}

TEST("menus: conversation actions go through the backend") {
    Harness       h;
    shell::Menus &m   = h.sh->menus();
    const ConvRef eng = h.conv("C0ENG");
    m.run(shell::Menus::kStar, eng);
    CHECK(h.store.conversation(eng).starred);
    m.run(shell::Menus::kUnstar, eng);
    CHECK_FALSE(h.store.conversation(eng).starred);
    m.run(shell::Menus::kNotifyMentions, eng);
    CHECK(h.store.conversation(eng).notify == NotifyLevel::Mentions);
    CHECK(m.chatItems(eng)[5].checked);
    m.run(shell::Menus::kNotifyMute, eng); // "Mute and hide" = muted
    CHECK(h.store.conversation(eng).muted);
    m.run(shell::Menus::kNotifyAll, eng); // any level unmutes
    CHECK_FALSE(h.store.conversation(eng).muted);
    CHECK(h.store.conversation(eng).notify == NotifyLevel::All);
    const ConvRef mira = h.conv("D0MIRA");
    m.run(shell::Menus::kMute, mira);
    CHECK(h.store.conversation(mira).muted);
    m.run(shell::Menus::kUnmute, mira);
    CHECK_FALSE(h.store.conversation(mira).muted);

    // Leaving the open conversation moves to its neighbour, and the row goes.
    const ConvRef design = h.conv("C0DESIGN");
    REQUIRE(h.sh->current() == design);
    const auto order = h.sh->sidebar().order();
    const auto it    = std::find(order.begin(), order.end(), design);
    REQUIRE(it + 1 != order.end());
    const ConvRef next = *(it + 1);
    m.run(shell::Menus::kLeave, design);
    pump();
    CHECK_FALSE(h.store.conversation(design).member);
    CHECK_FALSE(h.sh->sidebar().rowState(design).exists);
    CHECK(h.sh->current() == next);
    // A group DM's local name ("Name conversation…"): the dialog saves it.
    const ConvRef trio = h.conv("G0TRIO");
    m.run(shell::Menus::kRename, trio);
    pump();
    REQUIRE(h.win->topPopup() != nullptr);
    CHECK(h.win->topPopup()->place() == ui::Popup::Place::Fill); // an in-window dialog
    plat::Event te;
    te.type   = plat::EventType::TextInput;
    te.window = &h.win->native();
    te.text   = "Crit crew";
    h.win->handle(te);
    for (plat::Key k : {plat::Key::Enter}) {
        app().platform().testHooks()->injectKey(h.win->native(), k, true);
        app().platform().testHooks()->injectKey(h.win->native(), k, false);
    }
    pump();
    CHECK_STR(h.store.displayName(trio), "Crit crew");
    CHECK(labels(m.chatItems(trio)).rfind("Rename conversation…|", 0) == 0);
}

TEST("menus: workspace actions") {
    Harness       h;
    shell::Menus &m = h.sh->menus();
    std::string   opened;
    h.ctx.openUrl                  = [&](const std::string &u) { opened = u; };
    h.store.user(h.store.me).admin = true;
    h.store.workspaceUrl           = "https://lumen.slack.com/";
    m.run(shell::Menus::kWorkspaceAdmin, 0);
    CHECK_STR(opened, h.store.workspaceLink() + "admin/settings");
    m.run(shell::Menus::kMuteWorkspace, 0);
    CHECK(h.store.workspaceMuted);
    CHECK_STR(m.workspaceItems()[2].label, "Unmute");
    m.run(shell::Menus::kUnmuteWorkspace, 0);
    CHECK_FALSE(h.store.workspaceMuted);
}

TEST("menus: an untouched conversation follows the default level; All opts it back in") {
    Harness       h;
    shell::Menus &m   = h.sh->menus();
    const ConvRef rel = h.conv("C0RELEASES"); // unread, no mention
    CHECK(h.store.conversation(rel).notify == NotifyLevel::Default);
    CHECK(m.chatItems(rel)[4].checked); // "All new posts", Settings' default
    CHECK(h.sh->sidebar().rowState(rel).dot);
    h.settings.notifyLevel = 1; // Settings → "Just mentions"
    h.sh->applySettings();
    pump();
    CHECK(m.chatItems(rel)[5].checked); // the effective level is ticked
    CHECK_FALSE(h.sh->sidebar().rowState(rel).dot);
    m.run(shell::Menus::kNotifyAll, rel);
    pump();
    CHECK(h.store.conversation(rel).notify == NotifyLevel::All);
    CHECK(m.chatItems(rel)[4].checked);
    CHECK(h.sh->sidebar().rowState(rel).dot); // explicit "All new posts" wins
}

TEST("attention: the rail, tray and badge count unread DMs, mentions and chats") {
    Harness       h;
    const auto    now = h.backend.nowSecs();
    const ConvRef eng = h.conv("C0ENG"), rel = h.conv("C0RELEASES"), mira = h.conv("D0MIRA");
    auto          count = [&](NotifyLevel fallback) {
        return shell::workspaceAttention(h.store, fallback, now);
    };
    const shell::Attention a = count(NotifyLevel::All);
    CHECK(a.important == 3 && a.unread);
    // "Just mentions" by default: channels only count their mentions, no blue.
    h.backend.markRead(mira, h.store.conversation(mira).latest);
    h.backend.markRead(eng, h.store.conversation(eng).latest);
    CHECK(count(NotifyLevel::All).unread);
    CHECK_FALSE(count(NotifyLevel::Mentions).unread);
    // "Nothing" counts for nothing; neither does a channel I left.
    h.backend.setNotifyLevel(rel, NotifyLevel::Nothing);
    CHECK_FALSE(count(NotifyLevel::All).unread);
    h.backend.setNotifyLevel(rel, NotifyLevel::Default);
    h.store.updateConversation(rel, [](model::Conversation &c) { c.member = false; });
    CHECK_FALSE(count(NotifyLevel::All).unread);
    h.store.updateConversation(rel, [](model::Conversation &c) { c.member = true; });
    // Idle past the 30-day notification window.
    CHECK_FALSE(shell::workspaceAttention(h.store, NotifyLevel::All, now + 40 * 86400).unread);
}

TEST("menus: a sidebar row's menu works from the keyboard") {
    Harness h;
    h.sh->menus().showChat(h.conv("C0ENG"), {100, 100});
    pump();
    REQUIRE(h.win->topPopup() != nullptr);
    // Star channel, Move to section, then the three levels (the header is
    // skipped): Just mentions.
    for (plat::Key k :
         {plat::Key::Down, plat::Key::Down, plat::Key::Down, plat::Key::Down, plat::Key::Enter}) {
        app().platform().testHooks()->injectKey(h.win->native(), k, true);
        app().platform().testHooks()->injectKey(h.win->native(), k, false);
    }
    pump();
    CHECK(h.store.conversation(h.conv("C0ENG")).notify == NotifyLevel::Mentions);
    CHECK(h.win->topPopup() == nullptr);
}

TEST("profile card: a hover card — delay, grace period, Message") {
    Harness              h;
    shell::ProfileCards &pc   = h.sh->profiles();
    const UserRef        yuki = h.store.findUser("U0YUKI");
    pc.hover(yuki, {400, 400, 36, 36}, shell::ProfileCards::Hover);
    CHECK(pc.card() == nullptr); // 300 ms first
    for (int i = 0; i < 200 && !pc.card(); ++i)
        app().pump(5);
    REQUIRE(pc.card() != nullptr);
    CHECK(pc.card()->user() == yuki);
    CHECK_FALSE(pc.card()->modal()); // no focus grab
    // Beside the avatar: its left edge 16 px before the avatar's, above it.
    const ui::RectF r = pc.card()->windowRect();
    CHECK(r.x > 383 && r.x < 385);
    CHECK(r.y + r.h <= 400);
    // Leaving hides it after the grace period, unless the pointer comes back.
    pc.hover(yuki, {400, 400, 36, 36}, shell::ProfileCards::Leave);
    pc.cancelHide();
    pump();
    CHECK(pc.card() != nullptr);
    pc.scheduleHide();
    for (int i = 0; i < 200 && pc.card(); ++i)
        app().pump(5);
    CHECK(pc.card() == nullptr);
    // A click shows it at once; "Message" opens (here: creates) the DM.
    pc.hover(yuki, {400, 400, 36, 36}, shell::ProfileCards::Click);
    REQUIRE(pc.card() != nullptr);
    pump();
    const ui::RectF b  = pc.card()->messageButton();
    const ui::RectF cr = pc.card()->windowRect();
    auto           *th = app().platform().testHooks();
    th->injectPointerMove(h.win->native(), {cr.x + b.x + 10, cr.y + b.y + 10});
    th->injectButton(h.win->native(), plat::Button::Left, true);
    th->injectButton(h.win->native(), plat::Button::Left, false);
    for (int i = 0; i < 100 && h.store.conversation(h.sh->current()).dmUser != yuki; ++i)
        app().pump(5);
    CHECK(h.store.conversation(h.sh->current()).dmUser == yuki);
    pump();
    CHECK(pc.card() == nullptr);
}

TEST("composer: editing a message saves through the backend and gives the draft back") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    auto         &c      = h.sh->composer();
    bool          sent   = false;
    h.backend.send(
        design, "*bold* and <@U0MIRA> <https://x.test|link>", 0, [&](bool ok, const std::string &) {
            sent = ok;
        }
    );
    for (int i = 0; i < 200 && !sent; ++i)
        app().pump(5);
    const Ts mine = h.store.conversation(design).messages.back().ts;
    c.edit().insertText("half a thought");
    c.beginEdit(mine);
    CHECK(c.editing() == mine);
    // The message comes back with its marks, mention and link.
    CHECK_STR(c.mrkdwn(), "*bold* and <@U0MIRA> <https://x.test|link>");
    c.edit().insertText("!");
    CHECK(c.send());
    CHECK_STR(
        h.store.findMessage(design, mine)->text, "*bold* and <@U0MIRA> <https://x.test|link>!"
    );
    CHECK(h.store.findMessage(design, mine)->edited);
    CHECK(c.editing() == 0);
    CHECK_STR(c.edit().text(), "half a thought");
    // Escape cancels; someone else's message cannot be edited.
    c.beginEdit(mine);
    c.endEdit();
    CHECK_STR(c.edit().text(), "half a thought");
    c.beginEdit(h.store.conversation(design).messages.front().ts);
    CHECK(c.editing() == 0);
}

// ── Markdown formatting (Settings → Appearance) ─────────────────────────────

TEST("composer: in Markdown formatting *x* goes out as italic, **x** as bold") {
    Harness       h;
    const ConvRef design          = h.conv("C0DESIGN");
    auto         &c               = h.sh->composer();
    h.settings.markdownFormatting = true;
    h.sh->applySettings();
    const size_t before = h.store.conversation(design).messages.size();
    c.edit().insertText("*it* and **bold**, ***both*** `*code*`");
    CHECK(c.send());
    for (int i = 0; i < 200 && h.store.conversation(design).messages.size() == before; ++i)
        app().pump(5);
    REQUIRE(h.store.conversation(design).messages.size() == before + 1);
    CHECK_STR(
        h.store.conversation(design).messages.back().text, "_it_ and *bold*, *_both_* `*code*`"
    );
    // Slack's formatting again: *x* is bold, as before.
    h.settings.markdownFormatting = false;
    h.sh->applySettings();
    c.edit().insertText("*bold*");
    CHECK(c.send());
    for (int i = 0; i < 200 && h.store.conversation(design).messages.size() == before + 1; ++i)
        app().pump(5);
    CHECK_STR(h.store.conversation(design).messages.back().text, "*bold*");
}

TEST("composer: in Markdown formatting an edit shows **bold** and saves the same mrkdwn") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    auto         &c      = h.sh->composer();
    bool          sent   = false;
    h.backend.send(design, "*bold* _it_ <@U0MIRA>", 0, [&](bool ok, const std::string &) {
        sent = ok;
    });
    for (int i = 0; i < 200 && !sent; ++i)
        app().pump(5);
    const Ts mine                 = h.store.conversation(design).messages.back().ts;
    h.settings.markdownFormatting = true;
    h.sh->applySettings();
    c.beginEdit(mine);
    CHECK_STR(c.edit().text(), "**bold** _it_ @Mira Okafor");
    c.edit().insertText(" *new*");
    CHECK(c.send());
    pump();
    CHECK_STR(h.store.findMessage(design, mine)->text, "*bold* _it_ <@U0MIRA> _new_");
}

TEST("composer: the Bold and Italic buttons type the formatting's marks") {
    Harness h;
    auto   &c = h.sh->composer();
    c.edit().insertText("x");
    c.edit().selectAll();
    c.button(shell::shortcuts::Id::Bold)->onClick();
    CHECK_STR(c.edit().text(), "*x*");
    h.settings.markdownFormatting = true;
    h.sh->applySettings();
    c.edit().clear();
    c.edit().insertText("x");
    c.edit().selectAll();
    c.button(shell::shortcuts::Id::Bold)->onClick();
    CHECK_STR(c.edit().text(), "**x**");
    c.edit().clear();
    c.edit().insertText("y");
    c.edit().selectAll();
    c.button(shell::shortcuts::Id::Italic)->onClick();
    CHECK_STR(c.edit().text(), "*y*");
    CHECK_STR(mrkdwn::compose(c.mrkdwn()).mrkdwn, "_y_");
}

TEST("settings: switching the formatting rewrites what is typed and the drafts") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN"), eng = h.conv("C0ENG");
    auto         &c = h.sh->composer();
    c.edit().insertText("*bold* draft _it_");
    h.sh->open(eng); // stashed
    c.edit().insertText("*typed* 2 * 3");
    h.settings.markdownFormatting = true;
    h.sh->applySettings();
    CHECK_STR(c.edit().text(), "**typed** 2 * 3");
    CHECK_STR(mrkdwn::compose(c.mrkdwn()).mrkdwn, "*typed* 2 * 3");
    h.sh->open(design);
    CHECK_STR(c.edit().text(), "**bold** draft _it_");
    CHECK_STR(mrkdwn::compose(c.mrkdwn()).mrkdwn, "*bold* draft _it_");
    // A draft typed in Markdown's formatting, back to Slack's.
    c.edit().clear();
    c.edit().insertText("*it* **bold**");
    h.settings.markdownFormatting = false;
    h.sh->applySettings();
    CHECK_STR(c.edit().text(), "_it_ **bold**");
    CHECK_STR(mrkdwn::compose(c.mrkdwn()).mrkdwn, "_it_ *bold*");
    // **x** is bold in Slack's formatting too: that draft stays as it is.
    h.sh->open(eng);
    CHECK_STR(c.edit().text(), "**typed** 2 * 3");
    CHECK_STR(mrkdwn::compose(c.mrkdwn()).mrkdwn, "*typed* 2 * 3");
}

TEST("files: the attach button asks the OS dialog, chips show, send carries the files") {
    Harness h;
    Files   f;
    auto   &c = h.sh->composer();
    REQUIRE(hooks().fileDialogRespond({f.at("notes.txt"), f.at("chart.png")}));
    c.chooseAttachments();
    pump();
    plat::FileDialogDesc d;
    REQUIRE(hooks().lastFileDialog(&d));
    CHECK(d.mode == plat::FileDialogDesc::Mode::OpenMultiple);
    CHECK(d.parent == &h.win->native()); // parented to the main window
    const std::vector<std::string> want = {f.at("notes.txt"), f.at("chart.png")};
    CHECK(c.attachments() == want);
    CHECK(c.attachmentStrip()->visible());
    CHECK(c.attachmentStrip()->childCount() == 2);
    // Folders and duplicates are not attached.
    CHECK(c.addAttachments({f.at("sub"), f.at("notes.txt"), f.at("missing")}) == 0);
    // Remove one with its chip's "x" path, send the other with no text.
    c.removeAttachment(1);
    CHECK(c.attachments().size() == 1);
    const ConvRef conv = h.conv("C0DESIGN");
    CHECK(c.send());
    CHECK(c.attachments().empty());
    CHECK_FALSE(c.attachmentStrip()->visible());
    const Message *m = lastMessage(h.store, conv);
    REQUIRE(m);
    CHECK(m->user == h.store.me);
    REQUIRE(m->files().size() == 1);
    CHECK_STR(m->files()[0].name, "notes.txt");
    CHECK(m->files()[0].size == 10);
    CHECK_STR(m->files()[0].path, f.at("notes.txt"));
}

TEST("files: a cancelled dialog attaches nothing") {
    Harness h;
    REQUIRE(hooks().fileDialogRespond({}));
    h.sh->composer().chooseAttachments();
    pump();
    CHECK(h.sh->composer().attachments().empty());
}

#ifdef __linux__ // the in-app browser exists only on Linux
TEST("files: no OS dialog shows the in-app browser, which answers the same way") {
    Harness h;
    Files   f;
    REQUIRE(hooks().setFileDialogAvailable(false));
    std::optional<std::vector<std::string>> got;
    screens::pickFiles(h.ctx, [&](std::vector<std::string> p) { got = std::move(p); });
    pump();
    auto *b = static_cast<ui::FileBrowser *>(h.win->topPopup());
    REQUIRE(b);
    CHECK(b->setDir(f.dir));
    const int i = b->find("notes.txt");
    REQUIRE(i >= 0);
    b->select(i);
    CHECK(b->accept());
    pump();
    REQUIRE(got.has_value());
    CHECK(*got == std::vector<std::string>{f.at("notes.txt")});
    CHECK(h.win->topPopup() == nullptr);
    // Every mode falls back: a folder too.
    std::string folder = "unset";
    screens::pickFolder(h.ctx, [&](std::string p) { folder = std::move(p); }, f.dir);
    pump();
    b = static_cast<ui::FileBrowser *>(h.win->topPopup());
    REQUIRE(b);
    CHECK_STR(b->dir(), f.dir);
    b->cancel();
    pump();
    CHECK(folder.empty());
    hooks().setFileDialogAvailable(true);
}
#endif

TEST("files: save as suggests the name in Downloads and answers the path") {
    Harness           h;
    Files             f;
    const std::string dest = f.at("copy.txt");
    REQUIRE(hooks().fileDialogRespond({dest}));
    std::optional<std::string> got;
    screens::saveFile(h.ctx, "report.txt", [&](std::string p) { got = std::move(p); });
    pump();
    plat::FileDialogDesc d;
    REQUIRE(hooks().lastFileDialog(&d));
    CHECK(d.mode == plat::FileDialogDesc::Mode::Save);
    CHECK_STR(d.suggestedName, "report.txt");
    CHECK_STR(d.initialDir, app().platform().standardDir(plat::StandardDir::Downloads));
    REQUIRE(got.has_value());
    CHECK_STR(*got, dest);
    // Cancel: "".
    REQUIRE(hooks().fileDialogRespond({}));
    got.reset();
    screens::saveFile(h.ctx, "report.txt", [&](std::string p) { got = std::move(p); });
    pump();
    REQUIRE(got.has_value());
    CHECK(got->empty());
}

TEST("files: pick folder answers one folder") {
    Harness h;
    Files   f;
    REQUIRE(hooks().fileDialogRespond({f.at("sub")}));
    std::string got = "unset";
    screens::pickFolder(h.ctx, [&](std::string p) { got = std::move(p); });
    pump();
    plat::FileDialogDesc d;
    REQUIRE(hooks().lastFileDialog(&d));
    CHECK(d.mode == plat::FileDialogDesc::Mode::PickFolder);
    CHECK_STR(got, f.at("sub"));
}

TEST("files: attachments are part of the draft") {
    Harness h;
    Files   f;
    auto   &c = h.sh->composer();
    c.addAttachments({f.at("notes.txt")});
    h.sh->open(h.conv("C0RANDOM"));
    pump();
    CHECK(c.attachments().empty());
    h.sh->open(h.conv("C0DESIGN"));
    pump();
    CHECK(c.attachments() == std::vector<std::string>{f.at("notes.txt")});
}

TEST("files: dropping files on the composer or the message list attaches them") {
    Harness           h;
    Files             f;
    auto             &c = h.sh->composer();
    ui::RectF         r = c.windowRect();
    ui::PointF        p{r.x + r.w / 2, r.y + r.h / 2};
    const std::string uri = base::test::fileUri(f.at("notes.txt"));
    // DropEnter is accepted (Copy), the Drop adds the file.
    h.win->handle(dropEvent(*h.win, plat::EventType::DropEnter, p, {}));
    h.win->handle(dropEvent(*h.win, plat::EventType::Drop, p, {uri}));
    pump();
    CHECK(c.attachments() == std::vector<std::string>{f.at("notes.txt")});
#ifdef MSGA_HAVE_MESSAGES
    // Over the message list, through Context::attachFiles; the name is
    // percent-encoded in the URI.
    file::writeAtomic(f.at("a b.txt"), "x");
    ui::PointF above{r.x + r.w / 2, r.y - 120};
    h.win->handle(dropEvent(*h.win, plat::EventType::DropEnter, above, {}));
    h.win->handle(
        dropEvent(*h.win, plat::EventType::Drop, above, {base::test::fileUri(f.dir + "/a%20b.txt")})
    );
    pump();
    CHECK(c.attachments().size() == 2);
    CHECK(c.attachments().size() == 2 && c.attachments()[1] == f.at("a b.txt"));
    file::remove(f.at("a b.txt"));
#endif
    // Non-file URIs are ignored.
    h.win->handle(dropEvent(*h.win, plat::EventType::Drop, p, {"https://example.com/x"}));
    pump();
    CHECK(c.attachments().size() >= 1);
}

TEST("files: dropped URIs decode to local paths") {
    plat::Event e;
    e.uris = {
        "file:///tmp/a%20b.txt",
        "file://localhost/tmp/c.txt",
        "https://x.test/y",
        "file:///%E2%9C%93"
    };
    const auto                     p    = screens::droppedFiles(&e);
    const std::vector<std::string> want = {"/tmp/a b.txt", "/tmp/c.txt", "/\xE2\x9C\x93"};
    CHECK(p == want);
    CHECK(screens::dragOffersFiles(&e));
    plat::Event text;
    text.items = {{"text/plain;charset=utf-8", {}}};
    CHECK_FALSE(screens::dragOffersFiles(&text));
}

TEST("drafts: files are stashed with the text") {
    Store                        store;
    shell::DraftStash            d;
    const shell::DraftStash::Key k{3, 0};
    d.setStoreOf([&store](const std::string &) { return &store; });
    d.stash(k, "", {"/tmp/a"});
    CHECK(d.has(k));
    CHECK(d.files(k) == std::vector<std::string>{"/tmp/a"});
    d.stash(k, "", {});
    CHECK_FALSE(d.has(k));
}

TEST("drafts: a message put back goes in front, its files join") {
    Store                        ws, other;
    shell::DraftStash            d;
    const shell::DraftStash::Key k{3, 0};
    d.setScope("ws");
    d.setStoreOf([&](const std::string &scope) { return scope == "ws" ? &ws : &other; });
    d.putBack("ws", k, "lost", {"/tmp/a"});
    CHECK_STR(d.get(k), "lost");
    d.putBack("ws", k, "first", {"/tmp/a", "/tmp/b"});
    CHECK(d.get(k).find("first") < d.get(k).find("lost"));
    CHECK((d.files(k) == std::vector<std::string>{"/tmp/a", "/tmp/b"}));
    // Another workspace's draft, not this one's.
    d.putBack("other", k, "x", {});
    CHECK(d.get(k).find('x') == std::string::npos);
}

// ── Dialogs and the undo toast ──────────────────────────────────────────────

namespace {

void typeInto(ui::Window &w, const char *text) {
    plat::Event te;
    te.type   = plat::EventType::TextInput;
    te.window = &w.native();
    te.text   = text;
    w.handle(te);
    pump(2);
}

void press(ui::Window &w, plat::Key k) {
    app().platform().testHooks()->injectKey(w.native(), k, true);
    app().platform().testHooks()->injectKey(w.native(), k, false);
    pump(2);
}

} // namespace

TEST("forward: pick a channel, add a comment, the message is re-posted there") {
    Harness           h;
    const ConvRef     design = h.conv("C0DESIGN"), general = h.conv("C0GENERAL");
    const Ts          ts   = h.store.conversation(design).messages.back().ts;
    const std::string text = h.store.conversation(design).messages.back().text;
    REQUIRE(h.ctx.forwardMessage);
    h.ctx.forwardMessage(design, ts, {});
    pump();
    auto *dlg = static_cast<ui::Dialog *>(h.win->topPopup());
    REQUIRE(dlg != nullptr);
    typeInto(*h.win, "#gen");        // "#": channels only
    press(*h.win, plat::Key::Enter); // the first match
    const size_t                          before = h.store.conversation(general).messages.size();
    // Enter in the dialog's composer forwards.
    std::function<ui::View *(ui::View *)> byName = [&](ui::View *v) -> ui::View * {
        if (v->accessibleName() == "Add a message, if you'd like.")
            return v;
        for (size_t i = 0; i < v->childCount(); ++i)
            if (ui::View *f = byName(v->child(i)))
                return f;
        return nullptr;
    };
    ui::View *comment = byName(dlg);
    REQUIRE(comment != nullptr);
    comment->focus();
    typeInto(*h.win, "fyi");
    press(*h.win, plat::Key::Enter);
    pump();
    REQUIRE(h.store.conversation(general).messages.size() == before + 1);
    CHECK_STR(h.store.conversation(general).messages.back().text, "fyi\n" + text);
    CHECK(h.store.conversation(general).messages.back().user == h.store.me);

    // The comment is converted like any message: in Markdown's formatting
    // **x** is bold and *x* italic.
    h.settings.markdownFormatting = true;
    h.sh->applySettings();
    h.ctx.forwardMessage(design, ts, {});
    pump();
    dlg = static_cast<ui::Dialog *>(h.win->topPopup());
    REQUIRE(dlg != nullptr);
    typeInto(*h.win, "#gen");
    press(*h.win, plat::Key::Enter);
    comment = byName(dlg);
    REQUIRE(comment != nullptr);
    comment->focus();
    typeInto(*h.win, "**fyi** *now*");
    press(*h.win, plat::Key::Enter);
    pump();
    REQUIRE(h.store.conversation(general).messages.size() == before + 2);
    CHECK_STR(h.store.conversation(general).messages.back().text, "*fyi* _now_\n" + text);
}

TEST("forward: the picker matches people case-insensitively beyond ASCII") {
    Harness     h;
    model::User u;
    u.id          = "U0ORJAN";
    u.name        = "orjan";
    u.displayName = "\xC3\x96rjan Berg"; // "Örjan Berg": no DM with us yet
    h.store.addUser(std::move(u));
    h.store.usersChanged();
    const ConvRef design = h.conv("C0DESIGN");
    h.ctx.forwardMessage(design, h.store.conversation(design).messages.back().ts, {});
    pump();
    auto *dlg = static_cast<ui::Dialog *>(h.win->topPopup());
    REQUIRE(dlg != nullptr);
    std::function<ui::View *(ui::View *, std::string_view)> byName =
        [&](ui::View *v, std::string_view n) -> ui::View * {
        if (v->accessibleName() == n)
            return v;
        for (size_t i = 0; i < v->childCount(); ++i)
            if (ui::View *f = byName(v->child(i), n))
                return f;
        return nullptr;
    };
    auto *fwd = byName(dlg, "Forward");
    REQUIRE(fwd != nullptr);
    CHECK_FALSE(fwd->enabled());
    typeInto(*h.win, "@\xC3\xB6rj"); // "@örj"
    press(*h.win, plat::Key::Enter); // the first match: picked
    CHECK(fwd->enabled());
    dlg->reject();
    pump();
}

TEST("forward: Escape with the picker's list open closes both") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    h.ctx.forwardMessage(design, h.store.conversation(design).messages.back().ts, {});
    pump();
    ui::Popup *dlg = h.win->topPopup();
    REQUIRE(dlg != nullptr);
    typeInto(*h.win, "@m"); // the list opens over the dialog
    REQUIRE(h.win->topPopup() != dlg);
    press(*h.win, plat::Key::Escape);
    pump();
    CHECK(h.win->topPopup() == nullptr);
}

TEST("forward: Copy link copies the message's permalink, link or not in its text") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    const Ts      ts     = h.store.conversation(design).messages.back().ts;
    h.ctx.forwardMessage(design, ts, {});
    pump();
    auto *dlg = static_cast<ui::Dialog *>(h.win->topPopup());
    REQUIRE(dlg != nullptr);
    std::function<ui::View *(ui::View *)> byName = [&](ui::View *v) -> ui::View * {
        if (v->accessibleName() == "Copy link")
            return v;
        for (size_t i = 0; i < v->childCount(); ++i)
            if (ui::View *f = byName(v->child(i)))
                return f;
        return nullptr;
    };
    auto *copy = static_cast<ui::Clickable *>(byName(dlg));
    REQUIRE(copy != nullptr);
    copy->activate();
    std::string got;
    bool        done = false;
    app().platform().requestClipboard(
        "text/plain;charset=utf-8", [&](std::optional<std::string> s) {
            got  = s.value_or("");
            done = true;
        }
    );
    for (int i = 0; i < 100 && !done; ++i)
        pump();
    REQUIRE(done);
    CHECK_STR(got, h.store.permalink(design, ts));
    dlg->reject();
    pump();
}

namespace {

// The fixture's backend with a service that hosts files: downloads wait for
// the test to answer them; what is uploaded is recorded (with the bytes the
// file had then).
class HostingFake final : public fake::FakeBackend {
public:
    using FakeBackend::FakeBackend;
    struct Download {
        std::string url, toPath;
        Done        done;
    };
    std::vector<Download>    downloads;
    std::vector<std::string> sentPaths, sentBytes;
    int                      uploads    = 0;
    bool                     fileUpload = true;

    void downloadFile(const std::string &url, std::string toPath, Done done) override {
        downloads.push_back({url, std::move(toPath), std::move(done)});
    }
    void sendWithFiles(
        ConvRef conv, std::string text, Ts threadTs, std::vector<std::string> files, Done done
    ) override {
        if (!files.empty())
            ++uploads;
        for (const std::string &p : files) {
            std::string b;
            file::readAll(p, &b);
            sentPaths.push_back(p);
            sentBytes.push_back(std::move(b));
        }
        FakeBackend::sendWithFiles(
            conv, std::move(text), threadTs, std::move(files), std::move(done)
        );
    }
    Capabilities capabilities() const override {
        Capabilities c = FakeBackend::capabilities();
        c.fileUpload   = fileUpload;
        return c;
    }
};

struct HostingHarness {
    Store       store;
    HostingFake backend{store, app().platform()};
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
    ConvRef                       design = kNoConv, general = kNoConv;
    Ts                            ts = 0; // the message with the hosted file, in #design

    HostingHarness() {
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
        design        = store.findConversation("C0DESIGN");
        general       = store.findConversation("C0GENERAL");
        Message m;
        m.ts = ts = store.conversation(design).messages.back().ts + 1000;
        m.user    = store.me;
        m.text    = "see this";
        File f;
        f.id       = "F0HOSTED";
        f.name     = "photo.png";
        f.mime     = "image/png";
        f.path     = "https://files.example/F0HOSTED/photo_360.png"; // a thumbnail
        f.original = "https://files.example/F0HOSTED/photo.png";
        m.extras().files.push_back(std::move(f));
        store.addMessage(design, std::move(m));
        sh->open(design);
        pump();
    }
    // Forward dialog → #general, then Forward (Enter in its composer).
    void forward() {
        ctx.forwardMessage(design, ts, {});
        pump();
        auto *dlg = static_cast<ui::Dialog *>(win->topPopup());
        REQUIRE(dlg != nullptr);
        typeInto(*win, "#gen");
        press(*win, plat::Key::Enter);
        std::function<ui::View *(ui::View *)> byName = [&](ui::View *v) -> ui::View * {
            if (v->accessibleName() == "Add a message, if you'd like.")
                return v;
            for (size_t i = 0; i < v->childCount(); ++i)
                if (ui::View *f = byName(v->child(i)))
                    return f;
            return nullptr;
        };
        ui::View *comment = byName(dlg);
        REQUIRE(comment != nullptr);
        comment->focus();
        press(*win, plat::Key::Enter);
        pump();
    }
    bool waitFor(const std::function<bool()> &pred) {
        for (int i = 0; i < 500 && !pred(); ++i)
            app().pump(5);
        return pred();
    }
};

bool hasJob(std::string_view what) {
    for (const std::string &d : model::jobs().descriptions())
        if (d == what)
            return true;
    return false;
}

} // namespace

TEST("forward: a hosted file is downloaded, uploaded with the message, then its copy goes") {
    HostingHarness h;
    const size_t   before = h.store.conversation(h.general).messages.size();
    h.forward();
    // The original, not the thumbnail, fetched through the backend — a job
    // the footer lists while it runs.
    REQUIRE(h.backend.downloads.size() == 1);
    CHECK_STR(h.backend.downloads[0].url, "https://files.example/F0HOSTED/photo.png");
    const std::string tmp = h.backend.downloads[0].toPath;
    CHECK_STR(std::string(file::baseName(tmp)), "photo.png");
    CHECK(hasJob("Forwarding photo.png"));
    CHECK(h.store.conversation(h.general).messages.size() == before); // nothing yet
    REQUIRE(file::writeAtomic(tmp, "the original bytes"));
    h.backend.downloads[0].done(true, {});
    CHECK_FALSE(hasJob("Forwarding photo.png"));
    REQUIRE(h.backend.uploads == 1);
    REQUIRE(h.backend.sentPaths.size() == 1);
    CHECK_STR(h.backend.sentPaths[0], tmp);
    CHECK_STR(h.backend.sentBytes[0], "the original bytes");
    REQUIRE(h.store.conversation(h.general).messages.size() == before + 1);
    CHECK_STR(h.store.conversation(h.general).messages.back().text, "see this");
    // The downloaded copy is removed once the upload is over.
    REQUIRE(h.waitFor([&] { return !file::exists(tmp); }));
    CHECK_FALSE(file::exists(std::string(file::dirName(tmp))));
}

TEST("forward: a download that fails says so in the banner, and nothing is sent") {
    HostingHarness h;
    const size_t   before = h.store.conversation(h.general).messages.size();
    h.forward();
    REQUIRE(h.backend.downloads.size() == 1);
    h.backend.downloads[0].done(false, "http 404");
    pump();
    CHECK_FALSE(hasJob("Forwarding photo.png"));
    CHECK(h.backend.uploads == 0);
    CHECK(h.store.conversation(h.general).messages.size() == before);
    CHECK(h.sh->errorBanner()->visible());
    CHECK_STR(h.sh->errorBanner()->text(), "Couldn't forward the file.");
}

TEST("forward: a target that can't take uploads gets the file's link") {
    HostingHarness h;
    h.backend.fileUpload = false;
    const size_t before  = h.store.conversation(h.general).messages.size();
    h.forward();
    CHECK(h.backend.downloads.empty());
    CHECK(h.backend.uploads == 0);
    REQUIRE(h.waitFor([&] {
        return h.store.conversation(h.general).messages.size() == before + 1;
    }));
    CHECK_STR(
        h.store.conversation(h.general).messages.back().text,
        "see this\nhttps://files.example/F0HOSTED/photo.png"
    );
}

namespace {

ui::View *findNamed(ui::View *v, std::string_view name) {
    if (v->visible() && v->accessibleName() == name)
        return v;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (ui::View *f = findNamed(v->child(i), name))
            return f;
    return nullptr;
}

} // namespace

TEST("find a channel: the rows — by name, Joined, members · topic; no strangers") {
    Store        store;
    Conversation a, b, d;
    a.id = "C1", a.name = "Zeta", a.memberCount = 1, a.topic = "last";
    b.id = "G1", b.name = "alpha", b.kind = ConvKind::Private, b.member = false;
    b.memberCount = 3;
    d.id = "D1", d.kind = ConvKind::Dm;
    store.addConversation(std::move(a));
    store.addConversation(std::move(b));
    store.addConversation(std::move(d));
    const auto ch = shell::channelItems(store);
    REQUIRE(ch.size() == 2);
    CHECK_STR(ch[0].title, "alpha");
    CHECK(ch[0].titleIcon == uint16_t(gfx::Icon::Lock));
    CHECK(ch[0].badge.empty());
    CHECK_STR(ch[0].subtitle, "3 members");
    CHECK_STR(ch[1].subtitle, "1 member \xC2\xB7 last");
    CHECK_STR(ch[1].badge, "Joined");
    User u1, u2, u3;
    u1.id = "U1", u1.name = "bob", u1.displayName = "Bob B";
    u2.id = "U2", u2.name = "gone", u2.deleted = true;
    u3.id = "U3", u3.name = "ext", u3.stranger = true;
    store.addUser(std::move(u1));
    store.addUser(std::move(u2));
    store.addUser(std::move(u3));
    store.internUser("U4"); // a placeholder: nobody to message yet
    const auto pe = shell::peopleItems(store);
    REQUIRE(pe.size() == 1);
    CHECK_STR(pe[0].title, "Bob B");
    CHECK_STR(pe[0].subtitle, "@bob");
    CHECK_STR(shell::channelName("  Plan Budget "), "plan-budget");
}

TEST("find a channel: an unjoined channel is joined and opens; People opens the DM") {
    Harness       h;
    const ConvRef random = h.conv("C0RANDOM");
    h.store.updateConversation(random, [](Conversation &c) { c.member = false; });
    h.sh->openBrowseDialog(0);
    pump();
    REQUIRE(h.win->topPopup() != nullptr);
    typeInto(*h.win, "rand");
    press(*h.win, plat::Key::Down); // the first match
    press(*h.win, plat::Key::Enter);
    pump(30);
    CHECK(h.win->topPopup() == nullptr);
    CHECK(h.store.conversation(random).member);
    CHECK(h.sh->current() == random);

    h.sh->openBrowseDialog(1); // the Direct messages "+": People
    pump();
    typeInto(*h.win, "jonas");
    press(*h.win, plat::Key::Down);
    press(*h.win, plat::Key::Enter);
    pump(30);
    CHECK(h.sh->current() == h.conv("D0JONAS"));
}

TEST("create a channel: a name, Next, Private, Create — the backend makes it") {
    Harness h;
    h.sh->openCreateChannel();
    pump();
    auto *dlg = h.win->topPopup();
    REQUIRE(dlg != nullptr);
    typeInto(*h.win, "Plan Budget");
    press(*h.win, plat::Key::Enter); // Next
    auto *priv = findNamed(dlg, "Private — only specific people");
    REQUIRE(priv != nullptr);
    static_cast<ui::Clickable *>(priv)->activate();
    REQUIRE(findNamed(dlg, "Public — anyone in Lumen Studio") != nullptr);
    auto *create = findNamed(dlg, "Create");
    REQUIRE(create != nullptr);
    static_cast<ui::Clickable *>(create)->activate();
    pump(30);
    CHECK(h.win->topPopup() == nullptr);
    ConvRef made = kNoConv;
    for (ConvRef c = 0; c < h.store.conversationCount(); ++c)
        if (h.store.conversation(c).name == "plan-budget")
            made = c;
    REQUIRE(made != kNoConv);
    CHECK(h.store.conversation(made).kind == ConvKind::Private);
    CHECK(h.store.conversation(made).member);
}

TEST("undo send: the pill for 5 s, Ctrl+Z or a click takes the message back") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    auto         &c      = h.sh->composer();
    c.edit().insertText("oops");
    REQUIRE(c.send());
    pump(2);
    CHECK(c.undoOffered());
    const Ts ts = h.store.conversation(design).messages.back().ts;
    CHECK(h.store.findMessage(design, ts)->text == "oops");
    c.undoSend();
    pump();
    CHECK(h.store.findMessage(design, ts) == nullptr);
    CHECK_STR(c.edit().text(), "oops");
    CHECK_FALSE(c.undoOffered());
    // Ctrl+Z only with an empty editor.
    REQUIRE(c.send());
    pump(2);
    const Ts ts2 = h.store.conversation(design).messages.back().ts;
    c.edit().focus();
    // The primary modifier: Cmd+Z on macOS.
    const plat::Key mod =
        plat::primaryMod() == plat::ModSuper ? plat::Key::SuperLeft : plat::Key::ControlLeft;
    app().platform().testHooks()->injectKey(h.win->native(), mod, true);
    app().platform().testHooks()->injectKey(h.win->native(), plat::Key::Z, true);
    app().platform().testHooks()->injectKey(h.win->native(), plat::Key::Z, false);
    app().platform().testHooks()->injectKey(h.win->native(), mod, false);
    pump();
    CHECK(h.store.findMessage(design, ts2) == nullptr);
    CHECK_STR(c.edit().text(), "oops");
    // The canvas tab hides the composer: that withdraws the offer (the
    // message stays sent).
    c.edit().clear();
    c.edit().insertText("keep");
    REQUIRE(c.send());
    pump(2);
    REQUIRE(c.undoOffered());
    const Ts ts3 = h.store.conversation(design).messages.back().ts;
    h.sh->showCanvas(true);
    pump();
    CHECK_FALSE(c.undoOffered());
    CHECK(h.store.findMessage(design, ts3) != nullptr);
    h.sh->showCanvas(false);
}

TEST("workspace icon: the dialog; the rail shows the saved picture") {
    Harness h;
    CHECK(shell::customWorkspaceIconPath(app().platform(), h.store.workspaceId).empty());
    REQUIRE(h.sh->menus().hooks.changeWorkspaceIcon);
    h.sh->menus().run(shell::Menus::kChangeIcon, 0);
    pump();
    auto *dlg = static_cast<ui::Dialog *>(h.win->topPopup());
    REQUIRE(dlg != nullptr);
    dlg->reject();
    pump();
    CHECK(h.win->topPopup() == nullptr);
}

// ── First load, remote avatars ──────────────────────────────────────────────

namespace {

// Is a label with exactly this text on screen?
bool showsText(ui::View *v, std::string_view text) {
    if (!v->visible())
        return false;
    if (v->role() == ui::Role::Text && static_cast<ui::Label *>(v)->text() == text)
        return true;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (showsText(v->child(i), text))
            return true;
    return false;
}

bool until(const std::function<bool()> &done, int ms = 3000) {
    const double end = app().nowMs() + ms;
    while (!done()) {
        if (app().nowMs() > end)
            return false;
        app().pump(2);
    }
    return true;
}

} // namespace

TEST("first load: no conversations yet hides the column and shows the loading state") {
    Store             store;
    fake::FakeBackend backend{store, app().platform()};
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache images{app().platform()};
    screens::Context    ctx{app(), store, backend, images, {}, {}, {}, {}, {}};
#else
    alignas(16) char noCache[16]{};
    screens::Context ctx{
        app(), store, backend, *reinterpret_cast<screens::ImageCache *>(noCache), {}, {}, {}, {}, {}
    };
#endif
    shell::Settings  settings;
    plat::WindowDesc d;
    d.size = {1200, 800};
    ui::Window   win(d);
    shell::Shell sh(ctx, win, settings, std::string());
    pump();
    CHECK_FALSE(sh.waiting());
    // A workspace opens (Accounts::activate): nothing loaded yet.
    sh.setSignedIn(true);
    pump();
    CHECK(sh.waiting());
    CHECK_FALSE(sh.sidebar().visible());
    CHECK_FALSE(showsText(&win.root(), "Keyboard shortcuts"));
#ifdef MSGA_HAVE_MESSAGES
    // The hint after a second.
    CHECK(until([&] { return showsText(&win.root(), "Loading your stuff..."); }, 2500));
#endif
    // Connected: the column is back, and the shortcuts panel (nothing open).
    backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
    bool done = false;
    backend.connect([&](bool ok, const std::string &) { done = ok; });
    REQUIRE(until([&] { return done; }));
    sh.setLive(true);
    pump();
    CHECK_FALSE(sh.waiting());
    CHECK(sh.sidebar().visible());
    CHECK(showsText(&win.root(), "Keyboard shortcuts"));
    CHECK_FALSE(showsText(&win.root(), "Loading your stuff..."));
    // Signing out ends it too.
    sh.setSignedIn(false);
    sh.setSignedIn(true); // the store still has the workspace: no waiting
    CHECK_FALSE(sh.waiting());
}

TEST("workspace switch: the open chat and thread are left before the Store empties") {
    // Accounts::activate: leaveWorkspace, then Store::clear. The views that
    // still pointed into the old workspace rebuilt from it (a segfault on
    // adding the Claude Code workspace with a Slack chat open).
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    h.sh->open(h.conv("C0ENG"));
    h.sh->open(design);
    const Ts root = h.backend.findTs(design, "Proposal B");
    REQUIRE(root != 0);
    h.sh->openThread(design, root);
    REQUIRE(h.sh->threadOpen());
    h.sh->leaveWorkspace();
    h.store.clear();
    pump();
    CHECK(h.sh->current() == kNoConv);
    CHECK_FALSE(h.sh->threadOpen());
    CHECK_FALSE(h.sh->navigateHistory(true)); // the old workspace's chats are gone
    CHECK(showsText(&h.win->root(), "Keyboard shortcuts"));
    // The next workspace's data arrives and opens as usual.
    bool done = false;
    h.backend.connect([&](bool ok, const std::string &) { done = ok; });
    REQUIRE(until([&] { return done; }));
    h.sh->open(h.conv("C0DESIGN"));
    pump();
    CHECK(h.sh->current() == h.conv("C0DESIGN"));
}

// ── Workspace rail ──────────────────────────────────────────────────────────

namespace {

const std::string kSlackKey = "slack:T0LUMEN", kClaudeKey = "claude-code:local";

// The rail with a second workspace under the open one.
void twoWorkspaces(Harness &h) {
    h.sh->setWorkspaces(
        {{kSlackKey, "T0LUMEN", "Lumen Studio", "", false},
         {kClaudeKey, "local", "Claude Code", "", false}},
        kSlackKey
    );
    pump();
}

ui::PointF centre(ui::View *v) {
    const ui::RectF r = v->windowRect();
    return {r.x + r.w / 2, r.y + r.h / 2};
}

void moveTo(Harness &h, ui::PointF p) {
    app().platform().testHooks()->injectPointerMove(h.win->native(), {p.x, p.y});
    pump(2);
}

void button(Harness &h, plat::Button b, bool down) {
    app().platform().testHooks()->injectButton(h.win->native(), b, down);
    pump(2);
}

} // namespace

TEST("workspace rail: a tile per workspace; a click or its tray item switches") {
    Harness h;
    twoWorkspaces(h);
    std::vector<std::string> switched;
    h.sh->onSwitchWorkspace = [&](const std::string &k) { switched.push_back(k); };
    REQUIRE(h.sh->workspaceTile(kSlackKey));
    REQUIRE(h.sh->workspaceTile(kClaudeKey));
    CHECK(h.sh->railOrder() == std::vector<std::string>({kSlackKey, kClaudeKey}));
    CHECK_STR(h.sh->workspaceTile(kClaudeKey)->tooltip(), "Claude Code");
    // One under the other, kGap apart.
    const ui::RectF a = h.sh->workspaceTile(kSlackKey)->windowRect();
    const ui::RectF b = h.sh->workspaceTile(kClaudeKey)->windowRect();
    CHECK(b.y - a.y == 48);

    moveTo(h, centre(h.sh->workspaceTile(kSlackKey))); // the open one: nothing
    button(h, plat::Button::Left, true);
    button(h, plat::Button::Left, false);
    CHECK(switched.empty());
    moveTo(h, centre(h.sh->workspaceTile(kClaudeKey)));
    button(h, plat::Button::Left, true);
    button(h, plat::Button::Left, false);
    REQUIRE(switched.size() == 1);
    CHECK_STR(switched[0], kClaudeKey);

    // The tray menu: one item per workspace, in the rail's order.
    plat::Event e;
    e.type = plat::EventType::TrayMenuItem;
    e.id   = 101;
    h.sh->handleAppEvent(e);
    REQUIRE(switched.size() == 2);
    CHECK_STR(switched[1], kClaudeKey);
}

TEST("workspace rail: dragging a tile reorders the rail, without a click") {
    Harness h;
    twoWorkspaces(h);
    std::vector<std::string> switched, order;
    h.sh->onSwitchWorkspace   = [&](const std::string &k) { switched.push_back(k); };
    h.sh->onReorderWorkspaces = [&](const std::vector<std::string> &k) { order = k; };
    const ui::PointF from     = centre(h.sh->workspaceTile(kSlackKey));
    moveTo(h, from);
    button(h, plat::Button::Left, true);
    for (int dy = 6; dy <= 60; dy += 6) // past the drag distance, then over the next slot
        moveTo(h, {from.x, from.y + float(dy)});
    CHECK(order.empty()); // reported on the drop
    button(h, plat::Button::Left, false);
    CHECK(order == std::vector<std::string>({kClaudeKey, kSlackKey}));
    CHECK(h.sh->railOrder() == order);
    CHECK(switched.empty());
    // The tray follows; both settle into their new slots.
    CHECK_STR(h.sh->workspaces().front().key, kClaudeKey);
    REQUIRE(until([&] {
        return h.sh->workspaceTile(kSlackKey)->windowRect().y -
                   h.sh->workspaceTile(kClaudeKey)->windowRect().y ==
               48;
    }));

    // A drop back where it started reports nothing.
    order.clear();
    const ui::PointF p = centre(h.sh->workspaceTile(kSlackKey));
    moveTo(h, p);
    button(h, plat::Button::Left, true);
    moveTo(h, {p.x, p.y + 12});
    moveTo(h, {p.x, p.y + 2});
    button(h, plat::Button::Left, false);
    CHECK(order.empty());
    CHECK(switched.empty());
}

TEST("workspace rail: a background workspace's menu acts on it") {
    Harness       h;
    shell::Menus &m = h.sh->menus();
    twoWorkspaces(h);
    std::string signedOut = "-", muted;
    bool        mutedOn   = false;
    m.hooks.signOut       = [&](const std::string &k) { signedOut = k; };
    m.hooks.muteWorkspace = [&](const std::string &k, bool on) {
        muted   = k;
        mutedOn = on;
    };
    // main_window.cpp showWorkspaceMenu, for a workspace that isn't open.
    CHECK_STR(
        labels(m.workspaceItems({kClaudeKey, "Claude Code", false, false})),
        "Change icon…|Mute|Log out from Claude Code"
    );
    CHECK_STR(m.workspaceItems({kClaudeKey, "Claude Code", true, false})[1].label, "Unmute");
    // A right click on its tile opens that menu.
    moveTo(h, centre(h.sh->workspaceTile(kClaudeKey)));
    button(h, plat::Button::Right, true);
    button(h, plat::Button::Right, false);
    m.run(shell::Menus::kMuteWorkspace, 0);
    CHECK_STR(muted, kClaudeKey);
    CHECK(mutedOn);
    CHECK_FALSE(h.store.workspaceMuted); // the open workspace is not the one muted
    m.run(shell::Menus::kSignOut, 0);    // the menu was used up: the open one's now
    CHECK_STR(signedOut, "");
}

TEST("notifications: pictures are a rounded square (roundedNotifIcon)") {
    gfx::Bitmap b(80, 64); // wider than tall: centre-cropped to 64
    for (int i = 0; i < 80 * 64; ++i)
        b.pixels()[i] = 0xffffffffu;
    const plat::Image img = shell::roundedNotificationImage(b);
    REQUIRE(img.width == 64 && img.height == 64);
    const auto at = [&](int x, int y) { return img.pixels[size_t(y) * 64 + x]; };
    CHECK(at(0, 0) == 0); // outside the 14 px corner arc
    CHECK(at(63, 63) == 0);
    CHECK(at(32, 32) == 0xffffffffu); // inside
    CHECK(at(0, 32) == 0xffffffffu);  // a straight edge
    const uint32_t edge = at(2, 6);   // on the arc: partly covered, premultiplied
    CHECK((edge >> 24) > 0 && (edge >> 24) < 255);
    CHECK((edge & 0xff) == (edge >> 24));
}

TEST("avatars: a URL hands out a blank that fills in place when the download lands") {
    const std::string dir = file::join(
        std::getenv("HOME") ? std::getenv("HOME") : "/tmp",
        "avatar-test-" + std::to_string(base::nowMicros())
    );
    {
        screens::RemoteImages remote(app().platform(), nullptr, dir);
        int                   asked = 0;
        remote.setFetch([&](net::Request q, std::function<void(net::Response)> done) {
            ++asked;
            net::Response r;
            r.status = 200;
            if (q.url.find("/nope") != std::string::npos)
                r.status = 404;
            else
                file::readAll(std::string(MSGA_TEST_ASSETS) + "/avatars/mira.png", &r.body);
            app().platform().post([done, r] { done(r); });
        });
        screens::ImageCache images(app().platform());
        shell::Avatars      av(images);
        av.setRemote(&remote);
        int                                  loaded = 0;
        std::vector<shell::Avatars::Picture> landed;
        av.onLoaded = [&](const std::vector<shell::Avatars::Picture> &l) {
            ++loaded;
            landed = l;
        };
        const auto b = av.get("https://avatars.test/mira.png", 40);
        REQUIRE(b != nullptr);
        CHECK(b->empty()); // the placeholder shows meanwhile
        CHECK(av.get("https://avatars.test/mira.png", 40) == b);
        REQUIRE(until([&] { return loaded == 1; }));
        CHECK(b->width() == 40 && b->height() == 40);  // the same bitmap, filled
        CHECK((landed.size() == 1 && landed[0] == b)); // what the shell repaints
        CHECK(asked == 1);
        // On disk now: a fresh cache decodes it (on the worker), no download.
        shell::Avatars again(images);
        again.setRemote(&remote);
        const auto c = again.get("https://avatars.test/mira.png", 24);
        REQUIRE(c != nullptr);
        REQUIRE(until([&] { return !c->empty(); }));
        CHECK(c->width() == 24);
        CHECK(asked == 1);
        // A failed one keeps its blank; asking again during the cool-down
        // doesn't download again.
        const auto f = av.get("https://avatars.test/nope.png", 40);
        REQUIRE(until([&] { return asked == 2 && remote.pending() == 0; }));
        pump();
        CHECK(f->empty());
        av.get("https://avatars.test/nope.png", 40);
        pump(4);
        CHECK(asked == 2);
        // No RemoteImages: a URL is no picture.
        shell::Avatars none(images);
        CHECK(none.get("https://avatars.test/mira.png", 40) == nullptr);
        // whenReady: none while a URL downloads (the next notification has
        // it), the picture once it is there.
        int                     calls = 0;
        shell::Avatars::Picture got;
        av.whenReady("https://avatars.test/later.png", 40, [&](shell::Avatars::Picture p) {
            ++calls;
            got = p;
        });
        CHECK(calls == 1 && got == nullptr);
        av.whenReady("https://avatars.test/mira.png", 40, [&](shell::Avatars::Picture p) {
            ++calls;
            got = p;
        });
        CHECK(calls == 2 && got == b);
        REQUIRE(until([&] { return remote.pending() == 0 && images.pending() == 0; }));
    }
    std::vector<file::DirEntry> all;
    if (file::listDir(dir, &all))
        for (const auto &e : all)
            file::remove(file::join(dir, e.name));
    file::remove(dir);
}

TEST("avatars: files decode on the worker, never in get(); the cache is bounded") {
    screens::ImageCache images(app().platform());
    shell::Avatars      av(images);
    int                 loaded = 0;
    av.onLoaded                = [&](const std::vector<shell::Avatars::Picture> &) { ++loaded; };
    const std::string mira     = std::string(MSGA_TEST_ASSETS) + "/avatars/mira.png";
    // get() returns at once with a blank: the decode is the worker's.
    const auto        b        = av.get(mira, 40);
    REQUIRE(b != nullptr);
    CHECK(b->empty());
    CHECK(av.decodes() == 1);
    CHECK(images.pending() == 1);
    CHECK(av.get(mira, 40) == b); // asked again: the same blank, no second decode
    CHECK(av.decodes() == 1);
    // whenReady waits for it, and is never called inside the call.
    int                     calls = 0;
    shell::Avatars::Picture got;
    av.whenReady(mira, 40, [&](shell::Avatars::Picture p) {
        ++calls;
        got = p;
    });
    CHECK(calls == 0);
    REQUIRE(until([&] { return loaded == 1; }));
    CHECK(calls == 1 && got == b);
    CHECK(b->width() == 40 && b->height() == 40); // filled in place
    CHECK(av.bytes() == 40 * 40 * 4);
    // Decoded: whenReady answers at once.
    av.whenReady(mira, 40, [&](shell::Avatars::Picture p) {
        ++calls;
        got = p;
    });
    CHECK(calls == 2 && got == b);
    // Not an image: null from then on, and whenReady says so.
    const std::string junk = std::string(MSGA_TEST_ASSETS) + "/fixture.json";
    const auto        j    = av.get(junk, 40);
    REQUIRE(j != nullptr);
    av.whenReady(junk, 40, [&](shell::Avatars::Picture p) {
        ++calls;
        got = p;
    });
    REQUIRE(until([&] { return calls == 3; }));
    CHECK(got == nullptr);
    CHECK(j->empty());
    CHECK(av.get(junk, 40) == nullptr);
    // A budget of two 40 px pictures: the least recently asked for goes;
    // a view holding one keeps its pixels.
    av.setBudget(2 * 40 * 40 * 4);
    const std::string jonas = std::string(MSGA_TEST_ASSETS) + "/avatars/jonas.png";
    const std::string sam   = std::string(MSGA_TEST_ASSETS) + "/avatars/sam.png";
    const auto        jb    = av.get(jonas, 40);
    REQUIRE(until([&] { return !jb->empty(); }));
    const auto sb = av.get(sam, 40);
    REQUIRE(until([&] { return !sb->empty(); }));
    CHECK(av.bytes() <= 2 * 40 * 40 * 4);
    CHECK(b->width() == 40); // still held here
    CHECK(av.get(sam, 40) == sb);
    CHECK(av.get(jonas, 40) == jb);
    const size_t before = av.decodes();
    const auto   again  = av.get(mira, 40); // evicted: decoded anew
    CHECK(again != b);
    CHECK(av.decodes() == before + 1);
    REQUIRE(until([&] { return images.pending() == 0; }));
    CHECK(av.bytes() <= 2 * 40 * 40 * 4);
}

// ── The Threads page ────────────────────────────────────────────────────────

namespace {

// Pumps until the Threads page has answered (its status is no longer "Loading").
void waitForThreads(shell::ThreadsPage &page) {
    for (int i = 0; i < 200 && page.statusText() == "Loading threads…"; ++i)
        app().pump(5);
}

} // namespace

TEST("threads page: the sidebar entry opens the followed threads, newest first") {
    Harness h;
    REQUIRE(h.backend.capabilities().threadsView);
    h.sh->sidebar().onThreads(); // the entry's click
    CHECK(h.sh->pageOpen(shell::Shell::Page::Threads));
    CHECK(h.sh->sidebar().selectedNav() == shell::Sidebar::Nav::Threads);
    CHECK(h.sh->sidebar().selected() == kNoConv);
    CHECK(h.sh->current() == kNoConv);
    CHECK(!h.sh->composer().visible()); // the cards bring their own reply boxes
    shell::ThreadsPage &page = *h.sh->threadsPage();
    CHECK_STR(page.statusText(), "Loading threads…");
    waitForThreads(page);
    REQUIRE(page.cardCount() > 0);
    CHECK_STR(page.statusText(), "");
    Ts prev = INT64_MAX;
    for (size_t i = 0; i < page.cardCount(); ++i) {
        const auto *replies = h.store.replies(page.card(i).conv(), page.card(i).root());
        REQUIRE(replies && !replies->empty());
        CHECK(replies->back().ts <= prev);
        prev = replies->back().ts;
        CHECK(page.card(i).replyRows() == std::min<size_t>(replies->size(), 3));
        // Every fixture thread starts out read: no "New" pill.
        CHECK(!page.card(i).unread());
        CHECK(!page.card(i).newPill()->visible());
        CHECK(!page.card(i).participants().empty());
    }
    // Opening a conversation leaves the page and its highlight.
    h.sh->open(h.conv("C0DESIGN"));
    CHECK(!h.sh->pageOpen(shell::Shell::Page::Threads));
    CHECK(h.sh->sidebar().selectedNav() != shell::Sidebar::Nav::Threads);
    CHECK(h.sh->composer().visible());
}

TEST("threads page: a card's reply box posts into the thread and the card follows") {
    Harness h;
    h.sh->showPage(shell::Shell::Page::Threads);
    shell::ThreadsPage &page = *h.sh->threadsPage();
    waitForThreads(page);
    REQUIRE(page.cardCount() > 0);
    shell::ThreadsPage::Card &card = page.card(0);
    const size_t              rows = card.replyRows();
    CHECK(card.replyButton()->visible());
    card.showComposer();
    REQUIRE(card.composer());
    CHECK(!card.replyButton()->visible());
    CHECK(card.composer()->conv() == card.conv());
    CHECK(card.composer()->thread() == card.root());
    card.composer()->edit().insertText("from the threads page");
    CHECK(card.composer()->send());
    pump();
    CHECK(card.replyRows() == rows + 1);
    const auto *replies = h.store.replies(card.conv(), card.root());
    REQUIRE(replies);
    CHECK_STR(replies->back().text, "from the threads page");
    // Confirmed by the server: still one row for it.
    for (int i = 0; i < 100; ++i)
        app().pump(5);
    CHECK(card.replyRows() >= rows + 1);
    CHECK(!card.unread());
}

TEST("threads page: a message opens its thread; the channel name its channel") {
    Harness h;
    h.sh->showPage(shell::Shell::Page::Threads);
    shell::ThreadsPage &page = *h.sh->threadsPage();
    waitForThreads(page);
    REQUIRE(page.cardCount() > 0);
    const ConvRef conv = page.card(0).conv();
    page.card(0).openThread();
    CHECK(!h.sh->pageOpen(shell::Shell::Page::Threads));
    CHECK(h.sh->current() == conv);
    CHECK(h.sh->threadOpen());
    CHECK(h.sh->sidebar().selected() == conv);
    // Back to the page: reloaded, highlighted again, the thread closed.
    h.sh->showPage(shell::Shell::Page::Threads);
    CHECK(h.sh->sidebar().selectedNav() == shell::Sidebar::Nav::Threads);
    CHECK(!h.sh->threadOpen());
    waitForThreads(page);
    REQUIRE(page.cardCount() > 0);
    page.onOpenChannel(conv);
    CHECK(h.sh->current() == conv);
    CHECK(!h.sh->pageOpen(shell::Shell::Page::Threads));
}

TEST("threads page: participants, and a feed that can't be loaded") {
    Store         store;
    const UserRef me   = store.addUser({.id = "U1", .name = "me"});
    const UserRef adam = store.addUser({.id = "U2", .name = "adam"});
    const UserRef bea  = store.addUser({.id = "U3", .name = "bea"});
    const UserRef cy   = store.addUser({.id = "U4", .name = "cy"});
    const UserRef dee  = store.addUser({.id = "U5", .name = "dee"});
    store.me           = me;
    Message root;
    root.user = adam;
    CHECK_STR(shell::threadParticipants(store, root), "adam");
    root.replyUsers = {me};
    CHECK_STR(shell::threadParticipants(store, root), "adam and you");
    root.user       = me;
    root.replyUsers = {};
    CHECK_STR(shell::threadParticipants(store, root), "you");
    root.user       = adam;
    root.replyUsers = {bea, cy, dee, me};
    CHECK_STR(shell::threadParticipants(store, root), "adam, bea, cy and 1 others");

    // The error line when the feed fails (and nothing was shown before).
    struct NoFeed : fake::FakeBackend {
        using FakeBackend::FakeBackend;
        void loadThreadsView(std::string, ThreadsViewDone done) override {
            app().platform().post([done] { done(false, {}); });
        }
    };
    Harness            h;
    NoFeed             nf{h.store, app().platform()};
    screens::Context   failing{app(), h.store, nf, h.ctx.images, {}, {}, {}, {}, {}};
    shell::ThreadsPage page(failing, h.sh->avatars(), h.sh->drafts(), nullptr);
    page.open();
    waitForThreads(page);
    CHECK_STR(page.statusText(), "Couldn't load threads. Try again later.");
    CHECK(page.cardCount() == 0);

    // Slack's code goes with it: the clue a bug report carries.
    struct RefusedFeed : fake::FakeBackend {
        using FakeBackend::FakeBackend;
        void loadThreadsView(std::string, ThreadsViewDone done) override {
            app().platform().post([done] {
                ThreadsView failed;
                failed.error = "enterprise_is_restricted";
                done(false, std::move(failed));
            });
        }
    };
    RefusedFeed        rf{h.store, app().platform()};
    screens::Context   refused{app(), h.store, rf, h.ctx.images, {}, {}, {}, {}, {}};
    shell::ThreadsPage page2(refused, h.sh->avatars(), h.sh->drafts(), nullptr);
    page2.open();
    waitForThreads(page2);
    CHECK_STR(
        page2.statusText(), "Couldn't load threads. Try again later. (enterprise_is_restricted)"
    );
}

// ── AI assistance → the LLM layer ───────────────────────────────────────────

TEST("settings: the AI providers reach the LLM layer; Summarize's notice opens their page") {
    Harness      h;
    llm::Service ai(app().platform());
    h.ctx.ai = &ai;
    h.sh->applySettings();
    CHECK_FALSE(ai.available()); // the presets, no keys
    REQUIRE(ai.providers().size() == 2);
    h.settings.provider("openai")->key = "sk-test";
    h.settings.ai.push_back({"custom-1", "Box", "http://localhost:8000/v1", {}, "qwen", {}});
    h.settings.aiDefault = "custom-1";
    h.settings.language  = "ja";
    h.sh->applySettings();
    REQUIRE(ai.active() != nullptr);
    CHECK_STR(ai.active()->id, "custom-1");
    CHECK_STR(ai.active()->model, "qwen");
    CHECK_STR(ai.language(), "ja"); // follows the app language until one is picked
    h.settings.aiLanguage = "sv";
    h.settings.aiDefault  = "openai";
    h.sh->applySettings();
    CHECK_STR(ai.language(), "sv");
    CHECK_STR(ai.active()->id, "openai");
    CHECK_STR(ai.active()->model, "gpt-5.6-terra");

    // "Open settings" on the notice: Settings on AI assistance, also when
    // it is open on another page already.
    REQUIRE(bool(h.ctx.openAiSettings));
    h.ctx.openAiSettings();
    pump();
    REQUIRE(h.sh->settingsDialog() != nullptr);
    CHECK(h.sh->settingsDialog()->page() == settings::SettingsDialog::Page::Ai);
    h.sh->settingsDialog()->showPage(settings::SettingsDialog::Page::Storage);
    h.ctx.openAiSettings();
    CHECK(h.sh->settingsDialog()->page() == settings::SettingsDialog::Page::Ai);
    h.ctx.ai = nullptr;
}

// ── Voice input ─────────────────────────────────────────────────────────────

namespace {

std::string toneWav(float amplitude) {
    std::string pcm;
    for (int i = 0; i < 16 * 600; ++i) {
        const int16_t v = int16_t(amplitude * 32767 * std::sin(i * 2 * 3.14159265 * 440 / 16000));
        pcm += char(v & 0xff);
        pcm += char((v >> 8) & 0xff);
    }
    return plat::audio::wavFromPcm16(pcm, 16000, 1);
}

} // namespace

TEST("composer: voice input — the mic, the strip, the dictation at the caret, Escape cancels") {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake AI provider\n");
        return;
    }
    fakellm::ctl(app().platform(), "POST", "/_ctl/reset");
    const std::string speech = file::join(std::getenv("HOME"), "speech.wav");
    const std::string quiet  = file::join(std::getenv("HOME"), "quiet.wav");
    REQUIRE(file::writeAtomic(speech, toneWav(0.4f)));
    REQUIRE(file::writeAtomic(quiet, toneWav(0)));
    llm::Service ai(app().platform()); // outlives the shell's composers
    Harness      h(&ai);
    using shell::shortcuts::Id;
    auto &c   = h.sh->composer();
    auto *mic = c.button(Id::VoiceInput);
    REQUIRE(mic != nullptr);
    h.sh->applySettings();
    CHECK_FALSE(mic->visible()); // no provider with speech-to-text
    h.settings.provider("anthropic")->key = "sk-ant";
    h.sh->applySettings();
    CHECK_FALSE(mic->visible()); // Anthropic has none
    h.settings.ai.push_back({"custom-1", "Fake", base + "/v1", {}, "m", "gpt-transcribe"});
    h.settings.voiceGlossary = "msga\n  Zed  \n\n";
    h.settings.voiceCleanup  = false;
    h.sh->applySettings();
    CHECK(mic->visible());
    CHECK(ai.voice().glossary() == (std::vector<std::string>{"msga", "Zed"}));

    // Tap the mic: recording, the strip with its meter; tap again: the text
    // lands after what was typed, with a space.
    base::test::setEnv("MSGA_VOICE_FAKE_WAV", speech);
    c.edit().insertText("hello");
    mic->activate();
    shell::VoiceStrip *strip = c.voiceStrip();
    REQUIRE(strip != nullptr);
    CHECK(strip->mode() == shell::VoiceStrip::Mode::Recording);
    CHECK(strip->visible());
    CHECK(c.voiceActiveHere());
    CHECK(mic->tooltip().rfind("Stop and transcribe", 0) == 0);
    CHECK(strip->elapsedText() == "0:00");
    pump(20);
    mic->activate();
    CHECK(strip->mode() == shell::VoiceStrip::Mode::Transcribing);
    for (int i = 0; i < 2000 && c.voiceActiveHere(); ++i)
        app().pump(5);
    CHECK_FALSE(c.voiceActiveHere());
    CHECK(strip->mode() == shell::VoiceStrip::Mode::Hidden);
    CHECK_STR(c.edit().text(), "hello um so the deploy is uh ready");
    CHECK(mic->tooltip().rfind("Voice input", 0) == 0);
    json::Document log;
    REQUIRE(log.parse(fakellm::ctl(app().platform(), "GET", "/_ctl/log").body, nullptr));
    REQUIRE(log.root().size() == 1); // clean-up off: no chat request
    const std::string body(log.root()[0]["body"].str());
    CHECK(body.find("name=\"keywords[]\"\r\n\r\nmsga\r\n") != std::string::npos);
    CHECK(body.find("name=\"keywords[]\"\r\n\r\nZed\r\n") != std::string::npos);
    CHECK(body.find("Conversation: design") != std::string::npos);

    // Escape cancels a recording ahead of everything else.
    mic->activate();
    CHECK(c.voiceActiveHere());
    ui::Event esc{ui::EventType::KeyDown};
    esc.key = plat::Key::Escape;
    CHECK(c.edit().onKey(esc));
    CHECK_FALSE(c.voiceActiveHere());
    CHECK(strip->mode() == shell::VoiceStrip::Mode::Hidden);

    // A silent recording: the strip says so; × dismisses it.
    base::test::setEnv("MSGA_VOICE_FAKE_WAV", quiet);
    mic->activate();
    pump(4);
    mic->activate();
    for (int i = 0; i < 400 && c.voiceActiveHere(); ++i)
        app().pump(5);
    CHECK(strip->mode() == shell::VoiceStrip::Mode::Error);
    CHECK_STR(strip->error(), "No speech was recorded");
    REQUIRE(strip->onCancel);
    strip->onCancel();
    CHECK(strip->mode() == shell::VoiceStrip::Mode::Hidden);
    // Leaving never loses a dictation: the thread panel closing (an ancestor
    // hidden), the Threads page replacing the conversation, another
    // conversation opening — the recording stops, is transcribed all the
    // same, and the text lands where it was started.
    base::test::setEnv("MSGA_VOICE_FAKE_WAV", speech);
    const auto waitIdle = [&] {
        for (int i = 0; i < 2000 && ai.voice().state() != llm::VoiceInput::State::Idle; ++i)
            app().pump(5);
    };
    const model::ConvRef design  = h.sh->current();
    const model::ConvRef general = h.store.findConversation("C0GENERAL");
    const auto          &msgs    = h.store.conversation(design).messages;
    REQUIRE(!msgs.empty());
    const model::Ts root    = msgs.back().ts;
    // Slow answers, so the tests below see the transcriptions in flight.
    const char     *slowStt = R"([{"text":"um so the deploy is uh ready","__delay":0.6},
                              {"text":"um so the deploy is uh ready","__delay":0.6}])";
    fakellm::script(app().platform(), "/v1/audio/transcriptions", slowStt);
    h.sh->openThread(design, root);
    pump();
    shell::Composer *tc = h.sh->threadComposer();
    REQUIRE(tc != nullptr);
    REQUIRE(tc->button(Id::VoiceInput) != nullptr);
    tc->button(Id::VoiceInput)->activate();
    CHECK(tc->voiceActiveHere());
    pump(4);
    h.sh->closeThread();
    pump();
    CHECK(ai.voice().state() == llm::VoiceInput::State::Transcribing);
    // A new dictation meanwhile does not cancel it: both finish.
    c.edit().clear();
    mic->activate();
    CHECK(c.voiceActiveHere());
    pump(4);
    mic->activate();
    waitIdle();
    CHECK_STR(c.edit().text(), "um so the deploy is uh ready");
    h.sh->openThread(design, root);
    pump();
    CHECK_STR(tc->edit().text(), "um so the deploy is uh ready");
    tc->edit().clear();
    h.sh->closeThread();
    pump();
    // The Threads page, then back: the text is in the editor.
    c.edit().clear();
    mic->activate();
    pump(4);
    h.sh->showPage(shell::Shell::Page::Threads);
    pump();
    waitIdle();
    h.sh->open(design);
    pump();
    CHECK_STR(c.edit().text(), "um so the deploy is uh ready");
    // Another conversation: its composer shows nothing of it; the text goes
    // after design's draft.
    c.edit().setText("draft");
    fakellm::script(app().platform(), "/v1/audio/transcriptions", slowStt);
    mic->activate();
    pump(4);
    h.sh->open(general);
    pump();
    CHECK_FALSE(c.voiceActiveHere());
    CHECK(strip->mode() == shell::VoiceStrip::Mode::Hidden);
    CHECK(ai.voice().state() == llm::VoiceInput::State::Transcribing);
    // Back before it is done: the strip shows the transcription.
    h.sh->open(design);
    pump();
    CHECK(c.voiceActiveHere());
    CHECK(strip->mode() == shell::VoiceStrip::Mode::Transcribing);
    h.sh->open(general);
    pump();
    CHECK_STR(c.edit().text(), "");
    waitIdle();
    CHECK_STR(c.edit().text(), "");
    h.sh->open(design);
    pump();
    CHECK_STR(c.edit().text(), "draft um so the deploy is uh ready");
    c.edit().clear();
    // A failure after leaving is shown on coming back.
    base::test::setEnv("MSGA_VOICE_FAKE_WAV", quiet);
    mic->activate();
    pump(4);
    h.sh->open(general);
    pump();
    waitIdle();
    CHECK(strip->mode() == shell::VoiceStrip::Mode::Hidden);
    h.sh->open(design);
    pump();
    CHECK(strip->mode() == shell::VoiceStrip::Mode::Error);
    CHECK_STR(strip->error(), "No speech was recorded");
    strip->onCancel();
    base::test::unsetEnv("MSGA_VOICE_FAKE_WAV");

    // The provider goes: the mic hides.
    h.settings.ai.pop_back();
    h.sh->applySettings();
    CHECK_FALSE(mic->visible());
    file::remove(speech);
    file::remove(quiet);
}

// ── Running workspaces (a session per workspace) ────────────────────────────

namespace {

const std::string kKeyA = "slack:T0LUMEN", kKeyB = "slack:T0SECOND";

// The fixture's backend, counting what reaches its presence link.
struct LiveFake : fake::FakeBackend {
    using fake::FakeBackend::FakeBackend;
    void         setPresenceMode(PresenceMode m) override { mode = m; }
    void         noteUserActivity() override { ++activity; }
    Capabilities capabilities() const override {
        Capabilities c = fake::FakeBackend::capabilities();
        c.huddles      = huddles;
        return c;
    }
    bool threadFollowed(ConvRef, Ts root) const override { return root == followedRoot; }
    void requestPresence(UserRef u) override { presenceAsked.push_back(u); }
    std::vector<UserRef> presenceAsked;
    PresenceMode         mode         = PresenceMode::Native;
    int                  activity     = 0;
    bool                 huddles      = false; // Slack's: banner, pills, huddle notifications
    Ts                   followedRoot = 0;
};

// Two signed-in workspaces the way Accounts runs them: each its own Store
// and backend, attached to the shell; the screens' slot and proxy show one.
#ifdef MSGA_HAVE_MESSAGES
screens::ImageCache *gNotifyImages = nullptr; // the live TwoWorkspaces' cache
#endif

struct TwoWorkspaces {
    Store               blank, a, b;
    LiveFake            fa{a, app().platform()}, fb{b, app().platform()};
    model::BackendProxy proxy{blank, fa};
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache images{app().platform()};
    screens::Context    ctx{app(), a, proxy, images, {}, {}, {}, {}, {}};
#else
    alignas(16) char noCache[16]{};
    screens::Context ctx{
        app(), a, proxy, *reinterpret_cast<screens::ImageCache *>(noCache), {}, {}, {}, {}, {}
    };
#endif
    shell::Settings               settings;
    std::unique_ptr<ui::Window>   win;
    std::unique_ptr<shell::Shell> sh;
    std::vector<std::string>      switched;

    TwoWorkspaces() {
#ifdef MSGA_HAVE_MESSAGES
        gNotifyImages = &images;
#endif
        for (LiveFake *f : {&fa, &fb}) {
            f->setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
            bool done = false;
            f->connect([&](bool ok, const std::string &) { done = ok; });
            for (int i = 0; i < 200 && !done; ++i)
                app().pump(5);
        }
        b.workspaceId   = "T0SECOND";
        b.workspaceName = "Second Co";
        b.updateConversation(b.findConversation("C0DESIGN"), [](Conversation &c) {
            c.name = "second-design";
        });
        plat::WindowDesc d;
        d.size        = {1200, 800};
        d.decorations = plat::Decorations::Custom;
        win           = std::make_unique<ui::Window>(d);
        sh            = std::make_unique<shell::Shell>(ctx, *win, settings, std::string());
        sh->attachWorkspace(kKeyA, a, fa);
        sh->attachWorkspace(kKeyB, b, fb);
        sh->setWorkspaceLive(kKeyA, true);
        sh->setWorkspaceLive(kKeyB, true);
        sh->onSwitchWorkspace = [this](const std::string &key) {
            switched.push_back(key);
            show(key);
        };
        show(kKeyA);
        sh->open(a.findConversation("C0DESIGN"));
        pump();
    }
    ~TwoWorkspaces() {
#ifdef MSGA_HAVE_MESSAGES
        gNotifyImages = nullptr;
#endif
        sh->detachWorkspace(kKeyA);
        sh->detachWorkspace(kKeyB);
        sh.reset();
    }
    // Accounts::activate: the open one leaves, the proxy and the slot follow.
    void show(const std::string &key) {
        sh->leaveWorkspace();
        proxy.setTarget(key == kKeyA ? fa : fb);
        ctx.store.setTarget(key == kKeyA ? a : b);
        sh->setWorkspaces(
            {{kKeyA, "T0LUMEN", "Lumen Studio", "", false},
             {kKeyB, "T0SECOND", "Second Co", "", false}},
            key
        );
        sh->setSignedIn(true);
        sh->setLive(true);
        sh->workspaceChanged();
        pump();
    }
    // Someone else's message in `st` (a reply when `thread`).
    void post(Store &st, const char *conv, const char *text) {
        const ConvRef c = st.findConversation(conv);
        Message       m;
        m.ts   = st.conversation(c).latest + 1000;
        m.user = st.findUser("U0MIRA");
        m.text = text;
        st.addMessage(c, std::move(m));
    }
};

// The newest notification shown (0: none). A notification waits for its
// picture to be decoded (the ImageCache worker's job): let that finish first.
uint64_t lastNotification(plat::TestHooks::NotificationProbe *out) {
#ifdef MSGA_HAVE_MESSAGES
    if (gNotifyImages)
        until([] { return gNotifyImages->pending() == 0; });
#endif
    uint64_t last = 0;
    for (uint64_t id = 1; id < 4096; ++id) {
        plat::TestHooks::NotificationProbe p;
        if (app().platform().testHooks()->notificationProbe(id, &p)) {
            last = id;
            if (out)
                *out = p;
        }
    }
    return last;
}

} // namespace

TEST("workspaces: a background workspace notifies, its name first; the click goes there") {
    TwoWorkspaces  w;
    const uint64_t before = lastNotification(nullptr);
    w.post(w.b, "C0GENERAL", "hello from the other side");
    pump();
    plat::TestHooks::NotificationProbe n;
    const uint64_t                     id = lastNotification(&n);
    REQUIRE(id > before);
    CHECK(n.title.rfind("Second Co \xC2\xB7 ", 0) == 0); // main_window.cpp: "Team · …"
    CHECK(n.body.find("hello from the other side") != std::string::npos);
    // The open workspace's own say nothing about where they come from.
    w.post(w.a, "C0GENERAL", "and from here");
    pump();
    const uint64_t here = lastNotification(&n);
    REQUIRE(here > id);
    CHECK(n.title.find("\xC2\xB7") == std::string::npos);
    // A click: that workspace opens, on that chat.
    plat::Event e;
    e.type = plat::EventType::NotificationActivated;
    e.id   = id;
    w.sh->handleAppEvent(e);
    pump();
    REQUIRE(w.switched.size() == 1);
    CHECK_STR(w.switched[0], kKeyB);
    CHECK(&w.ctx.store() == &w.b);
    CHECK(w.sh->current() == w.b.findConversation("C0GENERAL"));
    // A muted workspace says nothing.
    w.a.workspaceMuted = true;
    w.post(w.a, "C0GENERAL", "shh");
    pump();
    CHECK(lastNotification(nullptr) == here);
}

TEST("workspaces: each tile shows its own dot; the badge sums the unmuted ones") {
    TwoWorkspaces w;
    const ConvRef bGeneral = w.b.findConversation("C0GENERAL");
    const ConvRef aGeneral = w.a.findConversation("C0GENERAL");
    // Everything read in both, to start from nothing.
    for (Store *st : {&w.a, &w.b})
        for (ConvRef c = 0; c < st->conversationCount(); ++c)
            if (st->conversation(c).latest)
                st->markRead(c, st->conversation(c).latest);
    pump();
    REQUIRE(until([&] { return w.sh->workspaceDot(kKeyB) == 0; }));
    CHECK(w.sh->workspaceDot(kKeyA) == 0);
    CHECK(app().platform().testHooks()->badgeCount() == 0);

    // Plain activity in the background one: its tile turns blue, the open one stays.
    w.post(w.b, "C0GENERAL", "news");
    REQUIRE(until([&] { return w.sh->workspaceDot(kKeyB) == 1; }));
    CHECK(w.sh->workspaceDot(kKeyA) == 0);
    // A mention: red, and it counts on the launcher badge.
    w.post(w.b, "C0GENERAL", "<@U0ALEX> look");
    REQUIRE(w.b.conversation(bGeneral).mentions == 1);
    REQUIRE(until([&] { return w.sh->workspaceDot(kKeyB) == 2; }));
    CHECK(app().platform().testHooks()->badgeCount() == 1);
    // One in the open workspace too: both add up.
    // (The open workspace recounts after its burst too: attentionSoon.)
    w.post(w.a, "C0GENERAL", "<@U0ALEX> here too");
    REQUIRE(until([&] { return w.sh->workspaceDot(kKeyA) == 2; }));
    CHECK(app().platform().testHooks()->badgeCount() == 2);
    // A muted workspace keeps its dot but leaves the totals.
    w.b.workspaceMuted = true;
    w.a.markRead(aGeneral, w.a.conversation(aGeneral).latest);
    REQUIRE(until([&] { return app().platform().testHooks()->badgeCount() == 0; }));
    CHECK(w.sh->workspaceDot(kKeyB) == 2);
    w.b.workspaceMuted = false;
    // Switching keeps each one's state: the tiles trade places, nothing reloads.
    w.show(kKeyB);
    CHECK(w.sh->workspaceDot(kKeyB) == 2);
    CHECK(w.sh->workspaceDot(kKeyA) == 0);
    CHECK(app().platform().testHooks()->badgeCount() == 1);
}

TEST("workspaces: switching shows the other Store; each keeps its chat and drafts") {
    TwoWorkspaces w;
    const ConvRef design = w.a.findConversation("C0DESIGN");
    REQUIRE(w.sh->current() == design);
    w.sh->composer().edit().insertText("half a thought");
    w.show(kKeyB);
    CHECK(w.sh->current() == kNoConv);
    CHECK(w.sh->sidebar().rowState(w.b.findConversation("C0DESIGN")).exists);
    // The same ConvRef in the other workspace is another conversation: no draft.
    w.sh->open(w.b.findConversation("C0DESIGN"));
    pump();
    CHECK_STR(w.sh->composer().edit().text(), "");
    CHECK_STR(w.ctx.store().displayName(w.sh->current()), "second-design");
    w.show(kKeyA);
    w.sh->open(design);
    pump();
    CHECK_STR(w.sh->composer().edit().text(), "half a thought");
}

// The exit crash: ~Accounts moved the slot to the blank Store with a chat
// and its thread still open, and the Users it told the views rebuilt the
// message list from a ConvRef the blank Store lacks.
TEST("workspaces: the slot leaving for the blank Store under an open chat and thread") {
    TwoWorkspaces w;
    const ConvRef design = w.a.findConversation("C0DESIGN");
    const Ts      root   = w.fa.findTs(design, "Proposal B");
    REQUIRE(root != 0);
    w.sh->openThread(design, root);
    pump();
    REQUIRE(w.sh->threadOpen());
    // Without the screens letting go first: a stale ref reads as nothing.
    w.ctx.store.setTarget(w.blank);
    pump();
    w.ctx.store.setTarget(w.a);
    pump();
    // Accounts' teardown: the screens let go, then the slot moves.
    w.sh->leaveWorkspace();
    w.proxy.setTarget(w.fa);
    w.ctx.store.setTarget(w.blank);
    pump();
    CHECK(w.sh->current() == kNoConv);
    CHECK_FALSE(w.sh->threadOpen());
}

TEST("history: back and forward cross workspaces; a signed-out one's entries go") {
    TwoWorkspaces w;
    const ConvRef aDesign  = w.a.findConversation("C0DESIGN");
    const ConvRef aGeneral = w.a.findConversation("C0GENERAL");
    const ConvRef bGeneral = w.b.findConversation("C0GENERAL");
    w.sh->open(aGeneral);
    w.show(kKeyB); // switching keeps the history (a location names its workspace)
    w.sh->open(bGeneral);
    pump();
    // Back: into the other workspace, on the chat open there, not its last.
    REQUIRE(w.sh->navigateHistory(true));
    pump();
    REQUIRE(!w.switched.empty());
    CHECK_STR(w.switched.back(), kKeyA);
    CHECK(&w.ctx.store() == &w.a);
    CHECK(w.sh->current() == aGeneral);
    REQUIRE(w.sh->navigateHistory(true));
    CHECK(w.sh->current() == aDesign);
    CHECK_FALSE(w.sh->navigateHistory(true));
    // Forward twice: back in the second workspace.
    REQUIRE(w.sh->navigateHistory(false));
    CHECK(w.sh->current() == aGeneral);
    REQUIRE(w.sh->navigateHistory(false));
    pump();
    CHECK(&w.ctx.store() == &w.b);
    CHECK(w.sh->current() == bGeneral);
    // A manual switch and a direct open; then the second signs out: its
    // entries go, back stays in this workspace.
    w.show(kKeyA);
    w.sh->open(aDesign);
    w.sh->purgeHistory(kKeyB);
    const size_t switches = w.switched.size();
    REQUIRE(w.sh->navigateHistory(true));
    CHECK(w.sh->current() == aGeneral);
    CHECK(w.switched.size() == switches);
    CHECK(&w.ctx.store() == &w.a);
}

TEST("zen mode: each workspace keeps its own") {
    TwoWorkspaces         w;
    shell::SidebarFooter &f = w.sh->sidebar().footer();
    REQUIRE(!f.zenOn());
    f.toggleZen();
    CHECK(w.settings.zenMode(kKeyA));
    CHECK_FALSE(w.settings.zenMode(kKeyB));
    w.show(kKeyB);
    CHECK_FALSE(f.zenOn());
    w.show(kKeyA);
    CHECK(f.zenOn());
}

TEST("header: names, presence, DND and my phantom state follow the Store") {
    TwoWorkspaces w;
    Store        &st   = w.a;
    const ConvRef dm   = st.findConversation("D0MIRA");
    const UserRef mira = st.findUser("U0MIRA");
    w.fa.presenceAsked.clear();
    w.sh->open(dm);
    pump();
    // The peer's presence is asked for on open.
    REQUIRE(w.fa.presenceAsked.size() == 1);
    CHECK(w.fa.presenceAsked[0] == mira);
    using P              = shell::Avatar::Presence;
    st.user(mira).active = true;
    st.usersChanged();
    pump();
    CHECK(w.sh->header().avatarPresence() == int(P::Active));
    st.user(mira).active = false;
    st.usersChanged();
    pump();
    CHECK(w.sh->header().avatarPresence() == int(P::Away));
    st.user(mira).dnd = true;
    st.usersChanged();
    pump();
    CHECK(w.sh->header().avatarPresence() == int(P::Dnd));
    st.user(mira).dnd         = false;
    st.user(mira).unavailable = true; // an unreachable peer is phantom too
    st.usersChanged();
    pump();
    CHECK(w.sh->header().avatarPresence() == int(P::Phantom));
    // A name that changes later: the title and "Message …" follow.
    st.user(mira).displayName = "Mira O.";
    st.usersChanged();
    pump();
    CHECK_STR(w.sh->header().title()->text(), st.displayName(dm));
    CHECK(w.sh->header().title()->text().find("Mira O.") != std::string::npos);
    CHECK(w.sh->composer().edit().accessibleName().find("Mira O.") != std::string::npos);
}

#ifdef MSGA_SELF_UPDATE
TEST("updates: opening chats leaves the update check and its events alone") {
    Harness         h;
    net::Client     client(app().platform());
    update::Updater u(app().platform(), client, 1);
    u.setUrls("http://127.0.0.1:9/msga.manifest", "http://127.0.0.1:9/msga");
    h.sh->setUpdater(&u);
    // Every open stamps a visit (Sidebar::onVisitedChanged).
    h.sh->open(h.conv("C0GENERAL"));
    h.sh->open(h.conv("D0MIRA"));
    pump();
    REQUIRE(!h.sh->errorBanner()->visible());
    u.checkNow(); // nothing listens on 127.0.0.1:9: Failed reaches the banner
    for (int i = 0; i < 400 && !h.sh->errorBanner()->visible(); ++i)
        app().pump(5);
    CHECK(h.sh->errorBanner()->visible());
    CHECK_FALSE(h.sh->errorBanner()->text().empty());
    h.sh->setUpdater(nullptr);
}
#endif

TEST("window: fit to screen — shrink to the work area, pull back, refit (issue #45)") {
    Harness       h;
    plat::Window &w = h.win->native();
    // Headless: monitor 1 is 1920×1080 with a 1040-high work area at 0,0;
    // monitor 2 is 1280×720 at 1920,0.
    w.setSize({3000, 2000});
    w.setPosition({100, 100});
    pump();
    plat::Event e;
    e.type = plat::EventType::MonitorsChanged;
    h.sh->handleAppEvent(e);
    pump();
    CHECK(w.size().w == 1920);
    CHECK(w.size().h == 1040);
    REQUIRE(w.position().has_value());
    CHECK(w.position()->x == 0);
    CHECK(w.position()->y == 0);
    // One that fits stays exactly where it is.
    w.setSize({900, 600});
    w.setPosition({200, 150});
    h.sh->fitToScreen();
    pump();
    CHECK((w.size().w == 900 && w.size().h == 600));
    CHECK((w.position()->x == 200 && w.position()->y == 150));
    // Dragged onto the smaller monitor, hanging off its bottom: pulled back.
    w.setSize({1200, 700});
    w.setPosition({2000, 100});
    pump();
    CHECK((w.size().w == 1200 && w.size().h == 700));
    CHECK(w.position()->x == 2000);
    CHECK(w.position()->y == 20);
    // Maximised: the window system's business.
    w.setMaximized(true);
    w.setPosition({5000, 5000});
    h.sh->fitToScreen();
    CHECK(w.position()->x == 5000);
    w.setMaximized(false);
}

TEST("tray: on macOS the plane is a template the menu bar tints") {
    Harness h;
    pump();
    REQUIRE(h.sh->tray() != nullptr);
    plat::TestHooks::TrayProbe p;
    REQUIRE(app().platform().testHooks()->trayProbe(*h.sh->tray(), &p));
    CHECK(!p.iconSizes.empty());
#ifdef __APPLE__
    CHECK(p.isTemplate);
#else
    CHECK_FALSE(p.isTemplate); // the coloured plane and dot elsewhere
#endif
}

TEST("workspaces: the quick switcher has a tab per workspace; ←/→ and typing move it") {
    TwoWorkspaces w;
    w.sh->showQuickSwitcher();
    pump();
    shell::QuickSwitcher *qs = w.sh->quickSwitcher();
    REQUIRE(qs);
    CHECK(qs->tab() == 0); // the open one's
    CHECK(showsText(
        w.win->topPopup(),
        "\xE2\x86\x91\xE2\x86\x93 to move \xC2\xB7 \xE2\x86\x90\xE2\x86\x92 to switch workspace "
        "\xC2\xB7 " +
            shell::shortcuts::nativeKeys(shell::shortcuts::Id::SendMessage) + " to open"
    ));
    press(*w.win, plat::Key::Right);
    CHECK(qs->tab() == 1);
    press(*w.win, plat::Key::Left);
    CHECK(qs->tab() == 0);
    press(*w.win, plat::Key::Tab); // Tab too, not the next focus
    CHECK(qs->tab() == 1);
    press(*w.win, plat::Key::Tab); // wrapping
    CHECK(qs->tab() == 0);
    // Only the second workspace has it: the tab follows the query.
    qs->field().insertText("second-des");
    CHECK(qs->tab() == 1);
    REQUIRE(qs->results().size() == 1);
    // Back on the first by hand: nothing here, the others have some.
    press(*w.win, plat::Key::Left);
    CHECK(qs->tab() == 0);
    CHECK(qs->results().empty());
    CHECK_STR(qs->emptyText(), "No matches in Lumen Studio. Other workspaces have some.");
    qs->field().insertText("zzz");
    CHECK_STR(qs->emptyText(), "No conversations match.");
    // A pick in another workspace's tab switches there and opens it.
    qs->field().setText("second-des"); // runs dry here: the tab moves on
    CHECK(qs->tab() == 1);
    REQUIRE(qs->results().size() == 1);
    qs->choose(0);
    pump();
    REQUIRE(w.switched.size() == 1);
    CHECK_STR(w.switched[0], kKeyB);
    CHECK(w.sh->current() == w.b.findConversation("C0DESIGN"));
}

TEST("workspaces: Workspace admin for a background workspace you administer") {
    TwoWorkspaces w;
    w.b.user(w.b.me).admin = true;
    w.b.workspaceUrl       = "https://second.slack.com/";
    std::string opened;
    w.ctx.openUrl             = [&](const std::string &u) { opened = u; };
    shell::Menus           &m = w.sh->menus();
    shell::Menus::Workspace bg{kKeyB, "Second Co", false, false, &w.b};
    REQUIRE(labels(m.workspaceItems(bg)).rfind("Workspace admin|", 0) == 0);
    m.showWorkspace(bg, {100, 100});
    pump();
    m.run(shell::Menus::kWorkspaceAdmin, 0);
    CHECK_STR(opened, "https://second.slack.com/admin/settings");
    // Not running (no Store): no item.
    CHECK(
        labels(m.workspaceItems({kKeyB, "Second Co", false, false})).rfind("Change icon", 0) == 0
    );
    w.win->closeAllPopups();
}

TEST("forward: portable text for another workspace — mentions become words") {
    TwoWorkspaces w;
    CHECK_STR(
        shell::portableMrkdwn(
            w.a,
            "<@U0MIRA> see <#C0DESIGN|design> and <!here>, <!subteam^S1|@devs>: "
            "*bold* <https://x.test|link> :tada: <!date^1^{date}|Sep 1>"
        ),
        "@" + std::string(w.a.user(w.a.findUser("U0MIRA")).label()) +
            " see #design and @here, @devs: *bold* <https://x.test|link> :tada: Sep 1"
    );
    CHECK_STR(shell::portableMrkdwn(w.a, "<@U0NOBODY|ghost> &lt;b&gt;"), "@ghost &lt;b&gt;");
}

TEST("forward: with two workspaces a picker sends it into the other one") {
    TwoWorkspaces w;
    const ConvRef design = w.a.findConversation("C0DESIGN");
    Message       m;
    m.ts   = w.a.conversation(design).latest + 1000;
    m.user = w.a.me;
    m.text = "ask <@U0MIRA>";
    w.a.addMessage(design, std::move(m));
    const Ts ts = w.a.conversation(design).messages.back().ts;
    w.ctx.forwardMessage(design, ts, {});
    pump();
    auto *dlg = static_cast<ui::Dialog *>(w.win->topPopup());
    REQUIRE(dlg != nullptr);
    // A dropdown of workspaces, on the message's own.
    std::function<ui::View *(ui::View *, std::string_view)> named =
        [&](ui::View *v, std::string_view name) -> ui::View * {
        if (v->accessibleName() == name)
            return v;
        for (size_t i = 0; i < v->childCount(); ++i)
            if (ui::View *f = named(v->child(i), name))
                return f;
        return nullptr;
    };
    auto *ws = static_cast<ui::Dropdown *>(named(dlg, "Lumen Studio")); // its selected option
    REQUIRE(ws != nullptr);
    CHECK(ws->options() == std::vector<std::string>({"Lumen Studio", "Second Co"}));
    CHECK(ws->selected() == 0);
    ws->setSelected(1);
    ws->onChange(1);
    pump();
    typeInto(*w.win, "#second-des"); // the other workspace's channels now
    press(*w.win, plat::Key::Enter);
    const ConvRef target = w.b.findConversation("C0DESIGN");
    const size_t  before = w.b.conversation(target).messages.size();
    const size_t  inA    = w.a.conversation(design).messages.size();
    REQUIRE(w.win->topPopup() == dlg);
    auto *fwd = static_cast<ui::Clickable *>(named(dlg, "Forward"));
    REQUIRE(fwd != nullptr);
    REQUIRE(fwd->enabled());
    fwd->onClick();
    pump();
    REQUIRE(w.b.conversation(target).messages.size() == before + 1);
    CHECK_STR(
        w.b.conversation(target).messages.back().text,
        "ask @" + std::string(w.a.user(w.a.findUser("U0MIRA")).label())
    );
    CHECK(w.a.conversation(design).messages.size() == inA); // nothing in the source
}

TEST("workspaces: the presence mode and real input reach every running workspace") {
    TwoWorkspaces w;
    w.settings.presence = 1; // Settings → System → Presence: while using
    w.sh->applySettings();
    CHECK(w.fa.mode == Backend::PresenceMode::WhileUsing);
    CHECK(w.fb.mode == Backend::PresenceMode::WhileUsing); // the background one too
    auto     *hooks = app().platform().testHooks();
    const int a = w.fa.activity, b = w.fb.activity;
    hooks->injectPointerMove(w.win->native(), {600, 400});
    hooks->injectButton(w.win->native(), plat::Button::Left, true);
    hooks->injectButton(w.win->native(), plat::Button::Left, false);
    pump();
    CHECK(w.fa.activity == a + 1);
    CHECK(w.fb.activity == b + 1);
}

// ── Notifications, huddles, reminders ───────────────────────────────────────

TEST("notifications: the titles and gates — level, mutes, replies to unloaded threads") {
    TwoWorkspaces w;
    Store        &a       = w.a;
    const ConvRef general = a.findConversation("C0GENERAL"), dm = a.findConversation("D0MIRA");
    plat::TestHooks::NotificationProbe n;
    uint64_t                           last  = lastNotification(nullptr);
    auto                               fresh = [&] {
        const uint64_t id = lastNotification(&n);
        const bool     is = id > last;
        last              = std::max(last, id);
        return is;
    };
    // A channel: "#name", the sender before the text.
    w.post(a, "C0GENERAL", "the build is green");
    pump();
    REQUIRE(fresh());
    CHECK_STR(n.title, "#general");
    CHECK_STR(n.body, "Mira Okafor: the build is green");
    // A DM: the sender is the title.
    w.post(a, "D0MIRA", "lunch?");
    pump();
    REQUIRE(fresh());
    CHECK_STR(n.title, "Mira Okafor");
    CHECK_STR(n.body, "lunch?");
    // Long text: 97 characters and "…".
    w.post(a, "D0MIRA", std::string(150, 'x').c_str());
    pump();
    REQUIRE(fresh());
    CHECK_STR(n.body, std::string(97, 'x') + "\xE2\x80\xA6");
    // "Direct messages and mentions only": a plain channel post is quiet, a
    // mention and a DM are not.
    w.settings.notifyLevel = 1;
    w.post(a, "C0GENERAL", "plain news");
    pump();
    CHECK(!fresh());
    w.post(a, "C0GENERAL", ("ping <@" + a.user(a.me).id + ">").c_str());
    pump();
    CHECK(fresh());
    w.post(a, "D0MIRA", "still here");
    pump();
    CHECK(fresh());
    // A muted conversation says nothing, a mention in it neither.
    a.updateConversation(general, [](Conversation &c) { c.muted = true; });
    w.post(a, "C0GENERAL", ("ping <@" + a.user(a.me).id + ">").c_str());
    pump();
    CHECK(!fresh());
    a.updateConversation(general, [](Conversation &c) { c.muted = false; });
    // A reply to a thread that isn't loaded (Store::announceReply): only
    // when it matters on this level — a thread I follow.
    Message reply;
    reply.ts       = a.conversation(general).latest + 5000;
    reply.threadTs = a.conversation(general).latest;
    reply.user     = a.findUser("U0MIRA");
    reply.text     = "thread news";
    a.announceReply(general, reply);
    pump();
    CHECK(!fresh());
    w.fa.followedRoot = reply.threadTs;
    a.announceReply(general, reply);
    pump();
    REQUIRE(fresh());
    CHECK_STR(n.body, "Mira Okafor: thread news");
    // Its click opens the conversation with that thread.
    plat::Event e;
    e.type = plat::EventType::NotificationActivated;
    e.id   = last;
    w.sh->handleAppEvent(e);
    pump();
    CHECK(w.sh->current() == general);
    CHECK(w.sh->threadOpen());
    (void)dm;
}

TEST("notifications: a sender's picture not decoded yet is waited for, not skipped") {
    TwoWorkspaces w;
    Store        &a = w.a;
    REQUIRE(!a.user(a.findUser("U0MIRA")).avatar.empty()); // the fixture's mira.png
    const uint64_t before = lastNotification(nullptr);
    w.post(a, "D0MIRA", "lunch?");
    plat::TestHooks::NotificationProbe n;
    REQUIRE(until([&] { return lastNotification(&n) > before; }));
    CHECK_STR(n.title, "Mira Okafor");
    CHECK(n.imageSize.w == 64 && n.imageSize.h == 64);
}

TEST("huddles: the start notifies once with Join; the pill and the banner follow the room") {
    TwoWorkspaces w;
    std::string   opened;
    w.ctx.openUrl          = [&](const std::string &u) { opened = u; };
    w.fa.huddles           = true;
    Store         &a       = w.a;
    const ConvRef  general = a.findConversation("C0GENERAL");
    const UserRef  mira    = a.findUser("U0MIRA");
    const uint64_t before  = lastNotification(nullptr);
    a.updateConversation(general, [&](Conversation &c) {
        c.huddleActive       = true;
        c.huddleLink         = "https://app.slack.com/huddle/T0LUMEN/C0GENERAL";
        c.huddleParticipants = {mira};
    });
    pump();
    plat::TestHooks::NotificationProbe n;
    const uint64_t                     id = lastNotification(&n);
    REQUIRE(id > before);
    CHECK_STR(n.title, "#general");
    CHECK_STR(n.body, "Mira Okafor started a huddle");
    REQUIRE(n.actionLabels.size() == 1);
    CHECK_STR(n.actionLabels[0], "Join");
    // The same huddle again (any Meta change) says nothing more.
    a.updateConversation(general, [&](Conversation &c) { c.topic = "x"; });
    pump();
    CHECK(lastNotification(nullptr) == id);
    // "Join" opens the room's link.
    plat::Event e;
    e.type   = plat::EventType::NotificationActivated;
    e.id     = id;
    e.action = "join";
    w.sh->handleAppEvent(e);
    CHECK_STR(opened, "https://app.slack.com/huddle/T0LUMEN/C0GENERAL");
    // The sidebar's pill: one participant; its click joins too.
    CHECK(w.sh->sidebar().rowState(general).huddle == 1);
    opened.clear();
    CHECK(w.sh->sidebar().joinHuddle(general));
    CHECK_STR(opened, "https://app.slack.com/huddle/T0LUMEN/C0GENERAL");
    // The banner while it is open, until the huddle ends.
    CHECK(!w.sh->huddleBanner()->visible());
    w.sh->open(general);
    pump();
    CHECK(w.sh->huddleBanner()->visible());
    a.updateConversation(general, [](Conversation &c) { c.huddleActive = false; });
    pump();
    CHECK(!w.sh->huddleBanner()->visible());
    CHECK(w.sh->sidebar().rowState(general).huddle == 0);
    // One I'm in never notifies; "Notify me when a huddle starts" off neither.
    const uint64_t quiet = lastNotification(nullptr);
    a.updateConversation(a.findConversation("D0MIRA"), [&](Conversation &c) {
        c.huddleActive       = true;
        c.huddleParticipants = {mira, a.me};
    });
    w.settings.notifyHuddles = false;
    a.updateConversation(a.findConversation("C0DESIGN"), [&](Conversation &c) {
        c.huddleActive       = true;
        c.huddleParticipants = {mira};
    });
    pump();
    CHECK(lastNotification(nullptr) == quiet);
}

TEST("reminders: the due notification names the place; its click opens the message") {
    TwoWorkspaces w;
    Store        &a       = w.a;
    const ConvRef general = a.findConversation("C0GENERAL");
    w.sh->open(a.findConversation("C0DESIGN"));
    pump();
    const Message &m = a.conversation(general).messages.front();
    a.setSavedItem(general, m.ts, true, 1);
    a.setSavedPreview(general, m.ts, &m);
    const uint64_t before = lastNotification(nullptr);
    w.sh->notifyReminderDue(kKeyA, a, general, m.ts);
    // Posted once the author's picture is decoded (the worker's job).
    plat::TestHooks::NotificationProbe n;
    REQUIRE(until([&] { return lastNotification(nullptr) > before; }));
    const uint64_t id = lastNotification(&n);
    CHECK(n.imageSize.w == 64); // the author's picture
    CHECK_STR(n.title, "Reminder \xE2\x80\x94 #general");
    CHECK(n.body.rfind(std::string(a.user(m.user).label()) + ": ", 0) == 0);
    plat::Event e;
    e.type = plat::EventType::NotificationActivated;
    e.id   = id;
    w.sh->handleAppEvent(e);
    pump();
    CHECK(w.sh->current() == general);
    // Without a preview: the stand-in sentence.
    a.setSavedItem(general, m.ts, false);
    a.setSavedItem(general, m.ts + 1, true, 1);
    w.sh->notifyReminderDue(kKeyA, a, general, m.ts + 1);
    REQUIRE(until([&] { return lastNotification(nullptr) > id; }));
    lastNotification(&n);
    CHECK_STR(n.body, "You asked to be reminded about a message.");
}

TEST("sidebar: the Threads entry is bright while followed threads have unread replies") {
    TwoWorkspaces w;
    CHECK(!w.sh->sidebar().threadsUnread());
    w.a.setUnreadThreads(2);
    pump();
    CHECK(w.sh->sidebar().threadsUnread());
    w.a.setUnreadThreads(0);
    pump();
    CHECK(!w.sh->sidebar().threadsUnread());
}

TEST("workspace icon: a picture that can't be saved keeps the dialog open and says so") {
    Harness           h;
    Files             f;
    const std::string pic = f.at("icon.png");
    std::string       bytes;
    REQUIRE(file::readAll(MSGA_TEST_ASSETS "/images/blog-hero.png", &bytes));
    REQUIRE(file::writeAtomic(pic, bytes));
    bool       saved = false;
    ui::Popup *d =
        shell::showWorkspaceIconDialog(h.ctx, *h.win, h.sh->avatars(), [&](const std::string &) {
            saved = true;
        });
    pump();
    const ui::PointF mid{h.win->size().w / 2, h.win->size().h / 2};
    h.win->handle(dropEvent(*h.win, plat::EventType::DropEnter, mid, {}));
    h.win->handle(dropEvent(*h.win, plat::EventType::Drop, mid, {base::test::fileUri(pic)}));
    // The picture is checked by decoding it on the worker.
    REQUIRE(until([&] { return h.ctx.images.pending() == 0; }));
    pump();
    std::function<ui::View *(ui::View *)> saveBtn = [&](ui::View *v) -> ui::View * {
        if (v->accessibleName() == "Save")
            return v;
        for (size_t i = 0; i < v->childCount(); ++i)
            if (ui::View *b = saveBtn(v->child(i)))
                return b;
        return nullptr;
    };
    auto *save = static_cast<ui::Clickable *>(saveBtn(d));
    REQUIRE(save != nullptr);
    file::remove(pic); // gone before Save: nothing to copy
    save->activate();
    model::waitBackground(); // the copy runs on a worker
    pump();
    CHECK(h.win->topPopup() == d);
    CHECK(showsText(d, "The icon could not be saved."));
    CHECK_FALSE(saved);
    d->close();
    pump();
}

TEST("quit: an app-level quit request (Cmd+Q, the Dock's Quit) quits like the tray's Quit") {
    Harness h;
    int     quits = 0;
    h.sh->onQuit  = [&] { ++quits; };
    plat::Event e;
    e.type = plat::EventType::QuitRequested;
    h.sh->handleAppEvent(e);
    CHECK(quits == 1);
}

TEST("title bar: controls take their presses, empty space drags the window") {
    Harness h;
    using H                   = plat::HitArea;
    shell::ConvHeader &hd     = h.sh->header();
    const ui::PointF   search = centre(hd.search());
    const auto         at = [&](ui::PointF p) { return h.sh->hitTest({double(p.x), double(p.y)}); };
#ifdef __APPLE__
    // The unified header: the conversation header is the title bar's
    // content, its title centred in the window.
    CHECK(search.y < 40);
    CHECK(at(search) == H::Client);
    CHECK(at(centre(hd.title())) == H::Caption);
    const ui::RectF t = hd.title()->windowRect();
    CHECK(std::abs(t.x + t.w / 2 - 600) < 1);
    CHECK(at({400, 20}) == H::Caption); // empty header space
    CHECK(at({600, 2}) == H::Caption);  // its edges too: AppKit resizes from its own zone
    CHECK(at({2, 20}) == H::Caption);
    CHECK(at({2, 300}) == H::ResizeLeft);
    // Settings blocks the header's actions; its backdrop still drags.
    h.sh->openSettings();
    pump();
    CHECK_FALSE(hd.search()->enabled());
    CHECK(at(search) == H::Caption);
    h.sh->settingsDialog()->close();
    pump();
    CHECK(hd.search()->enabled());
#else
    // The 22 px strip above the header: caption, but for its own buttons.
    CHECK(search.y > 22);
    CHECK(at(search) == H::Client);
    CHECK(at({400, 11}) == H::Caption);
#endif
}

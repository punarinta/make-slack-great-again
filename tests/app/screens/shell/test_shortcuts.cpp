// The keyboard (app/screens/shell/shortcuts.h), driven
// through plat's TestHooks key injection on the headless backend: the table
// itself, every window-scope shortcut, the composer's formatting and send
// keys, and bindings that must not exist.
#include "app/fake/fake_backend.h"
#include "base/file.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/testing.h"
#include "base/utf8.h"
#include "screens/common/message_text.h"
#include "screens/shell/composer.h"
#include "screens/shell/header.h"
#include "screens/shell/message_search.h"
#include "screens/shell/quick_switcher.h"
#include "screens/shell/shell.h"
#include "screens/shell/shortcuts.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/emoji_picker.h"
#include "app/screens/messages/image_cache.h"
#endif

#include <cstring>
#include <memory>

using namespace model;
using shell::shortcuts::Id;
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

plat::TestHooks &hooks() {
    return *app().platform().testHooks();
}

void pump(int n = 10) {
    for (int i = 0; i < n; ++i)
        app().pump(2);
}

// A fake backend where one DM is a Claude Code session.
struct AgentFake : fake::FakeBackend {
    using fake::FakeBackend::FakeBackend;
    bool    isAgentSession(ConvRef c) const override { return c == agent; }
    ConvRef agent = kNoConv;
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
    int                           closeRequests = 0;

    Harness() {
        backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
        bool done = false;
        backend.connect([&](bool ok, const std::string &) { done = ok; });
        for (int i = 0; i < 200 && !done; ++i)
            app().pump(5);
        plat::WindowDesc d;
        d.size                = {1200, 800};
        d.decorations         = plat::Decorations::Custom;
        win                   = std::make_unique<ui::Window>(d);
        win->onCloseRequested = [this] { ++closeRequests; };
        sh                    = std::make_unique<shell::Shell>(ctx, *win, settings, std::string());
        sh->open(store.findConversation("C0DESIGN"));
        pump();
    }
    ~Harness() { shell::shortcuts::setCtrlEnterSends(false); }
    ConvRef conv(const char *id) const { return store.findConversation(id); }

    // A key with portable modifiers (shortcuts::Ctrl = the primary key).
    void chord(uint8_t mods, K k) {
        const K       ctrl   = plat::primaryMod() == plat::ModSuper ? K::SuperLeft : K::ControlLeft;
        const K       held[] = {ctrl, K::AltLeft, K::ShiftLeft};
        const uint8_t bit[]  = {
            shell::shortcuts::Ctrl, shell::shortcuts::Alt, shell::shortcuts::Shift
        };
        for (int i = 0; i < 3; ++i)
            if (mods & bit[i])
                hooks().injectKey(win->native(), held[i], true);
        hooks().injectKey(win->native(), k, true);
        hooks().injectKey(win->native(), k, false);
        for (int i = 3; i-- > 0;)
            if (mods & bit[i])
                hooks().injectKey(win->native(), held[i], false);
        pump(10);
    }
    void             key(K k) { chord(0, k); }
    shell::Composer &composer() { return sh->composer(); }
    ui::TextEdit    &edit() { return sh->composer().edit(); }
};

constexpr uint8_t kCtrl = shell::shortcuts::Ctrl, kShift = shell::shortcuts::Shift,
                  kAlt = shell::shortcuts::Alt;

} // namespace

// ── The table ───────────────────────────────────────────────────────────────

TEST("shortcuts: one row per action, the help panel's rows in order") {
    using namespace shell::shortcuts;
    REQUIRE(tableSize() == size_t(Id::Count));
    for (size_t i = 0; i < size_t(Id::Count); ++i)
        CHECK(def(Id(i)).id == Id(i));
    const char *help[] = {
        "Open settings",
        "Jump to a conversation",
        "Search messages",
        "Send message",
        "New line in message",
        "Edit last message",
        "Bold",
        "Italic",
        "Strikethrough",
        "Inline code",
        "Insert link",
        "Attach file",
        "Emoji picker",
        "Cancel / exit edit",
        "Undo send",
    };
    size_t n = 0;
    for (size_t i = 0; i < tableSize(); ++i)
        if (table()[i].inHelp) {
            REQUIRE(n < std::size(help));
            CHECK_STR(table()[i].label, help[n]);
            ++n;
        }
    CHECK(n == std::size(help));
}

TEST("shortcuts: no two window-scope bindings share a key") {
    using namespace shell::shortcuts;
    std::vector<Keys> seen;
    for (size_t i = 0; i < tableSize(); ++i) {
        if (table()[i].scope != Scope::Window)
            continue;
        Keys         b[2];
        const size_t n = bindings(table()[i].id, b);
        CHECK(n >= 1);
        for (size_t j = 0; j < n; ++j) {
            for (const Keys &s : seen)
                CHECK_FALSE(s.key == b[j].key && s.mods == b[j].mods);
            seen.push_back(b[j]);
        }
    }
}

TEST("shortcuts: tooltips and chips spell the keys per platform") {
    using namespace shell::shortcuts;
#ifdef __APPLE__
    CHECK_STR(
        tip("Bold", Id::Bold),
        "Bold (\xE2\x8C\x98"
        "B)"
    );
#else
    CHECK_STR(tip("Bold", Id::Bold), "Bold (Ctrl+B)");
    CHECK_STR(nativeKeys(Id::CodeBlock), "Ctrl+Alt+Shift+C");
    CHECK_STR(nativeKeys(Id::EmojiPicker), "Ctrl+Shift+\\");
    CHECK_STR(nativeKeys(Id::OpenSettings), "Ctrl+,");
    CHECK_STR(nativeKeys(Id::SearchMessages), "Ctrl+F");
    CHECK_STR(nativeKeys(Id::RemoveIdleSession), "Shift+Del");
    CHECK_STR(nativeKeys(Id::VoiceInput), "Ctrl+Shift+Space");
#endif
    CHECK_STR(nativeKeys(Id::EditLastMessage), "\xE2\x86\x91");
    CHECK_STR(nativeKeys(Id::CancelOrExitEdit), "Esc");
    CHECK_STR(nativeKeys(Id::SendMessage), "Enter");
    // The Ctrl+Enter option swaps send and newline.
    setCtrlEnterSends(true);
    const auto chips = keyChips(Id::SendMessage);
    REQUIRE(chips.size() == 2);
    CHECK_STR(chips[1], "Enter");
    CHECK_STR(keyChips(Id::NewLine)[0], "Enter");
    setCtrlEnterSends(false);
    CHECK(keyChips(Id::NewLine).size() == 2); // Shift + Enter
    // Close is the platform's own close key set, which differs per platform.
    Keys         b[2];
    const size_t n = bindings(Id::CloseFrontmost, b);
#if defined(_WIN32)
    CHECK(n == 2 && b[0].key == K::F4 && b[1].key == K::W);
#elif defined(__APPLE__)
    CHECK(n == 2 && b[0].key == K::W && b[1].key == K::F4);
#else
    CHECK(n == 1 && b[0].key == K::W && b[0].mods == Ctrl);
#endif
}

// ── Window scope ────────────────────────────────────────────────────────────

TEST("shortcuts: Ctrl+K opens the switcher once, Enter opens the pick, Ctrl+W closes it") {
    Harness h;
    REQUIRE(h.edit().focused()); // the composer holds focus
    h.chord(kCtrl, K::K);
    auto *sw = h.sh->quickSwitcher();
    REQUIRE(sw != nullptr);
    CHECK(h.win->topPopup() == sw);
    h.chord(kCtrl, K::K); // not over itself
    CHECK(h.sh->quickSwitcher() == sw);
    // Ctrl+B in its field is no formatting key.
    h.chord(kCtrl, K::B);
    CHECK_FALSE(sw->field().formatActive(ui::TextEdit::Bold));
    h.chord(0, K::E); // headless types US text for letters
    h.chord(0, K::N);
    h.chord(0, K::G);
    REQUIRE(!sw->results().empty());
    const ConvRef want = sw->results()[0];
    h.chord(kShift, K::Enter); // any modifiers
    pump();
    CHECK(h.sh->current() == want);
    CHECK(h.sh->quickSwitcher() == nullptr);

    h.chord(kCtrl, K::K);
    REQUIRE(h.sh->quickSwitcher() != nullptr);
    h.chord(kCtrl, K::W);
    pump();
    CHECK(h.sh->quickSwitcher() == nullptr);
    CHECK(h.closeRequests == 0); // the dialog went, not the window
}

TEST("shortcuts: Ctrl+, opens Settings, Ctrl+W closes it, then the window") {
    Harness h;
    h.chord(kCtrl, K::Comma);
    REQUIRE(h.sh->settingsDialog() != nullptr);
    h.chord(kCtrl, K::Comma); // already open: nothing new
    h.chord(kCtrl, K::W);
    pump();
    CHECK(h.sh->settingsDialog() == nullptr);
    CHECK(h.closeRequests == 0);
    h.chord(kCtrl, K::W);
    CHECK(h.closeRequests == 1); // nothing open: the window's close (hide to tray)
}

TEST("shortcuts: Ctrl+F toggles the message search over the chat, the composer left alone") {
    Harness h;
    h.edit().insertText("draft");
    h.chord(kCtrl, K::F);
    shell::MessageSearch *s = h.sh->messageSearch();
    REQUIRE(s != nullptr);
    CHECK(s->shown());
    CHECK(h.win->topPopup() == nullptr); // an overlay in the window, not a popup
    CHECK(s->field().focused());
    CHECK_STR(h.edit().text(), "draft");
    CHECK(h.closeRequests == 0);
    CHECK_FALSE(s->list()->visible()); // no results before a search
    h.chord(kCtrl, K::F);              // Ctrl+F again hides it at once
    CHECK_FALSE(s->visible());
    CHECK(h.edit().focused());
    // The header's search button is the same toggle.
    h.sh->header().search()->onClick();
    pump();
    CHECK(s->shown());
    h.sh->header().search()->onClick();
    CHECK_FALSE(s->visible());
}

TEST("search: Enter searches, the arrows pick a result, Enter opens it and closes; Esc fades out") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN"), mira = h.conv("D0MIRA");
    h.sh->open(mira);
    pump();
    h.chord(kCtrl, K::F);
    shell::MessageSearch *s = h.sh->messageSearch();
    REQUIRE(s->shown());
    s->field().setText("  proposal b ");
    h.key(K::Enter);
    CHECK(s->list()->visible());
    CHECK_STR(s->statusText(), "Searching\xE2\x80\xA6");
    for (int i = 0; i < 1000 && s->statusText() == "Searching\xE2\x80\xA6"; ++i)
        pump(5);
    REQUIRE(!s->results().empty());
    CHECK(s->results()[0].conv == design);
    CHECK(s->selected() == -1);
    h.key(K::Up); // from nothing selected, Up takes the last one
    CHECK(s->selected() == int(s->results().size()) - 1);
    h.key(K::Down); // clamped, not wrapping
    CHECK(s->selected() == int(s->results().size()) - 1);
    for (size_t i = 0; i < s->results().size(); ++i)
        h.key(K::Up);
    CHECK(s->selected() == 0);
    h.key(K::Enter);
    CHECK(h.sh->current() == design);
    CHECK_FALSE(s->visible()); // opening another conversation hides it
    // Nothing matches: the status line.
    h.chord(kCtrl, K::F);
    REQUIRE(s->shown());
    CHECK_STR(s->field().text(), "  proposal b "); // kept, and selected
    CHECK(s->selected() == 0);                     // so is the pick: Enter would open it again
    s->runSearch("no such words anywhere");
    for (int i = 0; i < 1000 && s->statusText() == "Searching\xE2\x80\xA6"; ++i)
        pump(5);
    CHECK_STR(s->statusText(), "No results found.");
    CHECK(s->results().empty());
    h.key(K::Escape);
    CHECK_FALSE(s->shown()); // fading out
    for (int i = 0; i < 1000 && s->visible(); ++i)
        pump(5);
    CHECK_FALSE(s->visible());
    CHECK(h.edit().focused());
}

TEST("search: a user resolving later renames the rows in place; the pick stays") {
    Harness h;
    h.chord(kCtrl, K::F);
    shell::MessageSearch *s = h.sh->messageSearch();
    REQUIRE(s->shown());
    s->runSearch("proposal b");
    for (int i = 0; i < 1000 && s->statusText() == "Searching\xE2\x80\xA6"; ++i)
        pump(5);
    REQUIRE(!s->results().empty());
    h.key(K::Up);
    const int sel = s->selected();
    REQUIRE(sel >= 0);
    h.store.usersChanged();
    h.store.usersChanged(); // a burst: one pass over the rows
    pump();
    CHECK(s->selected() == sel);
    CHECK(s->results().size() > 0);
}

TEST("search: a result's labels") {
    Harness             h;
    const model::Store &st = h.store;
    CHECK_STR(shell::searchConvLabel(st, h.conv("C0DESIGN")), "#design");
    CHECK_STR(shell::searchConvLabel(st, h.conv("D0MIRA")), st.displayName(h.conv("D0MIRA")));
    CHECK_STR(shell::searchConvLabel(st, model::kNoConv), "Unknown channel");
    const std::string mira(st.user(st.findUser("U0MIRA")).label());
    CHECK_STR(
        shell::searchPreview(st, "ping <@U0MIRA>\nand <@U9NOBODY> *now*"),
        "ping @" + mira + " and @U9NOBODY now"
    );
    CHECK(utf8::countCodePoints(shell::searchPreview(st, std::string(300, 'x'))) == 120);
}

TEST("search: previews and notifications resolve mentions the same way") {
    model::Store st;
    model::User  u;
    u.id = "U0ANNA001", u.name = "anna";
    st.addUser(std::move(u));
    model::Conversation c;
    c.id = "C0GEN0001", c.name = "general", c.kind = model::ConvKind::Channel;
    st.addConversation(std::move(c));
    st.setChannelName("C0OTHER01", "elsewhere");
    st.setUsergroups({{"S0DEV0001", "devs", "Developers", {}}});
    const std::string text = "<@U0ANNA001> in <#C0GEN0001> and <#C0OTHER01>, <!subteam^S0DEV0001> "
                             ":smile: <@U9NOBODY9> <#C9NOWHERE>";
    const std::string want = "@anna in #general and #elsewhere, @devs \xF0\x9F\x98\x84 ";
    const std::string got  = screens::plainText(st, text);
    CHECK(got.rfind(want, 0) == 0);
    // Unknown ones keep the parser's text.
    CHECK(got.find("U9NOBODY9") != std::string::npos);
    CHECK(got.find("C9NOWHERE") != std::string::npos);
    // The search preview is the same text, newlines as spaces.
    CHECK_STR(shell::searchPreview(st, "<#C0GEN0001>\n:smile:"), "#general \xF0\x9F\x98\x84");
}

TEST("search: firstLink finds a URL or a message permalink") {
    CHECK_STR(screens::firstLink("no links here"), "");
    CHECK_STR(
        screens::firstLink("see <https://example.com/a|this> and https://b.example"),
        "https://example.com/a"
    );
    // A permalink to another message is a link too.
    const std::string pl =
        screens::firstLink("look: https://acme.slack.com/archives/C0GEN0001/p1700000000000100");
    CHECK(pl.rfind("https://acme.slack.com/archives/C0GEN0001/p1700000000000100", 0) == 0);
    CHECK_STR(screens::fileUrl("https://x.example/f"), "https://x.example/f");
    CHECK(screens::fileUrl("/tmp/a b.txt").rfind("file://", 0) == 0);
}

TEST("shortcuts: Alt+Left/Right and Back/Forward walk the conversations opened") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN"), eng = h.conv("C0ENG"), gen = h.conv("C0GENERAL");
    h.sh->open(eng);
    h.sh->open(gen);
#ifdef __APPLE__
    // Option+arrows move by word, as in any Mac text field: from outside the
    // composer,
    // which every open focuses again.
    const auto alt = [&](K k) {
        h.win->setFocus(nullptr);
        h.chord(kAlt, k);
    };
#else
    REQUIRE(h.edit().focused()); // from the composer: TextEdit leaves Alt+arrows alone
    const auto alt = [&](K k) { h.chord(kAlt, k); };
#endif
    alt(K::Left);
    CHECK(h.sh->current() == eng);
    alt(K::Left);
    CHECK(h.sh->current() == design);
    alt(K::Left); // nothing further back
    CHECK(h.sh->current() == design);
    alt(K::Right);
    CHECK(h.sh->current() == eng);
    h.key(K::Forward);
    CHECK(h.sh->current() == gen);
    h.key(K::Back);
    CHECK(h.sh->current() == eng);
    // A direct open discards the forward stack (editor undo/redo).
    const ConvRef dm = h.conv("D0MIRA");
    h.sh->open(dm);
    alt(K::Right);
    CHECK(h.sh->current() == dm);
    alt(K::Left);
    CHECK(h.sh->current() == eng);
    // Threads are not entries: opening one in the same conversation adds none.
    const Ts root = h.backend.findTs(design, "Proposal B");
    REQUIRE(root != 0);
    h.sh->openThread(design, root);
    h.sh->closeThread();
    alt(K::Left);
    CHECK(h.sh->current() == eng);
}

TEST("navigation: the mouse's side buttons walk the history") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN"), eng = h.conv("C0ENG");
    h.sh->open(eng);
    const auto click = [&](plat::Button b) {
        hooks().injectPointerMove(h.win->native(), {600, 400});
        hooks().injectButton(h.win->native(), b, true);
        hooks().injectButton(h.win->native(), b, false);
        pump();
    };
    click(plat::Button::Back);
    CHECK(h.sh->current() == design);
    click(plat::Button::Forward);
    CHECK(h.sh->current() == eng);
    CHECK(!h.sh->swipeBadge().running()); // the badge is the swipes' only
}

TEST("navigation: a horizontal trackpad swipe goes back or forward once") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN"), eng = h.conv("C0ENG"), gen = h.conv("C0GENERAL");
    h.sh->open(eng);
    h.sh->open(gen);
    using P          = plat::ScrollPhase;
    const auto swipe = [&](double dx, double dy) {
        hooks().injectPointerMove(h.win->native(), {600, 400});
        hooks().injectPhasedScroll(h.win->native(), 0, 0, P::Begin);
        for (int i = 0; i < 10; ++i) // 300 px in all: one step, not two
            hooks().injectPhasedScroll(h.win->native(), dx / 10, dy / 10, P::Update);
        hooks().injectPhasedScroll(h.win->native(), 0, 0, P::End);
        pump();
    };
    // Fingers to the right drag the content right (plat's dx < 0): back.
    swipe(-300, 0);
    CHECK(h.sh->current() == eng);
    CHECK(h.sh->swipeBadge().running());
    swipe(-300, 0);
    CHECK(h.sh->current() == design);
    swipe(300, 0);
    CHECK(h.sh->current() == eng);
    // The badge hides itself in its last tick, and plays again on the next
    // swipe (it used to crash the frame loop, then never tick again).
    const auto runOut = [&] {
        const int64_t until = base::monotonicMs() + shell::SwipeIndicator::kDurationMs + 300;
        while (h.sh->swipeBadge().running() && base::monotonicMs() < until)
            app().pump(5);
    };
    runOut();
    CHECK(!h.sh->swipeBadge().running());
    CHECK(!h.sh->swipeBadge().visible());
    swipe(-300, 0);
    CHECK(h.sh->current() == design);
    CHECK(h.sh->swipeBadge().visible());
    runOut();
    CHECK(!h.sh->swipeBadge().running());
    swipe(300, 0);
    CHECK(h.sh->current() == eng);
    // A scroll through the messages is not a swipe, nor a short drift.
    swipe(-100, 300);
    CHECK(h.sh->current() == eng);
    swipe(-60, 0);
    CHECK(h.sh->current() == eng);
    // Three-finger swipes (X11/Wayland gestures): finger motion, +x = back.
    hooks().injectGesture(h.win->native(), plat::Gesture::Swipe, 3, 200, 0);
    pump();
    CHECK(h.sh->current() == design);
    hooks().injectGesture(h.win->native(), plat::Gesture::Swipe, 3, -200, 10);
    pump();
    CHECK(h.sh->current() == eng);
}

TEST("navigation: the swipe recognizer, stream by stream") {
    using A           = shell::SwipeNav::Action;
    using T           = plat::EventType;
    using P           = plat::ScrollPhase;
    const auto scroll = [](double dx, double dy, P ph, bool precise = true, uint32_t mods = 0) {
        plat::Event e{.type = T::Scroll};
        e.dx      = dx;
        e.dy      = dy;
        e.phase   = ph;
        e.precise = precise;
        e.mods    = mods;
        return e;
    };
    {
        // Phase-less pixel deltas (Windows, X11): fires once, swallows the
        // rest, and an idle gap starts the next swipe.
        shell::SwipeNav n;
        CHECK(n.feed(scroll(-60, 0, P::None), 0) == A::Pass);
        CHECK(n.feed(scroll(-60, 0, P::None), 10) == A::Back);
        CHECK(n.feed(scroll(-60, 0, P::None), 20) == A::Swallow);
        CHECK(n.feed(scroll(150, 0, P::None), 400) == A::Forward);
    }
    {
        // Tilt-wheel notches.
        shell::SwipeNav n;
        CHECK(n.feed(scroll(1, 0, P::None, false), 0) == A::Pass);
        CHECK(n.feed(scroll(2, 0, P::None, false), 10) == A::Forward);
    }
    {
        // Momentum after the lift is not the swipe; after a jump the end
        // still reaches the list.
        shell::SwipeNav n;
        n.feed(scroll(0, 0, P::Begin), 0);
        CHECK(n.feed(scroll(-50, 0, P::Update), 1) == A::Pass);
        CHECK(n.feed(scroll(0, 0, P::End), 2) == A::Pass);
        CHECK(n.feed(scroll(-200, 0, P::Momentum), 3) == A::Pass);
        n.feed(scroll(0, 0, P::Begin), 10);
        CHECK(n.feed(scroll(-200, 0, P::Update), 11) == A::Back);
        CHECK(n.feed(scroll(-20, 0, P::Update), 12) == A::Swallow);
        CHECK(n.feed(scroll(0, 0, P::End), 13) == A::Pass);
    }
    {
        // A modifier held when the stream starts: not ours.
        shell::SwipeNav n;
        CHECK(n.feed(scroll(-200, 0, P::None, true, plat::ModCtrl), 0) == A::Pass);
    }
    {
        // Gestures with fewer than 3 fingers pass.
        shell::SwipeNav n;
        plat::Event     b{.type = T::GestureBegin};
        b.fingers = 2;
        n.feed(b, 0);
        plat::Event u{.type = T::GestureUpdate};
        u.dx = 300;
        CHECK(n.feed(u, 1) == A::Pass);
    }
    {
        // macOS's one-shot swipe: dx = +1 is back.
        shell::SwipeNav n;
        plat::Event     e{.type = T::SwipeGesture};
        e.dx = 1;
        CHECK(n.feed(e, 0) == A::Back);
        e.dx = -1;
        CHECK(n.feed(e, 0) == A::Forward);
        e.dx = 0;
        e.dy = 1;
        CHECK(n.feed(e, 0) == A::Pass);
    }
}

TEST("shortcuts: bindings that must not exist are gone") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    // Alt+Up/Down switched conversations.
    h.chord(kAlt, K::Down);
    h.chord(kAlt, K::Up);
    CHECK(h.sh->current() == design);
    // Escape closed the thread panel.
    const Ts root = h.backend.findTs(design, "Proposal B");
    REQUIRE(root != 0);
    h.sh->openThread(design, root);
    REQUIRE(h.sh->threadOpen());
    h.key(K::Escape);
    CHECK(h.sh->threadOpen());
    h.sh->closeThread();
    // Up/Down on a focused sidebar row changed the conversation (the list
    // only scrolls).
    ui::View  *row  = nullptr;
    const auto find = [&](auto &self, ui::View *v) -> void {
        if (!row && v->visible() && v->role() == ui::Role::ListItem)
            row = v;
        for (size_t i = 0; i < v->childCount() && !row; ++i)
            self(self, v->child(i));
    };
    find(find, &h.sh->sidebar());
    REQUIRE(row != nullptr);
    row->focus();
    h.key(K::Down);
    h.key(K::Up);
    CHECK(h.sh->current() == design);
}

// ── Composer scope ──────────────────────────────────────────────────────────

TEST("shortcuts: the composer's formatting keys") {
    Harness    h;
    auto      &c     = h.composer();
    auto      &e     = h.edit();
    const auto fresh = [&](const char *text) {
        e.clear();
        e.insertText(text);
        e.selectAll();
    };
    fresh("word");
    h.chord(kCtrl, K::B);
    CHECK_STR(c.mrkdwn(), "*word*");
    fresh("word");
    h.chord(kCtrl, K::I);
    CHECK_STR(c.mrkdwn(), "_word_");
    fresh("word");
    h.chord(kCtrl | kShift, K::X);
    CHECK_STR(c.mrkdwn(), "~word~");
    fresh("word");
    h.chord(kCtrl | kShift, K::C);
    CHECK_STR(c.mrkdwn(), "`word`");
    fresh("word");
    h.chord(kCtrl, K::U); // underline markers are typed
    CHECK_STR(e.text(), "__word__");
    e.clear();
    h.chord(kCtrl, K::U);
    CHECK_STR(e.text(), "____");
    CHECK(e.caret() == 2);
    fresh(" x ");
    h.chord(kCtrl | kAlt | kShift, K::C);
    CHECK_STR(e.text(), "```\nx\n```");
    e.clear();
    h.chord(kCtrl | kAlt | kShift, K::C);
    CHECK_STR(e.text(), "```\n\n```");
    CHECK(e.caret() == 4);
    fresh("a\nb");
    h.chord(kCtrl | kShift, K::Num7);
    CHECK_STR(e.text(), "1. a\n2. b");
    fresh("a\nb");
    h.chord(kCtrl | kShift, K::Num8);
    CHECK_STR(e.text(), "- a\n- b");
    fresh("a");
    h.chord(kCtrl | kShift, K::Num9);
    CHECK_STR(e.text(), "> a");

    // Link: the "Add a link" popup.
    fresh("site");
    h.chord(kCtrl | kShift, K::U);
    CHECK(h.win->topPopup() != nullptr);
    h.win->closeAllPopups();
    pump();
    // Emoji picker.
    h.edit().focus();
    h.chord(kCtrl | kShift, K::Backslash);
    CHECK(h.win->topPopup() != nullptr);
    h.win->closeAllPopups();
    pump();
    // Attach file: the OS file dialog.
    h.edit().focus();
    REQUIRE(hooks().fileDialogRespond({}));
    h.chord(kCtrl, K::O);
    pump();
    plat::FileDialogDesc d;
    REQUIRE(hooks().lastFileDialog(&d));
    CHECK(d.mode == plat::FileDialogDesc::Mode::OpenMultiple);
}

TEST("shortcuts: stubbed composer keys fall through as with the feature off") {
    Harness h;
    auto   &e = h.edit();
    e.insertText("ab");
    h.chord(kCtrl, K::Z); // no undo-send chip: the editor's undo
    CHECK(e.empty());
    h.chord(kCtrl | kShift, K::Space); // no speech-to-text: nothing
    h.chord(kCtrl, K::R);              // no prompt history: nothing
    CHECK(e.empty());
    CHECK(h.win->topPopup() == nullptr);
}

TEST("shortcuts: Enter sends, Shift+Enter breaks the line; the Ctrl+Enter option swaps them") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    auto         &e      = h.edit();
    const size_t  before = h.store.conversation(design).messages.size();
    e.insertText("one");
    h.chord(kShift, K::Enter);
    CHECK_STR(e.text(), "one\n");
    h.key(K::KpEnter);
    CHECK(e.empty());
    CHECK(h.store.conversation(design).messages.size() == before + 1);
    e.insertText("two");
    h.chord(kCtrl, K::Enter); // sends in both modes
    CHECK(e.empty());
    CHECK(h.store.conversation(design).messages.size() == before + 2);

    shell::shortcuts::setCtrlEnterSends(true);
    e.insertText("three");
    h.key(K::Enter);
    CHECK_STR(e.text(), "three\n");
    h.chord(kShift, K::Enter);
    CHECK_STR(e.text(), "three\n\n");
    h.chord(kCtrl, K::Enter);
    CHECK(e.empty());
    CHECK(h.store.conversation(design).messages.size() == before + 3);
}

TEST("shortcuts: Up in an empty composer edits my last message, Escape leaves the edit") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN");
    auto         &c      = h.composer();
    h.edit().insertText("mine");
    h.key(K::Enter);
    pump(20);
    const Message *last = nullptr;
    for (const Message &m : h.store.conversation(design).messages)
        if (m.user == h.store.me && !m.pending)
            last = &m;
    REQUIRE(last != nullptr);
    const Ts ts = last->ts;
    h.edit().insertText("x");
    h.key(K::Up); // not empty: just the caret
    CHECK(c.editing() == 0);
    h.edit().clear();
    h.key(K::Up);
    CHECK(c.editing() == ts);
    h.key(K::Escape);
    CHECK(c.editing() == 0);
}

// ── Documented keys ─────────────────────────────────────────────────────────

TEST("shortcuts: Shift+Del removes an idle Claude Code session, and only that") {
    Harness       h;
    const ConvRef dm = h.conv("D0JONAS");
    REQUIRE(dm != kNoConv);
    const UserRef peer = h.store.conversation(dm).dmUser;
    h.sh->open(dm);
    pump();
    // Not a session: nothing.
    h.chord(kShift, K::Delete);
    CHECK(h.sh->sidebar().rowState(dm).exists);
    // A working session (green dot) stays too.
    h.backend.agent           = dm;
    h.store.user(peer).active = true;
    h.chord(kShift, K::Delete);
    CHECK(h.sh->sidebar().rowState(dm).exists);
    // With a selection in the composer the key is the editor's.
    h.store.user(peer).active = false;
    h.edit().insertText("keep");
    h.edit().selectAll();
    h.chord(kShift, K::Delete);
    CHECK(h.store.conversation(dm).member);
    // Idle, nothing selected: removed.
    h.edit().clear();
    h.chord(kShift, K::Delete);
    pump();
    CHECK_FALSE(h.store.conversation(dm).member);
}

// ── The shortcuts panel ─────────────────────────────────────────────────────

TEST("shortcuts: the panel shows while no conversation is open") {
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
    // Find it by its title.
    bool       found = false;
    const auto walk  = [&](auto &self, ui::View *v) -> void {
        if (!v->visible())
            return;
        if (v->role() == ui::Role::Text &&
            static_cast<ui::Label *>(v)->text() == "Keyboard shortcuts")
            found = true;
        for (size_t i = 0; i < v->childCount(); ++i)
            self(self, v->child(i));
    };
    walk(walk, &win.root());
    CHECK(found);
}

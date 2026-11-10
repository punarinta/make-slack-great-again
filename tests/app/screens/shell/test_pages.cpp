// The shell's pages and dialogs: the Saved messages
// page, the quick switcher's fuzzy matching, the sidebar's persisted visit
// stamps and Settings' "Clear state", the tray picture, restarts and the
// sample notification's outcome.
#include "app/fake/fake_backend.h"
#include "app/model/jobs.h"
#include "app/mrkdwn/emoji.h"
#include "base/utf8.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/str.h"
#include "support/test.h"
#include "base/time.h"
#include "screens/settings/settings_dialog.h"
#include "screens/shell/fuzzy_match.h"
#include "screens/shell/quick_switcher.h"
#include "screens/shell/saved_page.h"
#include "screens/shell/shell.h"
#include "screens/shell/shell_dialogs.h"
#ifdef MSGA_SELF_UPDATE
#include "screens/shell/update_bar.h"
#endif
#include "plat/testing.h"
#include "ui/controls.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/emoji_picker.h"
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/thread_panel.h"
#endif

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <unistd.h>

using namespace model;
using shell::fuzzyScore;

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
    const int64_t end = base::monotonicMs() + ms; // pump() can return early
    while (!done() && base::monotonicMs() < end)
        app().pump(5);
    return done();
}

struct Harness {
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
    shell::Settings               settings;
    std::string                   path;
    std::unique_ptr<ui::Window>   win;
    std::unique_ptr<shell::Shell> sh;

    explicit Harness(shell::Settings s = {}, std::string settingsPath = {})
        : settings(std::move(s)), path(std::move(settingsPath)) {
        backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
        bool done = false;
        backend.connect([&](bool ok, const std::string &) { done = ok; });
        // By the clock: pump() returns early on every post from a worker
        // thread, so a count of pumps can run out before the connect.
        const int64_t end = base::monotonicMs() + 10000;
        while (!done && base::monotonicMs() < end)
            app().pump(5);
        plat::WindowDesc d;
        d.size        = {1200, 800};
        d.decorations = plat::Decorations::Custom;
        win           = std::make_unique<ui::Window>(d);
        sh            = std::make_unique<shell::Shell>(ctx, *win, settings, path);
        sh->open(store.findConversation("C0DESIGN"));
        pump();
    }
    ConvRef conv(const char *id) const { return store.findConversation(id); }
};

ui::View *findIn(ui::View *v, std::string_view name) {
    if (!v->visible())
        return nullptr;
    if (v->accessibleName() == name)
        return v;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (ui::View *f = findIn(v->child(i), name))
            return f;
    return nullptr;
}

std::string tempDir() {
    return base::test::makeTempDir("msga_pages_");
}

} // namespace

// ── Fuzzy matching ──────────────────────────────────────────────────────────

TEST("fuzzy: subsequences match, in order, any case") {
    CHECK(fuzzyScore("xdg", "xd-general").has_value()); // issue #60
    CHECK(fuzzyScore("bb", "Bob Builder").has_value());
    CHECK(fuzzyScore("BOB", "bob builder").has_value());
    CHECK(fuzzyScore("ÅSA", "åsa lind").has_value());        // folded by code point
    CHECK_FALSE(fuzzyScore("gx", "xd-general").has_value()); // order matters
    CHECK_FALSE(fuzzyScore("zzz", "general").has_value());
    CHECK_FALSE(fuzzyScore("generals", "general").has_value());
    CHECK(*fuzzyScore("", "anything") == 0.0);
    CHECK_FALSE(fuzzyScore("a", "").has_value());
}

TEST("fuzzy: a substring beats a scattered match, word starts beat the middle") {
    // Consecutive characters outrank any boundary bonus.
    CHECK(*fuzzyScore("gen", "general") > *fuzzyScore("gen", "green-lane"));
    // A word start (after '-') beats the same letters mid-word.
    CHECK(*fuzzyScore("ui", "team-ui") > *fuzzyScore("ui", "build"));
    // camelCase humps count as word starts.
    CHECK(*fuzzyScore("ws", "myWorkspace") > *fuzzyScore("ws", "mywsorkspace") - 1);
    // The whole name typed wins over a longer name with it as a prefix.
    CHECK(*fuzzyScore("design", "design") > *fuzzyScore("design", "design-review"));
    // Trailing characters cost nothing: equal prefixes tie, so the caller's
    // order (recency) decides.
    CHECK(*fuzzyScore("des", "design-review") == *fuzzyScore("des", "design-backend"));
}

namespace {

// The scorer as it was before names were prepared once: everything decoded,
// folded and weighed per call. The prepared path must match it exactly.
std::optional<double> unpreparedScore(std::string_view query, std::string_view haystack) {
    constexpr double kMin = -std::numeric_limits<double>::infinity();
    const auto       sep  = [](uint32_t c) {
        switch (c) {
        case ' ':
        case '-':
        case '_':
        case ',':
        case '/':
        case '\\':
        case '@':
        case '#':
        case ':':
        case '(':
        case ')':
        case '[':
        case ']':
            return true;
        default:
            if (utf8::isSpace(c))
                return true;
            if (c < 0x80)
                return (c >= 0x21 && c <= 0x2f) || (c >= 0x3a && c <= 0x40) ||
                       (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e);
            return !utf8::isWordChar(c);
        }
    };
    const auto upper = [](uint32_t c) { return utf8::foldCase(c) != c; };
    const auto lower = [&](uint32_t c) {
        return !upper(c) && utf8::isWordChar(c) && !utf8::isDigit(c);
    };
    const auto points = [](std::string_view s) {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < s.size();)
            out.push_back(utf8::decode(s, i));
        return out;
    };
    if (query.empty())
        return 0.0;
    if (haystack.empty())
        return std::nullopt;
    const std::vector<uint32_t> orig = points(haystack);
    std::vector<uint32_t>       q    = points(query), h(orig.size());
    for (uint32_t &c : q)
        c = utf8::foldCase(c);
    for (size_t j = 0; j < orig.size(); ++j)
        h[j] = utf8::foldCase(orig[j]);
    if (q.size() > h.size())
        return std::nullopt;
    size_t qi = 0;
    for (size_t hi = 0; hi < h.size() && qi < q.size(); ++hi)
        if (h[hi] == q[qi])
            ++qi;
    if (qi != q.size())
        return std::nullopt;
    if (h.size() > 512)
        return 0.0;
    const size_t        n = q.size(), m = h.size();
    std::vector<double> bonus(m);
    for (size_t j = 0; j < m; ++j) {
        if (j == 0) {
            bonus[j] = 0.9;
            continue;
        }
        const uint32_t prev = orig[j - 1], cur = orig[j];
        bonus[j] = prev == '.' ? 0.6 : sep(prev) ? 0.8 : lower(prev) && upper(cur) ? 0.7 : 0.0;
    }
    std::vector<double> prevM(m, kMin), prevD(m, kMin), curM(m), curD(m);
    for (size_t i = 0; i < n; ++i) {
        const double gap       = i == n - 1 ? 0.0 : -0.01;
        double       prevScore = kMin;
        for (size_t j = 0; j < m; ++j) {
            if (q[i] == h[j]) {
                double s = kMin;
                if (i == 0)
                    s = double(j) * -0.005 + bonus[j];
                else if (j > 0)
                    s = std::max(prevD[j - 1] + bonus[j], prevM[j - 1] + 1.0);
                curM[j]   = s;
                prevScore = std::max(s, prevScore + gap);
                curD[j]   = prevScore;
            } else {
                curM[j]   = kMin;
                prevScore = prevScore + gap;
                curD[j]   = prevScore;
            }
        }
        std::swap(prevM, curM);
        std::swap(prevD, curD);
    }
    return prevD[m - 1] + (n == m ? 1.0 : 0.0);
}

// quickSwitchFilter as it was: every name built and scored per keystroke.
std::vector<ConvRef>
unpreparedFilter(const Store &store, std::string_view query, const std::vector<ConvRef> &order) {
    const std::string_view                  q = str::trim(query);
    std::vector<std::pair<double, ConvRef>> scored;
    for (ConvRef c : order) {
        const std::string name = store.displayName(c);
        if (name.empty())
            continue;
        if (const std::optional<double> s = unpreparedScore(q, name))
            scored.emplace_back(
                *s + (store.conversation(c).kind == ConvKind::Group ? -0.5 : 0.0), c
            );
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    std::vector<ConvRef> out;
    for (const auto &s : scored)
        out.push_back(s.second);
    return out;
}

const char *const kQueries[] = {
    "",     " ",     "a",    "e",   "z",  "des",  "dsgn", "MIRA", "mira o",
    "lnch", "atlas", "gen",  "g-n", "ws", "ÅSA",  "å",    "rand", "random",
    "eng",  "o, j",  "trio", ".",   "#",  "qqqq", "Jo",   "lw",   "xdg",
};

} // namespace

TEST("fuzzy: a prepared name scores exactly as the plain call did") {
    std::string long_(600, 'a');
    long_ += "-b";
    const char *const hay[] = {
        "general",
        "xd-general",
        "Bob Builder",
        "åsa lind",
        "myWorkspace",
        "design-review",
        "a.b.c",
        "team_ui",
        "Mira Okafor, Jonas Weber",
        "ÅSA",
        "x",
        "",
        long_.c_str(),
        "ΣΊΣΥΦΟΣ (ops)",
        "über/straße",
        "launch-atlas",
        "Ma1nT",
        "a  b",
    };
    int compared = 0;
    for (const char *h : hay) {
        const shell::FuzzyText prepared(h);
        for (const char *q : kQueries) {
            const std::optional<double> want = unpreparedScore(q, h);
            const std::optional<double> got  = shell::fuzzyScore(shell::fuzzyQuery(q), prepared);
            CHECK(want.has_value() == got.has_value());
            if (want && got)
                CHECK(*want == *got); // bit for bit, not near
            const std::optional<double> plain = fuzzyScore(q, h);
            CHECK(plain.has_value() == got.has_value());
            if (plain && got)
                CHECK(*plain == *got);
            ++compared;
        }
    }
    CHECK(compared > 400);
}

TEST("quick switcher: prepared names give the same list; renames still show") {
    Harness    h;
    const auto order = h.sh->sidebar().order();
    const auto names = shell::quickSwitchNames(h.store, order);
    REQUIRE(names.size() == order.size());
    for (const char *q : kQueries) {
        const auto want = unpreparedFilter(h.store, q, order);
        CHECK(shell::quickSwitchFilter(q, order, names) == want);
        CHECK(shell::quickSwitchFilter(h.store, q, order) == want);
    }
    // Through the popup, keystroke by keystroke.
    h.sh->showQuickSwitcher();
    pump();
    shell::QuickSwitcher *qs = h.sh->quickSwitcher();
    REQUIRE(qs);
    const std::vector<ConvRef> shown = shell::quickSwitchOrder(h.store, h.sh->sidebar().visited());
    for (const char *typed : {"m", "i", "r", "a"}) {
        qs->field().insertText(typed);
        CHECK(qs->results() == unpreparedFilter(h.store, qs->field().text(), shown));
    }
    // A channel renamed while the switcher is up (Meta): the next keystroke
    // sees the new name.
    qs->field().setText("zqxw");
    CHECK(qs->results().empty());
    const ConvRef random = h.conv("C0RANDOM");
    h.store.updateConversation(random, [](Conversation &c) { c.name = "zqxw-room"; });
    qs->field().setText("zqx");
    REQUIRE(qs->results().size() == 1);
    CHECK(qs->results()[0] == random);
    // A person's new name (Users) renames their DM and group DMs too.
    const ConvRef mira = h.conv("D0MIRA");
    const UserRef u    = h.store.conversation(mira).dmUser;
    REQUIRE(u != kNoUser);
    h.store.user(u).displayName = "Vexilla Q";
    h.store.usersChanged();
    qs->field().setText("vexilla");
    REQUIRE(!qs->results().empty());
    CHECK(qs->results()[0] == mira);
    CHECK(qs->results() == unpreparedFilter(h.store, "vexilla", shown));
    qs->close();
    pump();
}

TEST("quick switcher: fuzzy, the placeholder and the empty state") {
    Harness    h;
    const auto order = h.sh->sidebar().order();
    auto       r     = shell::quickSwitchFilter(h.store, "dsgn", order);
    REQUIRE(!r.empty());
    CHECK(r[0] == h.conv("C0DESIGN"));
    r = shell::quickSwitchFilter(h.store, "lnch", order); // #launch-atlas
    REQUIRE(!r.empty());
    CHECK(r[0] == h.conv("G0LAUNCH"));
    h.sh->showQuickSwitcher();
    pump();
    shell::QuickSwitcher *qs = h.sh->quickSwitcher();
    REQUIRE(qs);
    CHECK(findIn(qs, "No conversations match.") == nullptr);
    qs->field().insertText("qqqq");
    pump();
    CHECK(qs->results().empty());
    CHECK(findIn(qs, "No conversations match.") != nullptr);
    qs->close();
    pump();
}

// ── Saved messages ──────────────────────────────────────────────────────────

TEST("saved messages: the entry opens the page; cards, due lines, order, remove") {
    Harness       h;
    const ConvRef design = h.conv("C0DESIGN"), general = h.conv("C0GENERAL");
    const Ts      a = h.store.conversation(design).messages.back().ts;
    const Ts      b = h.store.conversation(general).messages.front().ts;
    h.backend.setSaved(design, a, true);
    const int64_t due = base::fromLocal(2026, 9, 25, 9, 30);
    h.backend.setReminder(general, b, due);
    pump();

    h.sh->sidebar().onSavedMessages();
    pump();
    REQUIRE(h.sh->pageOpen(shell::Shell::Page::Saved));
    CHECK(h.sh->sidebar().selectedNav() == shell::Sidebar::Nav::Saved);
    CHECK(h.sh->current() == kNoConv);
    shell::SavedPage *page = h.sh->savedPage();
    REQUIRE(page->cardCount() == 2);
    // Reminders first, then the bookmarks.
    CHECK(page->dueText(0) == i18n::arg("Reminder set for %1", base::formatDateTime(due)));
    CHECK_STR(page->dueText(1), "Saved for later");
    CHECK(page->statusText().empty());
    CHECK(findIn(page->card(1), "Remove") != nullptr);

    // Remove: the item goes (and the reminder with it).
    page->remove(0);
    pump();
    CHECK(page->cardCount() == 1);
    CHECK(h.store.reminderAt(general, b) == 0);
    page->remove(0);
    pump();
    CHECK(page->cardCount() == 0);
    CHECK_STR(
        page->statusText(), "Messages you save for later or set reminders on will appear here."
    );
    // The entry hides with nothing saved, the page stays until left.
    CHECK(h.sh->pageOpen(shell::Shell::Page::Saved));
    h.sh->open(design);
    pump();
    CHECK_FALSE(h.sh->pageOpen(shell::Shell::Page::Saved));
    CHECK_FALSE(h.sh->sidebar().selectedNav() == shell::Sidebar::Nav::Saved);
}

TEST("saved messages: a click jumps to the message, a reply inside its thread") {
    Harness       h;
    const ConvRef general = h.conv("C0GENERAL");
    const Ts      top     = h.store.conversation(general).messages.front().ts;
    h.backend.setSaved(general, top, true);
    // A saved reply: the first thread with replies in #design.
    const ConvRef design = h.conv("C0DESIGN");
    Ts            root = 0, reply = 0;
    for (const model::Thread &t : h.store.conversation(design).threads)
        if (!t.replies.empty()) {
            root  = t.root;
            reply = t.replies.back().ts;
            break;
        }
    REQUIRE(reply);
    h.backend.setSaved(design, reply, true);
    pump();
    h.sh->showPage(shell::Shell::Page::Saved);
    pump();
    shell::SavedPage *page = h.sh->savedPage();
    REQUIRE(page->cardCount() == 2);
    // Newest saved first: the reply.
    page->activate(0);
    pump(20);
    CHECK(h.sh->current() == design);
    CHECK(h.sh->threadOpen());
#ifdef MSGA_HAVE_MESSAGES
    auto *tp = static_cast<screens::ThreadPanel *>(h.sh->threadPanel());
    REQUIRE(tp);
    CHECK(tp->root() == root);
#else
    (void)root;
#endif
    h.sh->showPage(shell::Shell::Page::Saved);
    pump();
    page->activate(1);
    pump(20);
    CHECK(h.sh->current() == general);
    CHECK_FALSE(h.sh->pageOpen(shell::Shell::Page::Saved));
}

TEST("saved messages: an item whose message isn't known asks for it once") {
    Harness       h;
    const ConvRef general = h.conv("C0GENERAL");
    // Saved elsewhere: a ts this workspace never loaded.
    h.store.setSavedItem(general, 1'000'000'000'000'123, true, 0, 5);
    const Store::SavedItem *it = h.store.findSaved(general, 1'000'000'000'000'123);
    REQUIRE(it);
    CHECK_FALSE(it->previewed);
    h.sh->showPage(shell::Shell::Page::Saved);
    pump();
    it = h.store.findSaved(general, 1'000'000'000'000'123);
    REQUIRE(it);
    CHECK(it->previewed); // asked; the fake has no such message
    shell::SavedPage *page = h.sh->savedPage();
    REQUIRE(page->cardCount() == 1);
    CHECK(findIn(page->card(0), "No preview available") != nullptr);
}

// ── Visit stamps, "Clear state" ─────────────────────────────────────────────

TEST("visited: the stamps round-trip through the settings file") {
    shell::Settings s;
    s.visitedAt           = {{"C0DESIGN", 1758000000}, {"D0MIRA", 1758000100}};
    s.trayMonochrome      = false;
    const std::string dir = tempDir();
    REQUIRE(!dir.empty());
    const std::string path = dir + "/settings.json";
    REQUIRE(s.save(path));
    const shell::Settings l = shell::Settings::load(path);
    REQUIRE(l.visitedAt.size() == 2);
    CHECK(l.visitedAt[0].first == "C0DESIGN");
    CHECK(l.visitedAt[0].second == 1758000000);
    CHECK(l.visitedAt[1].first == "D0MIRA");
    CHECK_FALSE(l.trayMonochrome);
    CHECK(shell::Settings().trayMonochrome); // on by default
    file::remove(path);
    file::remove(dir);
}

TEST("visited: opening a conversation is saved; the stamps keep it listed after a restart") {
    const std::string dir  = tempDir();
    const std::string path = dir + "/settings.json";
    ConvRef           sam  = kNoConv;
    {
        Harness h({}, path);
        sam = h.conv("D0SAM");
        h.sh->open(sam);
        pump();
        // Written 1.5 s after the last change.
        REQUIRE(until([&] { return file::exists(path); }, 3000));
        const shell::Settings l   = shell::Settings::load(path);
        bool                  has = false;
        for (const auto &[id, at] : l.visitedAt)
            has |= id == "D0SAM" && at > 0;
        CHECK(has);
    }
    // A narrow window: only the stamp keeps the DM listed.
    shell::Settings s = shell::Settings::load(path);
    s.relevantDays    = 1;
    Harness                 h(s, path);
    shell::Sidebar::Filters f;
    f.relevantDays = 1;
    h.sh->sidebar().setFilters(f);
    pump();
    CHECK(h.sh->sidebar().rowState(sam).exists);
    CHECK(h.sh->sidebar().visited().count("D0SAM") == 1);

    // Settings → Storage → "Clear state": the stamps go (file included).
    h.sh->openSettings();
    pump();
    settings::SettingsDialog *d = h.sh->settingsDialog();
    REQUIRE(d);
    d->showPage(settings::SettingsDialog::Page::Storage);
    pump();
    auto *clear = static_cast<ui::Button *>(d->find("Clear state"));
    REQUIRE(clear);
    clear->onClick();
    pump();
    CHECK_FALSE(clear->enabled());
    CHECK(h.sh->sidebar().visited().count("D0SAM") == 0);
    const shell::Settings after = shell::Settings::load(path);
    for (const auto &[id, at] : after.visitedAt)
        CHECK(id != "D0SAM");
    file::remove(path);
    file::remove(dir);
}

TEST("settings: a change is written a moment later, off the UI thread; saveState at once") {
    const std::string dir  = tempDir();
    const std::string path = dir + "/settings.json";
    {
        Harness h({}, path);
        h.settings.relevantDays = 9;
        h.sh->saveSettingsSoon();
        h.sh->saveSettingsSoon(); // the same write
        CHECK(shell::Settings::load(path).relevantDays != 9);
        CHECK(until([&] { return shell::Settings::load(path).relevantDays == 9; }, 3000));
        h.settings.relevantDays = 11;
        h.sh->saveSettingsSoon();
        h.sh->saveState(); // quitting: written now, and the waiting one is not left to overwrite it
        CHECK(shell::Settings::load(path).relevantDays == 11);
        model::waitBackground();
        pump(10);
        CHECK(shell::Settings::load(path).relevantDays == 11);
    }
    file::remove(path);
    file::remove(dir);
}

// ── Settings: restart, sample notification, updates ─────────────────────────

TEST("settings: \"Save and restart\" restarts; the sample notification reports back") {
    Harness h;
    h.sh->openSettings();
    pump();
    settings::SettingsDialog *d = h.sh->settingsDialog();
    REQUIRE(d);
    d->showPage(settings::SettingsDialog::Page::System);
    pump();
#ifdef MSGA_SELF_UPDATE
    // Without an updater: the state without a checker.
    CHECK(d->find("Update checks not available.") != nullptr);
    CHECK_FALSE(d->find("Check for updates")->enabled());
#else
    // Built without self-updates: no update checks at all.
    CHECK(d->find("Check for updates") == nullptr);
    CHECK(d->find("Check for updates automatically") == nullptr);
    CHECK(d->find("Update checks not available.") == nullptr);
#endif
    d->close();
    pump();

    bool quit               = false;
    h.sh->onQuit            = [&] { quit = true; };
    h.settings.slackSession = false; // the app keys box shows
    h.sh->openSettings();
    pump();
    d = h.sh->settingsDialog();
    d->showPage(settings::SettingsDialog::Page::System);
    pump();
    ui::View *save = d->find("Save and restart");
    REQUIRE(save);
    static_cast<ui::Button *>(save)->onClick(); // nothing changed
    pump();
    CHECK(d->find("No changes to save.") != nullptr);
    CHECK_FALSE(quit);
}

#ifdef MSGA_SELF_UPDATE
TEST("update bar: hidden until an update is ready, then its wording") {
    Harness           h;
    shell::UpdateBar *bar = h.sh->updateBar();
    REQUIRE(bar);
    CHECK_FALSE(bar->visible());
    bar->showUpdateReady();
    pump();
    CHECK(bar->visible());
#ifdef __APPLE__
    CHECK_STR(bar->button()->label(), "Open installer");
#else
    CHECK_STR(bar->button()->label(), "Restart now");
    CHECK(findIn(bar, "Restart now") != nullptr);
#endif
}
#endif

// ── Tray picture ────────────────────────────────────────────────────────────

TEST("tray icon: fitted into the square, and the monochrome silhouette") {
    // A wide opaque picture: a dark mark on a light backdrop.
    gfx::Bitmap src(40, 20);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 40; ++x)
            src.pixels()[y * 40 + x] = (x >= 15 && x < 25) ? 0xff202020U : 0xffeeeeeeU;
    const gfx::Bitmap fit = shell::trayPicture(src, false);
    REQUIRE(fit.width() == shell::kTrayIconSize);
    REQUIRE(fit.height() == shell::kTrayIconSize);
    const int n = shell::kTrayIconSize;
    CHECK((fit.pixels()[0] >> 24) == 0);                              // letterbox: transparent
    CHECK((fit.pixels()[(n / 2) * n + 2] >> 24) == 0xff);             // the picture, edge to edge
    CHECK((fit.pixels()[(n / 2) * n + n / 2] & 0xffffff) < 0x404040); // the mark, dark

    const gfx::Bitmap mono = shell::trayPicture(src, true);
    const uint32_t    mark = mono.pixels()[(n / 2) * n + n / 2];
    const uint32_t    bg   = mono.pixels()[(n / 2) * n + 4];
    CHECK((mark >> 24) == 0xff);
    CHECK(mark == 0xffffffffU); // white
    CHECK((bg >> 24) == 0);     // the backdrop keyed out
    CHECK((mono.pixels()[0] >> 24) == 0);

    // One flat colour: the whole picture is the silhouette.
    gfx::Bitmap flat(10, 10);
    std::fill(flat.pixels(), flat.pixels() + 100, 0xff3366ccU);
    const gfx::Bitmap fm = shell::trayPicture(flat, true);
    CHECK(fm.pixels()[(n / 2) * n + n / 2] == 0xffffffffU);
}

TEST("tray icon: a custom picture is decoded off the UI thread, then drawn") {
    const std::string a      = std::string(MSGA_TEST_ASSETS) + "/workspace.png";
    const std::string b      = std::string(MSGA_TEST_ASSETS) + "/images/palette-v3.png";
    const auto        expect = [](const std::string &path, bool mono) {
        std::string bytes;
        gfx::Bitmap bmp;
        CHECK(file::readAll(path, &bytes));
        CHECK(shell::decodeTrayPicture(bytes, &bmp));
        return shell::trayPicture(bmp, mono);
    };
    const auto same = [](const gfx::Bitmap *got, const gfx::Bitmap &want) {
        return got && got->width() == want.width() && got->height() == want.height() &&
               std::equal(
                   want.pixels(),
                   want.pixels() + size_t(want.width()) * want.height(),
                   got->pixels()
               );
    };
    shell::Settings s;
    s.customTrayIcon = true;
    s.trayIconPath   = a;
    s.trayMonochrome = false;
    Harness h(s);
    REQUIRE(until([&] { return h.sh->customTrayPicture() != nullptr; }));
    CHECK(same(h.sh->customTrayPicture(), expect(a, false)));
    plat::TestHooks::TrayProbe p;
    REQUIRE(app().platform().testHooks()->trayProbe(*h.sh->tray(), &p));
    CHECK(!p.iconSizes.empty());
    // Another picture: not decoded inside the call; drawn once it is in.
    h.settings.trayIconPath   = b;
    h.settings.trayMonochrome = true;
    h.sh->applySettings();
    CHECK(h.sh->customTrayPicture() == nullptr);
    REQUIRE(until([&] { return h.sh->customTrayPicture() != nullptr; }));
    CHECK(same(h.sh->customTrayPicture(), expect(b, true)));
    // A file that isn't a picture: the built-in plane, as before.
    h.settings.trayIconPath = std::string(MSGA_TEST_ASSETS) + "/fixture.json";
    h.sh->applySettings();
    pump(40);
    model::waitBackground();
    pump(10);
    CHECK(h.sh->customTrayPicture() == nullptr);
    REQUIRE(app().platform().testHooks()->trayProbe(*h.sh->tray(), &p));
    CHECK(!p.iconSizes.empty());
    // Back to the default: at once.
    h.settings.trayIconPath = a;
    h.sh->applySettings();
    h.settings.customTrayIcon = false; // turned off before the decode answers
    h.sh->applySettings();
    model::waitBackground();
    pump(10);
    CHECK(h.sh->customTrayPicture() == nullptr);
}

TEST("tray icon dialog: closed while its picture decodes, nothing answers into it") {
    Harness    h;
    ui::Popup *d = shell::showTrayIconDialog(
        h.ctx, *h.win, std::string(MSGA_TEST_ASSETS) + "/workspace.png", true, nullptr
    );
    REQUIRE(d);
    d->close();
    pump();
    model::waitBackground();
    pump(10); // the answer finds the dialog gone
    // Left open, it shows the picture once decoded (no crash, no hang).
    d = shell::showTrayIconDialog(
        h.ctx, *h.win, std::string(MSGA_TEST_ASSETS) + "/workspace.png", false, nullptr
    );
    REQUIRE(d);
    model::waitBackground();
    pump(10);
    CHECK(findIn(d, "Convert to monochrome") != nullptr);
    d->close();
    pump();
}

// ── Claude Code: a teammate's "Message" ─────────────────────────────────────

namespace {

struct TeamFake : fake::FakeBackend {
    using fake::FakeBackend::FakeBackend;
    Capabilities capabilities() const override {
        Capabilities c  = fake::FakeBackend::capabilities();
        c.agentSessions = true;
        return c;
    }
    std::vector<AgentRole> agentRoles() const override {
        AgentRole r;
        r.id   = "designer";
        r.name = "Designer";
        r.user = mate;
        return {r};
    }
    std::string agentSessionBlocker(const std::string &) const override { return {}; }
    UserRef     mate = kNoUser;
};

} // namespace

TEST("profile card \"Message\": a teammate's page, anyone else's DM") {
    Store    store;
    TeamFake backend{store, app().platform()};
#ifdef MSGA_HAVE_MESSAGES
    screens::ImageCache images{app().platform()};
    screens::Context    ctx{app(), store, backend, images, {}, {}, {}, {}, {}};
#else
    alignas(16) char noCache[16]{};
    screens::Context ctx{
        app(), store, backend, *reinterpret_cast<screens::ImageCache *>(noCache), {}, {}, {}, {}, {}
    };
#endif
    backend.setFixture(MSGA_TEST_ASSETS, base::fromLocal(2026, 9, 21, 16, 0));
    bool done = false;
    backend.connect([&](bool ok, const std::string &) { done = ok; });
    REQUIRE(until([&] { return done; }));
    backend.mate = store.findUser("U0LENA");
    plat::WindowDesc d;
    d.size        = {1200, 800};
    d.decorations = plat::Decorations::Custom;
    ui::Window      win(d);
    shell::Settings settings;
    shell::Shell    sh(ctx, win, settings, std::string());
    sh.open(store.findConversation("C0DESIGN"));
    pump();
    REQUIRE(ctx.messageUser);
    ctx.messageUser(backend.mate);
    pump();
    CHECK(sh.teammateOpen());
    CHECK(sh.sidebar().selectedTeammate() == "designer");
    ctx.messageUser(store.findUser("U0MIRA"));
    REQUIRE(until([&] { return sh.current() == store.findConversation("D0MIRA"); }));
    CHECK_FALSE(sh.teammateOpen());
}

// ── Composer @ list and emoji search cost (performance review M11) ─────────

TEST("composer: the @ list folds each user once and filters in the open list") {
    Harness h;
    auto   &c = h.sh->composer();
    c.edit().focus();
    c.edit().insertText("hey @m");
    pump();
    shell::PickList *list = c.pickList();
    REQUIRE(list != nullptr);
    const size_t folds = c.mentionFolds();
    CHECK(folds > 0);
    // One keystroke is one look (onChange and onSelectionChange both ask),
    // the same popup takes the new rows, and no label is folded again.
    size_t looks = c.pickRecomputes();
    for (const char *ch : {"i", "r"}) {
        c.edit().insertText(ch);
        pump();
        CHECK(c.pickRecomputes() == looks + 1);
        looks = c.pickRecomputes();
        CHECK(c.pickList() == list);
    }
    CHECK(c.mentionFolds() == folds);
    // The rows are what a fold-per-user filter gives, in roster order.
    std::vector<std::string> want;
    for (UserRef u = 0; u < h.store.userCount() && want.size() < 50; ++u) {
        const User &user = h.store.user(u);
        if (user.placeholder)
            continue;
        const std::string label(user.label());
        if (utf8::containsFolded(label, "mir") || utf8::containsFolded(user.name, "mir"))
            want.push_back(u == h.store.me ? "@" + label + " (you)" : "@" + label);
    }
    REQUIRE(!want.empty());
    REQUIRE(list->count() == want.size()); // "mir" matches no @channel alias
    for (size_t i = 0; i < want.size(); ++i)
        CHECK_STR(list->item(i).title, want[i]);
    // Asking again with nothing changed neither looks nor rebuilds the rows.
    const int builds = list->builds();
    c.updatePickList();
    CHECK(c.pickRecomputes() == looks);
    CHECK(list->builds() == builds);
    // Escape closes it; the same text then opens a fresh one, as before.
    ui::Event esc{ui::EventType::KeyDown};
    esc.key = plat::Key::Escape;
    CHECK(list->handleKey(esc));
    pump();
    CHECK(c.pickList() == nullptr);
    c.updatePickList();
    CHECK(c.pickList() != nullptr);
    // A blank ends the word: the list goes.
    c.edit().insertText(" ");
    pump();
    CHECK(c.pickList() == nullptr);
}

TEST("composer: the @ list refolds a renamed user only, and finds the new name") {
    Harness h;
    h.store.usersChanged(); // every user's revision noted, as after any Users change
    pump();
    auto &c = h.sh->composer();
    c.edit().focus();
    c.edit().insertText("@");
    pump();
    REQUIRE(c.pickList() != nullptr);
    const size_t  folds = c.mentionFolds();
    const UserRef u     = h.store.findUser("U0MIRA");
    REQUIRE(u != kNoUser);
    h.store.user(u).displayName = "Zanzibar Quux";
    h.store.usersChanged(u);
    pump();
    c.edit().insertText("zanz");
    pump();
    REQUIRE(c.pickList() != nullptr);
    CHECK(c.mentionFolds() == folds + 1); // the one user who changed
    REQUIRE(c.pickList()->count() == 1);
    CHECK_STR(c.pickList()->item(0).title, "@Zanzibar Quux");
    // Her handle still finds her (the label, handle and names are one search).
    c.edit().setText("@" + h.store.user(u).name.substr(0, 3));
    c.edit().setSelection(4, 4);
    pump();
    c.updatePickList();
    REQUIRE(c.pickList() != nullptr);
    bool found = false;
    for (size_t i = 0; i < c.pickList()->count(); ++i)
        found = found || c.pickList()->item(i).title == "@Zanzibar Quux";
    CHECK(found);
}

#ifdef MSGA_HAVE_MESSAGES
TEST("emoji picker: the search finds what a fold-per-name search finds; recents by name") {
    Harness h;
    h.store.setCustomEmoji("zed_caps", "https://emoji.example/z.png");
    h.store.setCustomEmoji("party_parrot", "https://emoji.example/p.png");
    auto *ep = screens::EmojiPicker::show(*h.win, {400, 700, 20, 20}, h.ctx, nullptr);
    REQUIRE(ep != nullptr);
    for (const char *q : {"par", "SMILE", "Zed", "+1", "flag-", "x"}) {
        std::vector<std::string> want;
        for (const auto &c : h.store.customEmojiImages())
            if (utf8::containsFolded(c.name, q))
                want.emplace_back(c.name);
        emoji::forEach([&](std::string_view n, const std::string &) {
            if (utf8::containsFolded(n, q))
                want.emplace_back(n);
            return true;
        });
        ep->filter(q);
        REQUIRE(ep->cellCount() == want.size());
        for (size_t i = 0; i < want.size(); ++i)
            CHECK_STR(ep->cellName(i), want[i]);
    }
    // "Frequently used": a custom one found by name, a gone one skipped.
    screens::EmojiPicker::restoreState({"zed_caps", "tada", "gone_one"}, 0);
    ep->filter({});
    REQUIRE(ep->cellCount() > 2);
    CHECK_STR(ep->cellName(0), "zed_caps");
    CHECK_STR(ep->cellName(1), "tada");
    CHECK_STR(ep->sections()[0], "Frequently used");
    screens::EmojiPicker::restoreState({}, 0);
    ep->close();
    pump();
}
#endif

// ── Sidebar hover ───────────────────────────────────────────────────────────

// M6: a row's hover recolours its name (Label::setColor), which never
// reshapes: sweeping the pointer down the sidebar builds no text layout.
TEST("pages: hovering sidebar rows reshapes no text") {
    Harness         h;
    const ui::RectF sb    = h.sh->sidebar().windowRect();
    auto            sweep = [&] {
        for (float y = sb.y + 4; y < sb.bottom() - 4; y += 7) {
            app().platform().testHooks()->injectPointerMove(h.win->native(), {sb.x + 90, y});
            app().pump(0);
        }
        app().platform().testHooks()->injectPointerMove(h.win->native(), {sb.right() + 300, 400});
        app().pump(0);
    };
    sweep(); // anything shaped lazily on a first hover (none expected) is done
    const size_t n0 = text::layoutBuilds();
    sweep();
    CHECK(text::layoutBuilds() == n0);
}

// Headless tests for the messages screens: image cache, grouping, date
// separators, anchoring on live appends (the fake backend's auto-replies),
// row actions, thread mode, URL images (RemoteImages) and the loading /
// empty states.
#include "app/fake/fake_backend.h"
#include "app/llm/audio_transcriber.h"
#include "app/llm/service.h"
#include "app/media/audio_player.h"
#include "support/fake_llm_server.h"
#include "app/model/jobs.h"
#include "app/screens/common/avatar_initial.h"
#include "app/screens/common/icon_button.h"
#include "app/screens/common/message_rules.h"
#include "app/screens/common/message_text.h"
#include "app/screens/common/remote_images.h"
#include "app/screens/messages/audio_card.h"
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/message_dialogs.h"
#include "app/screens/messages/message_list.h"
#include "app/screens/messages/rich.h"
#include "app/screens/messages/summary.h"
#include "app/screens/messages/thread_export.h"
#include "app/screens/messages/thread_panel.h"
#include "app/screens/messages/rows.h"
#include "app/screens/messages/table_view.h"
#include "app/screens/common/canvas_doc.h"
#include "base/str.h"
#include "base/file.h"
#include "base/json.h"
#include "base/process.h"
#include "support/test.h"
#include "gfx/icons_generated.h"
#include "base/time.h"
#include "plat/audio.h"
#include "plat/testing.h"
#include "ui/controls.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <cmath>
#include <cstring>
#include <ctime>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#endif
#include <cstdlib>
#include <random>
#include <functional>
#include <memory>

using namespace screens;
using Kind = MessageList::ItemKind;

// Heap allocations on this thread while counting (ImageCache lookups must
// make none). The replacement operator new serves the whole test binary, and
// the other suites in it count with these too (test_shell_ui.cpp).
namespace {
thread_local bool   tCountAllocs = false;
std::atomic<size_t> gAllocs{0};
} // namespace

void countAllocs(bool on) {
    tCountAllocs = on;
}
size_t testAllocs() {
    return gAllocs.load();
}

void *operator new(size_t n) {
    if (tCountAllocs)
        ++gAllocs;
    if (void *p = std::malloc(n ? n : 1))
        return p;
    std::abort(); // no exceptions in this build
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, size_t) noexcept {
    std::free(p);
}

namespace {

ui::App &app() {
    static std::unique_ptr<ui::App> a = [] {
        std::string err;
        auto        p = ui::App::create(&err);
        if (!p) {
            std::fprintf(stderr, "ui::App::create: %s\n", err.c_str());
            std::abort();
        }
        return p;
    }();
    return *a;
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

void pump(int n = 4) {
    for (int i = 0; i < n; ++i)
        app().pump(1);
}

// The newest message with this text: what a test just sent. Not simply the
// last one — the test fixture has messages at today's wall-clock times,
// which sort after a send made earlier in the day.
Ts sentTs(const model::Store &store, ConvRef conv, std::string_view text) {
    const auto &msgs = store.conversation(conv).messages;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (it->text == text)
            return it->ts;
    return 0;
}

std::string asset(const char *rel) {
    return std::string(MSGA_TEST_ASSETS) + "/" + rel;
}

// The fake backend, with Backend::downloadFile for https://….test/<rel>
// URLs (rel under the test assets), answering later like the real one.
struct DownloadingBackend : fake::FakeBackend {
    using fake::FakeBackend::FakeBackend;
    struct Asked {
        std::string url, to;
    };
    std::vector<Asked>       asked;
    std::vector<std::string> resolved;         // resolveChannel ids
    bool                     reminders = true; // Capabilities::messageReminders
    void         resolveChannel(const std::string &id) override { resolved.push_back(id); }
    Capabilities capabilities() const override {
        Capabilities c     = FakeBackend::capabilities();
        c.messageReminders = reminders;
        return c;
    }
    void downloadFile(const std::string &url, std::string toPath, Done done) override {
        asked.push_back({url, toPath});
        const size_t at = url.find(".test/");
        std::string  body;
        const bool   ok = at != std::string::npos &&
                          file::readAll(asset(url.substr(at + 6).c_str()), &body) &&
                          file::writeAtomic(toPath, body);
        app().platform().post([done, ok] { done(ok, ok ? "" : "http 404"); });
    }
};

struct Env {
    model::Store                        store;
    DownloadingBackend                  backend{store, app().platform()};
    ImageCache                          images{app().platform()};
    Context                             ctx{app(), store, backend, images};
    std::unique_ptr<ui::Window>         win;
    MessageList                        *list = nullptr;
    std::vector<std::pair<ConvRef, Ts>> threads;

    explicit Env(bool fixture) {
        app();
        ctx.openThread = [this](ConvRef c, Ts t) { threads.push_back({c, t}); };
        if (fixture) {
            backend.setFixture(MSGA_TEST_ASSETS, 0);
            bool done = false;
            backend.connect([&](bool ok, const std::string &) { done = ok; });
            until([&] { return done; });
        }
        plat::WindowDesc d;
        d.size = {800, 600};
        win    = std::make_unique<ui::Window>(d);
        list   = win->root().add<MessageList>(ctx);
        pump();
    }
    ~Env() { win.reset(); } // views before the store and cache they observe
    ConvRef   conv(const char *id) const { return store.findConversation(id); }
    // The live row showing message ts, or null.
    ui::View *row(Ts ts) {
        const auto &items = list->items();
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].ts == ts && items[i].kind == Kind::Message)
                return list->list().viewFor(int(i));
        return nullptr;
    }
};

// A hand-made conversation for layout-free item checks.
ConvRef addConv(model::Store &st, std::vector<model::Message> msgs) {
    model::User me;
    me.id          = "U1";
    me.displayName = "Me";
    st.me          = st.addUser(me);
    model::User mira;
    mira.id          = "U2";
    mira.displayName = "Mira";
    st.addUser(mira);
    model::Conversation c;
    c.id              = "C1";
    c.name            = "design";
    c.hasMoreBefore   = false;
    const ConvRef ref = st.addConversation(std::move(c));
    st.addPage(ref, std::move(msgs));
    return ref;
}

model::Message msg(model::UserRef u, int64_t secs, const char *text) {
    model::Message m;
    m.user = u;
    m.ts   = secs * 1000000;
    m.text = text;
    return m;
}

ui::View *findByTooltip(ui::View *v, std::string_view needle) {
    if (v->tooltip().find(needle) != std::string::npos)
        return v;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (ui::View *f = findByTooltip(v->child(i), needle))
            return f;
    return nullptr;
}

} // namespace

TEST("image: decodes off the UI thread, scales, masks circles, evicts by bytes") {
    ImageCache                cache(app().platform());
    const ImageCache::Request r{asset("avatars/mira.png"), 64, 64, ImageCache::Shape::Circle};
    CHECK(cache.get(r) == nullptr); // never blocks
    CHECK(cache.pending() == 1);
    REQUIRE(until([&] { return cache.pending() == 0; }));
    ImageCache::Bitmap b = cache.get(r);
    REQUIRE(b != nullptr);
    CHECK(b->width() == 64 && b->height() == 64);
    CHECK((b->pixels()[0] >> 24) == 0);               // the corner is masked out
    CHECK((b->pixels()[32 * 64 + 32] >> 24) == 0xff); // the middle is opaque
    CHECK(cache.bytes() == 64 * 64 * 4);
    const ImageCache::Request bad{asset("fixture.json"), 10, 10};
    cache.get(bad);
    REQUIRE(until([&] { return cache.pending() == 0; }));
    CHECK(cache.failed(bad) && cache.get(bad) == nullptr);
    // Animated GIF: all frames.
    const ImageCache::Request gif{asset("gifs/party-confetti.gif"), 80, 60};
    cache.frames(gif);
    REQUIRE(until([&] { return cache.pending() == 0; }));
    ImageCache::Frames f = cache.frames(gif);
    REQUIRE(f != nullptr);
    CHECK(f->size() > 1 && (*f)[0].frame.width() == 80);
    // Eviction: a budget of one avatar keeps the newest.
    cache.setBudget(64 * 64 * 4);
    CHECK(cache.bytes() <= 64 * 64 * 4);
    CHECK(b->width() == 64); // handed-out bitmaps survive eviction
    int w = 0, h = 0;
    CHECK(cache.naturalSize(asset("avatars/mira.png"), &w, &h) && w == 256 && h == 256);
}

TEST("image: CachedImage shows a placeholder, then the image, without blocking") {
    Env   e(false);
    auto *img = e.win->root().add<CachedImage>(
        e.images, asset("avatars/jonas.png"), ImageCache::Shape::Rounded, 6
    );
    img->style().size(36, 36).alignSelf(ui::Align::Start);
    pump();
    REQUIRE(until([&] { return e.images.pending() == 0; }));
    pump();
    uint32_t px = 0;
    REQUIRE(app().platform().testHooks()->readPixel(e.win->native(), 18, 18, &px));
    CHECK(px != ui::color(ui::C::Border)); // no longer the placeholder
}

TEST("image: a lookup of a cached picture allocates nothing; eviction is least recent first") {
    ImageCache        cache(app().platform());
    // Many entries: a lookup is a hash probe, not a walk over them.
    const std::string mira = asset("avatars/mira.png"), jonas = asset("avatars/jonas.png");
    for (int px = 8; px < 208; ++px)
        cache.get(ImageCache::Ref{mira, px, px});
    REQUIRE(until([&] { return cache.pending() == 0; }, 10000));
    CHECK(cache.entryCount() == 200);
    const ImageCache::Ref r{mira, 64, 64};
    REQUIRE(cache.get(r) != nullptr);
    const size_t before = testAllocs();
    countAllocs(true);
    bool all = true;
    for (int i = 0; i < 1000; ++i)
        all &= cache.get(ImageCache::Ref{mira, 8 + i % 200, 8 + i % 200}) != nullptr;
    countAllocs(false);
    CHECK(all);
    CHECK(testAllocs() == before);
    // Least recently used goes first: touch 64 px, then shrink the budget
    // to two entries' worth.
    const size_t two = size_t(64 * 64 + 207 * 207) * 4;
    cache.get(r);
    cache.get(ImageCache::Ref{mira, 207, 207});
    cache.setBudget(two);
    CHECK(cache.bytes() <= two);
    CHECK(cache.get(r) != nullptr);
    CHECK(cache.get(ImageCache::Ref{mira, 207, 207}) != nullptr);
    CHECK(cache.get(ImageCache::Ref{mira, 100, 100}) == nullptr); // evicted: decoding again
    REQUIRE(until([&] { return cache.pending() == 0; }));
    CHECK(cache.bytes() <= two);
    (void)jonas;
}

TEST("image: CachedImage keeps its ready picture; an eviction doesn't blank or re-decode it") {
    Env   e(false);
    auto *img = e.win->root().add<CachedImage>(
        e.images, asset("avatars/jonas.png"), ImageCache::Shape::Rounded, 6
    );
    img->style().size(36, 36).alignSelf(ui::Align::Start);
    pump();
    REQUIRE(until([&] { return e.images.pending() == 0; }));
    pump();
    uint32_t shown = 0;
    REQUIRE(app().platform().testHooks()->readPixel(e.win->native(), 18, 18, &shown));
    CHECK(shown != ui::color(ui::C::Border));
    e.images.setBudget(0); // everything evicted
    CHECK(e.images.bytes() == 0);
    img->update();
    pump();
    CHECK(e.images.pending() == 0); // no decode asked for again
    uint32_t px = 0;
    REQUIRE(app().platform().testHooks()->readPixel(e.win->native(), 18, 18, &px));
    CHECK(px == shown);
    e.images.setBudget(size_t(48) << 20);
}

TEST("image: a resized CachedImage paints its picture until the new size is there") {
    Env   e(false);
    auto *img = e.win->root().add<CachedImage>(
        e.images, asset("avatars/jonas.png"), ImageCache::Shape::Rounded, 6
    );
    img->style().size(60, 60).alignSelf(ui::Align::Start);
    pump();
    REQUIRE(until([&] { return e.images.pending() == 0; }));
    pump();
    const size_t entries = e.images.entryCount();
    uint32_t     px      = 0;
    // A few pixels (a window dragged wider): no new decode until the size
    // has held still, the old picture painted meanwhile.
    img->style().size(62, 62);
    pump();
    CHECK(e.images.entryCount() == entries);
    REQUIRE(app().platform().testHooks()->readPixel(e.win->native(), 30, 30, &px));
    CHECK(px != ui::color(ui::C::Border));
    REQUIRE(until([&] { return e.images.entryCount() == entries + 1; }, 2000));
    REQUIRE(until([&] { return e.images.pending() == 0; }));
    // More than a tenth: asked for at once, still no placeholder.
    img->style().size(120, 120);
    pump();
    CHECK(e.images.entryCount() == entries + 2);
    REQUIRE(app().platform().testHooks()->readPixel(e.win->native(), 30, 30, &px));
    CHECK(px != ui::color(ui::C::Border));
    REQUIRE(until([&] { return e.images.pending() == 0; }));
}

TEST("image: natural sizes are memoised, from decodes too, with a negative entry for URLs") {
    {
        RemoteImages remote(app().platform(), nullptr, ""); // no disk: URLs are never there
        ImageCache   cache(app().platform());
        cache.setRemote(&remote);
        int w = 0, h = 0;
        // A file: one header read, then the memo.
        CHECK(cache.naturalSize(asset("avatars/sam.png"), &w, &h) && w > 0 && h > 0);
        CHECK(cache.sizeProbes() == 1);
        CHECK(cache.naturalSize(asset("avatars/sam.png"), &w, &h));
        CHECK(cache.sizeProbes() == 1);
        // Not an image: remembered as such.
        CHECK_FALSE(cache.naturalSize(asset("fixture.json"), &w, &h));
        CHECK_FALSE(cache.naturalSize(asset("fixture.json"), &w, &h));
        CHECK(cache.sizeProbes() == 2);
        // A URL not on disk: false, and not looked for again right away.
        CHECK_FALSE(cache.naturalSize("https://img.test/none.png", &w, &h));
        CHECK_FALSE(cache.naturalSize("https://img.test/none.png", &w, &h));
        CHECK(cache.sizeProbes() == 3);
        // A picture the worker decoded: its size came along, no probe.
        cache.get(ImageCache::Ref{asset("avatars/alex.png"), 20, 20});
        REQUIRE(until([&] { return cache.pending() == 0; }));
        CHECK(cache.naturalSize(asset("avatars/alex.png"), &w, &h) && w > 0 && h > 0);
        CHECK(cache.sizeProbes() == 3);
    }
}

TEST("image: failed entries are bounded, oldest dropped first") {
    ImageCache cache(app().platform());
    for (int i = 0; i < 300; ++i)
        cache.get(ImageCache::Ref{"/no/such/image-" + std::to_string(i) + ".png", 8, 8});
    REQUIRE(until([&] { return cache.pending() == 0; }));
    CHECK(cache.entryCount() == 256);
    // The newest are still remembered as failed; the oldest are asked for again.
    CHECK(cache.failed(ImageCache::Ref{"/no/such/image-299.png", 8, 8}));
    CHECK_FALSE(cache.failed(ImageCache::Ref{"/no/such/image-0.png", 8, 8}));
}

TEST("image: avatar tiles take the letter's hue; hsl() is the one colour formula") {
    CHECK(hsl(0, 1, 0.5f) == 0xffff0000u);
    CHECK(hsl(120, 1, 0.5f) == 0xff00ff00u);
    CHECK(hsl(240, 1, 0.25f) == 0xff000080u);
    CHECK(hsl(300, 0, 0.5f) == 0xff808080u);
    // 'A': hue 65 · 37 mod 360 = 245, HSL(245, 130, 100) on 0-255 scales.
    CHECK(initialHue("A") == 0xff3a3197u);
    CHECK(initialHue("a") != initialHue("A"));
}

TEST("image: animations decode at the asked size, frame by frame") {
    ImageCache                cache(app().platform());
    const ImageCache::Request r{asset("gifs/party-confetti.gif"), 80, 60};
    cache.frames(r);
    REQUIRE(until([&] { return cache.pending() == 0; }));
    ImageCache::Frames f = cache.frames(r);
    REQUIRE(f != nullptr);
    // Decoded at the asked size frame by frame: the cache holds frames × 80×60.
    CHECK(f->size() > 1);
    CHECK(cache.bytes() == f->size() * 80 * 60 * 4);
    for (const gfx::AnimFrame &a : *f)
        CHECK(a.frame.width() == 80 && a.frame.height() == 60);
}

TEST("grouping: same author within 5 minutes; roots and system lines stand alone") {
    Env                         e(false);
    const int64_t               t0 = base::nowSecs() - 3600;
    std::vector<model::Message> ms;
    ms.push_back(msg(0, t0, "one"));
    ms.push_back(msg(0, t0 + 60, "two (grouped)"));
    ms.push_back(msg(0, t0 + 400, "three (gap > 5 min from two)"));
    ms.push_back(msg(1, t0 + 420, "other author"));
    model::Message root = msg(1, t0 + 430, "a thread root");
    root.replyCount     = 2;
    ms.push_back(std::move(root));
    ms.push_back(msg(1, t0 + 440, "after a root"));
    ms.push_back(msg(1, t0 + 450, "grouped again"));
    model::Message join   = msg(1, t0 + 460, "<@U2> has joined the channel");
    join.extras().subtype = "channel_join";
    ms.push_back(std::move(join));
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump();
    const auto &it = e.list->items();
    REQUIRE(it.size() == 9);
    CHECK(it[0].kind == Kind::Day);
    CHECK(!it[1].grouped && it[2].grouped && !it[3].grouped && !it[4].grouped);
    CHECK(!it[5].grouped && !it[6].grouped && it[7].grouped);
    CHECK(it[8].kind == Kind::System);
    // A new message by the same author a minute later joins the group, live.
    e.store.addMessage(c, msg(1, t0 + 520, "live"));
    pump();
    CHECK(e.list->items().size() == 10);
    CHECK(e.list->items()[9].kind == Kind::Message && !e.list->items()[9].grouped);
    e.store.addMessage(c, msg(1, t0 + 530, "live 2"));
    pump();
    CHECK(e.list->items()[10].grouped);
    // An edit keeps the items; replies make "live" a root, which stands
    // alone and ends the group under it.
    e.store.updateMessage(c, (t0 + 520) * 1000000, [](model::Message &m) { m.text = "edited"; });
    pump();
    CHECK(e.list->items().size() == 11 && e.list->items()[10].grouped);
    e.store.updateMessage(c, (t0 + 520) * 1000000, [](model::Message &m) { m.replyCount = 1; });
    pump();
    CHECK(!e.list->items()[9].grouped && !e.list->items()[10].grouped);
    // Inline, a thread's replies group by the same rules.
    model::Message bot1   = msg(1, t0 + 600, "build ok");
    bot1.extras().botName = "CI";
    model::Message bot2   = msg(1, t0 + 610, "deploy ok");
    bot2.extras().botName = "CD";
    CHECK_FALSE(groupable(bot1, bot2));
}

TEST("grouping: the rules — system lines, bots, authors, GIF paths") {
    model::Store st;
    model::User  mira;
    mira.id                = "U2";
    mira.displayName       = "Mira";
    mira.avatar            = "/a/mira.png";
    const model::UserRef u = st.addUser(mira);
    model::User          bot;
    bot.id                       = "B1";
    bot.displayName              = "deploybot";
    bot.bot                      = true;
    const model::UserRef b       = st.addUser(bot);
    auto                 withBot = [](model::Message m, const char *name, const char *avatar) {
        m.extras().botName   = name;
        m.extras().botAvatar = avatar;
        return m;
    };
    // A person posting through an app stays that person; a bot (or a
    // message with no user) is its own name and picture.
    const model::Message viaApp = withBot(msg(u, 100, "hi"), "Zapier", "/z.png");
    CHECK_STR(std::string(authorName(st, viaApp)), "Mira");
    CHECK_STR(authorAvatar(st, viaApp), "/a/mira.png");
    const model::Message byBot = withBot(msg(b, 100, "hi"), "Deploys", "/d.png");
    CHECK_STR(std::string(authorName(st, byBot)), "Deploys");
    CHECK_STR(authorAvatar(st, byBot), "/d.png");
    CHECK(isBot(st, byBot) && !isBot(st, viaApp));
    const model::Message noUser = withBot(msg(model::kNoUser, 100, "hi"), "Webhook", "/w.png");
    CHECK_STR(std::string(authorName(st, noUser)), "Webhook");
    CHECK_STR(authorAvatar(st, noUser), "/w.png");
    CHECK_STR(std::string(authorName(st, msg(u, 100, "plain"))), "Mira");

    model::Message join   = msg(u, 100, "joined");
    join.extras().subtype = "channel_join";
    CHECK(isSystem(join));
    model::Message pin   = msg(u, 100, "pinned");
    pin.extras().subtype = "pinned_item";
    CHECK(isSystem(pin) && !isSystem(byBot) && !isSystem(msg(u, 1, "x")));
    // Grouping: one author (and bot name), within five minutes.
    CHECK(groupable(msg(u, 100, "a"), msg(u, 399, "b")));
    CHECK_FALSE(groupable(msg(u, 100, "a"), msg(u, 400, "b")));
    CHECK_FALSE(groupable(msg(u, 100, "a"), join));
    CHECK_FALSE(
        groupable(withBot(msg(b, 100, "a"), "One", ""), withBot(msg(b, 101, "b"), "Two", ""))
    );
    model::Message root = msg(u, 100, "root");
    root.replyCount     = 1;
    CHECK_FALSE(groupable(root, msg(u, 101, "b")));

    CHECK(isGifPath("/x/party.gif"));
    CHECK(isGifPath("/x/PARTY.GIF"));
    CHECK(isGifPath("https://media.giphy.com/a/giphy.Gif?cid=1&rid=giphy.webp"));
    CHECK_FALSE(isGifPath("https://e/a.png?x=.gif"));
    CHECK_FALSE(isGifPath("/x/gif"));
}

TEST("users: a presence flip re-binds no row; a name re-binds only the rows showing it") {
    Env                         e(false);
    const int64_t               t0 = base::nowSecs() - 3600;
    std::vector<model::Message> ms;
    ms.push_back(msg(0, t0, "one :party: for <!subteam^S1>"));
    ms.push_back(msg(0, t0 + 400, "hi <@U3> in <#C9>"));
    ms.push_back(msg(1, t0 + 800, "two :partying: <!subteam^S2>"));
    ms.back().reactions.push_back({"cake::skin-tone-2", 1, {}});
    const ConvRef c = addConv(e.store, std::move(ms));
    e.store.setUsergroups({{"S1", "design", "Design", {}}, {"S2", "ops", "Ops", {}}});
    model::User lena;
    lena.id                 = "U3";
    lena.displayName        = "Lena";
    const model::UserRef u3 = e.store.addUser(lena);
    e.list->showConversation(c);
    e.store.usersChanged(); // the first Users after showing: everything once
    pump(8);
    REQUIRE(e.list->items().size() == 4);
    const int       binds = e.list->rowBinds();
    const ui::View *two   = e.row(e.list->items()[3].ts);
    REQUIRE(two != nullptr);

    // Presence and DND aren't drawn in message rows: nothing re-binds,
    // however many flips the burst holds.
    for (int i = 0; i < 5; ++i) {
        e.store.user(1).active = !e.store.user(1).active;
        e.store.user(1).dnd    = !e.store.user(1).dnd;
        e.store.usersChanged();
    }
    pump(8);
    CHECK(e.list->rowBinds() == binds);
    CHECK(e.row(e.list->items()[3].ts) == two); // the same row object

    // Mira's name: only her row.
    e.store.user(1).displayName = "Mira O.";
    e.store.usersChanged();
    pump(8);
    CHECK(e.list->rowBinds() == binds + 1);
    // Lena, mentioned in a message: only that one.
    e.store.user(u3).displayName = "Lena W.";
    e.store.usersChanged();
    pump(8);
    CHECK(e.list->rowBinds() == binds + 2);
    // A text change: only the rows that draw what changed. A custom emoji
    // (":party:", not ":partying:"); an alias of it.
    e.store.setCustomEmoji("party", "https://e/p.png");
    e.store.usersChanged();
    pump(8);
    CHECK(e.list->rowBinds() == binds + 3);
    e.store.setCustomEmoji("partying", "alias:party");
    e.store.usersChanged();
    pump(8);
    CHECK(e.list->rowBinds() == binds + 4);
    e.store.setCustomEmoji("party", "https://e/p2.png"); // the alias's row too
    e.store.usersChanged();
    pump(8);
    CHECK(e.list->rowBinds() == binds + 6);
    // A reaction's emoji (any skin tone).
    e.store.setCustomEmoji("cake", "https://e/c.png");
    e.store.usersChanged();
    pump(8);
    CHECK(e.list->rowBinds() == binds + 7);
    // A user group's handle.
    e.store.setUsergroups({{"S1", "design", "Design", {}}, {"S2", "sre", "Ops", {}}});
    pump(8);
    CHECK(e.list->rowBinds() == binds + 8);
    // The name of a channel the roster doesn't list.
    e.store.setChannelName("C9", "launch");
    pump(8);
    CHECK(e.list->rowBinds() == binds + 9);
    // Emoji gone (their rows show ":code:" again): only those rows.
    e.store.replaceCustomEmoji({{"cake", "https://e/c.png"}});
    e.store.usersChanged();
    pump(8);
    CHECK(e.list->rowBinds() == binds + 11);
    // More than can be listed (a first load): everything.
    std::unordered_map<std::string, std::string> many;
    for (int i = 0; i < 200; ++i)
        many["e" + std::to_string(i)] = "https://e/x.png";
    e.store.replaceCustomEmoji(std::move(many));
    e.store.usersChanged();
    pump(8);
    CHECK(e.list->rowBinds() == binds + 11 + 3);
}

TEST("actions: Copy message is the text as read, with full URLs, and asks the backend nothing") {
    Env                         e(false);
    const int64_t               t0 = base::nowSecs() - 3600;
    std::vector<model::Message> ms;
    ms.push_back(
        msg(0,
            t0,
            "*hi* <@U2> in <#C7> :thumbsup::skin-tone-3: "
            "<https://example.com/a/b/c/d|example.com/a/…/d>")
    );
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump(8);
    e.backend.resolved.clear();
    const Ts ts = e.list->items().back().ts;
    e.list->runMenuAction(ts, MessageList::kCopyText);
    std::string got;
    bool        done = false;
    app().platform().requestClipboard(
        "text/plain;charset=utf-8", [&](std::optional<std::string> s) {
            got  = s.value_or("");
            done = true;
        }
    );
    REQUIRE(until([&] { return done; }));
    CHECK_STR(got, "hi @Mira in #C7 \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBC https://example.com/a/b/c/d");
    CHECK(e.backend.resolved.empty());
    // Elsewhere the label stays as Slack wrote it.
    CHECK_STR(
        plainText(e.store, "<https://example.com/a/b/c/d|example.com/a/…/d>"), "example.com/a/…/d"
    );
}

TEST("rows: a mention shows the display name in either Names mode") {
    model::Store s;
    model::User  meg, tom;
    meg.id = "UMEG", meg.name = "meg", meg.realName = "Meg Ryan", meg.profileName = "Meg";
    tom.id = "UTOM", tom.name = "tom", tom.realName = "Tom Hanks"; // no display name
    s.addUser(meg);
    s.addUser(tom);
    for (bool real : {true, false}) {
        s.setRealNames(real);
        CHECK_STR(plainText(s, "<@UMEG> and <@UTOM>"), "@Meg and @Tom Hanks");
    }
    CHECK_STR(s.user(s.findUser("UMEG")).label(), "Meg"); // the author line follows the mode
}

TEST("rows: scrolling back over seen messages rebuilds nothing; edits and reactions do") {
    Env                         e(false);
    const int64_t               t0 = base::nowSecs() - 7200;
    std::vector<model::Message> ms;
    for (int i = 0; i < 60; ++i)
        ms.push_back(
            msg(model::UserRef(i % 2),
                t0 + i * 61,
                i % 3 ? "*bold* and _italic_ with <https://x.test|a link>" : "plain words")
        );
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump(8);
    ui::VirtualList &l = e.list->list();
    REQUIRE(l.pinned());
    // Up a screen and a half (those rows built once), then back down —
    // twice: rows built in the overscan paint (avatar letters) on the next.
    Ts seen = 0;
    for (int i = 0; i < 2; ++i) {
        l.scrollTo(l.scrollOffset() - 900);
        pump(8);
        seen = e.list->items()[size_t(l.firstVisible() + 2)].ts;
        REQUIRE(e.row(seen) != nullptr);
        l.scrollToBottom();
        pump(8);
        REQUIRE(e.row(seen) == nullptr);
    }

    // Back up and down again: no bind (so no mrkdwn parse), no shaping.
    const int    binds  = e.list->rowBinds();
    const size_t shaped = text::layoutBuilds();
    l.scrollTo(l.scrollOffset() - 900);
    pump(8);
    CHECK(e.row(seen) != nullptr);
    l.scrollToBottom();
    pump(8);
    CHECK(e.list->rowBinds() == binds);
    CHECK(text::layoutBuilds() == shaped);

    // Edited and reacted to while off screen: those two rows, rebuilt.
    e.store.updateMessage(c, seen, [](model::Message &m) {
        m.text   = "edited words";
        m.edited = true;
    });
    const Ts other = seen + 61 * 1000000;
    e.store.setReaction(c, other, "tada", 1, true);
    pump(8);
    CHECK(e.list->rowBinds() == binds);
    l.scrollTo(l.scrollOffset() - 900);
    pump(8);
    CHECK(e.list->rowBinds() == binds + 2);
    auto *edited = static_cast<MessageRow *>(e.row(seen));
    REQUIRE(edited != nullptr && !edited->selectionLabels().empty());
    CHECK(
        static_cast<RichLabel *>(edited->selectionLabels()[0])->text().find("edited words") !=
        std::string::npos
    );
    // A name change re-binds the rows showing it, kept ones too.
    l.scrollToBottom();
    pump(8);
    e.store.user(1).displayName = "Mira O.";
    e.store.usersChanged();
    pump(8);
    const int named = e.list->rowBinds();
    CHECK(named > binds + 2);
    l.scrollTo(l.scrollOffset() - 900);
    pump(8);
    CHECK(e.list->rowBinds() > named);
}

TEST("rows: a reaction keeps the row's body as built; an edit or a name rebuilds it") {
    Env                         e(false);
    const int64_t               t0 = base::nowSecs() - 3600;
    std::vector<model::Message> ms;
    ms.push_back(msg(1, t0, "*bold* words with <https://x.test|a link>"));
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump(8);
    const Ts ts  = e.list->items().back().ts;
    auto    *row = static_cast<MessageRow *>(e.row(ts));
    REQUIRE(row != nullptr && !row->selectionLabels().empty());
    // A mark only the body as built carries.
    const auto marked = [&] {
        auto *r = static_cast<MessageRow *>(e.row(ts));
        return r && !r->selectionLabels().empty() &&
               static_cast<RichLabel *>(r->selectionLabels()[0])->style().maxW == 123456.f;
    };
    static_cast<RichLabel *>(row->selectionLabels()[0])->style().maxW = 123456.f;
    const int binds                                                   = e.list->rowBinds();

    // Reactions, replies, a pin: bound again, the body kept.
    e.store.setReaction(c, ts, "tada", 1, true);
    pump(8);
    CHECK(e.list->rowBinds() == binds + 1);
    CHECK(marked());
    e.store.updateMessage(c, ts, [](model::Message &m) { m.pinned = true; });
    pump(8);
    CHECK(marked());
    e.store.setReaction(c, ts, "tada", 1, false);
    pump(8);
    CHECK(e.list->rowBinds() == binds + 3);
    CHECK(marked());

    // The author's name: everything again.
    e.store.user(1).displayName = "Mira O.";
    e.store.usersChanged();
    pump(8);
    CHECK_FALSE(marked());
    static_cast<RichLabel *>(static_cast<MessageRow *>(e.row(ts))->selectionLabels()[0])
        ->style()
        .maxW = 123456.f;
    // An edit: the body too.
    e.store.updateMessage(c, ts, [](model::Message &m) {
        m.text   = "edited words";
        m.edited = true;
    });
    pump(8);
    CHECK_FALSE(marked());
    row = static_cast<MessageRow *>(e.row(ts));
    REQUIRE(row != nullptr && !row->selectionLabels().empty());
    CHECK(
        static_cast<RichLabel *>(row->selectionLabels()[0])->text().find("edited words") !=
        std::string::npos
    );
}

TEST("dates: separators say Today, Yesterday, then the date") {
    Env                   e(false);
    const int64_t         now = base::nowSecs();
    const base::CivilTime lt  = base::localTime(now);
    auto                  at  = [&](int daysAgo, int hour) {
        return base::fromLocal(lt.year, lt.month, lt.day - daysAgo, hour, 0);
    };
    std::vector<model::Message> ms;
    ms.push_back(msg(0, at(9, 10), "old"));
    ms.push_back(msg(0, at(1, 10), "yesterday"));
    ms.push_back(msg(1, at(1, 11), "yesterday later"));
    ms.push_back(msg(0, std::min(now - 60, at(0, 0) + 60), "today"));
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump();
    std::vector<std::string> days;
    for (size_t i = 0; i < e.list->items().size(); ++i)
        if (e.list->items()[i].kind == Kind::Day)
            days.push_back(e.list->itemLabel(i));
    REQUIRE(days.size() == 3);
    CHECK_STR(days[1], "Yesterday");
    CHECK_STR(days[2], "Today");
    CHECK(days[0] != "Today" && days[0] != "Yesterday" && !days[0].empty());
}

TEST("anchoring: live appends keep the bottom pinned; scrolled up, the viewport stays") {
    Env           e(true);
    const ConvRef dm = e.conv("D0MIRA");
    REQUIRE(dm != model::kNoConv);
    e.list->showConversation(dm);
    pump(10);
    CHECK(e.list->list().pinned());
    // Send: the pending copy appears at once, then the canned reply after
    // "Mira is typing…".
    e.backend.send(dm, "Are we still on for the crit?", 0, nullptr);
    pump(4);
    const auto &msgs = e.store.conversation(dm).messages;
    REQUIRE(!msgs.empty());
    const Ts mine = msgs.back().ts;
    CHECK(e.store.user(msgs.back().user).id == "U0ALEX");
    REQUIRE(until([&] { return !e.list->typingText().empty(); }, 5000));
    CHECK(e.list->typingText().find("is typing") != std::string::npos);
    REQUIRE(until([&] { return e.store.conversation(dm).messages.back().ts != mine; }, 6000));
    pump(6);
    CHECK(e.list->typingText().empty());
    CHECK(e.list->list().pinned());
    ui::View *last = e.row(e.store.conversation(dm).messages.back().ts);
    REQUIRE(last != nullptr);
    CHECK(std::abs(last->frame().y + last->frame().h - e.list->list().height()) < 1);
    // Scrolled up in a channel longer than the viewport: an append must not
    // move what is on screen.
    const ConvRef design = e.conv("C0DESIGN");
    e.list->showConversation(design);
    pump(10);
    REQUIRE(e.list->list().canScroll());
    e.list->list().scrollBy(-120);
    pump(6);
    REQUIRE(!e.list->list().pinned());
    const int first = e.list->list().firstVisible();
    REQUIRE(first >= 0);
    ui::View   *v  = e.list->list().viewFor(first);
    const float y0 = v->frame().y;
    e.backend.postAs(design, e.store.findUser("U0MIRA"), "One more thing");
    pump(6);
    CHECK(e.list->list().viewFor(first) == v);
    CHECK(std::abs(v->frame().y - y0) < 0.5f);
}

TEST(
    "anchoring: jumpTo — a search hit scrolls into view and flashes, also once its first page lands"
) {
    Env           e(true);
    const ConvRef design = e.conv("C0DESIGN");
    const Ts      oldest = e.store.conversation(design).messages.front().ts;
    e.list->showConversation(design);
    pump(10);
    REQUIRE(e.list->list().canScroll());
    e.list->jumpTo(oldest);
    pump(4);
    CHECK(e.list->flashing(oldest));
    REQUIRE(until([&] { return e.row(oldest) != nullptr; }));
    // Not loaded yet (a jump that also switched conversations): the
    // jump waits for the first page.
    model::Conversation c;
    c.id                = "C0LATER";
    c.name              = "later";
    c.hasMoreBefore     = true;
    const ConvRef later = e.store.addConversation(std::move(c));
    e.list->showConversation(later);
    const model::UserRef mira = e.store.findUser("U0MIRA");
    e.list->jumpTo(5000 * 1000000LL);
    CHECK_FALSE(e.list->flashing(5000 * 1000000LL));
    std::vector<model::Message> page;
    page.push_back(msg(mira, 4000, "before"));
    page.push_back(msg(mira, 5000, "the hit"));
    e.store.addPage(later, std::move(page));
    pump(4);
    CHECK(e.list->flashing(5000 * 1000000LL));
    // A target the loaded list lacks is dropped, not kept for later.
    e.list->jumpTo(6000 * 1000000LL);
    e.store.addMessage(later, msg(mira, 6000, "arrives later"));
    pump(4);
    CHECK_FALSE(e.list->flashing(6000 * 1000000LL));
}

TEST("actions: a reaction pill toggles mine; the newest message on screen marks read") {
    Env           e(true);
    const ConvRef design = e.conv("C0DESIGN");
    e.store.updateConversation(design, [](model::Conversation &c) { c.lastRead = 0; });
    e.list->showConversation(design);
    pump(10);
    const auto &msgs = e.store.conversation(design).messages;
    REQUIRE(until([&] { return e.store.conversation(design).lastRead == msgs.back().ts; }));
    CHECK(!e.store.conversation(design).hasMoreBefore); // it asked for older history
    // The "+1" on Priya's empty-states message: Mira reacted, I have not.
    Ts target = 0;
    for (const model::Message &m : msgs)
        for (const model::Reaction &r : m.reactions)
            if (r.name == "+1" && !e.store.reactedByMe(r))
                target = m.ts;
    REQUIRE(target != 0);
    e.list->scrollToMessage(target, false, false);
    pump(8);
    ui::View *row = e.row(target);
    REQUIRE(row != nullptr);
    ui::View *pill = findByTooltip(row, "reacted with :+1:");
    REQUIRE(pill != nullptr);
    const ui::RectF r = pill->windowRect();
    auto           *h = app().platform().testHooks();
    h->injectPointerMove(e.win->native(), {r.x + r.w / 2, r.y + r.h / 2});
    h->injectButton(e.win->native(), plat::Button::Left, true);
    h->injectButton(e.win->native(), plat::Button::Left, false);
    pump(6);
    const model::Message *m = e.store.findMessage(design, target);
    REQUIRE(m != nullptr);
    bool mineNow = false;
    for (const model::Reaction &x : m->reactions)
        if (x.name == "+1")
            mineNow = e.store.reactedByMe(x);
    CHECK(mineNow);
    // The hover toolbar's "…" opens the message menu.
    e.list->openMenu(target, {200, 200});
    pump(4);
    CHECK(e.win->topPopup() != nullptr);
}

TEST("thread: an agent's thread of a link has a chip back to the Slack thread") {
    Env           e(true);
    const ConvRef design = e.conv("C0DESIGN");
    Ts            root   = 0;
    for (const model::Message &m : e.store.conversation(design).messages)
        if (m.replyCount > 0)
            root = m.ts;
    REQUIRE(root != 0);
    std::vector<Context::AgentLinkAction> actions;
    e.ctx.agentLinkSource = [&](ConvRef, Ts r) {
        return r == root ? std::string("#general in Lumen") : std::string();
    };
    e.ctx.agentLinkAction = [&](ConvRef, Ts r, Context::AgentLinkAction a) {
        CHECK(r == root);
        actions.push_back(a);
    };
    auto *panel = e.win->root().add<ThreadPanel>(e.ctx);
    panel->show(design, root);
    REQUIRE(panel->linkChip()->visible());
    CHECK_STR(panel->linkChipText(), "#general in Lumen");
    panel->linkChip()->activate();
    CHECK(actions == std::vector<Context::AgentLinkAction>{Context::AgentLinkAction::OpenSource});
    // A long name in a narrow panel: the chip gives way ("…", the whole
    // name in its tooltip), "Thread" stays one line at its own width.
    const std::string longName = "Vladimir Osipov in CityCity Consulting";
    e.ctx.agentLinkSource = [&](ConvRef, Ts r) { return r == root ? longName : std::string(); };
    panel->refreshLink();
    CHECK_STR(panel->linkChipText(), longName);
    CHECK(panel->linkChip()->tooltip().find(longName) != std::string::npos);
    panel->style().width(360);
    pump(4);
    ui::View   *title = panel->child(0)->child(0);
    const float tw    = title->measure(ui::kInf, ui::kInf).w;
    CHECK(title->width() >= tw - 1);
    CHECK(title->height() < 2 * title->measure(ui::kInf, ui::kInf).h);
    CHECK(panel->linkChip()->width() > 0);
    CHECK(panel->linkChip()->width() < panel->linkChip()->measure(ui::kInf, ui::kInf).w);
    // Another thread: none.
    e.ctx.agentLinkSource = [](ConvRef, Ts) { return std::string(); };
    panel->refreshLink();
    CHECK_FALSE(panel->linkChip()->visible());
}

TEST("thread: day dividers, the root and its replies; the summary opens the thread") {
    Env           e(true);
    const ConvRef design = e.conv("C0DESIGN");
    Ts            root   = 0;
    for (const model::Message &m : e.store.conversation(design).messages)
        if (m.replyCount > 0)
            root = m.ts;
    REQUIRE(root != 0);
    auto *panel = e.win->root().add<ThreadPanel>(e.ctx);
    panel->show(design, root);
    REQUIRE(until([&] { return panel->list().items().size() >= 3; }));
    // The thread view: a day divider above the root, no reply-count row.
    const auto messages = [&] {
        size_t n = 0;
        for (const auto &x : panel->list().items()) {
            CHECK(x.kind != Kind::Divider);
            n += x.kind == Kind::Message;
        }
        return n;
    };
    const auto &it = panel->list().items();
    CHECK(it[0].kind == Kind::Day && it[1].ts == root);
    const size_t replies = e.store.findMessage(design, root)->replyCount;
    CHECK(messages() == 1 + replies);
    auto      slot = std::make_unique<ui::TextEdit>();
    ui::View *c    = panel->setComposer(std::move(slot));
    CHECK(c && panel->composer() == c);
    // A live reply lands in the panel.
    e.backend.postAs(design, e.store.findUser("U0MIRA"), "late reply", root);
    pump(6);
    CHECK(messages() == 2 + replies);
    // In the channel, the root's summary asks the shell to open the thread.
    panel->setVisible(false); // it was stacked over the list
    e.list->showConversation(design);
    e.list->scrollToMessage(root, false, false);
    pump(8);
    ui::View *row = e.row(root);
    REQUIRE(row != nullptr);
    std::function<ui::View *(ui::View *)> findReplies = [&](ui::View *v) -> ui::View * {
        if (v->accessibleName().find("replies") != std::string::npos && v->parent() &&
            v->parent()->role() == ui::Role::Button)
            return v->parent();
        for (size_t i = 0; i < v->childCount(); ++i)
            if (ui::View *f = findReplies(v->child(i)))
                return f;
        return nullptr;
    };
    ui::View *bar = findReplies(row);
    REQUIRE(bar != nullptr);
    const ui::RectF r = bar->windowRect();
    auto           *h = app().platform().testHooks();
    h->injectPointerMove(e.win->native(), {r.x + r.w - 6, r.y + r.h / 2});
    h->injectButton(e.win->native(), plat::Button::Left, true);
    h->injectButton(e.win->native(), plat::Button::Left, false);
    pump(4);
    REQUIRE(e.threads.size() == 1);
    CHECK(e.threads[0].second == root);
}

namespace {

std::string exportPath(const char *name) {
    const char *t = std::getenv("TMPDIR");
    return file::join(
        t && *t ? t : "/tmp",
        std::string("msga-test-") + name + "-" + std::to_string(base::nowMicros()) + "-" +
            std::to_string(std::random_device()()) // distinct across parallel runs too + ".txt"
    );
}

Ts threadRoot(Env &e, ConvRef c) {
    for (const model::Message &m : e.store.conversation(c).messages)
        if (m.replyCount > 0)
            return m.ts;
    return 0;
}

} // namespace

TEST("thread: download fetches the thread and writes it in the background, past the panel") {
    Env           e(true);
    const ConvRef design = e.conv("C0DESIGN");
    const Ts      root   = threadRoot(e, design);
    REQUIRE(root != 0);
    auto *panel = e.win->root().add<ThreadPanel>(e.ctx);
    panel->show(design, root);
    pump(4);
    const size_t      jobsBefore = model::jobs().count();
    const std::string path       = exportPath("thread");
    int               calls      = 0;
    bool              ok         = false;
    exportThread(e.ctx, design, root, threadExportTitle(e.store, design), path, [&](bool o) {
        ++calls;
        ok = o;
    });
    // From the click: the job shows, nothing is written inside the call.
    CHECK(calls == 0);
    CHECK(model::jobs().count() == jobsBefore + 1);
    const std::vector<std::string> d = model::jobs().descriptions();
    CHECK(!d.empty() && d.back() == "Downloading thread…");
    // The panel goes to another thread and closes; the export carries on.
    panel->show(design, 0);
    e.win->root().clearChildren();
    REQUIRE(until([&] { return calls > 0; }));
    CHECK(calls == 1);
    CHECK(ok);
    CHECK(model::jobs().count() == jobsBefore);
    std::string out;
    REQUIRE(file::readAll(path, &out));
    const auto *replies = e.store.replies(design, root);
    REQUIRE(replies && !replies->empty());
    CHECK(out.rfind("Thread in #design\n", 0) == 0);
    CHECK(
        out.find(str::concat({"Messages: ", str::number(int64_t(1 + replies->size())), "\n\n"})) !=
        std::string::npos
    );
    const model::Message *r = e.store.findMessage(design, root);
    REQUIRE(r);
    // The root first, right under the header.
    const std::string who = r->extra && !r->extra->botName.empty()
                                ? r->extra->botName
                                : std::string(e.store.user(r->user).label());
    CHECK(out.find(who + " \xE2\x80\x94 ") == out.find("\n\n") + 2);
    file::remove(path);
}

TEST("thread: a failed download writes nothing and ends its job") {
    Env               e(true);
    const ConvRef     design     = e.conv("C0DESIGN");
    const size_t      jobsBefore = model::jobs().count();
    const std::string path       = exportPath("thread-fail");
    int               calls      = 0;
    bool              ok         = true;
    // No such thread: the fetch fails, so there is no (truncated) file.
    exportThread(e.ctx, design, 12345, "#design", path, [&](bool o) {
        ++calls;
        ok = o;
    });
    CHECK(model::jobs().count() == jobsBefore + 1);
    REQUIRE(until([&] { return calls > 0; }));
    CHECK(!ok);
    CHECK(!file::exists(path));
    CHECK(model::jobs().count() == jobsBefore);
    pump(10);
    CHECK(calls == 1);
}

TEST("thread: a download ends when the workspace goes before the replies arrive") {
    Env           e(true);
    const ConvRef design = e.conv("C0DESIGN");
    const Ts      root   = threadRoot(e, design);
    REQUIRE(root != 0);
    const size_t      jobsBefore = model::jobs().count();
    const std::string path       = exportPath("thread-switch");
    int               calls      = 0;
    bool              ok         = true;
    exportThread(e.ctx, design, root, "#design", path, [&](bool o) {
        ++calls;
        ok = o;
    });
    e.store.clear(); // signed out / another workspace
    CHECK(model::jobs().count() == jobsBefore);
    REQUIRE(until([&] { return calls > 0; }));
    CHECK(!ok);
    pump(20); // the backend's late answer changes nothing
    CHECK(calls == 1);
    CHECK(!file::exists(path));
}

namespace {

bool near(float a, float b, float eps = 0.5f) {
    return a > b - eps && a < b + eps;
}

std::string menuLabels(const std::vector<ui::MenuItem> &items) {
    std::string out;
    for (const ui::MenuItem &it : items) {
        if (!out.empty())
            out += '|';
        out += it.separator ? std::string("-") : it.label;
        if (!it.enabled && it.label == "Preview")
            out += "(off)";
    }
    return out;
}

std::string clipboard(const char *mime = "text/plain;charset=utf-8") {
    std::string out;
    bool        done = false;
    app().platform().requestClipboard(mime, [&](std::optional<std::string> s) {
        out  = s.value_or("");
        done = true;
    });
    until([&] { return done; });
    return out;
}

} // namespace

TEST("actions: the message menu, item for item, acts through the backend") {
    Env           e(true);
    const ConvRef eng = e.conv("C0ENG");
    e.list->showConversation(eng);
    pump(10);
    Ts linked = 0;
    for (const model::Message &m : e.store.conversation(eng).messages)
        if (m.text.find("<https://gitforge.dev/lumen/atlas-web/pull/1482|") != std::string::npos)
            linked = m.ts;
    REQUIRE(linked != 0);
    // message_list.cpp showContextMenu for someone else's message in Slack.
    CHECK_STR(
        menuLabels(e.list->menuItems(linked)),
        "Reply in thread|-|Copy link|Copy link from message|Copy message|-|Pin to channel|"
        "Save for later|Remind me|-|Forward message|Summarize down"
    );
    const auto items = e.list->menuItems(linked);
    CHECK_STR(items[0].hint, "T");
    CHECK_STR(items[2].hint, "L");
    CHECK_STR(items[4].hint, "Ctrl+C");
    CHECK_STR(items[6].hint, "P");
    CHECK(items[0].icon == uint16_t(gfx::Icon::MessageSquareReply));
    CHECK(items[8].icon == uint16_t(gfx::Icon::AlarmClock));
    CHECK(!items[10].enabled && !items[11].enabled); // no forwarding hook / AI provider here

    using M = MessageList;
    e.list->runMenuAction(linked, M::kCopyLinkInText);
    CHECK_STR(clipboard(), "https://gitforge.dev/lumen/atlas-web/pull/1482");
    e.list->runMenuAction(linked, M::kCopyLink);
    CHECK_STR(clipboard(), e.store.permalink(eng, linked));
    e.list->runMenuAction(linked, M::kCopyText);
    CHECK(clipboard().rfind("PR for the new tokens is up: atlas-web#1482", 0) == 0);
    e.list->runMenuAction(linked, M::kPin);
    e.list->runMenuAction(linked, M::kSave);
    CHECK(e.store.findMessage(eng, linked)->pinned && e.store.findMessage(eng, linked)->saved);
    CHECK(
        menuLabels(e.list->menuItems(linked)).find("Unpin from channel|Remove from saved|") !=
        std::string::npos
    );
    // Remind me: the presets; a reminder replaces "Remove from saved".
    CHECK_STR(
        menuLabels(MessageList::remindItems()),
        "Remind me about this…|In 20 minutes|In 1 hour|In 3 hours|Tomorrow|Next week|-|Custom…"
    );
    e.list->runMenuAction(linked, M::kRemindPreset + 1); // in 1 hour
    const int64_t due = e.store.reminderAt(eng, linked);
    CHECK(due > base::nowSecs() + 3500 && due < base::nowSecs() + 3700);
    CHECK(
        menuLabels(e.list->menuItems(linked)).find("Unpin from channel|Remove reminder|-|") !=
        std::string::npos
    );
    e.list->runMenuAction(linked, M::kRemoveReminder);
    CHECK(e.store.reminderAt(eng, linked) == 0);
    CHECK_FALSE(e.store.findMessage(eng, linked)->saved);

    // My own message: Edit (when the shell provides it) and Delete.
    bool sent = false;
    e.backend.send(eng, "mine", 0, [&](bool ok, const std::string &) { sent = ok; });
    REQUIRE(until([&] { return sent; }));
    const Ts mine    = sentTs(e.store, eng, "mine");
    Ts       editing = 0;
    e.list->onEdit   = [&](ConvRef, Ts ts) { editing = ts; };
    CHECK_STR(
        menuLabels(e.list->menuItems(mine)),
        "Reply in thread|-|Edit message|-|Copy link|Copy message|-|Pin to channel|"
        "Save for later|Remind me|-|Forward message|Move to thread…|Summarize down|-|"
        "Delete message…"
    );
    e.list->runMenuAction(mine, M::kEdit);
    CHECK(editing == mine);
    e.list->runMenuAction(mine, M::kDelete); // the confirmation first
    auto *dlg = static_cast<ui::Dialog *>(e.win->topPopup());
    REQUIRE(dlg != nullptr);
    dlg->accept();
    pump(2);
    CHECK(e.store.findMessage(eng, mine) == nullptr);

    // A thread's messages can mute it (Mute thread).
    Ts root = 0;
    for (const model::Message &m : e.store.conversation(eng).messages)
        if (m.replyCount > 0)
            root = m.ts;
    if (root) {
        CHECK(menuLabels(e.list->menuItems(root)).rfind("Reply in thread|Mute thread|-|", 0) == 0);
        e.list->runMenuAction(root, M::kMuteThread);
        CHECK(e.store.threadMuted(eng, root));
    }
}

TEST("actions: Save for later and Remind me need messageReminders (session tokens)") {
    // An OAuth workspace has no saved.* — no items, no toolbar Save.
    Env           e(true);
    const ConvRef eng = e.conv("C0ENG");
    e.list->showConversation(eng);
    pump(10);
    const Ts ts = e.store.conversation(eng).messages.back().ts;
    CHECK(menuLabels(e.list->menuItems(ts)).find("Save for later|Remind me") != std::string::npos);
    e.backend.reminders      = false;
    const std::string labels = menuLabels(e.list->menuItems(ts));
    CHECK(labels.find("Save for later") == std::string::npos);
    CHECK(labels.find("Remind me") == std::string::npos);
    CHECK(labels.find("Pin to channel") != std::string::npos);
}

TEST("actions: a message link is a chip; a click jumps there or opens the browser") {
    Env           e(false);
    const ConvRef c = addConv(e.store, {});
    model::User   jo;
    jo.id                     = "U3";
    jo.displayName            = "Jo";
    const auto          joRef = e.store.addUser(jo);
    model::Conversation dm;
    dm.id     = "D1";
    dm.kind   = model::ConvKind::Dm;
    dm.dmUser = e.store.findUser("U2");
    e.store.addConversation(std::move(dm));
    model::Conversation g;
    g.id   = "G1";
    g.name = "mpdm-me--mira--jo-1";
    g.kind = model::ConvKind::Group;
    e.store.addConversation(std::move(g));
    auto ref = [](const char *conv, const char *ts, const char *thread = "") {
        return mrkdwn::MessageRef{"acme.slack.com", conv, ts, thread, ""};
    };
    // The chip's label: the place, the author when Slack named one.
    CHECK_STR(messageLinkLabel(e.store, ref("C1", "1.000001")), "#design");
    CHECK_STR(messageLinkLabel(e.store, ref("D1", "1.000001")), "Mira");
    CHECK_STR(messageLinkLabel(e.store, ref("G1", "1.000001")), "group message");
    CHECK_STR(messageLinkLabel(e.store, ref("C9", "1.000001")), "message");
    e.store.setLinkedAuthor("C1", "2.000002", joRef);
    CHECK_STR(messageLinkLabel(e.store, ref("C1", "2.000002")), "Jo in #design");
    CHECK_STR(messageLinkLabel(e.store, ref("C9", "2.000002")), "message");
    CHECK_STR(
        mrkdwn::messagePermalink(ref("C9", "1712.000100", "1711.000001")),
        "https://acme.slack.com/archives/C9/p1712000100?thread_ts=1711.000001&cid=C9"
    );
    // In a message: the chip (icon box + label) on the mention tint.
    std::vector<std::string>   images;
    const text::AttributedText t = richText(
        e.ctx, "see <https://acme.slack.com/archives/C1/p1700000000000100> now", {}, &images
    );
    CHECK(t.text.find("#design") != std::string::npos);
    CHECK(t.text.find("archives") == std::string::npos);
    REQUIRE(images.size() == 1);
    bool tinted = false, box = false;
    for (const text::Span &sp : t.spans) {
        tinted |= std::string_view(t.text).substr(sp.start, sp.end - sp.start).find("#design") !=
                      std::string_view::npos &&
                  sp.style.background == ui::themed(ui::C::MentionBg);
        box |= sp.style.inlineBoxId == 1 && sp.style.background == ui::themed(ui::C::MentionBg);
    }
    CHECK(tinted);
    CHECK(box);
    // Clicks: here → the message (its thread for a reply); elsewhere → browser.
    struct Jump {
        ConvRef c;
        Ts      ts, thread;
    };
    std::vector<Jump>        jumps;
    std::vector<std::string> urls;
    e.ctx.openMessage = [&](ConvRef cc, Ts ts, Ts th) { jumps.push_back({cc, ts, th}); };
    e.ctx.openUrl     = [&](const std::string &u) { urls.push_back(u); };
    auto *label       = e.win->root().add<RichLabel>(e.ctx, nullptr);
    auto  click       = [&](const char *text) {
        jumps.clear();
        urls.clear();
        std::vector<RichLabel::Target> targets;
        const mrkdwn::Rich             r = mrkdwn::parse(text);
        REQUIRE(r.entities.size() == 1);
        targets.push_back({r.entities[0].kind, r.entities[0].data});
        label->setContent({}, std::move(targets), {});
        label->activate(1);
    };
    click("<https://acme.slack.com/archives/C1/p1700000000000100>");
    REQUIRE(jumps.size() == 1);
    CHECK(
        jumps[0].c == c && jumps[0].ts == model::parseTs("1700000000.000100") && !jumps[0].thread
    );
    click(
        "<https://acme.slack.com/archives/C1/p1700000000000200?thread_ts=1700000000.000100&cid=C1>"
    );
    REQUIRE(jumps.size() == 1);
    CHECK(jumps[0].thread == model::parseTs("1700000000.000100"));
    click("<https://other.slack.com/archives/C77/p1700000000000100>");
    CHECK(jumps.empty());
    REQUIRE(urls.size() == 1);
    CHECK_STR(urls[0], "https://other.slack.com/archives/C77/p1700000000000100");
}

// Hovering a URL link underlines it and shows the URL: only the label's
// layout is rebuilt, from the text the label holds (no second copy of the
// message text, none for the hover either).
TEST("actions: link hover underlines without copying the text") {
    Env                  e(false);
    auto                *label = e.win->root().add<RichLabel>(e.ctx, nullptr);
    text::AttributedText t;
    text::Style          st;
    t.append("see the ", st);
    st.linkId = 1;
    t.append("docs", st);
    std::vector<RichLabel::Target> targets;
    targets.push_back({mrkdwn::Kind::Link, "https://x.example/a"});
    label->setContent(std::move(t), std::move(targets), {});
    label->measureContent(400, 100);
    REQUIRE(label->textLayout() != nullptr);
    const ui::RectF  caret  = label->textLayout()->caretRect(10);
    const ui::PointF o      = label->textOrigin();
    const size_t     builds = text::layoutBuilds(), copied = text::layoutTextOwned();
    ui::Event        ev{ui::EventType::PointerMove, {o.x + caret.x, o.y + caret.y + caret.h / 2}};
    REQUIRE(label->linkAt(ev.pos) == 1);
    label->onEvent(ev);
    CHECK_STR(label->tooltip(), "https://x.example/a");
    label->measureContent(400, 100);
    CHECK(text::layoutBuilds() == builds); // the underline is paint time only
    CHECK(text::layoutTextOwned() == copied);
    // The label's own spans stay as set: the underline is the layout's.
    REQUIRE(label->richText() != nullptr);
    for (const text::Span &sp : label->richText()->spans)
        CHECK(!sp.style.underline);
    // Off the link: no tooltip.
    ev.type = ui::EventType::PointerLeave;
    label->onEvent(ev);
    CHECK(label->tooltip().empty());
}

TEST("image: two or more pictures are a gallery of equal 180-px tiles") {
    Env               e(false);
    model::Message    m   = msg(model::kNoUser, 1700000000, "pics");
    const std::string png = asset("avatars/mira.png");
    for (int i = 0; i < 4; ++i) {
        model::File f;
        f.name   = "p" + std::to_string(i) + ".png";
        f.mime   = "image/png";
        f.path   = png;
        f.width  = 400;
        f.height = 300;
        m.extras().files.push_back(f);
    }
    model::File doc;
    doc.name = "notes.txt";
    doc.mime = "text/plain";
    doc.path = "/tmp/notes.txt";
    m.extras().files.insert(m.extras().files.begin(), doc);
    std::vector<model::Message> msgs;
    msgs.push_back(std::move(m));
    const ConvRef c = addConv(e.store, std::move(msgs));
    e.list->showConversation(c);
    pump(10);
    // Find the gallery: a view whose children are four 180-px tiles, 2×2.
    std::function<ui::View *(ui::View *)> find = [&](ui::View *v) -> ui::View * {
        if (v->childCount() == 4 && v->child(0)->height() == 180 && v->child(3)->height() == 180)
            return v;
        for (size_t i = 0; i < v->childCount(); ++i)
            if (ui::View *f = find(v->child(i)))
                return f;
        return nullptr;
    };
    ui::View *g = find(&e.win->root());
    REQUIRE(g != nullptr);
    CHECK(g->width() <= 520);
    CHECK(g->child(0)->frame().y == g->child(1)->frame().y); // two per row for four
    CHECK(g->child(2)->frame().y == 180 + 8);
    CHECK(g->child(0)->width() == g->child(1)->width());
    CHECK(g->child(1)->frame().x == g->child(0)->width() + 8);
}

TEST("actions: the link and file menus") {
    Env           e(true);
    const ConvRef design = e.conv("C0DESIGN");
    e.list->showConversation(design);
    pump(10);
    CHECK_STR(menuLabels(MessageList::linkMenuItems()), "Open link|Copy link");
    Ts          withImage = 0;
    std::string path;
    for (const model::Message &m : e.store.conversation(design).messages)
        for (const model::File &f : m.files())
            if (f.isImage()) {
                withImage = m.ts;
                path      = f.path;
            }
    REQUIRE(withImage != 0);
    // Someone else's image: no delete.
    CHECK_STR(
        menuLabels(e.list->fileMenuItems(withImage, path)), "Copy link to image|Copy full image"
    );
    e.list->runMenuAction(withImage, MessageList::kCopyImageLink, path);
    CHECK_STR(clipboard(), file::toFileUrl(path));
    // Copy full image: a background job while the bytes are read off-thread.
    e.list->runMenuAction(withImage, MessageList::kCopyImage, path);
    REQUIRE(model::jobs().count() == 1);
    CHECK(model::jobs().descriptions()[0].rfind("Copying ", 0) == 0);
    REQUIRE(until([&] { return model::jobs().count() == 0; }));
    std::string png;
    REQUIRE(file::readAll(path, &png));
    CHECK(clipboard("image/png") == png);
    CHECK_STR(clipboard("text/uri-list"), file::toFileUrl(path));
    // As an admin: "Delete image…" after a separator, and it deletes.
    e.store.user(e.store.me).admin = true;
    CHECK_STR(
        menuLabels(e.list->fileMenuItems(withImage, path)),
        "Copy link to image|Copy full image|-|Delete image…"
    );
    e.list->runMenuAction(withImage, MessageList::kDeleteFile, path);
    CHECK(e.store.findMessage(design, withImage)->files().empty());
    // A CSV file: Preview (disabled: no table viewer yet), Copy link to file.
    const ConvRef general = e.conv("C0GENERAL");
    Ts            csvTs   = 0;
    std::string   csv;
    for (const model::Message &m : e.store.conversation(general).messages)
        for (const model::File &f : m.files())
            if (f.name.size() > 4 && f.name.compare(f.name.size() - 4, 4, ".csv") == 0) {
                csvTs = m.ts;
                csv   = f.path;
            }
    REQUIRE(csvTs != 0);
    e.list->showConversation(general);
    pump(10);
    CHECK(menuLabels(e.list->fileMenuItems(csvTs, csv)).rfind("Preview|Copy link to file", 0) == 0);
    // Preview: read and parsed in the background, then the table viewer.
    e.list->runMenuAction(csvTs, MessageList::kPreview, csv);
    CHECK(e.win->topPopup() == nullptr);
    REQUIRE(model::jobs().count() == 1);
    CHECK_STR(model::jobs().descriptions()[0], "Downloading signups-week-37.csv");
    REQUIRE(until([&] { return model::jobs().count() == 0; }));
    pump(2);
    REQUIRE(e.win->topPopup() != nullptr);
    e.win->topPopup()->close();
    pump(2);
}

TEST("actions: a remote CSV's Preview downloads it to a temporary file, removed once read") {
    Env           e(true);
    const ConvRef general = e.conv("C0GENERAL");
    Ts            ts      = 0;
    std::string   url;
    for (model::Message &m : e.store.conversation(general).messages)
        for (model::File &f : m.extras().files)
            if (f.name == "signups-week-37.csv") {
                ts     = m.ts;
                f.path = url = "https://files.csv.test/files/signups-week-37.csv";
            }
    REQUIRE(ts != 0);
    e.list->showConversation(general);
    pump(10);
    e.list->runMenuAction(ts, MessageList::kPreview, url);
    REQUIRE(until([&] { return model::jobs().count() == 0; }));
    pump(2);
    REQUIRE(e.backend.asked.size() == 1);
    CHECK_STR(e.backend.asked[0].url, url);
    CHECK_FALSE(file::exists(e.backend.asked[0].to));
    CHECK_FALSE(file::exists(std::string(file::dirName(e.backend.asked[0].to))));
    REQUIRE(e.win->topPopup() != nullptr);
    e.win->topPopup()->close();
    pump(2);
    // A failed download ends the job and opens nothing.
    for (model::Message &m : e.store.conversation(general).messages)
        for (model::File &f : m.extras().files)
            if (f.path == url)
                f.path = url = "https://files.csv.test/files/missing.csv";
    e.list->runMenuAction(ts, MessageList::kPreview, url);
    REQUIRE(until([&] { return model::jobs().count() == 0; }));
    pump(2);
    CHECK(e.win->topPopup() == nullptr);
    CHECK_FALSE(file::exists(e.backend.asked.back().to));
}

TEST("actions: hovering an avatar asks for the profile card; right click on the row is nothing") {
    Env            e(true);
    const ConvRef  eng      = e.conv("C0ENG");
    model::UserRef hovered  = model::kNoUser;
    int            lastMode = -1;
    e.ctx.profileHover      = [&](model::UserRef u, ui::RectF, int mode) {
        hovered  = u;
        lastMode = mode;
    };
    e.list->showConversation(eng);
    pump(10);
    const model::Message &last = e.store.conversation(eng).messages.back();
    ui::View             *row  = e.row(last.ts);
    REQUIRE(row != nullptr);
    const ui::RectF av = row->child(0)->windowRect();
    auto           *h  = app().platform().testHooks();
    h->injectPointerMove(e.win->native(), {av.x + 5, av.y + 5});
    pump(2);
    CHECK(hovered == last.user && lastMode == 1);
    h->injectPointerMove(e.win->native(), {av.x + 400, av.y + 30});
    pump(2);
    CHECK(lastMode == 0);
    h->injectButton(e.win->native(), plat::Button::Right, true);
    h->injectButton(e.win->native(), plat::Button::Right, false);
    pump(4);
    CHECK(e.win->topPopup() == nullptr);
}

TEST("actions: the hover toolbar — react, forward, save, more; save toggles") {
    Env           e(true);
    const ConvRef design  = e.conv("C0DESIGN");
    ConvRef       fwdConv = model::kNoConv;
    Ts            fwdTs   = 0;
    std::string   fwdFile = "-";
    e.ctx.forwardMessage  = [&](ConvRef c, Ts ts, const std::string &f) {
        fwdConv = c;
        fwdTs   = ts;
        fwdFile = f;
    };
    e.list->showConversation(design);
    pump(10);
    const model::Message &last = e.store.conversation(design).messages.back();
    ui::View             *row  = e.row(last.ts);
    REQUIRE(row != nullptr);
    auto           *h  = app().platform().testHooks();
    const ui::RectF rr = row->windowRect();
    h->injectPointerMove(e.win->native(), {rr.x + 300, rr.y + 20});
    pump(4);
    // The four buttons, their tooltips, 28-px buttons in a 16/12-padded card
    // (Ask agent hidden: no agent links here).
    ui::View *fwd = findByTooltip(e.win->root().child(0), "Forward message");
    REQUIRE(fwd != nullptr);
    ui::View *card = fwd->parent();
    REQUIRE(card->visible());
    REQUIRE(card->childCount() == 5);
    CHECK_STR(card->child(0)->tooltip(), "Add reaction");
    CHECK_STR(card->child(2)->tooltip(), "Save for later");
    CHECK_FALSE(card->child(3)->visible());
    CHECK_STR(card->child(4)->tooltip(), "More actions");
    CHECK(near(card->frame().w, 16 + 4 * 28 + 3 * 4) && near(card->frame().h, 40));
    // It straddles the row's top edge, 12 px from the right.
    CHECK(near(card->windowRect().y, rr.y - 20, 1));
    CHECK(near(card->windowRect().x + card->frame().w, e.win->size().w - 12, 1));
    // Forward asks the shell for its dialog; Save toggles the bookmark.
    static_cast<ui::Clickable *>(card->child(1))->activate();
    CHECK(fwdConv == design && fwdTs == last.ts && fwdFile.empty());
    static_cast<ui::Clickable *>(card->child(2))->activate();
    CHECK(e.store.findMessage(design, last.ts)->saved);
    CHECK_STR(card->child(2)->tooltip(), "Remove from saved");
    static_cast<ui::Clickable *>(card->child(2))->activate();
    CHECK_FALSE(e.store.findMessage(design, last.ts)->saved);
    // A reminder is a saved item too: Save removes it whole.
    e.backend.setReminder(design, last.ts, base::nowSecs() + 3600);
    static_cast<ui::Clickable *>(card->child(2))->activate();
    CHECK(e.store.reminderAt(design, last.ts) == 0 && !e.store.findMessage(design, last.ts)->saved);
    // No toolbar on a pending send.
    e.backend.send(design, "pending one", 0, nullptr);
    pump(2);
    const Ts pend = sentTs(e.store, design, "pending one");
    REQUIRE(e.store.findMessage(design, pend)->pending);
    e.list->scrollToMessage(pend, false, false);
    pump(4);
    if (ui::View *pr = e.row(pend)) {
        const ui::RectF p = pr->windowRect();
        h->injectPointerMove(e.win->native(), {p.x + 300, p.y + 10});
        pump(2);
        CHECK_FALSE(card->visible());
    }
}

TEST("actions: the robot asks an agent, active on a linked thread; its menu items") {
    Env                                                  e(true);
    const ConvRef                                        design     = e.conv("C0DESIGN");
    // The shell's links, stood in for: the newest message's thread linked.
    Ts                                                   linkedRoot = 0;
    std::vector<std::pair<Ts, Context::AgentLinkAction>> actions;
    e.ctx.agentLink = [&](ConvRef, const model::Message &m) {
        Context::AgentLink a;
        a.offered  = true;
        a.linked   = (m.isReply() ? m.threadTs : m.ts) == linkedRoot;
        a.root     = a.linked && m.ts == linkedRoot;
        a.canAllow = a.linked;
        return a;
    };
    e.ctx.agentLinkAction = [&](ConvRef, Ts ts, Context::AgentLinkAction a) {
        actions.push_back({ts, a});
    };
    e.list->showConversation(design);
    pump(10);
    const model::Message &last = e.store.conversation(design).messages.back();
    ui::View             *row  = e.row(last.ts);
    REQUIRE(row != nullptr);
    auto           *h  = app().platform().testHooks();
    const ui::RectF rr = row->windowRect();
    h->injectPointerMove(e.win->native(), {rr.x + 300, rr.y + 20});
    pump(4);
    // Hovering the message shows the robot before More actions.
    auto *robot = static_cast<IconButton *>(findByTooltip(e.win->root().child(0), "Ask agent"));
    REQUIRE(robot != nullptr);
    REQUIRE(robot->visible());
    CHECK(robot->icon() == gfx::Icon::Bot);
    CHECK(robot->parent()->child(4) == findByTooltip(e.win->root().child(0), "More actions"));
    CHECK(robot->ink() != ui::C::Accent);
    robot->activate();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].first == last.ts && actions[0].second == Context::AgentLinkAction::Ask);
    // Asking is the robot's alone; the menu doesn't repeat it.
    std::string labels = menuLabels(e.list->menuItems(last.ts));
    CHECK(labels.find("Ask agent") == std::string::npos);
    CHECK(labels.find("Unlink agent") == std::string::npos);
    // Linked: active, "Open agent thread"; the menu lets its author ask and
    // the root unlink.
    linkedRoot = last.ts;
    e.list->refreshToolbar();
    CHECK(robot->ink() == ui::C::Accent);
    CHECK_STR(robot->tooltip(), "Open agent thread");
    labels = menuLabels(e.list->menuItems(last.ts));
    CHECK(labels.find("Allow ") != std::string::npos);
    CHECK(labels.find(" to ask agent") != std::string::npos);
    CHECK(labels.find("Unlink agent") != std::string::npos);
    e.list->runMenuAction(last.ts, MessageList::kUnlinkAgent);
    REQUIRE(actions.size() == 2);
    CHECK(actions[1].second == Context::AgentLinkAction::Unlink);
    // Nothing offered (no Claude Code workspace): hidden.
    e.ctx.agentLink = [](ConvRef, const model::Message &) { return Context::AgentLink{}; };
    e.list->refreshToolbar();
    CHECK_FALSE(robot->visible());
    CHECK(menuLabels(e.list->menuItems(last.ts)).find("agent") == std::string::npos);
}

TEST("actions: the file bar on hovered files; right click on them does nothing") {
    Env           e(true);
    const ConvRef general = e.conv("C0GENERAL");
    std::string   shared;
    e.ctx.forwardMessage = [&](ConvRef, Ts, const std::string &f) { shared = f; };
    e.list->showConversation(general);
    pump(10);
    Ts          csvTs = 0;
    std::string csv;
    for (const model::Message &m : e.store.conversation(general).messages)
        for (const model::File &f : m.files())
            if (!f.isImage())
                csvTs = m.ts, csv = f.path;
    REQUIRE(csvTs != 0);
    e.list->scrollToMessage(csvTs, false, false);
    pump(6);
    ui::View *row = e.row(csvTs);
    REQUIRE(row != nullptr);
    // The chip: the column's last child; hover it.
    ui::View                       *chip = nullptr;
    std::function<void(ui::View *)> find = [&](ui::View *v) {
        for (size_t i = 0; i < v->childCount(); ++i) {
            if (v->child(i)->frame().h == 60 && v->child(i)->childCount() == 0)
                chip = v->child(i);
            find(v->child(i));
        }
    };
    find(row);
    REQUIRE(chip != nullptr);
    const ui::RectF c = chip->windowRect();
    auto           *h = app().platform().testHooks();
    h->injectPointerMove(e.win->native(), {c.x + 20, c.y + 30});
    pump(4);
    ui::View *dl = findByTooltip(e.win->root().child(0), "Download");
    REQUIRE(dl != nullptr);
    ui::View *bar = dl->parent();
    CHECK(bar->visible());
    CHECK(near(bar->frame().w, 108) && near(bar->frame().h, 40));
    CHECK(near(bar->windowRect().y, c.y - 20, 1));
    CHECK(near(bar->windowRect().x + 108, c.x + c.w - 1, 1));
    CHECK_STR(bar->child(1)->tooltip(), "Share");
    static_cast<ui::Clickable *>(bar->child(1))->activate();
    CHECK_STR(shared, csv); // "Forward this file"
    h->injectButton(e.win->native(), plat::Button::Right, true);
    h->injectButton(e.win->native(), plat::Button::Right, false);
    pump(2);
    CHECK(e.win->topPopup() == nullptr);
}

TEST("actions: the delete, move to thread and reminder dialogs and the CSV viewer") {
    Env           e(true);
    const ConvRef eng = e.conv("C0DESIGN"); // it has a thread
    e.list->showConversation(eng);
    pump(10);
    bool sent = false;
    e.backend.send(eng, "move me", 0, [&](bool ok, const std::string &) { sent = ok; });
    REQUIRE(until([&] { return sent; }));
    const Ts mine = sentTs(e.store, eng, "move me");
    // Delete: a dialog first, then the backend.
    e.list->runMenuAction(mine, MessageList::kDelete);
    auto *del = static_cast<ui::Dialog *>(e.win->topPopup());
    REQUIRE(del != nullptr);
    CHECK(e.store.findMessage(eng, mine) != nullptr);
    del->reject();
    pump(2);
    CHECK(e.store.findMessage(eng, mine) != nullptr);
    // Move to thread: the roots newest first, the text posted, the original gone.
    const auto roots = e.list->threadRoots(mine);
    REQUIRE(!roots.empty());
    CHECK_STR(movedMessageText(e.ctx, *e.store.findMessage(eng, mine), false), "move me");
    CHECK(
        movedMessageText(e.ctx, *e.store.findMessage(eng, mine), true)
            .rfind("_Moved from the channel \xC2\xB7 originally posted by ", 0) == 0
    );
    { // the files go along as links under the text (a link-less one is left out)
        model::Message withFiles = e.store.findMessage(eng, mine)->clone();
        model::File    a, b, c;
        a.name                   = "plan.pdf";
        a.permalink              = "https://x.slack.com/files/U1/F1/plan.pdf";
        b.permalink              = "https://x.slack.com/files/U1/F2";
        withFiles.extras().files = {a, b, c};
        CHECK_STR(
            movedMessageText(e.ctx, withFiles, false),
            "move me\n<https://x.slack.com/files/U1/F1/plan.pdf|plan.pdf>\n"
            "https://x.slack.com/files/U1/F2"
        );
        withFiles.text.clear();
        CHECK_STR(movedMessageText(e.ctx, withFiles, false).substr(0, 9), "<https://");
    }
    Ts opened        = 0;
    e.ctx.openThread = [&](ConvRef, Ts r) { opened = r; };
    e.list->runMenuAction(mine, MessageList::kMoveToThread);
    REQUIRE(e.win->topPopup() != nullptr);
    auto *hooks = app().platform().testHooks();
    hooks->injectKey(e.win->native(), plat::Key::Enter, true); // the first thread is preselected
    hooks->injectKey(e.win->native(), plat::Key::Enter, false);
    REQUIRE(until([&] { return e.store.findMessage(eng, mine) == nullptr; }));
    CHECK(opened == roots.front());
    bool found = false;
    for (const model::Message &r : *e.store.replies(eng, roots.front()))
        found |= r.text == "move me";
    CHECK(found);
    // Reminder: Save → a due time at least a minute ahead.
    const Ts any = e.store.conversation(eng).messages.front().ts;
    e.list->runMenuAction(any, MessageList::kRemindCustom);
    auto *rem = static_cast<ui::Dialog *>(e.win->topPopup());
    REQUIRE(rem != nullptr);
    rem->accept();
    pump(2);
    CHECK(e.store.reminderAt(eng, any) > base::nowSecs() + 3000);
    // CSV: parsing (quotes, the header row), then the viewer.
    const auto rows = parseCsv("a,\"b,c\",\"d\"\"e\"\r\n1,2,3\n\n");
    REQUIRE(rows.size() == 2);
    CHECK_STR(rows[0][1], "b,c");
    CHECK_STR(rows[0][2], "d\"e");
    CHECK(parseCsv("x;y;z\n")[0].size() == 3);
    ui::Popup *v = showTableViewer(*e.win, readCsvFile(asset("files/signups-week-37.csv")));
    REQUIRE(v != nullptr);
    pump(2);
    hooks->injectKey(e.win->native(), plat::Key::Escape, true);
    hooks->injectKey(e.win->native(), plat::Key::Escape, false);
    pump(2);
    CHECK(e.win->topPopup() == nullptr);
}

// ── Remote images ───────────────────────────────────────────────────────────

namespace {

// A fresh directory for one test's disk cache.
std::string tempDir(const char *name) {
    const char *t = std::getenv("TMPDIR");
    std::string d = file::join(
        t && *t ? t : "/tmp",
        std::string("msga-test-") + name + "-" + std::to_string(base::nowMicros()) + "-" +
            std::to_string(std::random_device()()) // distinct across parallel runs too
    );
    file::makeDirs(d);
    return d;
}

void wipe(const std::string &dir) {
    std::vector<file::DirEntry> all;
    if (file::listDir(dir, &all))
        for (const file::DirEntry &e : all)
            file::remove(file::join(dir, e.name));
    file::remove(dir);
}

// The network as RemoteImages sees it: https://<host>.test/<path under the test assets>
// served from there, answered on a later loop turn; page.html
// is Slack's sign-in page (HTML, status 200). Records every request.
struct FakeNet {
    std::vector<net::Request> asked;
    void                      install(RemoteImages &r) {
        r.setFetch([this](net::Request q, std::function<void(net::Response)> done) {
            asked.push_back(q);
            net::Response     resp;
            const size_t      at  = q.url.find(".test/");
            const std::string rel = at == std::string::npos ? "" : q.url.substr(at + 6);
            resp.status           = 200;
            if (rel == "page.html")
                resp.body = "<!DOCTYPE html><html><body>Sign in</body></html>";
            else if (!file::readAll(asset(rel.c_str()), &resp.body))
                resp.status = 404;
            app().platform().post([done, resp] { done(resp); });
        });
    }
};

} // namespace

TEST("image: URLs download once through RemoteImages, auth per URL, and stay on disk") {
    const std::string         dir = tempDir("img");
    const ImageCache::Request r{
        "https://files.img.test/avatars/mira.png", 64, 64, ImageCache::Shape::Circle
    };
    {
        RemoteImages remote(app().platform(), nullptr, dir);
        FakeNet      fake;
        fake.install(remote);
        remote.setAuth([](const std::string &url, std::vector<net::Header> &h) {
            if (url.find("://files.") != std::string::npos)
                h.push_back({"Authorization", "Bearer xoxc-test"});
        });
        ImageCache cache(app().platform());
        cache.setRemote(&remote);
        int w = 0, h = 0;
        CHECK_FALSE(cache.naturalSize(r.path, &w, &h)); // not downloaded yet
        CHECK(cache.get(r) == nullptr);
        CHECK(cache.pending() == 1);
        // The same picture at another size joins the download.
        const ImageCache::Request small{r.path, 24, 24};
        CHECK(cache.get(small) == nullptr);
        REQUIRE(until([&] { return cache.pending() == 0; }));
        REQUIRE(fake.asked.size() == 1);
        REQUIRE(fake.asked[0].headers.size() == 1);
        CHECK_STR(fake.asked[0].headers[0].value, "Bearer xoxc-test");
        ImageCache::Bitmap b = cache.get(r);
        REQUIRE(b != nullptr);
        CHECK(b->width() == 64 && b->height() == 64);
        CHECK(cache.get(small) != nullptr);
        CHECK_FALSE(remote.cachedPath(r.path).empty());
        CHECK(cache.naturalSize(r.path, &w, &h));
        CHECK(w > 0 && h > 0);
        CHECK(remote.diskBytes() > 0);
        // A public CDN picture goes without the token.
        const ImageCache::Request cdn{"https://cdn.img.test/avatars/jonas.png", 40, 40};
        cache.get(cdn);
        REQUIRE(until([&] { return cache.pending() == 0; }));
        REQUIRE(fake.asked.size() == 2);
        CHECK(fake.asked[1].headers.empty());
        CHECK(cache.get(cdn) != nullptr);
    }
    {
        // The next run decodes from disk without asking the network.
        RemoteImages remote(app().platform(), nullptr, dir);
        FakeNet      fake;
        fake.install(remote);
        ImageCache cache(app().platform());
        cache.setRemote(&remote);
        cache.get(r);
        REQUIRE(until([&] { return cache.pending() == 0; }));
        CHECK(cache.get(r) != nullptr);
        CHECK(fake.asked.empty());
    }
    wipe(dir);
}

TEST("image: an SVG URL (the \"Slack\" system user's avatar) downloads and renders") {
    const std::string dir = tempDir("imgsvg");
    {
        RemoteImages remote(app().platform(), nullptr, dir);
        FakeNet      fake;
        fake.install(remote);
        ImageCache cache(app().platform());
        cache.setRemote(&remote);
        const ImageCache::Request r{
            "https://cdn.img.test/svg/logo-orbit.svg", 36, 36, ImageCache::Shape::Square
        };
        cache.get(r);
        REQUIRE(until([&] { return cache.pending() == 0; }));
        CHECK_FALSE(cache.failed(r));
        CHECK_FALSE(remote.cachedPath(r.path).empty());
        ImageCache::Bitmap b = cache.get(r);
        REQUIRE(b != nullptr);
        CHECK(b->width() == 36 && b->height() == 36);
    }
    wipe(dir);
}

TEST("image: \"Copy full image\" of a Slack-hosted image copies the original, not the thumbnail") {
    const std::string dir = tempDir("imgcopy");
    {
        RemoteImages remote(app().platform(), nullptr, dir);
        FakeNet      fake;
        fake.install(remote);
        Env e(true);
        e.ctx.remote         = &remote;
        const ConvRef design = e.conv("C0DESIGN");
        Ts            ts     = 0;
        std::string   local, thumb, url;
        for (model::Message &m : e.store.conversation(design).messages)
            for (model::File &f : m.extras().files)
                if (f.isImage() && !ts) {
                    ts     = m.ts;
                    local  = f.path;
                    // Slack: path is a thumbnail, original the full image.
                    f.path = thumb = "https://files.img.test/thumbs/never-copied.png";
                    f.original     = url =
                        "https://files.img.test/" + local.substr(std::strlen(MSGA_TEST_ASSETS) + 1);
                }
        REQUIRE(ts != 0);
        e.list->showConversation(design);
        pump(10);
        std::string png;
        REQUIRE(file::readAll(local, &png));
        app().platform().setClipboardText("before");
        // Not on disk yet: the backend downloads the original to a temporary
        // file, read off the UI thread and removed; the cog runs meanwhile.
        e.list->runMenuAction(ts, MessageList::kCopyImage, thumb);
        REQUIRE(model::jobs().count() == 1);
        REQUIRE(until([&] { return model::jobs().count() == 0; }));
        pump(2);
        CHECK(clipboard("image/png") == png);
        REQUIRE(e.backend.asked.size() == 1);
        CHECK_STR(e.backend.asked[0].url, url);
        CHECK_FALSE(file::exists(e.backend.asked[0].to));
        CHECK_FALSE(file::exists(std::string(file::dirName(e.backend.asked[0].to))));
        // In the image cache already (the viewer fetched it): no download.
        bool cached = false;
        remote.fetch(url, [&](const std::string &p) { cached = !p.empty(); });
        REQUIRE(until([&] { return cached; }));
        app().platform().setClipboardText("before");
        e.list->runMenuAction(ts, MessageList::kCopyImage, thumb);
        REQUIRE(until([&] { return model::jobs().count() == 0; }));
        pump(2);
        CHECK(clipboard("image/png") == png);
        CHECK(e.backend.asked.size() == 1);
        // A failed download: the job ends, the clipboard is left alone.
        for (model::Message &m : e.store.conversation(design).messages)
            for (model::File &f : m.extras().files)
                if (f.path == thumb)
                    f.original = "https://files.img.test/missing.png";
        app().platform().setClipboardText("before");
        e.list->runMenuAction(ts, MessageList::kCopyImage, thumb);
        REQUIRE(until([&] { return model::jobs().count() == 0; }));
        pump(2);
        CHECK_STR(clipboard(), "before");
        CHECK_FALSE(file::exists(e.backend.asked.back().to));
    }
    wipe(dir);
}

TEST("actions: the file bar's Download saves the original where asked, a job until written") {
    const std::string dir = tempDir("download");
    {
        Env           e(true);
        const ConvRef general = e.conv("C0GENERAL");
        Ts            ts      = 0;
        std::string   path;
        for (model::Message &m : e.store.conversation(general).messages)
            for (model::File &f : m.extras().files)
                if (f.name == "signups-week-37.csv") {
                    ts     = m.ts;
                    f.path = path = "https://files.csv.test/transcodes/signups.bin";
                    f.original    = "https://files.csv.test/files/signups-week-37.csv";
                }
        REQUIRE(ts != 0);
        e.list->showConversation(general);
        pump(10);
        auto             *hooks = app().platform().testHooks();
        const std::string to    = file::join(dir, "saved.csv");
        REQUIRE(hooks->fileDialogRespond({to}));
        e.list->downloadFile(ts, path);
        REQUIRE(until([&] { return model::jobs().count() == 1; }));
        CHECK_STR(model::jobs().descriptions()[0], "Downloading signups-week-37.csv");
        plat::FileDialogDesc d;
        REQUIRE(hooks->lastFileDialog(&d));
        CHECK_STR(d.suggestedName, "signups-week-37.csv");
        CHECK_STR(d.initialDir, app().platform().standardDir(plat::StandardDir::Home));
        REQUIRE(until([&] { return model::jobs().count() == 0; }));
        REQUIRE(e.backend.asked.size() == 1);
        CHECK_STR(e.backend.asked[0].url, "https://files.csv.test/files/signups-week-37.csv");
        std::string want, got;
        REQUIRE(file::readAll(asset("files/signups-week-37.csv"), &want));
        REQUIRE(file::readAll(to, &got));
        CHECK(got == want);
        // Cancelled: no job, no download.
        REQUIRE(hooks->fileDialogRespond({}));
        e.list->downloadFile(ts, path);
        pump(6);
        CHECK(model::jobs().count() == 0);
        CHECK(e.backend.asked.size() == 1);
    }
    wipe(dir);
}

TEST("image: failed downloads are not kept, cool down, and end with a sign-in") {
    const std::string dir = tempDir("imgfail");
    {
        RemoteImages remote(app().platform(), nullptr, dir);
        FakeNet      fake;
        fake.install(remote);
        ImageCache cache(app().platform());
        cache.setRemote(&remote);
        // Slack's sign-in page instead of the file (no auth yet).
        const ImageCache::Request page{"https://files.img.test/page.html", 20, 20};
        cache.get(page);
        REQUIRE(until([&] { return cache.pending() == 0; }));
        CHECK(cache.failed(page));
        CHECK(remote.cachedPath(page.path).empty()); // never cached
        CHECK(remote.failedRecently(page.path));
        // Repaints ask again: no new request while it cools down.
        for (int i = 0; i < 3; ++i) {
            CHECK(cache.get(page) == nullptr);
            pump(2);
        }
        CHECK(fake.asked.size() == 1);
        // A 404: the same.
        const ImageCache::Request gone{"https://files.img.test/nope.png", 20, 20};
        cache.get(gone);
        REQUIRE(until([&] { return cache.pending() == 0; }));
        CHECK(cache.failed(gone));
        CHECK(fake.asked.size() == 2);
        bool        called = false;
        std::string got    = "x";
        remote.fetch(gone.path, [&](const std::string &p) {
            called = true;
            got    = p;
        });
        CHECK_FALSE(called); // never inside the call
        REQUIRE(until([&] { return called; }));
        CHECK(got.empty());
        CHECK(fake.asked.size() == 2);
        // Signing in forgets the failures: the next paint asks again.
        remote.setAuth([](const std::string &, std::vector<net::Header> &h) {
            h.push_back({"Authorization", "Bearer xoxc-test"});
        });
        CHECK_FALSE(remote.failedRecently(page.path));
        CHECK(cache.get(page) == nullptr);
        REQUIRE(until([&] { return cache.pending() == 0; }));
        CHECK(fake.asked.size() == 3);
        CHECK_STR(fake.asked[2].headers[0].value, "Bearer xoxc-test");
    }
    {
        // Without RemoteImages (signed out, tests) a URL just fails.
        ImageCache                cache(app().platform());
        const ImageCache::Request r{"https://x.test/a.png", 10, 10};
        CHECK(cache.get(r) == nullptr);
        REQUIRE(until([&] { return cache.pending() == 0; }));
        CHECK(cache.failed(r));
    }
    wipe(dir);
}

TEST("image: the disk cache stays under its limit, least recently used first") {
    const std::string dir = tempDir("imgsweep");
    const std::string blob(400 * 1024, 'x');
    const char *const names[] = {"old", "mid", "new"};
    for (int i = 0; i < 3; ++i) {
        const std::string p = file::join(dir, names[i]);
        REQUIRE(file::writeAtomic(p, blob));
#ifndef _WIN32
        // Hours apart, oldest first ("used" = mtime).
        struct timespec ts[2];
        ts[0].tv_sec = ts[1].tv_sec = time(nullptr) - (3 - i) * 3600;
        ts[0].tv_nsec = ts[1].tv_nsec = 0;
        utimensat(AT_FDCWD, p.c_str(), ts, 0);
#endif
    }
    RemoteImages remote(app().platform(), nullptr, dir);
    bool         swept = false;
    remote.setLimitMb(1); // 1.2 MB → the oldest goes
    remote.sweepNow([&] { swept = true; });
    REQUIRE(until([&] { return swept; }));
#ifndef _WIN32
    CHECK_FALSE(file::exists(file::join(dir, "old")));
    CHECK(file::exists(file::join(dir, "mid")));
#endif
    CHECK(file::exists(file::join(dir, "new")));
    CHECK(remote.diskBytes() == int64_t(blob.size()) * 2);
    // Settings → Clear cache.
    remote.clear();
    REQUIRE(until([&] { return remote.diskBytes() == 0; }));
    std::vector<file::DirEntry> left;
    REQUIRE(until([&] { return file::listDir(dir, &left) && left.empty(); }));
    wipe(dir);
}

TEST("image: the limit covers the audio and file downloads and counts the workspaces' data") {
    // The cache sweep: everything counts, only blobs go, oldest first.
    const std::string root = tempDir("cachesweep");
    const std::string blob(400 * 1024, 'x');
    const std::string images = file::join(root, "images"), audio = file::join(root, "audio"),
                      files = file::join(root, "files"), kept = file::join(root, "workspaces");
    const std::string paths[] = {
        file::join(audio, "a.m4a"),         // oldest
        file::join(images, "i"),            // then this
        file::join(files, "f.html"),        // newest blob
        file::join(kept, "slack_T1/m.json") // never evicted, however old
    };
    for (int i = 0; i < 4; ++i) {
        REQUIRE(file::makeDirs(std::string(file::dirName(paths[i]))));
        REQUIRE(file::writeAtomic(paths[i], blob));
#ifndef _WIN32
        struct timespec ts[2];
        ts[0].tv_sec = ts[1].tv_sec = time(nullptr) - (i == 3 ? 9 : 3 - i) * 3600;
        ts[0].tv_nsec = ts[1].tv_nsec = 0;
        utimensat(AT_FDCWD, paths[i].c_str(), ts, 0);
#endif
    }
    RemoteImages remote(app().platform(), nullptr, images);
    remote.coverDirs({audio, files}, {kept});
    bool swept = false;
    remote.setLimitMb(1); // 1.6 MB with the kept 400 KB → two blobs go
    remote.sweepNow([&] { swept = true; });
    REQUIRE(until([&] { return swept; }));
#ifndef _WIN32
    CHECK_FALSE(file::exists(paths[0]));
    CHECK_FALSE(file::exists(paths[1]));
#endif
    CHECK(file::exists(paths[2]));
    CHECK(file::exists(paths[3]));
    CHECK(remote.diskBytes() == int64_t(blob.size())); // blobs only

    // A download into a covered folder counts at once.
    const std::string more = file::join(audio, "b.m4a");
    REQUIRE(file::writeAtomic(more, blob));
    remote.noteWritten(more);
    REQUIRE(until([&] { return remote.diskBytes() == int64_t(blob.size()) * 2; }));

    // Settings → Clear cache: every blob, the kept data left to its owner.
    remote.clear();
    REQUIRE(until([&] { return remote.diskBytes() == 0 && !file::exists(paths[2]); }));
    CHECK_FALSE(file::exists(more));
    CHECK(file::exists(paths[3]));
    file::remove(paths[3]);
    file::remove(file::dirName(paths[3]));
    for (const std::string &d : {images, audio, files, kept})
        wipe(d);
    wipe(root);
}

TEST("image: a real download through net::Client from a local http.server") {
    const std::string py = base::findExecutable("python3");
    if (py.empty()) {
        std::fprintf(stderr, "  no python3: skipped\n");
        return;
    }
    const int port = net::freeLoopbackPort();
    REQUIRE(port > 0);
    // http.server's own server (python3 -m http.server) looks up its host's
    // name first, which can hang on macOS's mDNS for good: a plain
    // TCPServer with the same handler doesn't.
    struct Server : base::Process {
        ~Server() { kill(true); } // however the case ends
    } server;
    REQUIRE(server.start(
        py,
        {"-c",
         "import functools, http.server, socketserver, sys\n"
         "h = functools.partial(http.server.SimpleHTTPRequestHandler, directory=sys.argv[2])\n"
         "socketserver.TCPServer(('127.0.0.1', int(sys.argv[1])), h).serve_forever()\n",
         std::to_string(port),
         asset("avatars")}
    ));
    const std::string dir = tempDir("imghttp");
    {
        net::Client       client(app().platform());
        const std::string url       = "http://127.0.0.1:" + std::to_string(port) + "/mira.png";
        // The server takes a moment to listen: ask until it answers.
        bool              listening = false, asking = false;
        REQUIRE(until(
            [&] {
                if (!asking) {
                    asking = true;
                    net::Request q;
                    q.url = url;
                    client.send(std::move(q), [&](net::Response a) {
                        listening = a.status == 200;
                        asking    = false;
                    });
                }
                return listening;
            },
            30000
        ));
        RemoteImages remote(app().platform(), &client, dir);
        ImageCache   cache(app().platform());
        cache.setRemote(&remote);
        const ImageCache::Request r{url, 48, 48};
        // A failure meanwhile is forgotten (setAuth) and asked again.
        const bool                ok = until(
            [&] {
                if (cache.get(r))
                    return true;
                if (cache.failed(r))
                    remote.setAuth({});
                return false;
            },
            10000
        );
        CHECK(ok);
        CHECK_FALSE(remote.cachedPath(r.path).empty());
        CHECK(remote.pending() == 0);
    }
    wipe(dir);
}

// ── Loading and empty states ────────────────────────────────────────────────

namespace {

// A backend whose history and thread pages arrive when the test says so.
struct HoldBackend final : fake::FakeBackend {
    using FakeBackend::FakeBackend;
    std::vector<Done> history, threads;
    void              loadHistory(model::ConvRef, model::Ts, Done d) override {
        history.push_back(std::move(d));
    }
    void loadThread(model::ConvRef, model::Ts, Done d) override { threads.push_back(std::move(d)); }
};

struct HoldEnv {
    model::Store                store;
    HoldBackend                 backend{store, app().platform()};
    ImageCache                  images{app().platform()};
    Context                     ctx{app(), store, backend, images};
    std::unique_ptr<ui::Window> win;
    MessageList                *list = nullptr;
    UserRef                     mira = model::kNoUser;
    HoldEnv() {
        plat::WindowDesc d;
        d.size = {800, 600};
        win    = std::make_unique<ui::Window>(d);
        list   = win->root().add<MessageList>(ctx);
        model::User u;
        u.id          = "U2";
        u.displayName = "Mira";
        mira          = store.addUser(u);
        pump();
    }
    ~HoldEnv() { win.reset(); }
    ConvRef conv(const char *id) {
        model::Conversation c;
        c.id   = id;
        c.name = id;
        return store.addConversation(std::move(c)); // hasMoreBefore: never loaded
    }
};

} // namespace

TEST("loading: the ring and its hints until the first page, then No messages yet") {
    using State = MessageList::State;
    HoldEnv       e;
    const ConvRef c = e.conv("C1");
    e.list->showConversation(c);
    CHECK(e.list->state() == State::Loading);
    CHECK(e.list->stateText().empty()); // no hint in the first second
    REQUIRE(until([&] { return !e.backend.history.empty(); }));
    REQUIRE(until([&] { return !e.list->stateText().empty(); }, 2500));
    CHECK_STR(e.list->stateText(), "Loading your stuff...");
    // The page is in, and empty.
    e.store.updateConversation(c, [](model::Conversation &x) { x.hasMoreBefore = false; });
    e.backend.history.front()(true, {});
    pump();
    CHECK(e.list->state() == State::Empty);
    CHECK_STR(e.list->stateText(), "No messages yet");
    // A message: nothing in its way.
    e.store.addMessage(c, msg(e.mira, 1000, "first"));
    pump();
    CHECK(e.list->state() == State::None);
    CHECK(e.list->items().size() == 2); // the day, the message
    // The workspace's first load (no conversation open): the ring as well.
    e.list->clear();
    CHECK(e.list->state() == State::None);
    e.list->setWaiting(true);
    CHECK(e.list->state() == State::Loading);
    e.list->setWaiting(false);
    CHECK(e.list->state() == State::None);
}

TEST("loading: a thread shows the ring, not its root, until the replies are in") {
    using State = MessageList::State;
    HoldEnv        e;
    const ConvRef  c                   = e.conv("C2");
    model::Message root                = msg(e.mira, 2000, "root");
    root.replyCount                    = 1;
    const Ts                    rootTs = root.ts;
    std::vector<model::Message> page;
    page.push_back(std::move(root));
    e.store.addPage(c, std::move(page));
    e.list->showThread(c, rootTs);
    CHECK(e.list->items().empty());
    CHECK(e.list->state() == State::Loading);
    REQUIRE(e.backend.threads.size() == 1);
    model::Message reply = msg(e.mira, 2100, "reply");
    reply.threadTs       = rootTs;
    std::vector<model::Message> replies;
    replies.push_back(std::move(reply));
    e.store.addPage(c, std::move(replies));
    pump();
    CHECK(e.list->items().empty()); // still the page's to deliver
    e.backend.threads.front()(true, {});
    pump();
    CHECK(e.list->state() == State::None);
    size_t messages = 0;
    for (const auto &it : e.list->items())
        messages += it.kind == Kind::Message;
    CHECK(messages == 2);
}

// ── Summarize down (the job and its dialog) ─────────────────────────────────

namespace {

// The first view (depth first) whose accessible name contains `needle`.
ui::View *findNamed(ui::View *v, std::string_view needle) {
    if (v->accessibleName().find(needle) != std::string::npos)
        return v;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (ui::View *f = findNamed(v->child(i), needle))
            return f;
    return nullptr;
}

Ts firstTs(const model::Store &st, ConvRef c, std::string_view text) {
    for (const model::Message &m : st.conversation(c).messages)
        if (m.text.find(text) != std::string::npos)
            return m.ts;
    return 0;
}

} // namespace

TEST("actions: Summarize down without an AI provider says so and links to the settings") {
    Env          e(true);
    llm::Service ai(app().platform());
    int          opened  = 0;
    e.ctx.window         = e.win.get();
    e.ctx.openAiSettings = [&] { ++opened; };
    const ConvRef eng    = e.conv("C0ENG");
    e.list->showConversation(eng);
    pump(10);
    const Ts ts = firstTs(e.store, eng, "search index");
    REQUIRE(ts != 0);
    // Not wired (no Context::ai): shown, disabled.
    CHECK_FALSE(e.list->menuItems(ts).back().enabled);
    e.ctx.ai         = &ai;
    const auto items = e.list->menuItems(ts);
    CHECK_STR(items.back().label, "Summarize down");
    CHECK(items.back().icon == uint16_t(gfx::Icon::Sparkles));
    CHECK(items.back().enabled); // always offered; the notice explains
    e.list->runMenuAction(ts, MessageList::kSummarize);
    CHECK(model::jobs().count() == 0); // fails fast: no job, no fetches
    ui::Popup *dlg = e.win->topPopup();
    REQUIRE(dlg != nullptr);
    CHECK(findNamed(dlg, "Discussion summary") != nullptr);
    CHECK(
        findNamed(
            dlg,
            "Summaries need an AI provider. Connect one in Settings \xE2\x86\x92 AI assistance."
        ) != nullptr
    );
    auto *open = static_cast<ui::Button *>(findNamed(dlg, "Open settings"));
    REQUIRE(open != nullptr);
    open->onClick();
    pump(4);
    CHECK(opened == 1);
    CHECK(e.win->topPopup() == nullptr);
}

TEST(
    "actions: Summarize down fetches the span's threads in the background, then shows the report"
) {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake AI provider\n");
        return;
    }
    fakellm::ctl(app().platform(), "POST", "/_ctl/reset");
    Env          e(true);
    llm::Service ai(app().platform());
    ai.setProviders(
        {llm::fromSettings("anthropic", "Anthropic", "", "", "", ""),
         llm::fromSettings("custom-1", "Fake", base + "/v1", "", "fake-large", "")},
        "anthropic" // not connected: the custom server answers
    );
    ai.setLanguage("ja");
    e.ctx.ai             = &ai;
    e.ctx.window         = e.win.get();
    const ConvRef design = e.conv("C0DESIGN");
    e.list->showConversation(design);
    pump(10);
    const Ts from = firstTs(e.store, design, "Palette v3 is out");
    REQUIRE(from != 0);
    e.list->runMenuAction(from, MessageList::kSummarize);
    // Nothing blocks: the job runs, the report comes later.
    REQUIRE(model::jobs().count() == 1);
    CHECK_STR(model::jobs().descriptions()[0], "Summarizing discussion\xE2\x80\xA6");
    CHECK(e.win->topPopup() == nullptr);
    REQUIRE(until([&] { return e.win->topPopup() != nullptr; }, 10000));
    CHECK(model::jobs().count() == 0);
    ui::Popup *dlg = e.win->topPopup();
    CHECK(findNamed(dlg, "Discussion summary") != nullptr);
    CHECK(findNamed(dlg, "ship on Friday") != nullptr); // the Markdown, rendered

    // One request, to the custom server's light model (its own), with the
    // transcript: the span's messages, each thread's replies under its root.
    json::Document log;
    REQUIRE(log.parse(fakellm::ctl(app().platform(), "GET", "/_ctl/log").body, nullptr));
    REQUIRE(log.root().size() == 1);
    CHECK_STR(log.root()[0]["path"].str(), "/v1/chat/completions");
    json::Document body;
    REQUIRE(body.parse(std::string(log.root()[0]["body"].str()), nullptr));
    CHECK_STR(body.root()["model"].str(), "fake-large");
    CHECK(body.root()["max_tokens"].integer() == 512);
    const std::string_view system = body.root()["messages"][0]["content"].str();
    CHECK(system.find("Write in Japanese only") != std::string_view::npos);
    const std::string_view user  = body.root()["messages"][1]["content"].str();
    const size_t           root  = user.find("Mira Okafor: Palette v3 is out");
    const size_t           reply = user.find("    \xE2\x86\xB3 Priya Natarajan: Contrast on");
    const size_t           next  = user.find("Reminder for tomorrow's crit");
    CHECK(root != std::string_view::npos);
    CHECK(reply != std::string_view::npos && reply > root && reply < next);
    CHECK(user.find("\xE2\x86\xB3 Lena") != std::string_view::npos);    // Proposal B's thread too
    CHECK(user.find("Heads up: I'm moving") == std::string_view::npos); // another channel

    // Copy: the Markdown on the clipboard, "Copied" for a moment.
    auto *copy = static_cast<ui::Button *>(findNamed(dlg, "Copy"));
    REQUIRE(copy != nullptr);
    copy->onClick();
    CHECK_STR(copy->label(), "Copied");
    CHECK_STR(clipboard(), "The team agreed to **ship on Friday**.");
    REQUIRE(until([&] { return copy->label() == "Copy"; }, 3000));
    static_cast<ui::Dialog *>(dlg)->reject();
    pump(4);
}

TEST("actions: Summarize down in a thread, and when the provider fails") {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake AI provider\n");
        return;
    }
    fakellm::ctl(app().platform(), "POST", "/_ctl/reset");
    fakellm::script(
        app().platform(),
        "/v1/messages",
        R"([{"__status":529,"error":{"type":"overloaded_error","message":"Overloaded"}}])"
    );
    Env           e(true);
    llm::Service  ai(app().platform());
    llm::Provider a = llm::fromSettings("anthropic", "Anthropic", "", "sk-test", "", "");
    a.baseUrl       = base; // the native wire, at the fake
    ai.setProviders({a}, "anthropic");
    e.ctx.ai             = &ai;
    e.ctx.window         = e.win.get();
    const ConvRef design = e.conv("C0DESIGN");
    const Ts      root   = firstTs(e.store, design, "Proposal B");
    REQUIRE(root != 0);
    e.list->showThread(design, root);
    REQUIRE(until([&] { return e.list->items().size() > 3; }));
    e.list->runMenuAction(root, MessageList::kSummarize);
    REQUIRE(until([&] { return e.win->topPopup() != nullptr; }, 10000));
    ui::Popup *dlg = e.win->topPopup();
    CHECK(findNamed(dlg, "Couldn't summarize: Overloaded") != nullptr);
    CHECK(findNamed(dlg, "Copy") == nullptr); // a notice: no buttons
    CHECK(model::jobs().count() == 0);

    json::Document log;
    REQUIRE(log.parse(fakellm::ctl(app().platform(), "GET", "/_ctl/log").body, nullptr));
    REQUIRE(log.root().size() == 1);
    CHECK_STR(log.root()[0]["headers"]["x-api-key"].str(), "sk-test");
    json::Document body;
    REQUIRE(body.parse(std::string(log.root()[0]["body"].str()), nullptr));
    CHECK_STR(body.root()["model"].str(), "claude-haiku-4-5"); // the light model
    // A thread's span is its messages as they are: no reply markers.
    const std::string_view user = body.root()["messages"][0]["content"].str();
    CHECK(user.find("Proposal B for onboarding") != std::string_view::npos);
    CHECK(user.find("Skip for now") != std::string_view::npos);
    CHECK(user.find("\n    \xE2\x86\xB3 ") == std::string_view::npos);
}

// ── Selection, opening position, blocks, cards, inline threads ─────────────

namespace {

ui::View *findLeaf(ui::View *v, std::string_view needle) {
    if (v->accessibleName().find(needle) != std::string::npos && v->childCount() == 0)
        return v;
    for (size_t i = 0; i < v->childCount(); ++i)
        if (ui::View *f = findLeaf(v->child(i), needle))
            return f;
    return nullptr;
}

// $MSGA_TEST_DUMP=<dir>: the window as a PPM (for looking at the rows).
void dump(Env &e, const char *name) {
    if (const char *dir = std::getenv("MSGA_TEST_DUMP"))
        e.win->dumpFullRepaint(std::string(dir) + "/" + name + ".ppm");
}

ui::Event key(plat::Key k, uint32_t mods) {
    ui::Event ev{ui::EventType::KeyDown};
    ev.key  = k;
    ev.mods = mods;
    return ev;
}

} // namespace

TEST("selection: drag positions, across messages, Ctrl+C copies, Escape clears") {
    Env                         e(false);
    const int64_t               t0 = base::nowSecs() - 3600;
    std::vector<model::Message> ms;
    ms.push_back(msg(0, t0, "first message here"));
    ms.push_back(msg(1, t0 + 600, "second one\n```with code```"));
    model::Message ed = msg(1, t0 + 1200, "edited text");
    ed.edited         = true;
    ms.push_back(std::move(ed));
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump(8);
    const Ts   a = t0 * 1000000, b = (t0 + 600) * 1000000, d = (t0 + 1200) * 1000000;
    // What a selection runs over, also for rows not on screen.
    const auto texts = selectableTexts(e.ctx, *e.store.findMessage(c, b));
    REQUIRE(texts.size() == 2);
    CHECK_STR(texts[1], "with code");
    CHECK_STR(selectableTexts(e.ctx, *e.store.findMessage(c, d)).back(), "edited text (edited)");
    // A point on the first message's text maps to it.
    ui::View *l = findLeaf(e.row(a), "first message here");
    REQUIRE(l != nullptr);
    const ui::RectF r = l->windowRect();
    const auto      p = e.list->textPosAt({r.x + 1, r.y + r.h / 2});
    CHECK(p.ts == a && p.offset == 0);
    CHECK(e.list->textPosAt({r.x + 1, r.y - 30}).ts == 0); // the header is not text
    // From "message" in the first to "with" in the second.
    e.list->select({a, 6}, {b, 15}); // "second one\nwith code"
    CHECK(e.list->hasSelection());
    CHECK_STR(e.list->selectedText(), "message here\nsecond one\nwith");
    auto *lab = static_cast<ui::Label *>(l);
    CHECK(lab->selectionFrom() == 6 && lab->selectionTo() == 18);
    // Backwards is the same selection.
    e.list->select({b, 15}, {a, 6});
    CHECK_STR(e.list->selectedText(), "message here\nsecond one\nwith");
    ui::Event copy = key(plat::Key::C, plat::primaryMod());
    CHECK(e.list->onEvent(copy));
    CHECK_STR(clipboard(), "message here\nsecond one\nwith");
    // NumLock and CapsLock ride along as modifiers; Ctrl+C still copies.
    app().platform().setClipboardText("before");
    ui::Event locked = key(plat::Key::C, plat::primaryMod() | plat::ModNum | plat::ModCaps);
    CHECK(e.list->onEvent(locked));
    CHECK_STR(clipboard(), "message here\nsecond one\nwith");
    ui::Event esc = key(plat::Key::Escape, 0);
    CHECK(e.list->onEvent(esc));
    CHECK_FALSE(e.list->hasSelection());
    CHECK(lab->selectionTo() == 0);
    ui::Event copy2 = key(plat::Key::C, plat::primaryMod());
    CHECK_FALSE(e.list->onEvent(copy2)); // nothing selected: not ours
}

TEST("selection: a pointer drag runs from one message's text into the next ones") {
    Env                         e(false);
    const int64_t               t0 = base::nowSecs() - 3600;
    std::vector<model::Message> ms;
    ms.push_back(msg(0, t0, "first message here"));
    ms.push_back(msg(0, t0 + 60, "second one"));
    ms.push_back(msg(0, t0 + 120, "third text"));
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump(8);
    const Ts  a = t0 * 1000000, d = (t0 + 120) * 1000000;
    ui::View *l1 = findLeaf(e.row(a), "first message here");
    ui::View *l3 = findLeaf(e.row(d), "third text");
    REQUIRE(l1 != nullptr && l3 != nullptr);
    const ui::RectF r1 = l1->windowRect(), r3 = l3->windowRect();
    auto           *h = app().platform().testHooks();
    h->injectPointerMove(e.win->native(), {r1.x + 1, r1.y + r1.h / 2});
    pump(2);
    h->injectButton(e.win->native(), plat::Button::Left, true);
    pump(2);
    // Down through the second message's header and text into the third's.
    for (float y = r1.y + r1.h / 2; y < r3.y + r3.h / 2; y += 6) {
        h->injectPointerMove(e.win->native(), {r1.x + 40, y});
        pump(1);
    }
    h->injectPointerMove(e.win->native(), {r3.x + r3.w, r3.y + r3.h / 2});
    pump(2);
    CHECK_STR(e.list->selectedText(), "first message here\nsecond one\nthird text");
    // Over the header between texts: the next message's start.
    const ui::RectF r2 = findLeaf(e.row(d - 60000000), "second one")->windowRect();
    h->injectPointerMove(e.win->native(), {r2.x + 40, r2.y - 4});
    pump(2);
    CHECK_STR(e.list->selectedText(), "first message here");
    // Below the list (over the composer): the last text's end.
    const ui::RectF lr = e.list->list().windowRect();
    h->injectPointerMove(e.win->native(), {lr.x + 40, lr.y + lr.h + 10});
    pump(2);
    h->injectButton(e.win->native(), plat::Button::Left, false);
    pump(2);
    CHECK_STR(e.list->selectedText(), "first message here\nsecond one\nthird text");
}

TEST("selection: a drag held past the list's top scrolls it up, selecting on") {
    Env                         e(false);
    const int64_t               t0 = base::nowSecs() - 7200;
    std::vector<model::Message> ms;
    for (int i = 0; i < 60; ++i)
        ms.push_back(msg(i % 2, t0 + i * 60, ("message " + std::to_string(i)).c_str()));
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump(8);
    const Ts  last = (t0 + 59 * 60) * 1000000;
    ui::View *l    = findLeaf(e.row(last), "message 59");
    REQUIRE(l != nullptr);
    const ui::RectF r     = l->windowRect();
    const ui::RectF lr    = e.list->list().windowRect();
    const float     start = e.list->list().scrollOffset();
    auto           *h     = app().platform().testHooks();
    // Not soon after the last case's press: that would be a double click.
    for (const double until = app().nowMs() + 450; app().nowMs() < until;)
        app().pump(5);
    h->injectPointerMove(e.win->native(), {r.x + 1, r.y + r.h / 2});
    pump(2);
    h->injectButton(e.win->native(), plat::Button::Left, true);
    pump(2);
    REQUIRE(!e.list->hasSelection());
    h->injectPointerMove(e.win->native(), {r.x + 40, lr.y + 2}); // in the top band
    for (const double until = app().nowMs() + 400; app().nowMs() < until;)
        app().pump(5);
    const float scrolled = start - e.list->list().scrollOffset();
    CHECK(scrolled > 40);
    // Back inside: the scrolling stops.
    h->injectPointerMove(e.win->native(), {r.x + 40, lr.y + lr.h / 2});
    pump(2);
    const float held = e.list->list().scrollOffset();
    for (const double until = app().nowMs() + 100; app().nowMs() < until;)
        app().pump(5);
    CHECK(e.list->list().scrollOffset() == held);
    h->injectButton(e.win->native(), plat::Button::Left, false);
    pump(2);
    // From a message scrolled into view down to the start of the last one.
    const std::string sel = e.list->selectedText();
    CHECK(sel.size() > 40);
    CHECK(sel.size() >= 10 && sel.compare(sel.size() - 10, 10, "message 58") == 0);
}

TEST("selection: in a message taller than the list, its end scrolled out") {
    Env           e(false);
    const int64_t t0 = base::nowSecs() - 3600;
    std::string   body;
    for (int i = 0; i < 40; ++i)
        body += "paragraph " + std::to_string(i) + "\n> quoted " + std::to_string(i) + "\n";
    std::vector<model::Message> ms;
    ms.push_back(msg(0, t0, "before"));
    ms.push_back(msg(0, t0 + 60, body.c_str()));
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump(8);
    const Ts ts = (t0 + 60) * 1000000;
    REQUIRE(e.row(ts) != nullptr);
    REQUIRE(static_cast<MessageRow *>(e.row(ts))->selectionLabels().size() > 2);
    // Up a little: the message's last texts are now below the list.
    e.list->list().scrollBy(-200);
    pump(8);
    const ui::RectF lr = e.list->list().windowRect();
    ui::View       *l  = nullptr;
    for (int i = 39; i >= 0 && !l; --i) {
        ui::View *v = findLeaf(e.row(ts), ("paragraph " + std::to_string(i)).c_str());
        if (v && v->windowRect().y > lr.y + 40 &&
            v->windowRect().y + v->windowRect().h < lr.y + lr.h - 40)
            l = v;
    }
    REQUIRE(l != nullptr);
    const ui::RectF r = l->windowRect();
    auto           *h = app().platform().testHooks();
    for (const double until = app().nowMs() + 450; app().nowMs() < until;)
        app().pump(5);
    h->injectPointerMove(e.win->native(), {r.x + 1, r.y + r.h / 2});
    pump(2);
    h->injectButton(e.win->native(), plat::Button::Left, true);
    pump(2);
    h->injectPointerMove(e.win->native(), {r.x + r.w, r.y + r.h / 2});
    pump(2);
    h->injectButton(e.win->native(), plat::Button::Left, false);
    pump(2);
    const std::string sel = e.list->selectedText();
    CHECK(sel.rfind("paragraph ", 0) == 0);
}

TEST("opening: the first unread a third down, the saved position on return") {
    Env                         e(false);
    const int64_t               t0 = base::nowSecs() - 7200;
    std::vector<model::Message> ms;
    for (int i = 0; i < 60; ++i)
        ms.push_back(msg(i % 2, t0 + i * 60, "a message long enough to take one line"));
    const ConvRef c  = addConv(e.store, std::move(ms));
    const Ts      lr = (t0 + 20 * 60) * 1000000; // read up to the 21st
    e.store.updateConversation(c, [&](model::Conversation &x) { x.lastRead = lr; });
    model::Conversation other;
    other.id            = "C2";
    other.name          = "other";
    other.hasMoreBefore = false;
    const ConvRef c2    = e.store.addConversation(std::move(other));
    e.list->showConversation(c);
    pump(8);
    const Ts  first = (t0 + 21 * 60) * 1000000;
    ui::View *row   = e.row(first);
    REQUIRE(row != nullptr);
    const float h = e.list->list().height();
    CHECK(std::abs(row->frame().y - std::floor(h / 3)) < 2);
    CHECK(e.store.conversation(c).lastRead == lr); // not marked read: the newest is off screen
    // Scrolled somewhere, away and back: the same place.
    e.list->list().scrollTo(e.list->list().scrollOffset() - 150);
    pump(4);
    const auto before = e.list->list().anchor();
    e.list->showConversation(c2);
    pump(4);
    e.list->showConversation(c);
    pump(8);
    const auto after = e.list->list().anchor();
    CHECK(after.index == before.index && std::abs(after.offset - before.offset) < 1);
    // Left at the bottom: back at the bottom.
    e.list->list().scrollToBottom();
    pump(4);
    e.list->showConversation(c2);
    e.list->showConversation(c);
    pump(8);
    CHECK(e.list->list().lastVisible() == int(e.list->items().size()) - 1);
}

TEST("blocks: headers, dividers, images and tables; ten rows and the pill; cards") {
    Env            e(false);
    const int64_t  t0 = base::nowSecs() - 3600;
    model::Message m  = msg(1, t0, "fallback");
    using K           = model::Block::Kind;
    auto &x           = m.extras();
    x.blocks.push_back({K::Header, "Weekly *numbers*", {}, {}, 0, 0, {}});
    x.blocks.push_back({K::Divider, {}, {}, {}, 0, 0, {}});
    x.blocks.push_back({K::Text, "Body under the rule", {}, {}, 0, 0, {}});
    model::Block tb;
    tb.kind = K::Table;
    tb.rows.push_back({"*Step*", "Drop-off"});
    for (int i = 0; i < 14; ++i)
        tb.rows.push_back({"Row " + std::to_string(i), std::to_string(i * 3) + "%"});
    x.blocks.push_back(tb);
    model::Block img;
    img.kind  = K::Image;
    img.image = asset("avatars/mira.png");
    img.text  = "a picture";
    img.width = img.height = 256;
    x.blocks.push_back(img);
    // A shared message, a PDF with its page, a canvas.
    model::Attachment un;
    un.msgUnfurl      = true;
    un.linkPreview    = true;
    un.author         = "Jonas";
    un.channel        = "C1";
    un.ts             = t0 * 1000000;
    un.text           = "quoted words";
    model::Message m2 = msg(0, t0 + 600, "see this");
    m2.extras().attachments.push_back(un);
    model::File pdf;
    pdf.id     = "FPDF";
    pdf.name   = "spec.pdf";
    pdf.mime   = "application/pdf";
    pdf.path   = asset("files/does-not-matter.pdf");
    pdf.thumb  = asset("avatars/mira.png");
    pdf.width  = 256;
    pdf.height = 256;
    m2.extras().files.push_back(pdf);
    model::File cv;
    cv.id    = "FCANVAS";
    cv.name  = "notes";
    cv.title = "Huddle notes";
    cv.mime  = "application/vnd.slack-docs";
    m2.extras().files.push_back(cv);
    std::vector<model::Message> ms;
    ms.push_back(std::move(m));
    ms.push_back(std::move(m2));
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump(8);
    ui::View *row = e.row(t0 * 1000000);
    REQUIRE(row != nullptr);
    CHECK(findLeaf(row, "fallback") == nullptr); // the blocks, not the text
    CHECK(findLeaf(row, "Weekly numbers") != nullptr);
    CHECK(findLeaf(row, "Body under the rule") != nullptr);
    CHECK(findLeaf(row, "a picture") != nullptr);
    auto *table = static_cast<TableView *>(findLeaf(row, "*Step* | Drop-off"));
    REQUIRE(table != nullptr);
    CHECK(table->clipped()); // 15 rows: ten shown, the pill on hover
    // The header and text blocks and the table are what a selection runs over.
    CHECK(static_cast<MessageRow *>(row)->selectionLabels().size() == 3);
    ui::View *row2 = e.row((t0 + 600) * 1000000);
    REQUIRE(row2 != nullptr);
    CHECK(findLeaf(row2, "Posted in #design") != nullptr);
    CHECK(findLeaf(row2, "quoted words") != nullptr);
    CHECK(findLeaf(row2, "spec.pdf") != nullptr); // the page's name band
    dump(e, "blocks");
    e.list->list().scrollToItem(0, ui::VirtualList::ItemAlign::Start, false);
    pump(6);
    if (ui::View *tr = e.row(t0 * 1000000))
        if (auto *tv = static_cast<TableView *>(findLeaf(tr, "*Step* | Drop-off"))) {
            const ui::RectF r = tv->windowRect();
            if (auto *h = app().platform().testHooks())
                h->injectPointerMove(e.win->native(), {r.x + 20, r.y + 40});
            pump(6);
        }
    dump(e, "blocks_top");
    // Hiding a preview (not mine: hidden here only).
    CHECK_FALSE(e.list->removesPreviewServerSide(*e.store.findMessage(c, (t0 + 600) * 1000000)));
    e.list->dismissAttachment((t0 + 600) * 1000000, 0);
    pump(4);
    row2 = e.row((t0 + 600) * 1000000);
    REQUIRE(row2 != nullptr);
    CHECK(findLeaf(row2, "quoted words") == nullptr);
}

TEST("blocks: a click on the × beside a link preview hides it") {
    Env                         e(false);
    auto                       *h  = app().platform().testHooks();
    const int64_t               t0 = base::nowSecs() - 3600;
    // A message with its author's header, then one grouped under it.
    std::vector<model::Message> ms;
    for (int i = 0; i < 2; ++i) {
        model::Message    m = msg(1, t0 + i * 60, "https://example.com");
        model::Attachment a;
        a.linkPreview = true;
        a.service     = "regex101";
        a.title       = i ? "Grouped preview" : "Header preview";
        a.link        = "https://example.com";
        a.text        = "Explore and test regular expressions";
        m.extras().attachments.push_back(a);
        ms.push_back(std::move(m));
    }
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump(8);
    for (int i = 0; i < 2; ++i) {
        const Ts    ts    = (t0 + i * 60) * 1000000;
        const char *title = i ? "Grouped preview" : "Header preview";
        ui::View   *row   = e.row(ts);
        REQUIRE(row != nullptr);
        ui::View *t = findLeaf(row, title);
        REQUIRE(t != nullptr);
        const ui::RectF card = t->parent()->parent()->windowRect();
        // Over the card the "×" shows in the gutter, 4 px left of it: the
        // pointer gets there across the gap a pixel at a time, as a hand
        // moves it.
        for (float x = card.x + 30; x >= card.x - 13; x -= 1) {
            h->injectPointerMove(e.win->native(), {x, card.y + 9});
            pump(1);
        }
        pump(4);
        h->injectButton(e.win->native(), plat::Button::Left, true);
        h->injectButton(e.win->native(), plat::Button::Left, false);
        pump(6);
        CHECK(e.list->attachmentHidden(*e.store.findMessage(c, ts), 0));
        row = e.row(ts);
        REQUIRE(row != nullptr);
        CHECK(findLeaf(row, title) == nullptr);
    }
}

TEST("selection: runs over a table's cells, tab apart, a row per line") {
    Env            e(false);
    const int64_t  t0 = base::nowSecs() - 3600;
    model::Message m  = msg(0, t0, "fallback");
    using K           = model::Block::Kind;
    m.extras().blocks.push_back({K::Text, "Intro", {}, {}, 0, 0, {}});
    model::Block tb;
    tb.kind = K::Table;
    tb.rows = {{"*Name*", "Score"}, {"Ann", "12"}, {"Bob", "7"}};
    m.extras().blocks.push_back(tb);
    std::vector<model::Message> ms;
    ms.push_back(std::move(m));
    const ConvRef c = addConv(e.store, std::move(ms));
    e.list->showConversation(c);
    pump(8);
    const Ts   a     = t0 * 1000000;
    const auto texts = selectableTexts(e.ctx, *e.store.findMessage(c, a));
    REQUIRE(texts.size() == 2);
    CHECK_STR(texts[1], "Name\tScore\nAnn\t12\nBob\t7");
    auto *tv = static_cast<TableView *>(findLeaf(e.row(a), "*Name* | Score"));
    REQUIRE(tv != nullptr);
    CHECK(tv->textSize() == texts[1].size());
    // A cell's start, a point past a row's last cell, through the list.
    const ui::RectF r = tv->windowRect();
    CHECK(tv->textOffsetAt({13, 14}) == 0);
    CHECK(tv->textOffsetAt({r.w - 2, r.h - 14}) == tv->textSize());
    const auto p = e.list->textPosAt({r.x + 13, r.y + r.h - 14});
    CHECK(p.ts == a && p.offset == 6 + 18); // "Intro\n" + "Name\tScore\nAnn\t12\n"
    // Double click: the cell's word; triple click: the table row.
    uint32_t from = 0, to = 0;
    tv->wordAt(12, &from, &to);
    CHECK(from == 11 && to == 14);
    tv->lineAt({13, 14}, &from, &to);
    CHECK(from == 0 && to == 10);
    // From the text into the table; Ctrl+C copies the grid.
    e.list->select({a, 0}, {a, 6 + 17});
    pump(2);
    dump(e, "table_selection");
    CHECK_STR(e.list->selectedText(), "Intro\nName\tScore\nAnn\t12");
    ui::Event copy = key(plat::Key::C, plat::primaryMod());
    CHECK(e.list->onEvent(copy));
    CHECK_STR(clipboard(), "Intro\nName\tScore\nAnn\t12");
    CHECK(tv->cursorAt({13, 14}) == uint8_t(plat::Cursor::IBeam));
}

TEST("blocks: a table shapes its cells once; a narrower column shapes only what wraps") {
    Env                                   e(false);
    std::vector<std::vector<std::string>> cells = {
        {"*Step*", "Drop-off"}, {"Visited the page", "0%"}, {"Signed up for the newsletter", "12%"}
    };
    auto *tv = e.win->root().add<TableView>(e.ctx, cells);
    tv->measure(600, ui::kInf);
    const size_t shaped = text::layoutBuilds();
    const float  wide   = tv->measure(700, ui::kInf).w;
    CHECK(text::layoutBuilds() == shaped); // fits either way: nothing shaped
    CHECK_FALSE(tv->clipped());
    const ui::SizeF narrow = tv->measure(150, ui::kInf);
    CHECK(narrow.w <= 150 && narrow.w < wide && tv->clipped());
    const size_t wrapped = text::layoutBuilds() - shaped;
    CHECK(wrapped > 0 && wrapped < 6); // the cells that wrap; "0%" and "12%" fit
    tv->measure(700, ui::kInf);
    CHECK(text::layoutBuilds() - shaped == wrapped); // back to the natural ones
}

TEST("blocks: the full table is rich; a canvas card names mentions and draws emoji") {
    Env e(false);
    addConv(e.store, {});
    e.store.setCustomEmoji("partyparrot", asset("avatars/mira.png"));
    // The table's pill: every row in the viewer, cells as the message has them.
    std::vector<std::vector<std::string>> cells{{"*Who*", "Says"}};
    for (int i = 0; i < 12; ++i)
        cells.push_back({"<@U2>", ":partyparrot: <https://example.com|site> :smile:"});
    auto *tv = e.win->root().add<TableView>(e.ctx, cells);
    pump(2);
    REQUIRE(tv->onOpenFull);
    tv->onOpenFull();
    pump(3);
    REQUIRE(e.win->topPopup() != nullptr);
    e.win->topPopup()->close();
    pump(2);
    // The canvas preview: the title h1 out, <a>@U…</a> as "@Mira" on the
    // mention colours, :codes: as Unicode or a custom emoji box.
    model::File f;
    f.id    = "FCANVAS";
    f.title = "Plan :smile:";
    CHECK_STR(canvasTitle(e.ctx, f), "Plan \xF0\x9F\x98\x84");
    const std::string html =
        "<div class=\"quip-canvas-content\"><h1 id=\"t\">Plan \xF0\x9F\x98\x84</h1>"
        "<p id=\"a\">Ask <a>@U2</a> about "
        "<img src=\"x\" data-is-slack=\"1\">:smile:</img> and :partyparrot: now</p>"
        "<p id=\"b\"><code>keep :smile: here</code></p></div>";
    std::vector<std::string>   images;
    const text::AttributedText t = canvasPreviewText(e.ctx, html, f, &images);
    CHECK(t.text.find("Plan") == std::string::npos); // the title is the card's header
    CHECK(
        t.text.find("Ask @Mira about \xF0\x9F\x98\x84 and :partyparrot: now") != std::string::npos
    );
    CHECK(t.text.find("keep :smile: here") != std::string::npos); // never inside code
    REQUIRE(images.size() == 1);
    CHECK_STR(images[0], asset("avatars/mira.png"));
    bool chip = false, box = false;
    for (const text::Span &sp : t.spans) {
        const std::string_view piece = std::string_view(t.text).substr(sp.start, sp.end - sp.start);
        if (piece == "@Mira")
            chip = sp.style.background == ui::themed(ui::C::MentionBg) &&
                   sp.style.color == ui::themed(ui::C::MentionText);
        if (piece == ":partyparrot:")
            box = sp.style.inlineBoxId == 1 && sp.style.boxWidth > 0;
    }
    CHECK(chip);
    CHECK(box);
}

TEST("blocks: mentions of me, @here/@channel and my user group are the yellow chip") {
    // The own-mention background; everyone else's mention keeps the blue one.
    Env e(false);
    addConv(e.store, {});
    e.store.setUsergroups({{"S1", "design", "Design", {"U1"}}, {"S2", "ops", "Ops", {"U2"}}});
    const text::AttributedText t =
        richText(e.ctx, "<@U1> <@U2> <!here> <!channel> <!subteam^S1> <!subteam^S2>", {});
    auto bg = [&t](std::string_view piece) {
        for (const text::Span &sp : t.spans)
            if (std::string_view(t.text).substr(sp.start, sp.end - sp.start) == piece)
                return sp.style.background;
        return gfx::Color(0);
    };
    const gfx::Color self = ui::themed(ui::C::MentionSelfBg), other = ui::themed(ui::C::MentionBg);
    CHECK(bg("@Me") == self);
    CHECK(bg("@Mira") == other);
    CHECK(bg("@here") == self);
    CHECK(bg("@channel") == self);
    CHECK(bg("@design") == self);
    CHECK(bg("@ops") == other);
}

TEST("blocks: a table, a canvas card and a selection as rendered (MSGA_TEST_DUMP)") {
    struct CanvasBackend : DownloadingBackend {
        using DownloadingBackend::DownloadingBackend;
        void loadCanvasContent(const std::string &, CanvasHtmlDone done) override {
            app().platform().post([done] {
                done(
                    "<h1>Crit</h1><h2>Action items</h2><p>Ask <a>@U2</a> about the "
                    "skip link :partyparrot: :white_check_mark:</p><p>Then <b>ship</b> it "
                    ":rocket:</p>",
                    {}
                );
            });
        }
    };
    model::Store     store;
    CanvasBackend    backend{store, app().platform()};
    ImageCache       images{app().platform()};
    Context          ctx{app(), store, backend, images};
    plat::WindowDesc d;
    d.size     = {800, 600};
    auto  win  = std::make_unique<ui::Window>(d);
    auto *list = win->root().add<MessageList>(ctx);
    store.setCustomEmoji("partyparrot", asset("avatars/mira.png"));
    const int64_t  t0 = base::nowSecs() - 3600;
    const ConvRef  c  = addConv(store, {});
    model::Message m  = msg(1, t0, "fallback");
    model::Block   tb;
    tb.kind = model::Block::Kind::Table;
    tb.rows.push_back({"*Who*", "Says"});
    for (int i = 0; i < 11; ++i)
        tb.rows.push_back({"<@U2>", "*bold* :partyparrot: <https://example.com|site> :smile:"});
    m.extras().blocks.push_back(tb);
    model::Message m2 = msg(1, t0 + 60, "Selected words here and a mention <@U1>");
    model::File    cv;
    cv.id    = "FCANVAS";
    cv.name  = "crit";
    cv.title = "Crit";
    cv.mime  = "application/vnd.slack-docs";
    m2.extras().files.push_back(cv);
    store.addPage(c, [&] {
        std::vector<model::Message> v;
        v.push_back(std::move(m));
        v.push_back(std::move(m2));
        return v;
    }());
    list->showConversation(c);
    pump(10);
    const char *dir = std::getenv("MSGA_TEST_DUMP");
    for (const auto &it : list->items())
        if (it.ts == (t0 + 60) * 1000000)
            if (auto *row =
                    static_cast<MessageRow *>(list->list().viewFor(int(&it - &list->items()[0]))))
                if (!row->selectionLabels().empty())
                    row->selectionLabels()[0]->selectText(0, 14);
    pump(4);
    if (dir)
        win->dumpFullRepaint(std::string(dir) + "/rich_list.ppm");
    std::vector<std::vector<std::string>> cells = tb.rows;
    REQUIRE(showTableViewer(*win, ctx, cells) != nullptr);
    pump(6);
    if (dir)
        win->dumpFullRepaint(std::string(dir) + "/rich_table_viewer.ppm");
    win.reset();
}

TEST("blocks: a bot card's mrkdwn title, Show more and its own buttons (MSGA_TEST_DUMP)") {
    // Outlook Calendar's reminder: the title holds dates and a link, the
    // long text folds, the second attachment is only its button.
    model::Store       store;
    DownloadingBackend backend{store, app().platform()};
    ImageCache         images{app().platform()};
    Context            ctx{app(), store, backend, images};
    plat::WindowDesc   d;
    d.size                 = {800, 600};
    auto              win  = std::make_unique<ui::Window>(d);
    auto             *list = win->root().add<MessageList>(ctx);
    const int64_t     t0   = base::nowSecs() - 3600;
    const ConvRef     c    = addConv(store, {});
    model::Message    m    = msg(1, t0, "");
    model::Attachment a;
    a.color   = "#3AA3E3";
    a.pretext = ":loudspeaker: _1 minute until this event:_";
    a.author  = "Every weekday";
    a.title   = "<!date^1791184500^{time}|10:15 AM> - <!date^1791185400^{time}|10:30 AM> "
                "<https://outlook.office365.com/owa/?itemid=X&amp;path=/calendar/item|Stand-Up>";
    a.text    = "*Where:* Room 4\n*Guests:* <mailto:a@b.se|a@b.se> _(organizer)_, B, C\n"
                "*What:* Google Meet\nType : Video\nJoin Meeting : <https://meet.google.com/x>\n"
                " \nType : Phone\nPIN : 463247662";
    m.extras().attachments.push_back(a);
    m.extras().attachments.push_back({});
    model::Button b;
    b.label      = "Join Google Meet Meeting";
    b.url        = "https://meet.google.com/x";
    b.style      = model::Button::Style::Primary;
    b.attachment = 2;
    m.extras().buttons.push_back(b);
    store.addPage(c, [&] {
        std::vector<model::Message> v;
        v.push_back(std::move(m));
        return v;
    }());
    list->showConversation(c);
    pump(10);
    std::string                           shown;
    const std::function<void(ui::View *)> walk = [&](ui::View *v) {
        shown += v->accessibleName() + "\n";
        for (size_t i = 0; i < v->childCount(); ++i)
            walk(v->child(i));
    };
    walk(&win->root());
    CHECK(shown.find("<!date") == std::string::npos);
    CHECK(shown.find("Stand-Up") != std::string::npos);
    CHECK(shown.find("Show more") != std::string::npos);
    CHECK(shown.find("PIN") == std::string::npos); // folded
    CHECK(shown.find("Join Google Meet Meeting") != std::string::npos);
    if (const char *dir = std::getenv("MSGA_TEST_DUMP"))
        win->dumpFullRepaint(std::string(dir) + "/bot_card.ppm");
    win.reset();
}

TEST("blocks: a remove preview on my own message goes to the backend") {
    struct RemovingBackend : DownloadingBackend {
        using DownloadingBackend::DownloadingBackend;
        int          removed = -1;
        Capabilities capabilities() const override {
            Capabilities c  = DownloadingBackend::capabilities();
            c.removePreview = true;
            return c;
        }
        void deleteAttachment(ConvRef c, Ts ts, int id, Done done) override {
            removed = id;
            _store.updateMessage(c, ts, [](model::Message &m) { m.extra->attachments.clear(); });
            app().platform().post([done] { done(true, {}); });
        }
    };
    model::Store     store;
    RemovingBackend  backend{store, app().platform()};
    ImageCache       images{app().platform()};
    Context          ctx{app(), store, backend, images};
    plat::WindowDesc d;
    d.size                 = {800, 600};
    auto              win  = std::make_unique<ui::Window>(d);
    auto             *list = win->root().add<MessageList>(ctx);
    model::Message    m    = msg(0, base::nowSecs() - 60, "https://example.com");
    model::Attachment a;
    a.linkPreview = true;
    a.title       = "Example";
    m.extras().attachments.push_back(a);
    const Ts      ts = m.ts;
    const ConvRef c  = addConv(store, {});
    store.addPage(c, [&] {
        std::vector<model::Message> v;
        v.push_back(std::move(m));
        return v;
    }());
    list->showConversation(c);
    pump(6);
    CHECK(list->removesPreviewServerSide(*store.findMessage(c, ts)));
    list->dismissAttachment(ts, 0);
    REQUIRE(until([&] { return backend.removed == 1; }));
    pump(4);
    CHECK(store.findMessage(c, ts)->attachments().empty());
    win.reset();
}

TEST("threads: inline, the reply bar opens the replies under the message, and closes them") {
    Env e(true);
    e.ctx.threadsInline = true;
    e.list->setThreadsInline(true);
    const ConvRef c = e.conv("C0DESIGN");
    e.list->showConversation(c);
    pump(8);
    Ts root = 0;
    for (const model::Message &m : e.store.conversation(c).messages)
        if (m.replyCount > 0)
            root = m.ts;
    REQUIRE(root != 0);
    e.list->scrollToMessage(root, false, false);
    pump(6);
    e.list->replyBarClicked(root);
    CHECK(e.list->inlineOpen(root) && e.list->threadOpen(root));
    REQUIRE(until([&] { return e.store.replies(c, root) != nullptr; }));
    pump(8);
    ui::View *row = e.row(root);
    REQUIRE(row != nullptr);
    CHECK(findLeaf(row, "Reply to thread") != nullptr);
    CHECK(findLeaf(row, "Close thread") != nullptr);
    CHECK(e.threads.empty()); // no panel
    dump(e, "inline");
    e.list->replyBarClicked(root);
    pump(4);
    CHECK_FALSE(e.list->inlineOpen(root));
    row = e.row(root);
    REQUIRE(row != nullptr);
    CHECK(findLeaf(row, "Reply to thread") == nullptr);
}

TEST("blocks: the canvas section diff writes only what changed") {
    using namespace screens::canvas;
    const std::string html =
        "<div class=\"quip-canvas-content\"><h1 id=\"t\">Title</h1>"
        "<p id=\"a\">One</p><h2 id=\"b\">Two</h2>"
        "<div data-section-style='5'><ul id='c'><li id='c1'>x</li><li id='c2'>y</li></ul></div>"
        "<blockquote><p id='q'>quoted</p></blockquote><p id=\"d\">Four <b>bold</b></p></div>";
    std::vector<Chunk> base;
    REQUIRE(baseChunks(html, {"Title"}, &base));
    REQUIRE(base.size() == 5);
    CHECK_STR(base[0].md, "One");
    CHECK_STR(base[2].md, "- x\n- y");
    CHECK(base[3].fragile && base[2].id == "c");
    CHECK_STR(base[4].md, "Four **bold**");
    std::vector<Change> ops;
    // Unchanged: nothing.
    REQUIRE(
        diff(base, documentChunks("One\n\n## Two\n\n- x\n- y\n\n> quoted\n\nFour **bold**"), &ops)
    );
    CHECK(ops.empty());
    // A paragraph edited, one inserted after it, the list removed.
    REQUIRE(diff(base, documentChunks("One!\n\nNew\n\n## Two\n\n> quoted\n\nFour **bold**"), &ops));
    REQUIRE(ops.size() == 3);
    CHECK(
        ops[0].op == Change::Op::InsertAfter && ops[0].sectionId == "a" && ops[0].markdown == "New"
    );
    CHECK(ops[1].op == Change::Op::ReplaceSection && ops[1].sectionId == "a");
    CHECK(ops[2].op == Change::Op::DeleteSection && ops[2].sectionId == "c");
    // The quote touched: the whole document instead.
    CHECK_FALSE(
        diff(base, documentChunks("One\n\n## Two\n\n- x\n- y\n\n> changed\n\nFour **bold**"), &ops)
    );
}

TEST("blocks: a huddle row says who, how many and how long in one sentence") {
    model::Store st;
    model::User  u;
    u.id                       = "UME";
    u.name                     = "me";
    st.me                      = st.addUser(u);
    u.id                       = "UMIRA";
    u.name                     = "mira";
    u.displayName              = "Mira";
    const model::UserRef mira  = st.addUser(u);
    u.id                       = "UJONAS";
    u.displayName              = "Jonas";
    const model::UserRef jonas = st.addUser(u);
    u.id                       = "UKAI";
    u.displayName              = "Kai";
    const model::UserRef kai   = st.addUser(u);
    model::Huddle        h;
    CHECK_STR(screens::huddleSummaryText(st, h), "The huddle is waiting for people to join.");
    h.attendees = {mira};
    CHECK_STR(screens::huddleSummaryText(st, h), "Mira is in the huddle.");
    h.attendees = {mira, st.me};
    CHECK_STR(screens::huddleSummaryText(st, h), "You and Mira are in the huddle.");
    h.ended     = true;
    h.startSec  = 1000;
    h.endSec    = 1000 + 65 * 60;
    h.attendees = {mira};
    CHECK_STR(screens::huddleSummaryText(st, h), "Mira was in the huddle for 1h 5m.");
    h.endSec    = 1000 + 20;
    h.attendees = {mira, jonas, kai, st.me};
    CHECK_STR(
        screens::huddleSummaryText(st, h), "You, Mira and 2 others were in the huddle for 1m."
    );
    h.startSec  = 0;
    h.attendees = {mira, jonas};
    CHECK_STR(screens::huddleSummaryText(st, h), "Mira and Jonas were in the huddle.");
    h.attendees.clear();
    CHECK_STR(screens::huddleSummaryText(st, h), "Nobody joined the huddle.");
}

// ── Inline audio player ─────────────────────────────────────────────────────

namespace {

// plat's player, scripted: loads on the next turn, plays a 3 s clip.
struct FakePlayer final : plat::audio::Player {
    struct Log {
        std::vector<std::string> loaded;
        bool                     playing = false;
        int64_t                  pos = 0, seekedTo = -1;
    };
    explicit FakePlayer(Log &l) : log(l) {}
    void load(const std::string &path) override {
        log.loaded.push_back(path);
        app().platform().post([this, alive = std::weak_ptr<char>(alive)] {
            if (!alive.expired() && onLoaded)
                onLoaded();
        });
    }
    void                  play() override { log.playing = true; }
    void                  pause() override { log.playing = false; }
    void                  seek(int64_t ms) override { log.pos = log.seekedTo = ms; }
    void                  stop() override { log.playing = false; }
    int64_t               positionMs() const override { return log.pos; }
    int64_t               durationMs() const override { return 3000; }
    Log                  &log;
    std::shared_ptr<char> alive = std::make_shared<char>(0);
};

media::AudioPlayer::State audioState(const media::AudioPlayer &p) {
    return p.status().state;
}

// The voice note in #random (the fixture's slack_audio file) and its card.
struct AudioEnv : Env {
    FakePlayer::Log    log;
    media::AudioPlayer player{app().platform()};
    ConvRef            random = model::kNoConv;
    Ts                 ts     = 0;
    model::File        file;

    AudioEnv() : Env(true) {
        player.playerFactory = [this] { return std::make_unique<FakePlayer>(log); };
        ctx.audio            = &player;
        ctx.window           = win.get();
        random               = conv("C0RANDOM");
        for (const model::Message &m : store.conversation(random).messages)
            for (const model::File &f : m.files())
                if (f.subtype == "slack_audio") {
                    ts   = m.ts;
                    file = f;
                }
        list->showConversation(random);
        pump(10);
    }
    // The cards observe the player: the views go first (Env's destructor
    // would run only after the player is gone).
    ~AudioEnv() { win.reset(); }
    ui::View *card() {
        list->scrollToMessage(ts, false, false);
        pump(6);
        ui::View *r = row(ts);
        return r ? findNamed(r, file.name) : nullptr;
    }
};

} // namespace

TEST("audio: a voice clip is a card: size, transcript line, play / pause / seek") {
    AudioEnv e;
    REQUIRE(e.ts != 0);
    ui::View *card = e.card();
    REQUIRE(card != nullptr);
    // 380 wide (the list is wide enough), 88 + the 26-px transcript line.
    CHECK(card->width() == kAudioCardMaxW);
    CHECK(card->height() == kAudioCardH + kAudioTranscriptH);
    CHECK(findNamed(card, "View transcript") != nullptr);
    CHECK(findByTooltip(card, "Transcribe with AI") != nullptr);

    // Play: a local file plays as it is (no download); then pause, resume.
    auto *play = static_cast<ui::Clickable *>(findNamed(card, "Play"));
    REQUIRE(play != nullptr);
    play->activate();
    CHECK(audioState(e.player) == media::AudioPlayer::State::Loading);
    REQUIRE(until([&] { return audioState(e.player) == media::AudioPlayer::State::Playing; }));
    REQUIRE(e.log.loaded.size() == 1);
    CHECK_STR(e.log.loaded[0], e.file.path);
    CHECK(e.log.playing);
    CHECK(model::jobs().count() == 0);
    card = e.card();
    REQUIRE(card != nullptr);
    CHECK(findNamed(card, "Pause") != nullptr);
    // The position ticks in.
    e.log.pos = 1200;
    REQUIRE(until([&] { return e.player.status().positionMs == 1200; }, 1000));
    // A click anywhere on the card pauses; again resumes.
    static_cast<ui::Clickable *>(card)->activate();
    CHECK(audioState(e.player) == media::AudioPlayer::State::Paused);
    CHECK_FALSE(e.log.playing);
    static_cast<ui::Clickable *>(card)->activate();
    CHECK(audioState(e.player) == media::AudioPlayer::State::Playing);
    // Seek: press on the bar, release; lands where released.
    ui::Event down{ui::EventType::PointerDown};
    down.pos = {18 + 10, 62};
    CHECK(card->onEvent(down));
    ui::Event up{ui::EventType::PointerUp};
    up.pos = {card->width() / 2, 62};
    CHECK(card->onEvent(up));
    CHECK(e.log.seekedTo > 0 && e.log.seekedTo < 3000);
    // Ended → shows the length; a click restarts from 0.
    e.log.playing = false;
    e.player.stop();
    CHECK(audioState(e.player) == media::AudioPlayer::State::Idle);
}

TEST("audio: a remote clip downloads once into the audio cache as a job; one plays at a time") {
    Env                e(false);
    FakePlayer::Log    log;
    media::AudioPlayer player(app().platform());
    player.playerFactory = [&] { return std::make_unique<FakePlayer>(log); };
    e.ctx.audio          = &player;
    model::File a;
    a.id          = "FA1";
    a.name        = "voice-note.mp3";
    a.mime        = "audio/mpeg";
    a.path        = "https://files.test/audio/voice-note.mp3";
    a.durationMs  = 3000;
    model::File b = a;
    b.id          = "FB2";
    toggleAudio(e.ctx, a);
    CHECK(audioState(player) == media::AudioPlayer::State::Loading);
    REQUIRE(model::jobs().count() == 1);
    CHECK_STR(model::jobs().descriptions()[0], "Downloading voice-note.mp3");
    REQUIRE(until([&] { return audioState(player) == media::AudioPlayer::State::Playing; }));
    CHECK(model::jobs().count() == 0);
    REQUIRE(e.backend.asked.size() == 1);
    const std::string cached = e.backend.asked[0].to;
    CHECK(cached.find("/audio/FA1-") != std::string::npos);
    CHECK(str::endsWith(cached, ".mp3"));
    CHECK(file::size(cached) > 0);
    CHECK_STR(log.loaded.back(), cached);
    // Another clip takes over: the first goes back to idle.
    std::vector<std::string> changed;
    const auto obs = player.observe([&](const std::string &k) { changed.push_back(k); });
    toggleAudio(e.ctx, b);
    CHECK(player.isCurrent("FB2"));
    CHECK(std::find(changed.begin(), changed.end(), "FA1") != changed.end());
    REQUIRE(until([&] { return audioState(player) == media::AudioPlayer::State::Playing; }));
    // Back to the first: already on disk, no second download.
    toggleAudio(e.ctx, a);
    REQUIRE(until([&] { return audioState(player) == media::AudioPlayer::State::Playing; }));
    CHECK(e.backend.asked.size() == 2);
    player.unobserve(obs);
    // A failed download says so on the card.
    model::File gone = a;
    gone.id          = "FGONE";
    gone.path        = "https://files.test/nope.mp3";
    toggleAudio(e.ctx, gone);
    REQUIRE(until([&] { return audioState(player) == media::AudioPlayer::State::Error; }));
    CHECK_STR(player.status().error, "Download failed");
    file::remove(cached);
}

TEST("audio: View transcript shows Slack's line; Transcribe with AI replaces it") {
    const std::string &base = fakellm::server();
    if (base.empty()) {
        std::printf("skip: no python3 for the fake AI provider\n");
        return;
    }
    fakellm::ctl(app().platform(), "POST", "/_ctl/reset");
    llm::Service ai(app().platform()); // outlives the cards that observe it
    AudioEnv     e;
    e.ctx.ai       = &ai;
    ui::View *card = e.card();
    REQUIRE(card != nullptr);

    // Slack's transcript (no cues in the fixture): the preview line.
    static_cast<ui::Clickable *>(findNamed(card, "View transcript"))->activate();
    ui::Popup *dlg = e.win->topPopup();
    REQUIRE(dlg != nullptr);
    CHECK(findNamed(dlg, "Transcript (auto-generated)") != nullptr);
    CHECK(findNamed(dlg, "Okay so the queue is about twenty people") != nullptr);
    CHECK(findNamed(dlg, "Ravi") != nullptr); // "Ravi … at 12:48"
    static_cast<ui::Dialog *>(dlg)->reject();
    pump(4);

    // No speech-to-text provider: the dialog says so, nothing is fetched.
    auto *tb = static_cast<ui::Clickable *>(findByTooltip(card, "Transcribe with AI"));
    REQUIRE(tb != nullptr);
    tb->activate();
    dlg = e.win->topPopup();
    REQUIRE(dlg != nullptr);
    CHECK(findNamed(dlg, "Transcription needs an AI provider.") != nullptr);
    static_cast<ui::Dialog *>(dlg)->reject();
    pump(4);
    ai.setProviders({llm::fromSettings("anthropic", "Anthropic", "", "k", "", "")}, "anthropic");
    tb->activate();
    dlg = e.win->topPopup();
    REQUIRE(dlg != nullptr);
    CHECK(findNamed(dlg, "Anthropic does not support speech-to-text.") != nullptr);
    static_cast<ui::Dialog *>(dlg)->reject();
    pump(4);

    // An OpenAI-compatible server beside the Anthropic default transcribes.
    ai.setProviders(
        {llm::fromSettings("anthropic", "Anthropic", "", "k", "", ""),
         llm::fromSettings("custom-1", "Fake", base + "/v1", "", "m", "")},
        "anthropic"
    );
    tb->activate();
    dlg = e.win->topPopup();
    REQUIRE(dlg != nullptr);
    CHECK(findNamed(dlg, "transcribed by Fake") != nullptr);
    CHECK(findNamed(dlg, "Loading") != nullptr);
    REQUIRE(
        until([&] { return findNamed(dlg, "um so the deploy is uh ready") != nullptr; }, 10000)
    );
    CHECK(ai.transcriber().cached(e.file.id) != nullptr);
    // Adopted: the Store keeps it, and the card shows it in Slack's place.
    REQUIRE(e.store.aiTranscript(e.file.id) != nullptr);
    CHECK_STR(e.store.aiTranscript(e.file.id)->by, "Fake");
    static_cast<ui::Dialog *>(dlg)->reject();
    pump(6);
    card = e.card();
    REQUIRE(card != nullptr);
    static_cast<ui::Clickable *>(findNamed(card, "View transcript"))->activate();
    dlg = e.win->topPopup();
    REQUIRE(dlg != nullptr);
    CHECK(findNamed(dlg, "um so the deploy is uh ready") != nullptr);
    CHECK(findNamed(dlg, "transcribed by Fake") != nullptr);
    static_cast<ui::Dialog *>(dlg)->reject();
    pump(4);

    json::Document log;
    REQUIRE(log.parse(fakellm::ctl(app().platform(), "GET", "/_ctl/log").body, nullptr));
    REQUIRE(log.root().size() == 1);
    const std::string body(log.root()[0]["body"].str());
    CHECK(body.find("filename=\"" + e.file.id + ".mp3\"") != std::string::npos);
    CHECK(body.find("Content-Type: audio/mpeg") != std::string::npos);
    CHECK(body.find("whisper-1") != std::string::npos);
}

TEST("audio: WebVTT cues and duration labels") {
    const auto cues = parseVtt(
        "\xEF\xBB\xBFWEBVTT\r\n\r\n1\r\n00:00:01.500 --> 00:00:03.000\r\n- <v Mira>Hello</v>\r\n"
        "there\r\n\r\n01:02.250 --> 01:04.000\n- Second <i>cue</i>\n\nbad --> line\nx\n"
    );
    REQUIRE(cues.size() == 2);
    CHECK(cues[0] == (VttCue{1500, "Hello there"}));
    CHECK(cues[1] == (VttCue{62250, "Second cue"}));
    CHECK_STR(formatDuration(4980), "0:04");
    CHECK_STR(formatDuration(4980, true), "0:05");
    CHECK_STR(formatDuration(754000), "12:34");
    CHECK_STR(formatDuration(3723000), "1:02:03");
    CHECK_STR(media::AudioPlayer::extensionOf("https://f/x/Clip.MP3?t=1"), "mp3");
    CHECK_STR(media::AudioPlayer::extensionOf("voice note"), "");
}

TEST("audio: playback ticks reshape nothing but a changed time label") {
    AudioEnv e;
    REQUIRE(e.ts != 0);
    ui::View *card = e.card();
    REQUIRE(card != nullptr);
    auto *play = static_cast<ui::Clickable *>(findNamed(card, "Play"));
    REQUIRE(play != nullptr);
    play->activate();
    REQUIRE(until([&] { return audioState(e.player) == media::AudioPlayer::State::Playing; }));
    card = e.card();
    REQUIRE(card != nullptr);
    e.log.pos = 1200;
    REQUIRE(until([&] { return e.player.status().positionMs == 1200; }, 1000));
    gfx::Bitmap  bmp(int(card->width()) + 1, int(card->height()) + 1);
    gfx::Painter p(bmp.view(), 1.f);
    card->paint(p);
    const size_t warm = audioCardLayoutBuilds();
    // The same second, painted again and again (the player's 200 ms ticks).
    for (int i = 0; i < 5; ++i)
        card->paint(p);
    CHECK(audioCardLayoutBuilds() == warm);
    // A new second: the time label alone is shaped again (maybe already by
    // the window's own repaint), once.
    e.log.pos = 2200;
    REQUIRE(until([&] { return e.player.status().positionMs == 2200; }, 1000));
    card->paint(p);
    card->paint(p);
    CHECK(audioCardLayoutBuilds() == warm + 1);
    e.log.playing = false;
    e.player.stop();
}

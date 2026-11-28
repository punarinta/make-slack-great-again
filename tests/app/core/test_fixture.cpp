// The fake workspace: time specs, loadFixture (on the test fixture,
// tests/assets), the interactive backend, on the plat headless loop.
#include "app/fake/fake_backend.h"
#include "app/fake/fixture.h"
#include "app/model/image_size.h"
#include "app/mrkdwn/mrkdwn.h"
#include "base/file.h"
#include "base/json.h"
#include "support/test.h"
#include "base/time.h"

#include <cstdlib>
#include <memory>
#include <unistd.h>

using namespace model;

namespace {

const int64_t kNow = base::fromLocal(2026, 9, 21, 16, 0);

std::string writeFixture(const std::string &json) {
    std::string dir = base::test::makeTempDir("msga_fixture_");
    if (dir.empty())
        dir = "/tmp";
    file::writeAtomic(file::join(dir, "fixture.json"), json);
    file::writeAtomic(file::join(dir, "data.csv"), "a,b\n1,2\n");
    return dir;
}

const std::string kMinimal = R"({
  "workspace": {"id": "T1", "name": "Acme"},
  "me": "U1",
  "startConversation": "C2",
  "users": [
    {"id": "U1", "name": "me", "displayName": "Me", "tz": "+02:00"},
    {"id": "U2", "name": "mira", "displayName": "Mira", "active": true, "status": {"emoji": "art", "text": "Busy"}}
  ],
  "conversations": [
    {"id": "C1", "kind": "channel", "name": "general", "unread": 1},
    {"id": "C2", "kind": "channel", "name": "design"},
    {"id": "D1", "kind": "dm", "user": "U2", "unread": 5},
    {"id": "G1", "kind": "group", "members": ["U2"]}
  ],
  "messages": [
    {"conv": "C1", "user": "U2", "time": "-1d 09:00", "text": "first *bold* <@U1>",
     "reactions": [{"name": "tada", "users": ["U1", "U2"]}],
     "replies": [
       {"user": "U1", "time": "+10m", "text": "reply one"},
       {"user": "U2", "time": "+5m", "text": "reply two"}
     ]},
    {"conv": "C1", "user": "U1", "time": "-1d 09:30", "text": "second", "files": [{"path": "data.csv"}]},
    {"conv": "C1", "user": "U2", "time": "-1d 08:00", "text": "actually the oldest"},
    {"conv": "D1", "user": "U2", "time": "-30m", "text": "dm"},
    {"conv": "C2", "user": "U2", "time": "-20m", "text": "",
     "files": [{"path": "data.csv", "name": "Voice note", "subtype": "slack_audio", "durationMs": 3000, "transcript": "hello there"}]}
  ],
  "unfurls": [
    {"url": "https://example.test/doc", "service": "Docs", "title": "A doc", "text": "preview", "color": "#123456"}
  ],
  "ai": {
    "default": "generic summary",
    "replies": [{"match": "palette", "text": "palette summary"}]
  },
  "autoReplies": [
    {"conv": "C2", "user": "U2", "afterMs": 0, "typingMs": 0, "text": "canned"},
    {"conv": "C1", "user": "U2", "afterMs": 10, "typingMs": 30, "text": "typed answer"}
  ]
})";

std::string replace(std::string s, std::string_view from, std::string_view to) {
    const size_t p = s.find(from);
    if (p != std::string::npos)
        s.replace(p, from.size(), to);
    return s;
}

int64_t spec(std::string_view s, int64_t prev = -1) {
    int64_t out = 0;
    return fake::parseTimeSpec(s, kNow, prev, &out) ? out : INT64_MIN;
}

// The plat headless loop drives the fake backend's timers.
std::unique_ptr<plat::App> headlessApp() {
    base::test::setEnv("PLAT_BACKEND", "headless");
    std::string err;
    auto        app = plat::App::create(&err);
    if (!app)
        std::fprintf(stderr, "    plat::App::create: %s\n", err.c_str());
    return app;
}

bool waitFor(plat::App &app, const std::function<bool()> &pred, int ms = 3000) {
    const int64_t until = base::monotonicMs() + ms;
    while (!pred() && base::monotonicMs() < until)
        app.pump(10);
    return pred();
}

std::string rendered(const Message &m) {
    return mrkdwn::parse(m.text).text;
}

} // namespace

// ── time specs ────────────────────────────────────────────────────────────────

TEST("fixture: parseTimeSpec wall clock with day offset") {
    CHECK(spec("-2d 09:14") == base::fromLocal(2026, 9, 19, 9, 14));
    CHECK(spec("09:14") == base::fromLocal(2026, 9, 21, 9, 14));
    CHECK(spec("0d 23:59") == base::fromLocal(2026, 9, 21, 23, 59));
    CHECK(spec(" 7:05 ") == base::fromLocal(2026, 9, 21, 7, 5));
}

TEST("fixture: parseTimeSpec relative to now and to the previous message") {
    const int64_t prev = kNow - 3600;
    CHECK(spec("-45m", prev) == kNow - 45 * 60);
    CHECK(spec("-2h", prev) == kNow - 7200);
    CHECK(spec("-3d", prev) == kNow - 3 * 86400);
    CHECK(spec("+7m", prev) == prev + 7 * 60);
    CHECK(spec("+90s") == kNow + 90); // no prev → now
}

TEST("fixture: parseTimeSpec malformed specs are invalid") {
    for (const char *bad : {"yesterday", "25:99", "", "-2x", "+m", "1d09:00", "12:3", "d 09:00"})
        if (!CHECK(spec(bad) == INT64_MIN))
            std::fprintf(stderr, "    accepted: \"%s\"\n", bad);
}

// ── loadFixture ───────────────────────────────────────────────────────────────

TEST("fixture: workspace, users and conversations") {
    Store         s;
    fake::Fixture fx;
    std::string   err;
    REQUIRE(fake::loadFixture(writeFixture(kMinimal), s, &fx, &err, kNow));
    CHECK(err.empty());
    CHECK_STR(s.workspaceId, "T1");
    CHECK_STR(s.workspaceName, "Acme");
    CHECK_STR(s.user(s.me).id, "U1");
    CHECK_STR(s.conversation(fx.startConversation).id, "C2");

    REQUIRE(s.userCount() == 2);
    CHECK(s.user(0).hasTz);
    CHECK(s.user(0).tzOffset == 7200);
    CHECK(s.user(1).active);
    CHECK_STR(s.user(1).statusEmoji, "art");

    REQUIRE(s.conversationCount() == 4);
    const auto &dm = s.conversation(2);
    CHECK(dm.kind == ConvKind::Dm);
    CHECK(dm.dmUser == s.findUser("U2"));
    CHECK_STR(dm.name, "mira"); // defaulted to the peer's handle
    const auto &group = s.conversation(3);
    CHECK(group.kind == ConvKind::Group);
    CHECK(group.members.size() == 2); // me appended
    CHECK_STR(group.name, "mpdm-mira--me-1");
    CHECK_STR(s.displayName(3), "Mira");
}

TEST("fixture: history is sorted, threaded and cursor-derived") {
    Store         s;
    fake::Fixture fx;
    std::string   err;
    REQUIRE(fake::loadFixture(writeFixture(kMinimal), s, &fx, &err, kNow));
    const ConvRef c1   = s.findConversation("C1");
    const auto   &msgs = s.conversation(c1).messages;
    REQUIRE(msgs.size() == 3);
    CHECK_STR(rendered(msgs[0]), "actually the oldest"); // sorted regardless of file order
    CHECK(rendered(msgs[1]).rfind("first bold ", 0) == 0);
    const auto rich = mrkdwn::parse(msgs[1].text);
    REQUIRE(rich.entities.size() == 2); // Bold + the mention
    CHECK(rich.entities[1].kind == mrkdwn::Kind::User);
    CHECK_STR(rich.entities[1].data, "U1");
    CHECK_STR(rendered(msgs[2]), "second");
    CHECK(msgs[0].ts < msgs[1].ts);
    CHECK(msgs[1].ts < msgs[2].ts);
    CHECK_STR(msgs[1].text, "first *bold* <@U1>");
    REQUIRE(msgs[1].reactions.size() == 1);
    CHECK(msgs[1].reactions[0].count == 2);
    CHECK(s.reactedByMe(msgs[1].reactions[0]));

    // Thread wiring on the root and the replies.
    CHECK(msgs[1].replyCount == 2);
    CHECK(msgs[1].replyUsers.size() == 2);
    const auto *replies = s.replies(c1, msgs[1].ts);
    REQUIRE(replies);
    REQUIRE(replies->size() == 2);
    CHECK((*replies)[0].threadTs == msgs[1].ts);
    CHECK(
        (*replies)[1].ts - (*replies)[0].ts == 5 * 60 * 1000000LL
    ); // "+5m" after the previous reply
    CHECK(msgs[1].latestReply == (*replies)[1].ts);
    CHECK_FALSE(msgs[1].isReply());
    CHECK((*replies)[0].isReply());

    // File chip built from the local csv.
    REQUIRE(msgs[2].files().size() == 1);
    CHECK_STR(msgs[2].files()[0].name, "data.csv");
    CHECK(msgs[2].files()[0].size == 8);
    CHECK_STR(msgs[2].files()[0].mime, "text/csv");
    CHECK_STR(msgs[2].files()[0].prettyType, "CSV");
    CHECK(file::isAbsolute(msgs[2].files()[0].path));

    // A voice clip.
    const auto &voice = s.conversation(s.findConversation("C2")).messages[0].files()[0];
    CHECK(voice.isAudio());
    CHECK(voice.durationMs == 3000);
    CHECK_STR(voice.transcript, "hello there");

    // unread=1 → lastRead is the second-newest; latest the newest.
    const auto &conv = s.conversation(c1);
    CHECK(conv.latest == msgs[2].ts);
    CHECK(conv.lastRead == msgs[1].ts);
    CHECK(conv.unread == 1);
    // unread larger than the history → 0 (everything unread).
    CHECK(s.conversation(2).lastRead == 0);
    // no unread → lastRead is the newest.
    CHECK(s.conversation(1).lastRead == s.conversation(1).messages.back().ts);
}

TEST("fixture: errors are reported, not swallowed") {
    Store             s;
    fake::Fixture     fx;
    std::string       err;
    const std::string dir = writeFixture(kMinimal);
    CHECK_FALSE(fake::loadFixture(file::join(dir, "missing"), s, &fx, &err, kNow));
    CHECK(err.find("cannot open") != std::string::npos);

    auto expectError = [&](const std::string &json, std::string_view why) {
        err.clear();
        const bool ok = fake::loadFixture(writeFixture(json), s, &fx, &err, kNow);
        CHECK_FALSE(ok);
        if (!CHECK(err.find(why) != std::string::npos))
            std::fprintf(stderr, "    error was: %s\n", err.c_str());
        CHECK(s.conversationCount() == 0); // left cleared
    };
    expectError(
        replace(
            kMinimal, "\"user\": \"U2\", \"time\": \"-30m\"", "\"user\": \"U9\", \"time\": \"-30m\""
        ),
        "unknown user U9"
    );
    expectError(replace(kMinimal, "\"time\": \"-30m\"", "\"time\": \"noon\""), "bad time noon");
    expectError(replace(kMinimal, "\"me\": \"U1\"", "\"me\": \"U7\""), "not in users");
    expectError(replace(kMinimal, "\"kind\": \"dm\"", "\"kind\": \"forum\""), "unknown kind forum");
    expectError(
        replace(kMinimal, "\"time\": \"+10m\"", "\"time\": \"-2d 09:00\""), "dated before its root"
    );
    expectError(
        replace(kMinimal, "\"startConversation\": \"C2\"", "\"startConversation\": \"C9\""),
        "startConversation C9"
    );
    expectError(replace(kMinimal, "\"tz\": \"+02:00\"", "\"tz\": \"two\""), "bad tz two");
    expectError("{\"me\": \"U1\", ", "fixture.json: line 1");
}

TEST("fixture: the test fixture loads and its counts match the file") {
    Store         s;
    fake::Fixture fx;
    std::string   err;
    REQUIRE(fake::loadFixture(MSGA_TEST_ASSETS, s, &fx, &err, kNow));
    CHECK(err.empty());

    // Count straight from the JSON.
    std::string text;
    REQUIRE(file::readAll(MSGA_TEST_ASSETS "/fixture.json", &text));
    json::Document d;
    REQUIRE(d.parse(std::move(text), nullptr));
    const auto root = d.root();
    CHECK(s.userCount() == root["users"].size());
    CHECK(s.conversationCount() == root["conversations"].size());
    size_t top = 0, replies = 0, threads = 0, files = 0;
    for (json::Value m : root["messages"]) {
        ++top;
        replies += m["replies"].size();
        threads += m["replies"].size() > 0;
        files += m["files"].size();
    }
    size_t storeTop = 0, storeReplies = 0, storeThreads = 0, storeFiles = 0;
    for (ConvRef c = 0; c < s.conversationCount(); ++c) {
        const auto &conv = s.conversation(c);
        storeTop += conv.messages.size();
        storeThreads += conv.threads.size();
        for (const auto &m : conv.messages)
            storeFiles += m.files().size();
        for (const auto &t : conv.threads)
            storeReplies += t.replies.size();
    }
    CHECK(storeTop == top);
    CHECK(storeReplies == replies);
    CHECK(storeThreads == threads);
    CHECK(storeFiles == files);
    CHECK(fx.autoReplies.size() == root["autoReplies"].size());
    CHECK(fx.unfurls.size() == root["unfurls"].size());
    CHECK(fx.aiReplies.size() == root["ai"]["replies"].size());
    CHECK(fx.canvases.size() == 1);
    CHECK_STR(s.conversation(fx.startConversation).id, "C0DESIGN");
    const ConvRef eng = s.findConversation("C0ENG");
    CHECK(s.conversation(eng).unread == 3);
    CHECK(s.conversation(eng).mentions == 1);

    // Every referenced asset exists on disk, and images have dimensions.
    for (UserRef u = 0; u < s.userCount(); ++u)
        if (!s.user(u).avatar.empty())
            CHECK(file::exists(s.user(u).avatar));
    int images = 0;
    for (ConvRef c = 0; c < s.conversationCount(); ++c)
        for (const auto &m : s.conversation(c).messages) {
            for (const auto &f : m.files()) {
                CHECK(file::exists(f.path));
                if (f.mime.rfind("image/", 0) == 0) {
                    CHECK(f.isImage());
                    ++images;
                }
            }
            for (const auto &a : m.attachments())
                if (!a.image.empty())
                    CHECK(a.imageWidth > 0);
        }
    CHECK(images >= 4); // png screenshots + the gif
    // Bots post with their own identity, no user.
    const auto &rel = s.conversation(s.findConversation("C0RELEASES")).messages;
    CHECK_STR(rel.front().subtype(), "bot_message");
    CHECK_STR(rel.front().extra->botName, "Deploybot");
    CHECK(rel.front().user == kNoUser);
    CHECK(rel.front().attachments().front().fields.size() == 2);
    CHECK_FALSE(rel.front().attachments().front().linkPreview);
}

TEST("fixture: image headers give dimensions without decoding") {
    int32_t w = 0, h = 0;
    CHECK(model::imageSize(MSGA_TEST_ASSETS "/images/palette-v3.png", &w, &h));
    CHECK((w > 0 && h > 0));
    CHECK(model::imageSize(MSGA_TEST_ASSETS "/gifs/party-confetti.gif", &w, &h));
    CHECK((w > 0 && h > 0));
    CHECK_FALSE(model::imageSize(MSGA_TEST_ASSETS "/fixture.json", &w, &h));
}

// ── FakeBackend ───────────────────────────────────────────────────────────────

TEST("fake backend: connect and reads complete asynchronously") {
    auto app = headlessApp();
    REQUIRE(app);
    Store             s;
    fake::FakeBackend be(s, *app);
    be.setFixture(writeFixture(kMinimal), kNow);
    bool connected = false, done = false;
    be.connect([&](bool ok, const std::string &err) {
        connected = ok;
        done      = true;
        if (!ok)
            std::fprintf(stderr, "    connect: %s\n", err.c_str());
    });
    CHECK_FALSE(done); // never synchronous
    REQUIRE(waitFor(*app, [&] { return done; }));
    REQUIRE(connected);
    CHECK(s.conversationCount() == 4);

    const ConvRef c1       = s.findConversation("C1");
    bool          histDone = false;
    be.loadHistory(c1, 0, [&](bool ok, const std::string &) { histDone = ok; });
    CHECK_FALSE(histDone);
    REQUIRE(waitFor(*app, [&] { return histDone; }));
    CHECK_FALSE(s.conversation(c1).hasMoreBefore);

    std::vector<model::Backend::SearchHit> hits;
    bool                                   searched = false;
    be.search("REPLY", [&](std::vector<model::Backend::SearchHit> h) {
        hits     = std::move(h);
        searched = true;
    });
    REQUIRE(waitFor(*app, [&] { return searched; }));
    CHECK(hits.size() == 2);
    CHECK(hits[0].ts > hits[1].ts); // newest first
    CHECK(hits[0].thread != 0);

    CHECK(be.findTs(c1, "THE OLDEST") == s.conversation(c1).messages[0].ts);
    CHECK_STR(be.aiReply("summarise the PALETTE thread"), "palette summary");
    CHECK_STR(be.aiReply("anything"), "generic summary");
}

TEST("fake backend: a send lands pending, is confirmed, and draws the canned reply") {
    auto app = headlessApp();
    REQUIRE(app);
    Store             s;
    fake::FakeBackend be(s, *app);
    be.setFixture(writeFixture(kMinimal), kNow);
    bool connected = false;
    be.connect([&](bool ok, const std::string &) { connected = ok; });
    REQUIRE(waitFor(*app, [&] { return connected; }));

    const ConvRef       c2 = s.findConversation("C2");
    std::vector<Change> log;
    s.observe(c2, [&](const Change &ch) { log.push_back(ch); });
    bool confirmed = false;
    be.send(c2, "hello *there* https://example.test/doc/1", 0, [&](bool ok, const std::string &) {
        confirmed = ok;
    });
    // The optimistic copy is there at once…
    const auto &msgs = s.conversation(c2).messages;
    REQUIRE(msgs.size() == 2);
    CHECK(msgs.back().pending);
    CHECK(msgs.back().user == s.me);
    CHECK_FALSE(confirmed); // …the confirmation is async, like a real round trip
    REQUIRE(waitFor(*app, [&] { return confirmed; }));
    const Ts own = s.conversation(c2).messages[1].ts;
    CHECK_FALSE(s.findMessage(c2, own)->pending);

    // Then the fixture's autoReply for C2, from U2.
    REQUIRE(waitFor(*app, [&] { return s.conversation(c2).messages.size() == 3; }));
    const Message &reply = s.conversation(c2).messages.back();
    CHECK(reply.user == s.findUser("U2"));
    CHECK_STR(reply.text, "canned");
    CHECK(reply.ts > own);
    CHECK(s.conversation(c2).unread == 1); // someone else's message after my read cursor

    // The sent link grows its preview a moment later.
    REQUIRE(waitFor(*app, [&] { return !s.findMessage(c2, own)->attachments().empty(); }));
    CHECK_STR(s.findMessage(c2, own)->attachments()[0].title, "A doc");
    CHECK(s.findMessage(c2, own)->attachments()[0].linkPreview);

    // The reply is consumed: a second send gets no second canned answer.
    bool again = false;
    be.send(c2, "anyone?", 0, [&](bool ok, const std::string &) { again = ok; });
    REQUIRE(waitFor(*app, [&] { return again; }));
    for (int i = 0; i < 20; ++i)
        app->pump(10);
    CHECK(s.conversation(c2).messages.size() == 4);

    // Change kinds the list saw: Append (pending), Update (confirmed), …
    REQUIRE(log.size() >= 3);
    CHECK(log[0].kind == ChangeKind::Append);
}

TEST("fake backend: typing indicator precedes a typed reply") {
    auto app = headlessApp();
    REQUIRE(app);
    Store             s;
    fake::FakeBackend be(s, *app);
    be.setFixture(writeFixture(kMinimal), kNow);
    bool connected = false;
    be.connect([&](bool ok, const std::string &) { connected = ok; });
    REQUIRE(waitFor(*app, [&] { return connected; }));
    const ConvRef c1 = s.findConversation("C1");
    const size_t  n  = s.conversation(c1).messages.size();
    be.send(c1, "ping", 0, nullptr);
    REQUIRE(waitFor(*app, [&] { return !s.typing(c1).empty(); }));
    CHECK(s.typing(c1)[0].user == s.findUser("U2"));
    REQUIRE(waitFor(*app, [&] { return s.conversation(c1).messages.size() == n + 2; }));
    CHECK(s.typing(c1).empty()); // the message ended the indicator
    CHECK_STR(s.conversation(c1).messages.back().text, "typed answer");
}

TEST("fake backend: reactions, edits, deletes, stars and thread replies round-trip") {
    auto app = headlessApp();
    REQUIRE(app);
    Store             s;
    fake::FakeBackend be(s, *app);
    be.setFixture(writeFixture(kMinimal), kNow);
    bool connected = false;
    be.connect([&](bool ok, const std::string &) { connected = ok; });
    REQUIRE(waitFor(*app, [&] { return connected; }));
    const ConvRef c1   = s.findConversation("C1");
    const Ts      root = s.conversation(c1).messages[1].ts;

    be.react(c1, root, "rocket", true);
    CHECK(s.findMessage(c1, root)->reactions.size() == 2);
    be.react(c1, root, "tada", false);
    CHECK(s.findMessage(c1, root)->reactions[0].count == 1);
    be.edit(c1, root, "edited text");
    CHECK(s.findMessage(c1, root)->edited);
    CHECK_STR(s.findMessage(c1, root)->text, "edited text");
    be.setStarred(c1, true);
    CHECK(s.conversation(c1).starred);

    bool sent = false;
    be.send(c1, "in thread", root, [&](bool ok, const std::string &) { sent = ok; });
    REQUIRE(waitFor(*app, [&] { return sent; }));
    CHECK(s.findMessage(c1, root)->replyCount == 3);
    CHECK(s.replies(c1, root)->back().user == s.me);

    const Ts oldest = s.conversation(c1).messages[0].ts;
    be.remove(c1, oldest);
    CHECK(s.findMessage(c1, oldest) == nullptr);
    be.markRead(c1, s.conversation(c1).latest);
    CHECK(s.conversation(c1).unread == 0);

    // Another user posts: lands as unread.
    const Ts posted = be.postAs(c1, s.findUser("U2"), "news");
    CHECK(s.findMessage(c1, posted) != nullptr);
    CHECK(s.conversation(c1).unread == 1);
    CHECK(s.conversation(c1).canvasId.empty());
}

TEST("fake backend: mute, notification level, pins, saved, leave and opening DMs") {
    auto app = headlessApp();
    REQUIRE(app);
    Store             s;
    fake::FakeBackend be(s, *app);
    be.setFixture(writeFixture(kMinimal), kNow);
    bool connected = false;
    be.connect([&](bool ok, const std::string &) { connected = ok; });
    REQUIRE(waitFor(*app, [&] { return connected; }));
    const ConvRef c1 = s.findConversation("C1");
    const Ts      ts = s.conversation(c1).messages[1].ts;

    be.setMuted(c1, true);
    CHECK(s.conversation(c1).muted);
    be.setNotifyLevel(c1, NotifyLevel::Mentions);
    CHECK(s.conversation(c1).notify == NotifyLevel::Mentions);
    be.setPinned(c1, ts, true);
    be.setSaved(c1, ts, true);
    CHECK(s.findMessage(c1, ts)->pinned && s.findMessage(c1, ts)->saved);
    be.setSaved(c1, ts, false);
    CHECK_FALSE(s.findMessage(c1, ts)->saved);
    be.markRead(c1, s.conversation(c1).latest);
    be.markUnread(c1, ts); // then only "second" (mine) and it: one unread
    CHECK(s.conversation(c1).unread == 1);

    be.setStarred(c1, true);
    be.leave(c1);
    CHECK_FALSE(s.conversation(c1).member);
    CHECK_FALSE(s.conversation(c1).starred);

    // The existing DM with Mira: closed, then opened again.
    const ConvRef d1 = s.findConversation("D1");
    be.leave(d1);
    ConvRef got = kNoConv;
    be.openDm(s.findUser("U2"), [&](ConvRef c) { got = c; });
    CHECK(got == kNoConv); // never synchronous
    REQUIRE(waitFor(*app, [&] { return got != kNoConv; }));
    CHECK(got == d1 && s.conversation(d1).member);
    // A DM that does not exist yet (with myself) is created.
    got = kNoConv;
    be.openDm(s.me, [&](ConvRef c) { got = c; });
    REQUIRE(waitFor(*app, [&] { return got != kNoConv; }));
    CHECK(got != d1 && s.conversation(got).kind == ConvKind::Dm);
    CHECK(s.conversation(got).dmUser == s.me);
    CHECK_FALSE(be.isAgentSession(got));
}

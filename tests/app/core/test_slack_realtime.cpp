// SlackBackend's realtime half against the local fake Slack, WebSockets
// included (tests/support/fake_slack.py /ws/…): Socket Mode envelopes, acks and
// events into the Store, the overlapping recycle, contention, the reconnect
// backfill (resyncUnreads), the polls a push workspace skips, the OAuth
// token refresh, and the RTM presence link. Nothing here talks to the real
// Slack.
#include "app/cache/workspace_cache.h"
#include "app/slack/rtm_presence.h"
#include "app/slack/slack_backend.h"
#include "app/slack/socket_mode.h"
#include "support/fake_slack_server.h"
#include "base/json.h"
#include "base/str.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/plat.h"
#include "plat/testing.h"

#include <cstdlib>
#include <memory>
#include <string>

#ifndef _WIN32

namespace {

using fakeslack::app;
using fakeslack::ctl;
using fakeslack::pumpFor;
using fakeslack::pumpUntil;
using model::ConvRef;
using model::kNoConv;
using model::Ts;
using Link = model::Backend::PresenceLink;
using Mode = model::Backend::PresenceMode;

void set(const std::string &json) {
    ctl("POST", "/_ctl/set", json);
}
void script(const char *method, const std::string &responses) {
    ctl("POST",
        "/_ctl/script",
        std::string("{\"method\":\"") + method + "\",\"responses\":" + responses + "}");
}

struct Log {
    json::Document doc;
    Log() { doc.parse(ctl("GET", "/_ctl/log").body, nullptr); }
    int count(std::string_view method) const {
        int n = 0;
        for (json::Value r : doc.root())
            n += r["method"].str() == method;
        return n;
    }
    json::Value get(std::string_view method, int n = 0) const {
        for (json::Value r : doc.root())
            if (r["method"].str() == method && n-- == 0)
                return r;
        return {};
    }
    // A ws:sm / ws:rtm frame whose text contains `needle`.
    bool frame(std::string_view method, std::string_view needle) const {
        for (json::Value r : doc.root())
            if (r["method"].str() == method &&
                r["form"]["text"].str().find(needle) != std::string_view::npos)
                return true;
        return false;
    }
};

int count(std::string_view method) {
    return Log().count(method);
}

// A server frame on the newest open Socket Mode socket.
bool push(const std::string &frame) {
    std::string body = "{\"text\":";
    json::escapeString(body, frame);
    body += '}';
    json::Document d;
    d.parse(ctl("POST", "/_ctl/ws/send", std::move(body)).body, nullptr);
    return d.root()["ok"].boolean();
}

// An events_api envelope for workspace `team`.
std::string envelope(const std::string &id, const std::string &event, const char *team = "T1") {
    return str::concat(
        {R"({"type":"events_api","envelope_id":")",
         id,
         R"(","payload":{"team_id":")",
         team,
         R"(","event":)",
         event,
         "}}"}
    );
}

const char *kWorkspace = R"({
 "auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
               "url": "https://lumen.slack.com/"},
 "users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
     {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira Okafor"}}],
   "response_metadata": {"next_cursor": ""}},
 "users.conversations": {"ok": true, "channels": [
     {"id": "C1", "name": "general", "is_channel": true, "is_member": true},
     {"id": "D1", "is_im": true, "user": "UMIRA"}],
   "response_metadata": {"next_cursor": ""}},
 "conversations.info?channel=D1": {"ok": true, "channel": {"id": "D1", "is_im": true,
     "user": "UMIRA", "unread_count": 4, "last_read": "1700000000.000000",
     "latest": {"ts": "1700000500.000000"}}},
 "conversations.info?channel=G9": {"ok": true, "channel": {"id": "G9", "is_mpim": true,
     "name": "mpdm-me--mira-1", "is_member": true, "unread_count": 7}},
 "conversations.info?channel=CX": {"ok": false, "error": "channel_not_found"},
 "stars.list": {"ok": true, "items": []},
 "emoji.list": {"ok": true, "emoji": {}},
 "usergroups.list": {"ok": true, "usergroups": []},
 "users.getPresence": {"ok": true, "presence": "away", "online": false},
 "conversations.history": {"ok": true, "messages": [], "has_more": false}
})";

struct Env {
    model::Store                         store;
    net::Client                          client{app()};
    std::shared_ptr<slack::SocketMode>   socket;
    std::unique_ptr<slack::SlackBackend> be;
    std::string                          lostAuth;
    int                                  parallel = 0;
    std::vector<slack::Credentials>      saved;

    // session: an xoxc workspace (no socket); else OAuth, with Socket Mode
    // when `realtime`.
    // `extra`: standing answers set before the backend exists (its first
    // calls may go out while a later ctl() pumps the loop).
    explicit Env(
        bool               session  = false,
        bool               realtime = true,
        slack::Credentials cr       = {},
        const char        *extra    = nullptr
    ) {
        base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "100");
        ctl("POST", "/_ctl/reset");
        set(kWorkspace);
        if (extra)
            set(extra);
        cr.token            = cr.token.empty() ? (session ? "xoxc-test" : "xoxp-test") : cr.token;
        cr.cookie           = session ? "xoxd-test" : "";
        cr.teamId           = "T1";
        be                  = std::make_unique<slack::SlackBackend>(store, app(), client, cr);
        be->onAuthLost      = [this](const std::string &e) { lostAuth = e; };
        be->onParallelUsage = [this] { ++parallel; };
        be->onCredentialsChanged = [this](const slack::Credentials &c) { saved.push_back(c); };
        be->setAppConfig({"cid", "csecret", "xapp-test"});
        if (realtime) {
            socket = std::make_shared<slack::SocketMode>(app(), client, "xapp-test");
            slack::SocketMode::Timing t;
            t.backoffMs    = 20;
            t.backoffMaxMs = 200;
            socket->setTimingForTest(t);
            be->setRealtime(socket);
        }
    }
    ~Env() {
        be.reset();
        socket.reset();
        base::test::unsetEnv("MSGA_SLACK_TEST_SPEEDUP");
    }
    bool connect() {
        bool called = false, ok = false;
        be->connect([&](bool o, const std::string &) {
            called = true;
            ok     = o;
        });
        return pumpUntil([&] { return called && !be->connecting(); }) && ok;
    }
    bool live() {
        return pumpUntil([&] { return socket->connected() && count("ws-open") > 0; }, 5000);
    }
    ConvRef conv(const char *id) const { return store.findConversation(id); }
};

bool haveServer() {
    if (!fakeslack::server().empty())
        return true;
    std::fprintf(stderr, "  skip: no fake Slack (python3 missing)\n");
    return false;
}

} // namespace

TEST("slack realtime: socket mode opens with the app token, acks and delivers a message") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    CHECK(e.be->hasRealtimePush());
    // apps.connections.open carries the xapp token, not the workspace's.
    CHECK_STR(std::string(Log().get("apps.connections.open")["auth"].str()), "Bearer xapp-test");
    const ConvRef c = e.conv("C1");
    REQUIRE(push(envelope(
        "env-1",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"hi <@UME>","ts":"1800000000.000100"})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, model::parseTs("1800000000.000100")); }));
    CHECK(e.store.conversation(c).unread == 1 && e.store.conversation(c).mentions == 1);
    // Acked by envelope id.
    REQUIRE(pumpUntil([&] { return Log().frame("ws:sm", R"("envelope_id":"env-1")"); }));
    // The same envelope again (a redelivery) changes nothing.
    REQUIRE(push(envelope(
        "env-1",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"hi <@UME>","ts":"1800000000.000100"})"
    )));
    pumpFor(150);
    CHECK(e.store.conversation(c).unread == 1);
}

TEST("slack realtime: a message read as it lands (the open, focused chat) leaves no badge") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef c  = e.conv("C1");
    // The shell's observer: a new message on screen is read at once.
    const auto    id = e.store.observe(c, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Append && !ch.thread)
            e.be->markRead(c, e.store.conversation(c).messages.back().ts);
    });
    REQUIRE(push(envelope(
        "env-r1",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"hi <@UME>","ts":"1800000000.000100"})"
    )));
    const Ts ts = model::parseTs("1800000000.000100");
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts); }));
    e.store.unobserve(id);
    CHECK(e.store.conversation(c).lastRead == ts);
    CHECK(e.store.conversation(c).unread == 0);
    CHECK(e.store.conversation(c).mentions == 0);
    CHECK(e.store.conversation(c).latest == ts);
    // Not read (the window in the background): it counts.
    REQUIRE(push(envelope(
        "env-r2",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"again","ts":"1800000000.000200"})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, model::parseTs("1800000000.000200")); }));
    CHECK(e.store.conversation(c).unread == 1);
}

TEST("slack realtime: a reply to a thread I started is followed, its root unloaded") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef c    = e.conv("C1");
    const Ts      root = model::parseTs("1800000000.000050");
    REQUIRE(push(envelope(
        "env-p1",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"done","ts":"1800000000.000300",)"
        R"("thread_ts":"1800000000.000050","parent_user_id":"UME"})"
    )));
    REQUIRE(pumpUntil([&] { return e.be->threadFollowed(c, root); }));
    // Someone else's thread: not followed.
    REQUIRE(push(envelope(
        "env-p2",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"ok","ts":"1800000000.000400",)"
        R"("thread_ts":"1800000000.000060","parent_user_id":"UMIRA"})"
    )));
    pumpFor(150);
    CHECK(!e.be->threadFollowed(c, model::parseTs("1800000000.000060")));
}

TEST("slack realtime: edits, deletions, reactions, presence, users and marks") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef c  = e.conv("C1");
    const Ts      ts = model::parseTs("1800000000.000200");
    REQUIRE(push(envelope(
        "a",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"v1","ts":"1800000000.000200"})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts); }));
    REQUIRE(push(envelope(
        "b",
        R"({"type":"message","subtype":"message_changed","channel":"C1","message":{"type":"message","user":"UMIRA","text":"v2","ts":"1800000000.000200","edited":{"ts":"1"}}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts)->text == "v2"; }));
    CHECK(e.store.findMessage(c, ts)->edited);
    REQUIRE(push(envelope(
        "c",
        R"({"type":"reaction_added","user":"UMIRA","reaction":"tada","item":{"type":"message","channel":"C1","ts":"1800000000.000200"}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts)->reactions.size() == 1; }));
    CHECK(e.store.findMessage(c, ts)->reactions[0].count == 1);
    REQUIRE(push(envelope(
        "d",
        R"({"type":"reaction_removed","user":"UMIRA","reaction":"tada","item":{"type":"message","channel":"C1","ts":"1800000000.000200"}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, ts)->reactions.empty(); }));
    REQUIRE(push(envelope(
        "e",
        R"({"type":"message","subtype":"message_deleted","channel":"C1","deleted_ts":"1800000000.000200","previous_message":{"ts":"1800000000.000200"}})"
    )));
    REQUIRE(pumpUntil([&] { return !e.store.findMessage(c, ts); }));
    // Presence, DND, a profile change (presence kept), a read mark.
    const auto mira = e.store.findUser("UMIRA");
    REQUIRE(
        push(envelope("f", R"({"type":"presence_change","user":"UMIRA","presence":"active"})"))
    );
    REQUIRE(pumpUntil([&] { return e.store.user(mira).active; }));
    REQUIRE(push(envelope(
        "g", R"({"type":"dnd_updated_user","user":"UMIRA","dnd_status":{"dnd_enabled":true}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.user(mira).dnd; }));
    REQUIRE(push(envelope(
        "h",
        R"({"type":"user_change","user":{"id":"UMIRA","name":"mira","profile":{"real_name":"Mira O."}}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.user(mira).displayName == "Mira O."; }));
    CHECK(e.store.user(mira).active && e.store.user(mira).dnd);
    REQUIRE(push(envelope(
        "i",
        R"({"type":"channel_marked","channel":"C1","ts":"1800000001.000000","unread_count_display":0,"mention_count_display":0})"
    )));
    REQUIRE(pumpUntil([&] {
        return e.store.conversation(c).lastRead == model::parseTs("1800000001.000000");
    }));
    // A user group change reloads the groups (debounced).
    const int groups = count("usergroups.list");
    REQUIRE(push(envelope("j", R"({"type":"subteam_members_changed","subteam_id":"S1"})")));
    REQUIRE(pumpUntil([&] { return count("usergroups.list") > groups; }, 3000));
}

TEST("slack realtime: new channels and group DMs join the list; foreign events don't") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    // A group DM nobody announced: its first message fetches it, then counts.
    REQUIRE(push(envelope(
        "a",
        R"({"type":"message","channel":"G9","user":"UMIRA","text":"yo","ts":"1800000000.000300"})"
    )));
    REQUIRE(pumpUntil([&] { return e.conv("G9") != kNoConv; }));
    REQUIRE(pumpUntil([&] {
        return e.store.findMessage(e.conv("G9"), model::parseTs("1800000000.000300"));
    }));
    // conversations.info's unread_count (7) is not taken: the replay counts.
    CHECK(e.store.conversation(e.conv("G9")).unread == 1);
    {
        // The count is opt-in on conversations.info; the header shows it.
        bool      withCount = false;
        const Log log;
        for (json::Value r : log.doc.root())
            withCount |= r["method"].str() == "conversations.info" &&
                         r["form"]["channel"].str() == "G9" &&
                         r["form"]["include_num_members"].str() == "true";
        CHECK(withCount);
    }
    // channel_created joins it.
    REQUIRE(push(envelope(
        "b", R"({"type":"channel_created","channel":{"id":"C7","name":"launch","is_channel":true}})"
    )));
    REQUIRE(pumpUntil([&] { return e.conv("C7") != kNoConv; }));
    // Another workspace's events (the socket is the app's): nothing fetched,
    // nothing added.
    const int infos = count("conversations.info");
    REQUIRE(push(envelope(
        "c",
        R"({"type":"message","channel":"CZ","user":"UX","text":"x","ts":"1800000000.000400"})",
        "T2"
    )));
    REQUIRE(push(
        envelope("d", R"({"type":"channel_created","channel":{"id":"C8","name":"other"}})", "T2")
    ));
    pumpFor(200);
    CHECK(e.conv("CZ") == kNoConv && e.conv("C8") == kNoConv);
    CHECK(count("conversations.info") == infos);
    // Ours but dead (channel_not_found): asked once, then remembered.
    REQUIRE(push(envelope(
        "e", R"({"type":"message","channel":"CX","user":"UX","text":"1","ts":"1800000000.000500"})"
    )));
    REQUIRE(pumpUntil([&] { return count("conversations.info") == infos + 1; }));
    pumpFor(100);
    REQUIRE(push(envelope(
        "f", R"({"type":"message","channel":"CX","user":"UX","text":"2","ts":"1800000000.000600"})"
    )));
    pumpFor(200);
    CHECK(count("conversations.info") == infos + 1);
}

TEST("slack realtime: a disconnect warning brings up an overlapping replacement") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    REQUIRE(push(R"({"type":"disconnect","reason":"warning"})"));
    REQUIRE(pumpUntil([&] { return count("ws-open") == 2 && e.socket->socketsForTest() == 1; }));
    CHECK(count("apps.connections.open") == 2);
    // The new socket delivers; the old one is gone.
    REQUIRE(push(envelope(
        "z",
        R"({"type":"message","channel":"C1","user":"UMIRA","text":"after","ts":"1800000000.000700"})"
    )));
    REQUIRE(pumpUntil([&] {
        return e.store.findMessage(e.conv("C1"), model::parseTs("1800000000.000700"));
    }));
    CHECK(pumpUntil([&] {
        json::Document d;
        d.parse(ctl("GET", "/_ctl/ws").body, nullptr);
        int n = 0;
        for (json::Value s : d.root())
            n += s["open"].boolean();
        return n == 1;
    }));
    // refresh_requested: the socket is closed on purpose and replaced.
    REQUIRE(push(R"({"type":"disconnect","reason":"refresh_requested"})"));
    REQUIRE(pumpUntil([&] { return count("ws-open") == 3 && e.socket->connected(); }));
    CHECK(e.parallel == 0);
}

TEST("slack realtime: the watchdog keeps the socket alive with pings, never closing on silence") {
    if (!haveServer())
        return;
    Env                       e;
    slack::SocketMode::Timing t;
    t.watchdogMs = 40; // staleMs stays 50 s: no suspend gap in a test
    t.backoffMs  = 20;
    e.socket->setTimingForTest(t);
    REQUIRE(e.connect());
    REQUIRE(e.live());
    // Slack never pongs them (the fake does; it changes nothing): pings go
    // out tick after tick on the one socket, which stays.
    REQUIRE(pumpUntil([&] { return count("ws-ping") >= 3; }, 5000));
    CHECK(e.socket->connected());
    CHECK(count("ws-open") == 1);
    CHECK(count("apps.connections.open") == 1);
}

TEST("slack realtime: the network coming back reconnects a dropped socket at once") {
    if (!haveServer())
        return;
    // The socket alone: a backend's safety poll would reconnect it too.
    Env                       e(false, false);
    slack::SocketMode         sock(app(), e.client, "xapp-test");
    slack::SocketMode::Timing t;
    t.backoffMs = t.backoffMaxMs = 60'000; // a retry the test would never see
    sock.setTimingForTest(t);
    sock.start();
    REQUIRE(pumpUntil([&] { return sock.connected(); }, 5000));
    // The OS reports the network the way main.cpp hands it on (headless
    // plat's fake reachability source).
    plat::App &pa = app();
    pa.setEventHandler([&](const plat::Event &ev) {
        if (ev.type == plat::EventType::NetworkChanged)
            sock.networkChanged(ev.online);
    });
    plat::TestHooks *hooks = pa.testHooks();
    REQUIRE(hooks);
    // Online while connected: a ping, nothing else.
    REQUIRE(hooks->simulateSystemEvent(plat::EventType::NetworkChanged, true));
    REQUIRE(pumpUntil([&] { return count("ws-ping") >= 1; }));
    CHECK(count("ws-open") == 1);
    // The network drops the socket; the backoff would wait a minute.
    ctl("POST", "/_ctl/ws/close", R"({"code": 1001})");
    REQUIRE(pumpUntil([&] { return !sock.connected(); }));
    REQUIRE(hooks->simulateSystemEvent(plat::EventType::NetworkChanged, false));
    pumpFor(150);
    CHECK(count("ws-open") == 1); // offline: nothing to do
    REQUIRE(hooks->simulateSystemEvent(plat::EventType::NetworkChanged, true));
    REQUIRE(pumpUntil([&] { return count("ws-open") == 2 && sock.connected(); }, 3000));
    CHECK(count("apps.connections.open") == 2);
    pa.setEventHandler({});
    sock.stop();
}

TEST("slack realtime: contention raises the parallel-usage notice once") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    CHECK(e.parallel == 0); // alone in the pool
    // Another device joins the app's pool: the next hello counts it.
    ctl("POST", "/_ctl/ws/opt", R"({"ws_extra": 1})");
    REQUIRE(push(R"({"type":"disconnect","reason":"refresh_requested"})"));
    REQUIRE(pumpUntil([&] { return e.parallel == 1; }));
    // Throttled: another contended hello within the window says nothing.
    REQUIRE(pumpUntil([&] { return count("ws-open") == 2 && e.socket->connected(); }));
    REQUIRE(push(R"({"type":"disconnect","reason":"refresh_requested"})"));
    REQUIRE(pumpUntil([&] { return count("ws-open") == 3 && e.socket->connected(); }));
    pumpFor(100);
    CHECK(e.parallel == 1);
}

TEST("slack realtime: a reconnect backfills the roster and the DM badges") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const int lists = count("users.conversations");
    CHECK(e.store.conversation(e.conv("D1")).unread == 0);
    // Slack drops the socket: a later hello is a reconnect after a gap.
    ctl("POST", "/_ctl/ws/close", R"({"code": 1001})");
    REQUIRE(pumpUntil([&] { return count("ws-open") == 2 && e.socket->connected(); }));
    REQUIRE(pumpUntil([&] { return count("users.conversations") > lists; }));
    // resyncUnreads: conversations.info on the DM, merged upward.
    REQUIRE(pumpUntil([&] { return e.store.conversation(e.conv("D1")).unread == 4; }, 5000));
    CHECK(e.store.conversation(e.conv("D1")).mentions == 4);
    CHECK(e.store.conversation(e.conv("D1")).lastRead == model::parseTs("1700000000.000000"));
}

TEST("slack realtime: a reconnect soon after a full DM sweep asks only the most active") {
    if (!haveServer())
        return;
    // 30 DMs: more than a short sweep takes.
    std::string list = R"({"users.conversations": {"ok": true, "channels": [)";
    for (int i = 0; i < 30; ++i)
        list += str::concat(
            {i ? "," : "",
             R"({"id": "D)",
             str::number(100 + i),
             R"(", "is_im": true, "user": "U)",
             str::number(100 + i),
             "\"}"}
        );
    list += R"(], "response_metadata": {"next_cursor": ""}}})";
    Env e(false, true, {}, list.c_str());
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const auto reconnect = [&](int opens) {
        ctl("POST", "/_ctl/ws/close", R"({"code": 1001})");
        REQUIRE(pumpUntil([&] { return count("ws-open") == opens && e.socket->connected(); }));
    };
    // A sweep's calls past `from`: waited for (the paced lane stalls under
    // load), then a quiet spell for any beyond them.
    const auto swept = [](int from, int expect) {
        pumpUntil([&] { return count("conversations.info") - from >= expect; }, 5000);
        pumpFor(300);
        return count("conversations.info") - from;
    };
    // The DM activity sweep after connect first.
    int infos = swept(0, 30);
    REQUIRE(infos == 30);
    reconnect(2);
    CHECK(swept(infos, 30) == 30); // the first: every DM
    // Past the 2 min gap (1.2 s here), inside the full sweep's 10 min (6 s).
    pumpFor(1000);
    infos = count("conversations.info");
    reconnect(3);
    CHECK(swept(infos, 24) == 24);
}

TEST("slack realtime: user group events patch the groups; only the unclear reload them") {
    if (!haveServer())
        return;
    Env e(false, true, {}, R"({"usergroups.list": {"ok": true, "usergroups": [
        {"id": "S1", "handle": "eng", "name": "Eng", "users": ["UMIRA"]}]}})");
    REQUIRE(e.connect());
    REQUIRE(e.live());
    REQUIRE(pumpUntil([&] { return e.store.findUsergroup("S1") != nullptr; }, 3000));
    CHECK(e.store.myGroups.empty());
    const int lists = count("usergroups.list");
    REQUIRE(push(envelope("a", R"({"type":"subteam_self_added","subteam_id":"S1"})")));
    REQUIRE(pumpUntil([&] { return e.store.myGroups.size() == 1; }));
    REQUIRE(push(envelope(
        "b",
        R"({"type":"subteam_updated","subteam":{"id":"S1","handle":"core","name":"Core","date_delete":0}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.findUsergroup("S1")->handle == "core"; }));
    CHECK(e.store.findUsergroup("S1")->mine); // no member list in it: kept
    REQUIRE(push(envelope(
        "c",
        R"({"type":"subteam_created","subteam":{"id":"S9","handle":"ops","name":"Ops","date_delete":0,"users":["UME"]}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.myGroups.size() == 2; }));
    REQUIRE(push(envelope(
        "d",
        R"({"type":"subteam_members_changed","subteam_id":"S9","added_users":[],"removed_users":["UME"]})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.myGroups.size() == 1; }));
    REQUIRE(push(envelope("e", R"({"type":"subteam_self_removed","subteam_id":"S1"})")));
    REQUIRE(pumpUntil([&] { return e.store.myGroups.empty(); }));
    pumpFor(200); // past the reload's debounce (20 ms here)
    CHECK(count("usergroups.list") == lists);
    // A group we don't hold: read them all again.
    REQUIRE(push(envelope("f", R"({"type":"subteam_self_added","subteam_id":"S5"})")));
    REQUIRE(pumpUntil([&] { return count("usergroups.list") > lists; }, 3000));
}

TEST("slack realtime: an away author posting is probed once, not per message") {
    if (!haveServer())
        return;
    Env e;
    e.be->setWindowVisible(false); // no presence rounds: only the probes count
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const auto probes = [] {
        int       n = 0;
        const Log l;
        for (json::Value r : l.doc.root())
            n += r["method"].str() == "users.getPresence" && r["form"]["user"].str() == "UMIRA";
        return n;
    };
    pumpFor(200);
    const int before = probes();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(push(envelope(
            str::concat({"m", str::number(i)}),
            str::concat(
                {R"({"type":"message","channel":"C1","user":"UMIRA","text":"hi","ts":"18000000)",
                 str::number(10 + i),
                 R"(.000100"})"}
            )
        )));
        pumpFor(100);
    }
    REQUIRE(pumpUntil([&] {
        return e.store.findMessage(e.conv("C1"), model::parseTs("1800000012.000100")) != nullptr;
    }));
    CHECK(probes() - before == 1);
    // Another workspace's message is not mapped: its author stays unknown here.
    REQUIRE(push(envelope(
        "x",
        R"({"type":"message","channel":"CZ","user":"UFOREIGN","text":"x","ts":"1800000020.000100"})",
        "T2"
    )));
    pumpFor(200);
    CHECK(e.store.findUser("UFOREIGN") == model::kNoUser);
}

TEST("slack realtime: a push workspace polls only as a safety net") {
    if (!haveServer())
        return;
    {
        Env e;
        REQUIRE(e.connect());
        REQUIRE(e.live());
        const int lists = count("users.conversations");
        pumpFor(1500); // 2.5 roster gaps at 100× speed
        CHECK(count("users.conversations") == lists);
    }
    {
        Env e(false, false);
        REQUIRE(e.connect());
        CHECK_FALSE(e.be->hasRealtimePush());
        const int lists = count("users.conversations");
        REQUIRE(pumpUntil([&] { return count("users.conversations") > lists; }, 3000));
    }
    {
        // A session workspace never takes the app's socket.
        Env e(true, true);
        CHECK_FALSE(e.be->hasRealtimePush());
        REQUIRE(e.connect());
        pumpFor(100);
        CHECK(count("apps.connections.open") == 0);
    }
}

TEST("slack realtime: a missed message re-establishes the socket") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    e.be->setActiveConversation(e.conv("C1"), 0);
    // The open chat's poll finds what the socket never pushed.
    set(R"({"conversations.history": {"ok": true, "messages": [
        {"type": "message", "user": "UMIRA", "text": "first", "ts": "1800000000.000800"}],
        "has_more": false}})");
    REQUIRE(pumpUntil(
        [&] { return e.store.findMessage(e.conv("C1"), model::parseTs("1800000000.000800")); }, 5000
    ));
    set(R"({"conversations.history": {"ok": true, "messages": [
        {"type": "message", "user": "UMIRA", "text": "missed", "ts": "1800000000.000900"},
        {"type": "message", "user": "UMIRA", "text": "first", "ts": "1800000000.000800"}],
        "has_more": false}})");
    REQUIRE(pumpUntil(
        [&] { return e.store.findMessage(e.conv("C1"), model::parseTs("1800000000.000900")); }, 5000
    ));
    REQUIRE(pumpUntil([&] { return count("apps.connections.open") == 2; }));
    REQUIRE(pumpUntil([&] { return e.socket->connected() && e.socket->socketsForTest() == 1; }));
}

TEST("slack realtime: a rejected token is refreshed once and the call re-issued") {
    if (!haveServer())
        return;
    slack::Credentials cr;
    cr.refreshToken = "xoxe-1";
    Env e(false, false, cr);
    script("auth.test", R"([{"ok": false, "error": "invalid_auth"}])");
    set(R"({"oauth.v2.access": {"ok": true, "access_token": "xoxp-new", "refresh_token": "xoxe-2",
            "expires_in": 43200}})");
    REQUIRE(e.connect());
    CHECK(e.lostAuth.empty());
    Log log;
    REQUIRE(log.count("oauth.v2.access") == 1);
    const json::Value form = log.get("oauth.v2.access")["form"];
    CHECK_STR(std::string(form["grant_type"].str()), "refresh_token");
    CHECK_STR(std::string(form["client_id"].str()), "cid");
    CHECK_STR(std::string(form["client_secret"].str()), "csecret");
    CHECK_STR(std::string(form["refresh_token"].str()), "xoxe-1");
    // auth.test went again, with the new token; so does everything after.
    CHECK(log.count("auth.test") == 2);
    CHECK_STR(std::string(log.get("auth.test", 1)["auth"].str()), "Bearer xoxp-new");
    CHECK_STR(std::string(log.get("users.list")["auth"].str()), "Bearer xoxp-new");
    // Persisted through the owner.
    REQUIRE(e.saved.size() == 1);
    CHECK_STR(e.saved[0].token, "xoxp-new");
    CHECK_STR(e.saved[0].refreshToken, "xoxe-2");
    CHECK(e.saved[0].expiresAt > base::nowSecs() + 43000);
}

TEST(
    "slack realtime: calls wait while the token refreshes; a late rejection doesn't refresh again"
) {
    if (!haveServer())
        return;
    slack::Credentials cr;
    cr.refreshToken = "xoxe-1";
    Env e(false, false, cr);
    REQUIRE(e.connect());
    const ConvRef c1 = e.conv("C1"), d1 = e.conv("D1");
    REQUIRE(c1 != kNoConv && d1 != kNoConv);
    // The token is refused; the refresh takes a while to answer. The second
    // call, out on the old token too, hears so only after the refresh
    // started: it must not spend the next refresh token.
    set(R"({"oauth.v2.access": {"ok": true, "access_token": "xoxp-new", "refresh_token": "xoxe-2",
            "expires_in": 43200, "__delay": 0.4}})");
    script("stars.add", R"([{"ok": false, "error": "invalid_auth"},
                            {"ok": false, "error": "invalid_auth", "__delay": 0.15},
                            {"ok": true}])");
    e.be->setStarred(c1, true);
    e.be->setStarred(d1, true);
    REQUIRE(pumpUntil([&] { return Log().count("oauth.v2.access") == 1; }));
    // Issued during the refresh: held, then sent on the new token.
    e.be->setStarred(c1, false);
    REQUIRE(pumpUntil([&] { return Log().count("stars.add") == 4; }, 5000));
    REQUIRE(pumpUntil([&] { return Log().count("stars.remove") == 1; }, 5000));
    pumpFor(200);
    Log log;
    CHECK(log.count("oauth.v2.access") == 1);
    for (int i = 2; i < 4; ++i)
        CHECK_STR(std::string(log.get("stars.add", i)["auth"].str()), "Bearer xoxp-new");
    CHECK_STR(std::string(log.get("stars.remove")["auth"].str()), "Bearer xoxp-new");
    CHECK(e.lostAuth.empty());
}

TEST("slack realtime: a dead refresh token signs out; a transient failure doesn't") {
    if (!haveServer())
        return;
    slack::Credentials cr;
    cr.refreshToken = "xoxe-1";
    {
        Env e(false, false, cr);
        script("auth.test", R"([{"ok": false, "error": "token_expired"}])");
        set(R"({"oauth.v2.access": {"ok": false, "error": "invalid_refresh_token"}})");
        bool        called = false;
        std::string err;
        e.be->connect([&](bool, const std::string &x) {
            called = true;
            err    = x;
        });
        REQUIRE(pumpUntil([&] { return called && !e.lostAuth.empty(); }));
        CHECK_STR(e.lostAuth, "invalid_refresh_token");
        CHECK_STR(err, "token_expired"); // what the call itself got: a dead sign-in
    }
    {
        Env e(false, false, cr);
        script("auth.test", R"([{"ok": false, "error": "invalid_auth"}])");
        set(R"({"oauth.v2.access": {"ok": false, "error": "internal_error"}})");
        std::string err;
        bool        called = false;
        e.be->connect([&](bool, const std::string &x) {
            called = true;
            err    = x;
        });
        REQUIRE(pumpUntil([&] { return called; }));
        CHECK_STR(err, "token_refresh_failed");
        pumpFor(100);
        CHECK(e.lostAuth.empty());
    }
    {
        // Still rejected after a "successful" refresh: no second refresh.
        Env e(false, false, cr);
        script("auth.test", R"([{"ok": false, "error": "invalid_auth"},
                                {"ok": false, "error": "invalid_auth"}])");
        set(R"({"oauth.v2.access": {"ok": true, "access_token": "xoxp-new"}})");
        bool called = false;
        e.be->connect([&](bool, const std::string &) { called = true; });
        REQUIRE(pumpUntil([&] { return called && !e.lostAuth.empty(); }));
        CHECK(count("oauth.v2.access") == 1);
        CHECK_STR(e.lostAuth, "invalid_auth");
    }
}

TEST("slack realtime: a token close to expiry is refreshed ahead of time") {
    if (!haveServer())
        return;
    slack::Credentials cr;
    cr.refreshToken = "xoxe-1";
    cr.expiresAt    = base::nowSecs() + 600; // inside the 1 h window
    Env e(
        false,
        false,
        cr,
        R"({"oauth.v2.access": {"ok": true, "access_token": "xoxp-new", "expires_in": 43200}})"
    );
    REQUIRE(pumpUntil([&] { return e.saved.size() == 1; }));
    CHECK_STR(e.be->auth().token, "xoxp-new");
    // Healthy now: the periodic check leaves it alone.
    pumpFor(1500);
    CHECK(count("oauth.v2.access") == 1);
}

TEST("slack realtime: the presence link holds an RTM socket with the d cookie") {
    if (!haveServer())
        return;
    Env                 e(true, false);
    slack::RtmPresence *rtm = e.be->presenceLinkForTest();
    REQUIRE(rtm != nullptr);
    CHECK(e.be->presenceLink() == Link::Off);
    e.be->setPresenceMode(Mode::WhileRunning);
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Active; }));
    Log log;
    CHECK_STR(std::string(log.get("ws-open")["cookie"].str()), "d=xoxd-test");
    CHECK_STR(std::string(log.get("rtm.connect")["auth"].str()), "Bearer xoxc-test");
    // Active at once: a tickle right after hello.
    REQUIRE(pumpUntil([&] { return Log().frame("ws:rtm", "tickle"); }));
    // Native drops it.
    e.be->setPresenceMode(Mode::Native);
    CHECK(e.be->presenceLink() == Link::Off);
    CHECK_FALSE(rtm->connected());
    // An OAuth workspace has none.
    Env o(false, false);
    CHECK(o.be->presenceLinkForTest() == nullptr);
}

TEST("slack realtime: the presence link reconnects without pongs and idles while unused") {
    if (!haveServer())
        return;
    Env                        e(true, false);
    slack::RtmPresence        *rtm = e.be->presenceLinkForTest();
    slack::RtmPresence::Timing t;
    t.pingMs         = 30;
    t.reconnectMinMs = 20;
    t.reconnectMaxMs = 100;
    t.idleMs         = 400;
    rtm->setTimingForTest(t);
    ctl("POST", "/_ctl/ws/opt", R"({"rtm_nopong": true})");
    e.be->setPresenceMode(Mode::WhileRunning);
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Active; }));
    // Two unanswered pings: a fresh rtm.connect.
    REQUIRE(pumpUntil([&] { return count("rtm.connect") >= 2; }, 5000));
    ctl("POST", "/_ctl/ws/opt", R"({"rtm_nopong": false})");
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Active && rtm->connected(); }));
    // WhileUsing: no input for idleMs drops it; input brings it back.
    e.be->setPresenceMode(Mode::WhileUsing);
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Idle; }, 5000));
    CHECK_FALSE(rtm->connected());
    e.be->noteUserActivity();
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Active; }));
}

TEST("slack realtime: rtm.connect refusing the token makes the link unavailable") {
    if (!haveServer())
        return;
    Env e(true, false);
    set(R"({"rtm.connect": {"ok": false, "error": "not_allowed_token_type"}})");
    e.be->setPresenceMode(Mode::WhileRunning);
    REQUIRE(pumpUntil([&] { return e.be->presenceLink() == Link::Unavailable; }));
    pumpFor(200);
    CHECK(count("rtm.connect") == 1); // given up, not retried into a rate limit
}

TEST("slack realtime: a huddle_thread starts a live huddle, its edit ends it") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(e.live());
    const ConvRef c = e.conv("C1");
    REQUIRE(push(envelope(
        "h-1",
        R"({"type":"message","subtype":"huddle_thread","channel":"C1","ts":"1800000000.000700",
            "text":"","room":{"call_family":"huddle","huddle_link":"https://app.slack.com/huddle/T1/C1",
            "participants":["UMIRA"],"date_start":1800000000,"date_end":0}})"
    )));
    REQUIRE(pumpUntil([&] { return e.store.conversation(c).huddleActive; }));
    CHECK_STR(e.store.conversation(c).huddleLink, "https://app.slack.com/huddle/T1/C1");
    // The message itself arrives too: a huddle row (no Slackbot author).
    const model::Message *m = e.store.findMessage(c, model::parseTs("1800000000.000700"));
    REQUIRE(m && m->isHuddle());
    CHECK(!m->extra->huddle.ended);
    // Slack edits it when the huddle ends (date_end as a string).
    REQUIRE(push(envelope(
        "h-2",
        R"({"type":"message","subtype":"message_changed","channel":"C1",
            "message":{"type":"message","subtype":"huddle_thread","ts":"1800000000.000700","text":"",
            "room":{"call_family":"huddle","participants":[],"participant_history":["UMIRA","UME"],
                    "date_start":1800000000,"date_end":"1800000600"}}})"
    )));
    REQUIRE(pumpUntil([&] { return !e.store.conversation(c).huddleActive; }));
    m = e.store.findMessage(c, model::parseTs("1800000000.000700"));
    REQUIRE(m && m->isHuddle());
    CHECK(m->extra->huddle.ended);
    CHECK(m->extra->huddle.attendees.size() == 2);
}

TEST("slack realtime: a conversation found dead is never looked up again, across restarts") {
    if (!haveServer())
        return;
    cache::WorkspaceCache::remove(app(), "slack:T1");
    {
        Env e;
        e.be->openCache(cache::WorkspaceCache::dirFor(app(), "slack:T1"));
        REQUIRE(e.connect());
        REQUIRE(e.live());
        REQUIRE(push(envelope(
            "d-1",
            R"({"type":"message","channel":"CX","user":"UX","text":"1","ts":"1800000000.000800"})"
        )));
        REQUIRE(pumpUntil([&] { return e.be->isDead("CX"); }));
    }
    Env e;
    e.be->openCache(cache::WorkspaceCache::dirFor(app(), "slack:T1"));
    REQUIRE(e.connect());
    REQUIRE(e.live());
    REQUIRE(push(envelope(
        "d-2",
        R"({"type":"message","channel":"CX","user":"UX","text":"2","ts":"1800000000.000900"})"
    )));
    fakeslack::pumpFor(200);
    // (the DM sweep may ask about D1; nothing asks about CX)
    int       asked = 0;
    const Log log; // outlives the loop (a temporary in the range would not)
    for (json::Value r : log.doc.root())
        asked += r["method"].str() == "conversations.info" && r["form"]["channel"].str() == "CX";
    CHECK(asked == 0);
    e.be.reset();
    cache::WorkspaceCache::remove(app(), "slack:T1");
}

#endif

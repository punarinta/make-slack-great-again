// SlackBackend's agent-thread-link calls against the local fake Slack
// (tests/support/fake_slack.py): posting into a thread as the user with the
// msga marker (and without it where the token refuses metadata), editing and
// deleting by ts, reading a thread after a ts, watching threads (the
// cadence, the staggered first polls, the feed and client.counts pulling a
// poll forward), resolving a message's thread root and the user's own name.
// Nothing here talks to the real Slack. Delays are compressed
// (MSGA_SLACK_TEST_SPEEDUP): a 15 s poll interval is 150 ms here.
#include "app/slack/slack_backend.h"
#include "support/fake_slack_server.h"
#include "base/file.h"
#include "base/process.h"
#include "base/json.h"
#include "base/str.h"
#include "base/time.h"
#include "support/test.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32

namespace {

using fakeslack::app;
using fakeslack::ctl;
using fakeslack::pumpFor;
using fakeslack::pumpUntil;
using model::ConvRef;
using model::Ts;
using Reply = model::Backend::ThreadReply;

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
};

int calls(std::string_view method) {
    return Log().count(method);
}

const char *kWorkspace = R"({
 "auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
               "url": "https://lumen.slack.com/"},
 "users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself", "display_name": "memyself"}},
     {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira Okafor"}}],
   "response_metadata": {"next_cursor": ""}},
 "users.conversations": {"ok": true, "channels": [
     {"id": "C1", "name": "general", "is_channel": true, "is_member": true},
     {"id": "C2", "name": "random", "is_channel": true, "is_member": true},
     {"id": "C3", "name": "dev", "is_channel": true, "is_member": true}],
   "response_metadata": {"next_cursor": ""}},
 "stars.list": {"ok": true, "items": []},
 "emoji.list": {"ok": true, "emoji": {}},
 "usergroups.list": {"ok": true, "usergroups": []},
 "users.getPresence": {"ok": true, "presence": "active", "online": true},
 "client.counts": {"ok": true, "channels": [], "ims": [], "mpims": []},
 "saved.list": {"ok": true, "saved_items": []},
 "subscriptions.thread.getView": {"ok": true, "threads": [], "total_unread_replies": 0},
 "conversations.history": {"ok": true, "messages": [], "has_more": false}
})";

// A thread of C1 as conversations.replies answers it: the root, then each
// "ts:user:text" reply; a reply whose user is "AGENT" is mine with the marker.
std::string replies(const std::string &root, std::vector<std::string> rows) {
    std::string out = str::concat(
        {R"({"ok": true, "messages": [{"ts": ")",
         root,
         R"(", "thread_ts": ")",
         root,
         R"(", "user": "UMIRA", "text": "the question", "reply_count": 3})"}
    );
    for (const std::string &row : rows) {
        const size_t a = row.find(':'), b = row.find(':', a + 1);
        std::string  user(row.substr(a + 1, b - a - 1));
        std::string  meta;
        if (user == "AGENT") {
            user = "UME";
            meta = R"(, "metadata": {"event_type": "msga_agent_reply", "event_payload": {}})";
        }
        out += str::concat(
            {R"(, {"ts": ")",
             row.substr(0, a),
             R"(", "thread_ts": ")",
             root,
             R"(", "user": ")",
             user,
             R"(", "text": ")",
             row.substr(b + 1),
             "\"",
             meta,
             "}"}
        );
    }
    return out + "]}";
}

struct Env {
    model::Store                         store;
    net::Client                          client{app()};
    std::unique_ptr<slack::SlackBackend> be;
    std::vector<std::string>             errors;
    struct Report {
        ConvRef            conv = model::kNoConv;
        Ts                 root = 0;
        std::vector<Reply> replies;
        std::string        error;
    };
    std::vector<Report> reports;

    // speedup: how much faster than real time every delay runs.
    explicit Env(const std::string &extra = {}, bool session = true, const char *speedup = "100") {
        base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", speedup);
        ctl("POST", "/_ctl/reset");
        set(kWorkspace);
        if (!extra.empty())
            set(extra);
        slack::Credentials cr;
        cr.token            = session ? "xoxc-test" : "xoxp-test";
        cr.cookie           = session ? "xoxd-test" : "";
        cr.teamId           = "T1";
        be                  = std::make_unique<slack::SlackBackend>(store, app(), client, cr);
        be->onError         = [this](const std::string &m) { errors.push_back(m); };
        be->onThreadReplies = [this](
                                  ConvRef c, Ts root, std::vector<Reply> rs, const std::string &err
                              ) { reports.push_back({c, root, std::move(rs), err}); };
    }
    ~Env() {
        be.reset();
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
    ConvRef conv(const char *id) const { return store.findConversation(id); }
    // The Store by hand, for the tests that need no connect (no polls).
    ConvRef addChannel(const char *id) {
        model::User me;
        me.id          = "UME";
        me.realName    = "Me Myself";
        me.profileName = "memyself";
        store.me       = store.addUser(me);
        model::Conversation c;
        c.id   = id;
        c.name = "general";
        return store.addConversation(std::move(c));
    }
};

bool haveServer() {
    if (!fakeslack::server().empty())
        return true;
    std::fprintf(stderr, "  skip: no fake Slack (python3 missing)\n");
    return false;
}

int64_t msNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

} // namespace

TEST("slack links: an agent reply posts into the thread, marked, and gives its ts") {
    if (!haveServer())
        return;
    Env           e;
    const ConvRef c    = e.addChannel("C1");
    const Ts      root = model::parseTs("1700000000.000100"); // not loaded
    Ts            got  = -1;
    std::string   err  = "unset";
    e.be->postAgentReply(
        c,
        root,
        "\xF0\x9F\xA4\x96 Looking into it\xE2\x80\xA6",
        R"([{"type":"context","elements":[{"type":"mrkdwn","text":"AI reply"}]}])",
        [&](Ts ts, const std::string &e2) {
            got = ts;
            err = e2;
        }
    );
    const auto *pending = e.store.replies(c, root);
    REQUIRE(pending && pending->size() == 1 && pending->front().pending);
    CHECK(got == -1); // never from inside the call
    REQUIRE(pumpUntil([&] { return got != -1; }));
    CHECK(err.empty());
    Log         log;
    json::Value req = log.get("chat.postMessage");
    CHECK_STR(req["form"]["thread_ts"].str(), "1700000000.000100");
    CHECK(req["form"]["blocks"].str().find("\"context\"") != std::string::npos);
    json::Document meta;
    REQUIRE(meta.parse(req["form"]["metadata"].str(), nullptr));
    CHECK_STR(meta.root()["event_type"].str(), "msga_agent_reply");
    CHECK(meta.root()["event_payload"].isObject());
    // The ts is the server's, and the Store's copy is the confirmed one.
    CHECK(got > root);
    const auto *have = e.store.replies(c, root);
    REQUIRE(have && have->size() == 1);
    CHECK(have->front().ts == got && !have->front().pending);
    // Reading the thread back finds it marked.
    std::vector<Reply> rs;
    bool               read = false;
    e.be->loadThreadReplies(c, root, 0, [&](std::vector<Reply> r, const std::string &) {
        rs   = std::move(r);
        read = true;
    });
    REQUIRE(pumpUntil([&] { return read; }));
    REQUIRE(rs.size() == 1);
    CHECK(rs[0].message.ts == got && rs[0].agentReply);
    CHECK_STR(Log().get("conversations.replies")["form"]["include_all_metadata"].str(), "true");
}

TEST("slack links: a token that refuses metadata posts unmarked, then never tries again") {
    if (!haveServer())
        return;
    Env           e;
    const ConvRef c    = e.addChannel("C1");
    const Ts      root = model::parseTs("1700000000.000100");
    script("chat.postMessage", R"([{"ok": false, "error": "metadata_must_be_sent_from_app"}])");
    Ts          got = -1;
    std::string err;
    e.be->postAgentReply(c, root, "answer", {}, [&](Ts ts, const std::string &e2) {
        got = ts;
        err = e2;
    });
    REQUIRE(pumpUntil([&] { return got != -1; }));
    CHECK(got > 0 && err.empty());
    CHECK(e.errors.empty()); // no banner: it went out
    {
        Log log;
        REQUIRE(log.count("chat.postMessage") == 2);
        CHECK(log.get("chat.postMessage", 0)["form"].has("metadata"));
        CHECK(!log.get("chat.postMessage", 1)["form"].has("metadata"));
        CHECK(!log.get("chat.postMessage", 1)["form"].has("blocks"));
    }
    const auto *have = e.store.replies(c, root);
    REQUIRE(have && have->size() == 1); // posted once
    got = -1;
    e.be->postAgentReply(c, root, "second", {}, [&](Ts ts, const std::string &) { got = ts; });
    REQUIRE(pumpUntil([&] { return got != -1; }));
    Log log;
    REQUIRE(log.count("chat.postMessage") == 3);
    CHECK(!log.get("chat.postMessage", 2)["form"].has("metadata"));
    // Edits leave it out too.
    bool edited = false;
    e.be->editAgentReply(c, got, "second, edited", {}, [&](bool, const std::string &) {
        edited = true;
    });
    REQUIRE(pumpUntil([&] { return edited; }));
    CHECK(!Log().get("chat.update")["form"].has("metadata"));
}

TEST("slack links: a refused post fails with Slack's reason and ts 0") {
    if (!haveServer())
        return;
    Env           e;
    const ConvRef c = e.addChannel("C1");
    script("chat.postMessage", R"([{"ok": false, "error": "is_archived"}])");
    Ts          got = -1;
    std::string err;
    e.be->postAgentReply(
        c, model::parseTs("1700000000.000100"), "x", {}, [&](Ts ts, const std::string &e2) {
            got = ts;
            err = e2;
        }
    );
    REQUIRE(pumpUntil([&] { return got != -1; }));
    CHECK(got == 0);
    CHECK_STR(err, "is_archived");
    CHECK(calls("chat.postMessage") == 1);
    const auto *have = e.store.replies(c, model::parseTs("1700000000.000100"));
    CHECK(!have || have->empty()); // the ghost went
}

TEST("slack links: edit and delete work by ts, loaded or not") {
    if (!haveServer())
        return;
    Env           e;
    const ConvRef c    = e.addChannel("C1");
    const Ts      root = model::parseTs("1700000000.000100");
    // Not in the Store: the calls still go out.
    const Ts      far  = model::parseTs("1700000005.000100");
    bool          ok = false, called = false;
    std::string   err;
    e.be->editAgentReply(
        c,
        far,
        "status: reading files",
        R"([{"type":"divider"}])",
        [&](bool o, const std::string &) {
            ok     = o;
            called = true;
        }
    );
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(ok);
    {
        Log         log;
        json::Value req = log.get("chat.update");
        CHECK_STR(req["form"]["channel"].str(), "C1");
        CHECK_STR(req["form"]["ts"].str(), "1700000005.000100");
        CHECK_STR(req["form"]["text"].str(), "status: reading files");
        CHECK_STR(req["form"]["blocks"].str(), R"([{"type":"divider"}])");
        json::Document meta;
        REQUIRE(meta.parse(req["form"]["metadata"].str(), nullptr));
        CHECK_STR(meta.root()["event_type"].str(), "msga_agent_reply");
    }
    // In the Store: it follows once Slack agreed.
    Ts posted = 0;
    e.be->postAgentReply(c, root, "Looking into it", {}, [&](Ts ts, const std::string &) {
        posted = ts;
    });
    REQUIRE(pumpUntil([&] { return posted != 0; }));
    script("chat.update", R"([{"ok": true, "message": {"text": "Answered below"}}])");
    called = false;
    e.be->editAgentReply(c, posted, "Answered below", {}, [&](bool o, const std::string &) {
        ok     = o;
        called = true;
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(ok);
    const model::Message *m = e.store.findMessage(c, posted);
    REQUIRE(m);
    CHECK_STR(m->text, "Answered below");
    CHECK(m->edited);
    // Delete: gone from Slack and the Store; one already gone is done too.
    called = false;
    e.be->deleteAgentReply(c, posted, [&](bool o, const std::string &e2) {
        ok     = o;
        err    = e2;
        called = true;
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(ok && err.empty());
    CHECK(!e.store.findMessage(c, posted));
    CHECK_STR(Log().get("chat.delete")["form"]["ts"].str(), model::formatTs(posted));
    script("chat.delete", R"([{"ok": false, "error": "message_not_found"}])");
    called = false;
    e.be->deleteAgentReply(c, far, [&](bool o, const std::string &) {
        ok     = o;
        called = true;
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(ok);
    script("chat.delete", R"([{"ok": false, "error": "cant_delete_message"}])");
    called = false;
    e.be->deleteAgentReply(c, far, [&](bool o, const std::string &e2) {
        ok     = o;
        err    = e2;
        called = true;
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(!ok);
    CHECK_STR(err, "cant_delete_message");
}

TEST("slack links: the message menu's Delete takes an agent post back like any own message") {
    if (!haveServer())
        return;
    Env           e;
    const ConvRef c      = e.addChannel("C1");
    const Ts      root   = model::parseTs("1700000000.000100");
    Ts            posted = 0;
    e.be->postAgentReply(c, root, "an answer", {}, [&](Ts ts, const std::string &) {
        posted = ts;
    });
    REQUIRE(pumpUntil([&] { return posted != 0; }));
    const model::Message *m = e.store.findMessage(c, posted);
    REQUIRE(m);
    CHECK(m->user == e.store.me && !m->pending); // mine: the menu offers Delete
    e.be->remove(c, posted);                     // what the delete dialog calls
    REQUIRE(pumpUntil([&] { return calls("chat.delete") == 1; }));
    CHECK_STR(Log().get("chat.delete")["form"]["ts"].str(), model::formatTs(posted));
    CHECK(!e.store.findMessage(c, posted));
}

TEST("slack links: an answer's files go up into the thread and give the message's ts") {
    if (!haveServer())
        return;
    Env               e;
    const ConvRef     c    = e.addChannel("C1");
    const Ts          root = model::parseTs("1700000000.000100");
    const std::string path = file::join(base::env("HOME"), "agent-out.csv");
    REQUIRE(file::writeAtomic(path, "a,b\n1,2\n"));
    Ts          got = -1;
    std::string err = "unset";
    e.be->postAgentFiles(c, root, {}, {path}, [&](Ts ts, const std::string &e2) {
        got = ts;
        err = e2;
    });
    REQUIRE(pumpUntil([&] { return got != -1; }));
    CHECK(err.empty());
    CHECK(got > root);
    const Log   log; // the values below point into its document
    json::Value done = log.get("files.completeUploadExternal")["form"];
    CHECK_STR(done["thread_ts"].str(), "1700000000.000100");
    CHECK_STR(done["files"].str(), R"([{"id":"F0001","title":"agent-out.csv"}])");
    // None to upload: said at once, nothing sent.
    got = -1;
    e.be->postAgentFiles(c, root, {}, {}, [&](Ts ts, const std::string &e2) {
        got = ts;
        err = e2;
    });
    REQUIRE(pumpUntil([&] { return got != -1; }));
    CHECK(got == 0 && !err.empty());
    file::remove(path);
}

TEST("slack links: a thread read after a ts — every page, sorted, root on demand, marked") {
    if (!haveServer())
        return;
    Env           e;
    const ConvRef c    = e.addChannel("C1");
    const Ts      root = model::parseTs("1700000000.000100");
    // Two pages; Slack repeats the root on each.
    std::string   p1   = replies("1700000000.000100", {"1700000002.000100:UMIRA:second"});
    p1.insert(p1.size() - 1, R"(, "response_metadata": {"next_cursor": "r2"})");
    set(str::concat(
        {R"({"conversations.replies": )",
         p1,
         R"(, "conversations.replies?cursor=r2": )",
         replies(
             "1700000000.000100",
             {"1700000001.000100:UMIRA:first", "1700000003.000100:AGENT:the answer"}
         ),
         "}"}
    ));
    std::vector<Reply> rs;
    std::string        err = "unset";
    e.be->loadThreadReplies(c, root, 0, [&](std::vector<Reply> r, const std::string &e2) {
        rs  = std::move(r);
        err = e2;
    });
    REQUIRE(pumpUntil([&] { return err != "unset"; }));
    CHECK(err.empty());
    REQUIRE(rs.size() == 4);
    CHECK(rs[0].message.ts == root && !rs[0].agentReply); // the root, once
    CHECK_STR(rs[0].message.text, "the question");
    CHECK_STR(rs[1].message.text, "first");
    CHECK_STR(rs[2].message.text, "second");
    CHECK(rs[3].agentReply && rs[3].message.user == e.store.me);
    for (const Reply &r : rs)
        CHECK(r.message.threadTs == root);
    CHECK(rs[1].message.user == e.store.findUser("UMIRA"));
    // After a ts: oldest goes along, and the root (older) is left out.
    err = "unset";
    e.be->loadThreadReplies(
        c,
        root,
        model::parseTs("1700000001.000100"),
        [&](std::vector<Reply> r, const std::string &e2) {
            rs  = std::move(r);
            err = e2;
        }
    );
    REQUIRE(pumpUntil([&] { return err != "unset"; }));
    REQUIRE(rs.size() == 2);
    CHECK_STR(rs[0].message.text, "second");
    Log log;
    CHECK_STR(log.get("conversations.replies", 2)["form"]["oldest"].str(), "1700000001.000100");
    CHECK(!log.get("conversations.replies", 0)["form"].has("oldest"));
    // A thread that isn't there.
    script("conversations.replies", R"([{"ok": false, "error": "thread_not_found"}])");
    err = "unset";
    e.be->loadThreadReplies(c, root, 0, [&](std::vector<Reply> r, const std::string &e2) {
        rs  = std::move(r);
        err = e2;
    });
    REQUIRE(pumpUntil([&] { return err != "unset"; }));
    CHECK_STR(err, "thread_not_found");
    CHECK(rs.empty());
}

TEST("slack links: a watched thread reports each new message once, from its cursor") {
    if (!haveServer())
        return;
    const std::string root = "1700000000.000100";
    Env               e(
        str::concat(
            {R"({"conversations.replies": [)",
             replies(root, {"1700000001.000100:UMIRA:first"}),
             ",",
             replies(root, {"1700000001.000100:UMIRA:first", "1700000002.000100:AGENT:on it"}),
             "]}"}
        )
    );
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    e.be->watchThread(c, model::parseTs(root), 0, true);
    REQUIRE(pumpUntil([&] { return e.reports.size() >= 2; }, 5000));
    REQUIRE(e.reports[0].error.empty());
    CHECK(e.reports[0].conv == c && e.reports[0].root == model::parseTs(root));
    REQUIRE(e.reports[0].replies.size() == 2); // the root and "first"
    CHECK_STR(e.reports[0].replies[1].message.text, "first");
    REQUIRE(e.reports[1].replies.size() == 1); // only what is new
    CHECK_STR(e.reports[1].replies[0].message.text, "on it");
    CHECK(e.reports[1].replies[0].agentReply);
    {
        Log log;
        CHECK(!log.get("conversations.replies", 0)["form"].has("oldest"));
        CHECK_STR(log.get("conversations.replies", 1)["form"]["oldest"].str(), "1700000001.000100");
    }
    // Busy: polled every 15 s (150 ms here); nothing new, nothing reported.
    pumpFor(600);
    CHECK(e.reports.size() == 2);
    const int polls = calls("conversations.replies");
    CHECK(polls >= 4);
    e.be->unwatchThread(c, model::parseTs(root));
    pumpFor(400);
    CHECK(calls("conversations.replies") <= polls + 1); // at most the one in flight
}

TEST("slack links: watching starts after a start with the cursor given, staggered") {
    if (!haveServer())
        return;
    // Every thread answers the same: a root and two replies.
    const std::string root = "1700000000.000100";
    Env               e(
        str::concat(
            {R"({"conversations.replies": )",
             replies(root, {"1700000001.000100:UMIRA:old", "1700000002.000100:UMIRA:asked late"}),
             "}"}
        ),
        true,
        "10" // the 5 s tick is 500 ms here, the 1.2 s lane 120 ms
    );
    // Watched before the connect: nothing goes out until it settled.
    e.addChannel("C1");
    model::Conversation c2, c3;
    c2.id = "C2";
    c3.id = "C3";
    e.store.addConversation(std::move(c2));
    e.store.addConversation(std::move(c3));
    for (const char *id : {"C1", "C2", "C3"})
        e.be->watchThread(
            e.conv(id), model::parseTs(root), model::parseTs("1700000001.000100"), false
        );
    pumpFor(100);
    CHECK(calls("conversations.replies") == 0);
    e.be->connect(nullptr);
    REQUIRE(pumpUntil([&] { return calls("conversations.replies") >= 1; }, 5000));
    const int64_t first = msNow();
    REQUIRE(pumpUntil([&] { return calls("conversations.replies") >= 3; }, 5000));
    // Two 3 s steps, each rounded up to the next tick: about a second. All
    // at once they would be the paced lane's 240 ms apart.
    CHECK(msNow() - first >= 600);
    REQUIRE(pumpUntil([&] { return e.reports.size() == 3; }, 5000));
    for (const auto &r : e.reports) {
        REQUIRE(r.replies.size() == 1); // only after the cursor: no root, no "old"
        CHECK_STR(r.replies[0].message.text, "asked late");
    }
    CHECK_STR(Log().get("conversations.replies")["form"]["oldest"].str(), "1700000001.000100");
}

TEST("slack links: a quiet thread's poll comes forward when the feed shows a reply") {
    if (!haveServer())
        return;
    // A day-old thread: polled every 15 min (9 s here) — the feed brings it
    // forward long before that.
    const std::string root = "1700000000.000100";
    const std::string feedThread =
        R"({"root_msg": {"channel": "C1", "ts": "1700000000.000100", "user": "UMIRA", "text": "q"},
           "latest_replies": [{"ts": "1700000009.000100", "user": "UMIRA", "text": "news"}]})";
    Env e(
        str::concat(
            {R"({"conversations.replies": [)",
             replies(root, {}),
             ",",
             replies(root, {"1700000009.000100:UMIRA:news"}),
             R"(], "subscriptions.thread.getView": [
           {"ok": true, "threads": [], "total_unread_replies": 0},
           {"ok": true, "threads": [], "total_unread_replies": 0},
           {"ok": true, "total_unread_replies": 1, "threads": [)",
             feedThread,
             "]}]}"}
        )
    );
    REQUIRE(e.connect());
    e.be->watchThread(e.conv("C1"), model::parseTs(root), model::parseTs(root), false);
    REQUIRE(pumpUntil([&] { return calls("conversations.replies") == 1; }, 5000));
    REQUIRE(pumpUntil([&] { return !e.reports.empty(); }, 3000));
    REQUIRE(e.reports[0].replies.size() == 1);
    CHECK_STR(e.reports[0].replies[0].message.text, "news");
    CHECK(calls("conversations.replies") == 2);
}

TEST("slack links: client.counts' thread unread flag brings a quiet thread's poll forward") {
    if (!haveServer())
        return;
    const std::string root = "1700000000.000100";
    Env               e(
        str::concat(
            {R"({"conversations.replies": [)",
             replies(root, {}),
             ",",
             replies(root, {"1700000009.000100:UMIRA:news"}),
             R"(], "subscriptions.thread.getView": {"ok": false, "error": "not_allowed_token_type"},
           "client.counts": [
             {"ok": true, "channels": [], "ims": [], "mpims": [], "threads": {"has_unreads": false}},
             {"ok": true, "channels": [], "ims": [], "mpims": [], "threads": {"has_unreads": false}},
             {"ok": true, "channels": [], "ims": [], "mpims": [], "threads": {"has_unreads": false}},
             {"ok": true, "channels": [], "ims": [], "mpims": [], "threads": {"has_unreads": true}}]})"}
        )
    );
    REQUIRE(e.connect());
    e.be->watchThread(e.conv("C1"), model::parseTs(root), model::parseTs(root), false);
    REQUIRE(pumpUntil([&] { return calls("conversations.replies") == 1; }, 5000));
    REQUIRE(pumpUntil([&] { return !e.reports.empty(); }, 3000));
    CHECK_STR(e.reports[0].replies[0].message.text, "news");
}

TEST("slack links: a thread that can't be read is reported once per streak") {
    if (!haveServer())
        return;
    const std::string root = "1700000000.000100";
    Env               e(
        str::concat(
            {R"({"conversations.replies": [
              {"ok": false, "error": "thread_not_found"},
              {"ok": false, "error": "thread_not_found"},
              {"ok": false, "error": "thread_not_found"}, )",
             replies(root, {"1700000001.000100:UMIRA:back"}),
             "]}"}
        )
    );
    REQUIRE(e.connect());
    e.be->watchThread(e.conv("C1"), model::parseTs(root), 0, true);
    REQUIRE(pumpUntil([&] { return calls("conversations.replies") >= 3; }, 5000));
    REQUIRE(pumpUntil([&] { return e.reports.size() >= 2; }, 5000));
    CHECK_STR(e.reports[0].error, "thread_not_found");
    CHECK(e.reports[0].replies.empty());
    CHECK(e.reports[1].error.empty() && e.reports[1].replies.size() == 2);
}

TEST("slack links: a message's thread root, and my name as the workspace shows names") {
    if (!haveServer())
        return;
    Env                         e;
    const ConvRef               c    = e.addChannel("C1");
    const Ts                    root = model::parseTs("1700000000.000100");
    const Ts                    top  = model::parseTs("1700000050.000100");
    const Ts                    cast = model::parseTs("1700000060.000100");
    std::vector<model::Message> page;
    for (Ts ts : {root, top, cast}) {
        model::Message m;
        m.ts   = ts;
        m.text = "m";
        if (ts == root) {
            m.threadTs   = root;
            m.replyCount = 1;
        }
        if (ts == cast) // "also sent to the channel": its thread is unknown here
            m.extras().subtype = "thread_broadcast";
        page.push_back(std::move(m));
    }
    e.store.addPage(c, std::move(page));
    std::vector<model::Message> thread;
    model::Message              reply;
    reply.ts       = model::parseTs("1700000001.000100");
    reply.threadTs = root;
    thread.push_back(std::move(reply));
    e.store.addPage(c, std::move(thread));
    auto resolve = [&](Ts ts) {
        Ts got = -1;
        e.be->resolveThreadRoot(c, ts, [&](Ts r) { got = r; });
        pumpUntil([&] { return got != -1; });
        return got;
    };
    CHECK(resolve(root) == root);
    CHECK(resolve(model::parseTs("1700000001.000100")) == root);
    CHECK(resolve(top) == top);
    CHECK(calls("conversations.replies") == 0); // all from the Store
    script(
        "conversations.replies",
        R"([{"ok": true, "messages": [{"ts": "1700000060.000100", "thread_ts": "1700000040.000100",
            "user": "UME", "text": "m", "subtype": "thread_broadcast"}]}])"
    );
    CHECK(resolve(cast) == model::parseTs("1700000040.000100"));
    script("conversations.replies", R"([{"ok": false, "error": "thread_not_found"}])");
    CHECK(resolve(model::parseTs("1700000099.000100")) == 0);
    // Names: full names, or display names, as the workspace's setting says.
    CHECK_STR(e.be->selfName(), "Me Myself");
    e.store.setRealNames(false);
    CHECK_STR(e.be->selfName(), "memyself");
}

TEST("slack links: the user's name follows Slack's Names preference after connect") {
    if (!haveServer())
        return;
    Env e(R"({"users.prefs.get": {"ok": true, "prefs": {"display_real_names_override": -1}}})");
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return !e.store.realNames(); }, 3000));
    CHECK_STR(e.be->selfName(), "memyself");
    e.be->setNamesMode(model::Backend::NamesMode::Full);
    CHECK_STR(e.be->selfName(), "Me Myself");
}

#endif // !_WIN32

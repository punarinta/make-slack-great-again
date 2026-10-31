// SlackBackend's write side against a local fake Slack (tests/support/fake_slack.py):
// the optimistic Store changes, the exact Web API calls, the lost-answer
// reconcile that must never post twice, uploads, search mapping. Nothing
// here talks to the real Slack: MSGA_SLACK_API_BASE points every call at
// the fake before the first one is made.
#include "app/slack/slack_backend.h"
#include "support/fake_slack_server.h"
#include "base/file.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "support/test.h"
#include "net/net.h"
#include "plat/plat.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#ifndef _WIN32
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

namespace {

using model::ConvRef;
using model::Ts;

using fakeslack::app;
using fakeslack::ctl;
using fakeslack::pumpFor;
using fakeslack::pumpUntil;
using fakeslack::server;

void script(const char *method, const char *responsesJson) {
    ctl("POST",
        "/_ctl/script",
        std::string("{\"method\":\"") + method + "\",\"responses\":" + responsesJson + "}");
}

// The requests the fake has seen.
struct Log {
    json::Document doc;
    Log() { doc.parse(ctl("GET", "/_ctl/log").body, nullptr); }
    int count(std::string_view method) const {
        int n = 0;
        for (json::Value r : doc.root())
            n += r["method"].str() == method;
        return n;
    }
    // The n-th request of `method` (an empty Value if there are fewer).
    json::Value get(std::string_view method, int n = 0) const {
        for (json::Value r : doc.root())
            if (r["method"].str() == method && n-- == 0)
                return r;
        return {};
    }
    std::string order() const { // "a,b,c"
        std::string s;
        for (json::Value r : doc.root())
            s.append(s.empty() ? "" : ",").append(r["method"].str());
        return s;
    }
};

// A signed-in session workspace with #general, a DM and one older message.
struct Env {
    model::Store                         store;
    net::Client                          client{app()};
    std::unique_ptr<slack::SlackBackend> be;
    ConvRef                              general = model::kNoConv, dm = model::kNoConv;
    model::UserRef                       me = model::kNoUser, mira = model::kNoUser;
    Ts                                   old = 0; // my message in #general

    Env() {
        ctl("POST", "/_ctl/reset");
        model::User u;
        u.id     = "UME";
        u.name   = "me";
        me       = store.addUser(u);
        store.me = me;
        u.id     = "UMIRA";
        u.name   = "mira";
        mira     = store.addUser(u);
        model::Conversation c;
        c.id    = "C1";
        c.name  = "general";
        general = store.addConversation(std::move(c));
        model::Conversation d;
        d.id     = "D1";
        d.kind   = model::ConvKind::Dm;
        d.dmUser = mira;
        dm       = store.addConversation(std::move(d));
        model::Message m;
        m.ts = old = model::parseTs("1700000000.000100");
        m.user     = me;
        m.text     = "first";
        std::vector<model::Message> page;
        page.push_back(std::move(m));
        store.addPage(general, std::move(page));
        slack::Credentials cr;
        cr.token  = "xoxc-test";
        cr.cookie = "xoxd-test";
        cr.teamId = "T1";
        be        = std::make_unique<slack::SlackBackend>(store, app(), client, cr);
    }
    const model::Message *last(ConvRef c) const {
        const auto &ms = store.conversation(c).messages;
        return ms.empty() ? nullptr : &ms.back();
    }
};

struct Result {
    bool                 called = false, ok = false;
    std::string          err;
    model::Backend::Done cb() {
        return [this](bool o, const std::string &e) {
            called = true;
            ok     = o;
            err    = e;
        };
    }
};

bool haveServer() {
    if (!server().empty())
        return true;
    std::fprintf(stderr, "  skip: no python3 for the fake Slack\n");
    return false;
}

} // namespace

TEST("slack write: send shows a pending copy, then the server's message") {
    if (!haveServer())
        return;
    Env    e;
    Result r;
    e.be->send(e.general, "hi <@UMIRA> & co", 0, r.cb());
    const model::Message *p = e.last(e.general);
    REQUIRE(p && p->pending);
    CHECK(p->user == e.me && p->text == "hi <@UMIRA> & co");
    const Ts local = p->ts;
    CHECK(!r.called); // never from inside the call
    REQUIRE(pumpUntil([&] { return r.called; }));
    CHECK(r.ok);
    Log log;
    REQUIRE(log.count("chat.postMessage") == 1);
    json::Value req = log.get("chat.postMessage");
    CHECK_STR(req["form"]["channel"].str(), "C1");
    CHECK_STR(req["form"]["text"].str(), "hi <@UMIRA> & co");
    CHECK(!req["form"].has("thread_ts"));
    CHECK_STR(req["auth"].str(), "Bearer xoxc-test");
    CHECK_STR(req["cookie"].str(), "d=xoxd-test");
    CHECK(req["ctype"].str().substr(0, 33) == "application/x-www-form-urlencoded");
    const model::Message *m = e.last(e.general);
    REQUIRE(m);
    CHECK(!m->pending && m->ts != local && !e.store.findMessage(e.general, local));
    CHECK(e.store.conversation(e.general).messages.size() == 2);
    CHECK(e.store.conversation(e.general).lastRead == m->ts); // my post reads up to it
}

TEST("slack write: a broadcast reply carries thread_ts and reply_broadcast") {
    if (!haveServer())
        return;
    Env    e;
    Result r;
    e.be->sendBroadcast(e.general, "also here", e.old, r.cb());
    REQUIRE(pumpUntil([&] { return r.called; }));
    CHECK(r.ok);
    Log         log;
    json::Value req = log.get("chat.postMessage");
    CHECK(req["form"]["thread_ts"].str() == model::formatTs(e.old));
    CHECK_STR(req["form"]["reply_broadcast"].str(), "true");
    const auto *replies = e.store.replies(e.general, e.old);
    REQUIRE(replies && replies->size() == 1);
    CHECK(!replies->front().pending && replies->front().threadTs == e.old);
}

TEST("slack write: replying in a thread follows it") {
    if (!haveServer())
        return;
    Env      e;
    const Ts root = model::parseTs("1690000000.000100"); // someone's, not loaded
    CHECK(!e.be->threadFollowed(e.general, root));
    e.be->send(e.general, "on it", root, nullptr);
    CHECK(e.be->threadFollowed(e.general, root)); // at once, before the answer
    Result r;
    e.be->send(e.general, "top level", 0, r.cb());
    REQUIRE(pumpUntil([&] { return r.called; }));
    CHECK(!e.be->threadFollowed(e.general, 0));
}

TEST("slack write: a lost answer to a delivered post reconciles, never posts twice") {
    if (!haveServer())
        return;
    Env e;
    script("chat.postMessage", R"([{"__partial":true,"__deliver":true}])");
    Result r;
    e.be->send(e.general, "a & b <c>", 0, r.cb()); // stored escaped: the scan unescapes
    REQUIRE(pumpUntil([&] { return r.called; }, 8000));
    CHECK(r.ok);
    pumpFor(300); // nothing else may follow
    Log log;
    CHECK_STR(log.order(), "chat.postMessage,conversations.history");
    CHECK(log.get("conversations.history")["form"]["oldest"].str() == model::formatTs(e.old));
    const model::Message *m = e.last(e.general);
    REQUIRE(m);
    CHECK(!m->pending && e.store.conversation(e.general).messages.size() == 2);
}

TEST("slack write: a lost post that never landed is posted again once") {
    if (!haveServer())
        return;
    Env e;
    script("chat.postMessage", R"([{"__partial":true}])");
    Result r;
    e.be->send(e.general, "retry me", 0, r.cb());
    REQUIRE(pumpUntil([&] { return r.called; }, 8000));
    CHECK(r.ok);
    Log log;
    CHECK_STR(log.order(), "chat.postMessage,conversations.history,chat.postMessage");
    CHECK(e.store.conversation(e.general).messages.size() == 2);
    CHECK(!e.last(e.general)->pending);
}

TEST("slack write: a rejected send drops the pending copy and says why") {
    if (!haveServer())
        return;
    Env e;
    script("chat.postMessage", R"([{"ok":false,"error":"not_in_channel"}])");
    Result r;
    e.be->send(e.general, "nope", 0, r.cb());
    CHECK(e.store.conversation(e.general).messages.size() == 2);
    REQUIRE(pumpUntil([&] { return r.called; }));
    CHECK(!r.ok);
    CHECK_STR(r.err, "not_in_channel");
    CHECK(e.store.conversation(e.general).messages.size() == 1);
    pumpFor(200);
    CHECK(Log().count("chat.postMessage") == 1);
}

TEST("slack write: undo while in flight deletes the server copy when it lands") {
    if (!haveServer())
        return;
    Env    e;
    Result r;
    e.be->send(e.general, "oops", 0, r.cb());
    const Ts local = e.last(e.general)->ts;
    e.be->remove(e.general, local);
    CHECK(!e.store.findMessage(e.general, local));
    REQUIRE(pumpUntil([&] { return r.called; }));
    CHECK(!r.ok);
    REQUIRE(pumpUntil([&] { return Log().count("chat.delete") == 1; }));
    Log log; // the server's ts, never the local one
    CHECK(model::parseTs(log.get("chat.delete")["form"]["ts"].str()) != 0);
    CHECK(log.get("chat.delete")["form"]["ts"].str() != model::formatTs(local));
    CHECK(e.store.conversation(e.general).messages.size() == 1);
    // Undo after confirmation names the local ts too: mapped to the server's.
    Result r2;
    e.be->send(e.general, "again", 0, r2.cb());
    const Ts local2 = e.last(e.general)->ts;
    REQUIRE(pumpUntil([&] { return r2.called; }));
    const Ts server2 = e.last(e.general)->ts;
    e.be->remove(e.general, local2);
    REQUIRE(pumpUntil([&] { return Log().count("chat.delete") == 2; }));
    CHECK(Log().get("chat.delete", 1)["form"]["ts"].str() == model::formatTs(server2));
    CHECK(!e.store.findMessage(e.general, server2));
}

TEST("slack write: edit, delete and react update the Store and send the right params") {
    if (!haveServer())
        return;
    Env e;
    e.be->edit(e.general, e.old, "second");
    CHECK(e.store.findMessage(e.general, e.old)->text == "second");
    CHECK(e.store.findMessage(e.general, e.old)->edited);
    e.be->react(e.general, e.old, "tada", true);
    REQUIRE(e.store.findMessage(e.general, e.old)->reactions.size() == 1);
    // Two independent requests: either can reach Slack first.
    REQUIRE(pumpUntil([&] {
        const Log l;
        return l.count("chat.update") == 1 && l.count("reactions.add") == 1;
    }));
    Log         log;
    json::Value up = log.get("chat.update")["form"];
    CHECK_STR(up["channel"].str(), "C1");
    CHECK(up["ts"].str() == model::formatTs(e.old));
    CHECK_STR(up["text"].str(), "second");
    json::Value ra = log.get("reactions.add")["form"];
    CHECK_STR(ra["channel"].str(), "C1");
    CHECK(ra["timestamp"].str() == model::formatTs(e.old));
    CHECK_STR(ra["name"].str(), "tada");
    // Refused: everything goes back.
    script("chat.update", R"([{"ok":false,"error":"cant_update_message"}])");
    script("reactions.remove", R"([{"ok":false,"error":"no_permission"}])");
    e.be->edit(e.general, e.old, "third");
    e.be->react(e.general, e.old, "tada", false);
    CHECK(e.store.findMessage(e.general, e.old)->reactions.empty());
    REQUIRE(pumpUntil([&] {
        const model::Message *m = e.store.findMessage(e.general, e.old);
        return m->text == "second" && m->reactions.size() == 1;
    }));
    // Delete: gone at once; refused, it comes back.
    script("chat.delete", R"([{"ok":false,"error":"cant_delete_message"}])");
    e.be->remove(e.general, e.old);
    CHECK(!e.store.findMessage(e.general, e.old));
    REQUIRE(pumpUntil([&] { return e.store.findMessage(e.general, e.old) != nullptr; }));
    CHECK(e.store.conversation(e.general).unread == 0);
    e.be->remove(e.general, e.old);
    REQUIRE(pumpUntil([&] { return Log().count("chat.delete") == 2; }));
    pumpFor(100);
    CHECK(!e.store.findMessage(e.general, e.old));
    CHECK(Log().get("chat.delete", 1)["form"]["ts"].str() == model::formatTs(e.old));
}

TEST("slack write: reactions, stars and marks outlast lost connections, 429s and gateway pages") {
    if (!haveServer())
        return;
    Env e;
    script("reactions.add", R"([
      {"__partial": true},
      {"__status": 429, "__headers": {"Retry-After": "1"}, "ok": false, "error": "ratelimited"},
      {"__status": 502, "__raw": "<html><body>Bad gateway</body></html>"},
      {"ok": true}
    ])");
    e.be->react(e.general, e.old, "tada", true);
    REQUIRE(pumpUntil([&] { return Log().count("reactions.add") == 4; }, 8000));
    pumpFor(100);
    CHECK(Log().count("reactions.add") == 4);
    CHECK(e.store.findMessage(e.general, e.old)->reactions.size() == 1); // kept
    script("stars.add", R"([{"__status": 503, "__raw": "Service Unavailable"}, {"ok": true}])");
    e.be->setStarred(e.dm, true);
    REQUIRE(pumpUntil([&] { return Log().count("stars.add") == 2; }, 5000));
    CHECK(e.store.conversation(e.dm).starred);
    script("conversations.mark", R"([{"__partial": true}, {"ok": true}])");
    e.be->markRead(e.general, e.old);
    REQUIRE(pumpUntil([&] { return Log().count("conversations.mark") == 2; }, 5000));
    // A refusal still rolls back, and is not retried.
    script("reactions.remove", R"([{"ok":false,"error":"no_permission"}])");
    e.be->react(e.general, e.old, "tada", false);
    REQUIRE(pumpUntil([&] {
        return e.store.findMessage(e.general, e.old)->reactions.size() == 1;
    }));
    pumpFor(100);
    CHECK(Log().count("reactions.remove") == 1);
}

TEST("slack write: add then remove during the add's retry stays in order, ends without it") {
    if (!haveServer())
        return;
    Env e;
    // The add's first try is lost and its retry is slow: the remove that
    // follows must wait for it, not overtake it.
    script("reactions.add", R"([{"__partial": true}, {"ok": true, "__delay": 0.3}])");
    e.be->react(e.general, e.old, "tada", true);
    REQUIRE(pumpUntil([&] { return Log().count("reactions.add") >= 1; }));
    e.be->react(e.general, e.old, "tada", false);
    e.be->react(e.general, e.old, "tada", true); // flip-flops coalesce to the last wish
    e.be->react(e.general, e.old, "tada", false);
    CHECK(e.store.findMessage(e.general, e.old)->reactions.empty());
    REQUIRE(pumpUntil([&] { return Log().count("reactions.remove") == 1; }, 5000));
    pumpFor(300);
    Log log;
    CHECK(log.count("reactions.add") == 2);
    CHECK(log.count("reactions.remove") == 1);
    CHECK_STR(log.order().substr(log.order().rfind("reactions.")), "reactions.remove");
    CHECK(e.store.findMessage(e.general, e.old)->reactions.empty());
    // A wish equal to what is in flight sends nothing more.
    e.be->react(e.general, e.old, "tada", true);
    e.be->react(e.general, e.old, "tada", false);
    e.be->react(e.general, e.old, "tada", true);
    REQUIRE(pumpUntil([&] { return Log().count("reactions.add") == 3; }));
    pumpFor(300);
    CHECK(Log().count("reactions.add") == 3 && Log().count("reactions.remove") == 1);
    CHECK(e.store.findMessage(e.general, e.old)->reactions.size() == 1);
}

TEST("slack write: read cursors — conversations.mark, thread marks, mark unread") {
    if (!haveServer())
        return;
    Env            e;
    model::Message reply;
    reply.ts       = e.old + 50;
    reply.threadTs = e.old;
    reply.user     = e.mira;
    std::vector<model::Message> page;
    page.push_back(std::move(reply));
    e.store.addPage(e.general, std::move(page));
    e.be->markRead(e.general, e.old);
    CHECK(e.store.conversation(e.general).lastRead == e.old);
    e.be->markRead(e.general, e.old); // read already: no second call
    e.be->markRead(e.general, e.old + 50);
    e.be->markUnread(e.general, e.old);
    CHECK(e.store.conversation(e.general).lastRead == e.old - 1);
    REQUIRE(pumpUntil([&] { return Log().count("conversations.mark") == 2; }));
    pumpFor(100);
    Log log;
    CHECK(log.count("conversations.mark") == 2);
    CHECK(log.get("conversations.mark")["form"]["ts"].str() == model::formatTs(e.old));
    CHECK(log.get("conversations.mark", 1)["form"]["ts"].str() == model::formatTs(e.old - 1));
    json::Value tm = log.get("subscriptions.thread.mark")["form"];
    CHECK(tm["thread_ts"].str() == model::formatTs(e.old));
    CHECK(tm["ts"].str() == model::formatTs(e.old + 50));
}

TEST("slack write: an upload goes URL, bytes, complete, then replaces the pending copy") {
    if (!haveServer())
        return;
    Env               e;
    const std::string path = file::join(base::env("HOME"), "upload-test.txt");
    REQUIRE(file::writeAtomic(path, "hello file"));
    Result r;
    e.be->sendWithFiles(e.general, "look", 0, {path}, r.cb());
    const model::Message *p = e.last(e.general);
    REQUIRE(p && p->pending && p->files().size() == 1);
    CHECK_STR(p->files()[0].name, "upload-test.txt");
    const Ts local = p->ts;
    REQUIRE(pumpUntil([&] { return r.called; }));
    CHECK(r.ok);
    REQUIRE(pumpUntil([&] { return !e.store.findMessage(e.general, local); }));
    Log log;
    CHECK_STR(
        log.order(),
        "files.getUploadURLExternal,upload,files.completeUploadExternal,conversations.history"
    );
    CHECK_STR(log.get("files.getUploadURLExternal")["form"]["filename"].str(), "upload-test.txt");
    CHECK_STR(log.get("files.getUploadURLExternal")["form"]["length"].str(), "10");
    CHECK(log.get("upload")["len"].integer() == 10);
    CHECK(log.get("upload")["auth"].str().empty()); // pre-signed: no token
    json::Value done = log.get("files.completeUploadExternal")["form"];
    CHECK_STR(done["channel_id"].str(), "C1");
    CHECK_STR(done["initial_comment"].str(), "look");
    CHECK_STR(done["files"].str(), R"([{"id":"F0001","title":"upload-test.txt"}])");
    const model::Message *m = e.last(e.general);
    REQUIRE(m);
    CHECK(!m->pending && m->ts != local);
    CHECK(e.store.conversation(e.general).messages.size() == 2);
    // A failed upload: the ghost goes and the banner says so.
    std::vector<std::string> errors;
    e.be->onError = [&](const std::string &msg) { errors.push_back(msg); };
    script("files.completeUploadExternal", R"([{"ok":false,"error":"not_in_channel"}])");
    Result bad;
    e.be->sendWithFiles(e.general, "again", 0, {path}, bad.cb());
    REQUIRE(pumpUntil([&] { return bad.called; }));
    CHECK(!bad.ok);
    REQUIRE(errors.size() == 1);
    CHECK_STR(errors[0], "Upload failed: not_in_channel");
}

TEST("slack write: a forwarded file is downloaded with the workspace's auth, then uploaded") {
    if (!haveServer())
        return;
    Env               e;
    const std::string dir = file::join(base::env("HOME"), "forward-test");
    REQUIRE(file::makeDirs(dir));
    const std::string to = file::join(dir, "photo.png");
    Result            got;
    e.be->downloadFile(server() + "/files-pri/T1-F9/photo.png", to, got.cb());
    REQUIRE(pumpUntil([&] { return got.called; }));
    CHECK(got.ok);
    std::string bytes;
    REQUIRE(file::readAll(to, &bytes));
    CHECK_STR(bytes, "bytes of photo.png");
    // The copy goes up as my own upload; the pending copy shows it at once,
    // while the bytes are read off the UI thread.
    Result sent;
    e.be->sendWithFiles(e.general, "", 0, {to}, sent.cb());
    const model::Message *p = e.last(e.general);
    REQUIRE(p && p->pending && p->files().size() == 1);
    CHECK_STR(p->files()[0].name, "photo.png");
    REQUIRE(pumpUntil([&] { return sent.called; }));
    CHECK(sent.ok);
    Log log;
    CHECK(
        log.order().rfind(
            "download,files.getUploadURLExternal,upload,files.completeUploadExternal", 0
        ) == 0
    );
    CHECK_STR(log.get("download")["auth"].str(), "Bearer xoxc-test");
    CHECK_STR(log.get("files.getUploadURLExternal")["form"]["filename"].str(), "photo.png");
    CHECK_STR(log.get("files.getUploadURLExternal")["form"]["length"].str(), "18");
    CHECK_STR(log.get("upload")["body"].str(), "bytes of photo.png");
    CHECK_STR(
        log.get("files.completeUploadExternal")["form"]["files"].str(),
        R"([{"id":"F0001","title":"photo.png"}])"
    );
}

TEST("slack write: an upload of files that can't be read fails, and nothing is sent") {
    if (!haveServer())
        return;
    Env    e;
    Result r;
    e.be->sendWithFiles(
        e.general, "look", 0, {file::join(base::env("HOME"), "no-such-file.txt")}, r.cb()
    );
    REQUIRE(e.last(e.general) && e.last(e.general)->pending); // the pending copy, at once
    REQUIRE(pumpUntil([&] { return r.called; }));
    CHECK(!r.ok);
    CHECK(e.store.conversation(e.general).messages.size() == 1); // the copy went
    CHECK(!e.last(e.general)->pending);
    CHECK(Log().count("files.getUploadURLExternal") == 0);
}

TEST("slack write: search maps channel, ts and thread; unknown channels are skipped") {
    if (!haveServer())
        return;
    Env e;
    script(
        "search.messages",
        R"([{"ok":true,"messages":{"matches":[
            {"channel":{"id":"C1"},"ts":"1700000001.000200","permalink":"https://x/p1"},
            {"channel":{"id":"D1"},"ts":"1700000002.000300",
             "permalink":"https://x/archives/D1/p1700000002000300?thread_ts=1700000000.000100&cid=D1"},
            {"channel":{"id":"C404"},"ts":"1700000003.000400"}]}}])"
    );
    std::vector<model::Backend::SearchHit> hits;
    bool                                   got = false;
    e.be->search("hello world", [&](std::vector<model::Backend::SearchHit> h) {
        hits = std::move(h);
        got  = true;
    });
    REQUIRE(pumpUntil([&] { return got; }));
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].conv == e.general && hits[0].ts == model::parseTs("1700000001.000200"));
    CHECK(hits[0].thread == 0);
    CHECK(hits[1].conv == e.dm && hits[1].thread == model::parseTs("1700000000.000100"));
    const Log   log;
    json::Value q = log.get("search.messages")["form"];
    CHECK_STR(q["query"].str(), "hello world");
    CHECK_STR(q["sort"].str(), "timestamp");
}

TEST("slack write: save and remind use saved.*; a rejection rolls back") {
    if (!haveServer())
        return;
    Env e;
    e.be->setReminder(e.general, e.old, 1900000000);
    CHECK(e.store.reminderAt(e.general, e.old) == 1900000000);
    CHECK(e.store.findMessage(e.general, e.old)->saved);
    e.be->setSaved(e.general, e.old, false);
    CHECK(e.store.reminderAt(e.general, e.old) == 0);
    REQUIRE(pumpUntil([&] { return Log().count("saved.delete") == 1; }));
    Log         log;
    json::Value add = log.get("saved.add")["form"];
    CHECK_STR(add["item_type"].str(), "message");
    CHECK_STR(add["item_id"].str(), "C1");
    CHECK(add["ts"].str() == model::formatTs(e.old));
    CHECK_STR(add["date_due"].str(), "1900000000");
    std::vector<std::string> errors; // the banner says why the tint went
    e.be->onError = [&](const std::string &m) { errors.push_back(m); };
    script("saved.add", R"([{"ok":false,"error":"not_allowed"}])");
    e.be->setSaved(e.general, e.old, true);
    CHECK(e.store.findMessage(e.general, e.old)->saved);
    REQUIRE(pumpUntil([&] { return !e.store.findMessage(e.general, e.old)->saved; }));
    CHECK(!Log().get("saved.add", 1)["form"].has("date_due")); // a plain bookmark
    REQUIRE(errors.size() == 1);
    CHECK_STR(errors[0], "Couldn't save the message: not_allowed");
}

TEST(
    "slack write: schedule send — a session schedules a draft, a token uses "
    "chat.scheduleMessage"
) {
    if (!haveServer())
        return;
    Env    e;
    Result r;
    // Session tokens: chat.scheduleMessage answers not_allowed_token_type,
    // so it is the web client's dated draft, its body a rich_text block.
    e.be->scheduleMessage(e.general, "*later* <@UMIRA>", 0, 1900000000, r.cb());
    REQUIRE(pumpUntil([&] { return r.called; }));
    CHECK(r.ok);
    Log         log;
    json::Value d = log.get("drafts.create")["form"];
    CHECK(log.count("chat.scheduleMessage") == 0);
    CHECK_STR(d["date_scheduled"].str(), "1900000000");
    CHECK_STR(d["destinations"].str(), R"([{"channel_id":"C1"}])");
    CHECK_STR(d["file_ids"].str(), "[]");
    CHECK_STR(d["is_from_composer"].str(), "true");
    const std::string id(d["client_msg_id"].str());
    CHECK(id.size() == 36 && id[14] == '4' && id[8] == '-' && id[23] == '-');
    CHECK(id.find_first_of("ABCDEF") == std::string::npos);
    CHECK(d["blocks"].str().find(R"("type":"rich_text")") != std::string_view::npos);
    CHECK(d["blocks"].str().find(R"("user_id":"UMIRA")") != std::string_view::npos);
    CHECK(d["blocks"].str().find(R"("bold":true)") != std::string_view::npos);
    // A thread reply names its thread; a fresh id each time.
    Result t;
    e.be->scheduleMessage(e.general, "in thread", e.old, 1900000000, t.cb());
    REQUIRE(pumpUntil([&] { return t.called; }));
    Log         log2;
    json::Value d2 = log2.get("drafts.create", 1)["form"];
    CHECK(
        d2["destinations"].str() ==
        str::concat({R"([{"channel_id":"C1","thread_ts":")", model::formatTs(e.old), R"("}])"})
    );
    CHECK(d2["client_msg_id"].str() != id);
    // A rejection says why.
    std::vector<std::string> errors;
    e.be->onError = [&](const std::string &m) { errors.push_back(m); };
    script("drafts.create", R"([{"ok":false,"error":"attached_draft_exists"}])");
    Result f;
    e.be->scheduleMessage(e.general, "again", 0, 1900000000, f.cb());
    REQUIRE(pumpUntil([&] { return f.called; }));
    CHECK_FALSE(f.ok);
    REQUIRE(errors.size() == 1);
    CHECK_STR(
        errors[0],
        "Couldn't schedule message: Slack has an unsent draft in this conversation. Send or "
        "clear it first."
    );

    // A token workspace (no cookie): the public method.
    slack::Credentials cr;
    cr.token  = "xoxp-test";
    cr.teamId = "T1";
    slack::SlackBackend tok(e.store, app(), e.client, cr);
    Result              p;
    tok.scheduleMessage(e.general, "later", 0, 1900000000, p.cb());
    REQUIRE(pumpUntil([&] { return p.called; }));
    Log         log3;
    json::Value s = log3.get("chat.scheduleMessage")["form"];
    CHECK_STR(s["channel"].str(), "C1");
    CHECK_STR(s["text"].str(), "later");
    CHECK_STR(s["post_at"].str(), "1900000000");
}

TEST("slack write: scheduled messages — a session lists dated drafts and cancels one") {
    if (!haveServer())
        return;
    Env e;
    // Undated drafts are plain drafts; sent or deleted ones may linger.
    script(
        "drafts.list",
        R"([{"ok":true,"drafts":[
            {"id":"Dr2","last_updated_ts":"1700000001.5","date_scheduled":1900000500,
             "destinations":[{"channel_id":"D1"}],
             "blocks":[{"type":"rich_text","elements":[{"type":"rich_text_section",
               "elements":[{"type":"text","text":"later"}]}]}]},
            {"id":"Dr1","last_updated_ts":"1700000000.123456","date_scheduled":"1900000000",
             "destinations":[{"channel_id":"C1","thread_ts":"1700000000.000100"}],
             "blocks":[{"type":"rich_text","elements":[{"type":"rich_text_section",
               "elements":[{"type":"text","text":"in thread"}]}]}]},
            {"id":"Dr3","last_updated_ts":"1","date_scheduled":0,
             "destinations":[{"channel_id":"C1"}],"blocks":[]},
            {"id":"Dr4","last_updated_ts":"1","date_scheduled":1900000000,"is_sent":true,
             "destinations":[{"channel_id":"C1"}],"blocks":[]}
        ]}])"
    );
    e.be->refreshScheduled();
    REQUIRE(pumpUntil([&] { return e.store.hasScheduled(); }));
    const auto &l = e.store.scheduled();
    REQUIRE(l.size() == 2);
    CHECK_STR(l[0].id, "Dr1"); // soonest first
    CHECK(l[0].conv == e.general);
    CHECK(l[0].thread == e.old);
    CHECK(l[0].threadKnown);
    CHECK(l[0].at == 1900000000);
    CHECK_STR(l[0].text, "in thread");
    CHECK_STR(l[1].id, "Dr2");
    CHECK(l[1].conv == e.dm);
    CHECK(l[1].thread == 0);
    CHECK_STR(Log().get("drafts.list")["form"]["is_active"].str(), "true");

    // Cancel: drafts.delete with the version padded to 7 decimals; the
    // Store drops it.
    Result r;
    e.be->cancelScheduled("Dr1", r.cb());
    REQUIRE(pumpUntil([&] { return r.called; }));
    CHECK(r.ok);
    {
        Log         log;
        json::Value d = log.get("drafts.delete")["form"];
        CHECK_STR(d["draft_id"].str(), "Dr1");
        CHECK_STR(d["client_last_updated_ts"].str(), "1700000000.1234560");
    }
    CHECK(std::none_of(e.store.scheduled().begin(), e.store.scheduled().end(), [](const auto &s) {
        return s.id == "Dr1";
    }));

    // Edited elsewhere meanwhile: once more with a newer version, then gone.
    model::Store::ScheduledItem it;
    it.id      = "Dr9";
    it.version = "1700000000.5";
    it.conv    = e.general;
    it.at      = 1900000000;
    REQUIRE(pumpUntil([&] { return Log().count("drafts.list") == 2; }));
    pumpFor(50);
    e.store.setScheduled({it});
    script("drafts.delete", R"([{"ok":false,"error":"draft_has_conflict"}])");
    Result c;
    e.be->cancelScheduled("Dr9", c.cb());
    REQUIRE(pumpUntil([&] { return c.called; }));
    CHECK(c.ok);
    {
        Log log;
        CHECK(log.count("drafts.delete") == 3);
        const std::string v(log.get("drafts.delete", 2)["form"]["client_last_updated_ts"].str());
        CHECK(v.size() == 18 && v[10] == '.' && v > "1700000000.5000000");
    }
    CHECK_FALSE(e.store.hasScheduled());

    // A refusal says why and keeps it.
    std::vector<std::string> errors;
    e.be->onError = [&](const std::string &m) { errors.push_back(m); };
    REQUIRE(pumpUntil([&] { return Log().count("drafts.list") == 3; }));
    pumpFor(50);
    e.store.setScheduled({it});
    script("drafts.delete", R"([{"ok":false,"error":"ratelimited_custom"}])");
    script("drafts.list", R"([{"ok":true,"drafts":[{"id":"Dr9","last_updated_ts":"1700000000.5",
        "date_scheduled":1900000000,"destinations":[{"channel_id":"C1"}],"blocks":[]}]}])");
    Result f;
    e.be->cancelScheduled("Dr9", f.cb());
    REQUIRE(pumpUntil([&] { return f.called; }));
    CHECK_FALSE(f.ok);
    CHECK(e.store.hasScheduled());
    REQUIRE(errors.size() == 1);
    CHECK_STR(errors[0], "Couldn't cancel the scheduled message: ratelimited_custom");
}

TEST("slack write: scheduled messages — a token lists and cancels Slack's scheduled messages") {
    if (!haveServer())
        return;
    Env                e;
    slack::Credentials cr;
    cr.token  = "xoxp-test";
    cr.teamId = "T1";
    slack::SlackBackend tok(e.store, app(), e.client, cr);
    script(
        "chat.scheduledMessages.list",
        R"([{"ok":true,"scheduled_messages":[
            {"id":"Q1","channel_id":"C1","post_at":1900000000,"text":"*hi*"}]}])"
    );
    tok.refreshScheduled();
    REQUIRE(pumpUntil([&] { return e.store.hasScheduled(); }));
    const auto &l = e.store.scheduled();
    REQUIRE(l.size() == 1);
    CHECK_STR(l[0].id, "Q1");
    CHECK(l[0].conv == e.general);
    CHECK_FALSE(l[0].threadKnown); // the list doesn't say: no Send now
    CHECK_STR(l[0].text, "*hi*");
    // Gone already (sent meanwhile): dropped all the same.
    script(
        "chat.deleteScheduledMessage", R"([{"ok":false,"error":"invalid_scheduled_message_id"}])"
    );
    Result r;
    tok.cancelScheduled("Q1", r.cb());
    REQUIRE(pumpUntil([&] { return r.called; }));
    Log         log;
    json::Value d = log.get("chat.deleteScheduledMessage")["form"];
    CHECK_STR(d["channel"].str(), "C1");
    CHECK_STR(d["scheduled_message_id"].str(), "Q1");
    CHECK_FALSE(e.store.hasScheduled());
}

TEST("slack write: pins, stars, leave, members, DMs") {
    if (!haveServer())
        return;
    Env e;
    e.be->setPinned(e.general, e.old, true);
    CHECK(e.store.findMessage(e.general, e.old)->pinned);
    CHECK(e.store.findMessage(e.general, e.old)->pinnedBy == e.me);
    script("stars.add", R"([{"ok":false,"error":"channel_not_found"}])");
    e.be->setStarred(e.general, true);
    CHECK(e.store.conversation(e.general).starred);
    e.be->leave(e.dm); // a DM closes
    CHECK(!e.store.conversation(e.dm).member);
    std::vector<model::UserRef> members;
    bool                        gotMembers = false;
    e.be->loadMembers(e.general, [&](std::vector<model::UserRef> m, std::string) {
        members    = std::move(m);
        gotMembers = true;
    });
    ConvRef opened = model::kNoConv - 1;
    e.be->openDm(e.mira, [&](ConvRef c) { opened = c; }); // closed: reopened
    REQUIRE(pumpUntil([&] {
        return gotMembers && opened != model::kNoConv - 1 &&
               !e.store.conversation(e.general).starred;
    }));
    REQUIRE(members.size() == 3);                            // two pages
    CHECK(e.store.conversation(e.general).memberCount == 3); // the header's count
    CHECK(e.store.user(members[2]).id == "U3");
    CHECK(opened == e.store.findConversation("D0NEW")); // the fake's answer
    REQUIRE(opened != model::kNoConv);
    CHECK(e.store.conversation(opened).dmUser == e.mira);
    Log log;
    CHECK(log.get("pins.add")["form"]["timestamp"].str() == model::formatTs(e.old));
    CHECK_STR(log.get("conversations.close")["form"]["channel"].str(), "D1");
    CHECK_STR(log.get("conversations.members", 1)["form"]["cursor"].str(), "page2");
    CHECK_STR(log.get("conversations.open")["form"]["users"].str(), "UMIRA");
    // A refusal reaches the error banner as is.
    std::string banner;
    e.be->onError = [&](const std::string &m) { banner = m; };
    script("conversations.open", R"([{"ok":false,"error":"cannot_dm_bot"}])");
    const model::UserRef three = e.store.findUser("U3");
    REQUIRE(three != model::kNoUser);
    opened = model::kNoConv - 1;
    e.be->openDm(three, [&](ConvRef c) { opened = c; });
    REQUIRE(pumpUntil([&] { return opened != model::kNoConv - 1; }));
    CHECK(opened == model::kNoConv);
    CHECK_STR(banner, "cannot_dm_bot");
    e.be->onError = nullptr;
}

TEST("slack write: status, profile and photo") {
    if (!haveServer())
        return;
    Env    e;
    Result st;
    e.be->setStatus("palm_tree", "Away", 1900000000, st.cb());
    REQUIRE(pumpUntil([&] { return st.called; }));
    CHECK(st.ok);
    CHECK(e.store.user(e.me).statusEmoji == "palm_tree" && e.store.user(e.me).statusText == "Away");
    model::Backend::MyProfile p;
    bool                      loaded = false;
    e.be->loadMyProfile([&](model::Backend::MyProfile x) {
        p      = std::move(x);
        loaded = true;
    });
    REQUIRE(pumpUntil([&] { return loaded; }));
    CHECK(p.displayName == "Me" && p.email == "me@example.com" && p.phone == "123");
    CHECK_STR(p.avatar, "https://img/192.png");
    Result up;
    e.be->updateProfile("New me", "me@example.com", "123", up.cb()); // only the name changed
    REQUIRE(pumpUntil([&] { return up.called; }));
    CHECK(up.ok && e.store.user(e.me).displayName == "New me");
    const std::string photo = file::join(base::env("HOME"), "me.png");
    REQUIRE(file::writeAtomic(photo, "not really a png"));
    Result ph;
    e.be->setPhoto(photo, ph.cb());
    REQUIRE(pumpUntil([&] { return ph.called; }));
    CHECK(ph.ok);
    CHECK_STR(e.store.user(e.me).avatar, "https://img/new512.png");
    Log log;
    CHECK_STR(
        log.get("users.profile.set")["form"]["profile"].str(),
        R"({"status_text":"Away","status_emoji":":palm_tree:","status_expiration":1900000000})"
    );
    CHECK_STR(
        log.get("users.profile.set", 1)["form"]["profile"].str(), R"({"display_name":"New me"})"
    );
    json::Value set = log.get("users.setPhoto");
    CHECK_STR(set["form"]["filename"].str(), "me.png");
    CHECK_STR(set["form"]["field"].str(), "image");
    CHECK(set["ctype"].str().substr(0, 19) == "multipart/form-data");
    CHECK_STR(set["auth"].str(), "Bearer xoxc-test");
    CHECK(set["len"].integer() > int64_t(std::string_view("not really a png").size()));
    // A failure also reaches the banner, with the re-auth hint.
    std::vector<std::string> errors;
    e.be->onError = [&](const std::string &m) { errors.push_back(m); };
    script("users.profile.set", R"([{"ok":false,"error":"missing_scope"}])");
    Result bad;
    e.be->updateProfile("Newer me", "me@example.com", "123", bad.cb());
    REQUIRE(pumpUntil([&] { return bad.called; }));
    CHECK(!bad.ok);
    REQUIRE(errors.size() == 1);
    CHECK_STR(
        errors[0],
        "Could not update profile: missing_scope \xE2\x80\x94 sign in to this workspace again "
        "to grant the new permission"
    );
}

TEST("slack write: presence — away or auto, then the re-polled snapshot answers") {
    if (!haveServer())
        return;
    Env e;
    script(
        "users.getPresence",
        R"([{"ok":true,"presence":"away","online":false,"manual_away":true},
            {"ok":true,"presence":"away","online":false,"manual_away":false}])"
    );
    Result away;
    e.be->setPresence(true, away.cb());
    REQUIRE(pumpUntil([&] { return away.called; }));
    CHECK(away.ok);
    // Answered only once the snapshot is in, so the footer settles on it.
    CHECK(e.be->selfPresence().loaded && e.be->selfPresence().manualAway);
    CHECK_FALSE(e.store.user(e.me).active);
    Result automatic;
    e.be->setPresence(false, automatic.cb());
    REQUIRE(pumpUntil([&] { return automatic.called; }));
    CHECK(automatic.ok && !e.be->selfPresence().manualAway);
    CHECK(e.be->selfPresence().phantomAway()); // no official client holds a socket
    Log log;
    CHECK_STR(
        log.order(), "users.setPresence,users.getPresence,users.setPresence,users.getPresence"
    );
    CHECK_STR(log.get("users.setPresence")["form"]["presence"].str(), "away");
    CHECK_STR(log.get("users.setPresence", 1)["form"]["presence"].str(), "auto");
    // A rejection says why and polls nothing.
    script("users.setPresence", R"([{"ok":false,"error":"missing_scope"}])");
    Result no;
    e.be->setPresence(true, no.cb());
    REQUIRE(pumpUntil([&] { return no.called; }));
    CHECK(!no.ok && no.err == "missing_scope");
    CHECK(Log().count("users.getPresence") == 2);
}

TEST("slack write: canvases — look-up, meta, content, create, edit one change per call, delete") {
    if (!haveServer())
        return;
    Env e;
    using Change = model::Backend::CanvasChange;
    script("conversations.info", R"([{"ok":true,"channel":{"id":"C1","name":"general",
        "properties":{"canvas":{"file_id":"FC1"}}}}])");
    std::string id = "-";
    e.be->loadChannelCanvas(e.general, [&](std::string f) { id = f; });
    REQUIRE(pumpUntil([&] { return id != "-"; }));
    CHECK_STR(id, "FC1");
    CHECK_STR(e.store.conversation(e.general).canvasId, "FC1");

    script("files.info", R"([{"ok":true,"file":{"title":"Plan","permalink":"https://x/FC1"}}])");
    std::string title, link;
    bool        meta = false;
    e.be->loadCanvasMeta("FC1", [&](std::string t, std::string l, model::Backend::CanvasState) {
        title = t, link = l, meta = true;
    });
    REQUIRE(pumpUntil([&] { return meta; }));
    CHECK_STR(title, "Plan");
    CHECK_STR(link, "https://x/FC1");
    CHECK_STR(e.store.conversation(e.general).canvasTitle, "Plan");

    // The content: files.info's url_private, fetched with the workspace's auth.
    script(
        "files.info",
        (R"([{"ok":true,"file":{"url_private":")" + server() + R"(/files-pri/T1-FC1/canvas"}}])")
            .c_str()
    );
    std::string html, herr = "-";
    e.be->loadCanvasContent("FC1", [&](std::string h, std::string err) { html = h, herr = err; });
    REQUIRE(pumpUntil([&] { return herr != "-"; }));
    CHECK_STR(herr, "");
    CHECK(html.find("<p id=\"a\">Hello</p>") != std::string::npos);
    CHECK_STR(Log().get("download")["auth"].str(), "Bearer xoxc-test");

    // A rename then a whole-document replace: two canvases.edit calls.
    Result r;
    e.be->editCanvas(
        "FC1", {{Change::Op::Rename, "Roadmap"}, {Change::Op::ReplaceAll, "# Hi"}}, r.cb()
    );
    REQUIRE(pumpUntil([&] { return r.called; }));
    CHECK(r.ok);
    {
        Log log;
        CHECK(log.count("canvases.edit") == 2);
        json::Document a, b;
        REQUIRE(
            a.parse(std::string(log.get("canvases.edit", 0)["form"]["changes"].str()), nullptr)
        );
        REQUIRE(
            b.parse(std::string(log.get("canvases.edit", 1)["form"]["changes"].str()), nullptr)
        );
        CHECK_STR(a.root()[0]["operation"].str(), "rename");
        CHECK_STR(a.root()[0]["title_content"]["markdown"].str(), "Roadmap");
        CHECK_STR(b.root()[0]["operation"].str(), "replace");
        CHECK_STR(b.root()[0]["document_content"]["markdown"].str(), "# Hi");
        CHECK_STR(log.get("canvases.edit")["form"]["canvas_id"].str(), "FC1");
    }
    CHECK_STR(e.store.conversation(e.general).canvasTitle, "Roadmap");

    Result del;
    e.be->deleteCanvas("FC1", del.cb());
    REQUIRE(pumpUntil([&] { return del.called; }));
    CHECK(del.ok);
    CHECK(e.store.conversation(e.general).canvasId.empty());
    CHECK(e.store.conversation(e.general).canvasTitle.empty());

    // Created with its markdown; the Store names it until the rename.
    script("conversations.canvases.create", R"([{"ok":true,"canvas_id":"FC2"}])");
    std::string created, cerr = "-";
    e.be->createChannelCanvas(e.general, "Body", [&](std::string f, std::string err) {
        created = f, cerr = err;
    });
    REQUIRE(pumpUntil([&] { return cerr != "-"; }));
    CHECK_STR(created, "FC2");
    CHECK_STR(e.store.conversation(e.general).canvasId, "FC2");
    json::Document dc;
    REQUIRE(dc.parse(
        std::string(Log().get("conversations.canvases.create")["form"]["document_content"].str()),
        nullptr
    ));
    CHECK_STR(dc.root()["markdown"].str(), "Body");
    CHECK_STR(Log().get("conversations.canvases.create")["form"]["channel_id"].str(), "C1");
}

TEST("slack write: downloadFile fetches with the workspace's credentials, writes off-thread") {
    if (!haveServer())
        return;
    const char       *t = std::getenv("TMPDIR");
    const std::string dir =
        file::join(t && *t ? t : "/tmp", "msga-test-download-" + std::to_string(getpid()));
    REQUIRE(file::makeDirs(dir));
    const std::string to = file::join(dir, "photo.bin");
    {
        Env    e;
        Result r;
        e.be->downloadFile(server() + "/files-pri/T1-F1/photo.bin", to, r.cb());
        CHECK_FALSE(r.called); // never inside the call
        REQUIRE(pumpUntil([&] { return r.called; }));
        CHECK(r.ok);
        std::string got;
        REQUIRE(file::readAll(to, &got));
        REQUIRE(got.size() == 1024);
        for (size_t i = 0; i < got.size(); ++i)
            if (uint8_t(got[i]) != i % 256) {
                CHECK(false);
                break;
            }
        Log log;
        CHECK_STR(log.get("download")["auth"].str(), "Bearer xoxc-test");
        CHECK(log.get("download")["cookie"].str().find("d=xoxd-test") != std::string_view::npos);
        // An error status: a failure, nothing written.
        Result            miss;
        const std::string none = file::join(dir, "none.bin");
        e.be->downloadFile(server() + "/files-pri/T1-F2/missing", none, miss.cb());
        REQUIRE(pumpUntil([&] { return miss.called; }));
        CHECK_FALSE(miss.ok);
        CHECK_STR(miss.err, "http 404");
        CHECK_FALSE(file::exists(none));
        // The backend gone before the answer: done still runs (the caller's
        // background job ends), as a failure.
        Result gone;
        e.be->downloadFile(
            server() + "/files-pri/T1-F3/late.bin", file::join(dir, "late.bin"), gone.cb()
        );
        e.be.reset();
        REQUIRE(pumpUntil([&] { return gone.called; }));
        CHECK_FALSE(gone.ok);
        CHECK_STR(gone.err, "cancelled");
    }
    file::remove(to);
    file::remove(file::join(dir, "late.bin"));
    file::remove(dir);
}

#endif // !_WIN32

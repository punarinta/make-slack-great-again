// SlackBackend's Slack-only features against the local fake Slack
// (tests/support/fake_slack.py): huddle rooms, slash commands (msga's built-ins,
// commands.list, chat.command), DND snooze, bot buttons (blocks.actions),
// the account's Slack theme, message reminders going off, the Threads
// entry's unread state, the dead-conversation cache, the rate-limit and
// send-failure banners, mentioned channels and user groups, the app-keys
// OAuth sign-in. Nothing here talks to the real Slack.
#include "app/cache/workspace_cache.h"
#include "app/slack/oauth.h"
#include "app/slack/slack_backend.h"
#include "app/slack/slack_json.h"
#include "support/fake_slack_server.h"
#include "base/json.h"
#include "base/str.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/testing.h"

#include <algorithm>
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
using model::kNoConv;
using model::Ts;

void set(const std::string &json) {
    ctl("POST", "/_ctl/set", json);
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

int count(std::string_view method) {
    return Log().count(method);
}

const char *kWorkspace = R"({
 "auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
               "url": "https://lumen.slack.com/"},
 "users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
     {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira Okafor"}}],
   "response_metadata": {"next_cursor": ""}},
 "conversations.list": {"ok": true, "channels": [
     {"id": "C1", "name": "general", "is_channel": true, "is_member": true},
     {"id": "D1", "is_im": true, "user": "UMIRA"}],
   "response_metadata": {"next_cursor": ""}},
 "stars.list": {"ok": true, "items": []},
 "emoji.list": {"ok": true, "emoji": {}},
 "usergroups.list": {"ok": true, "usergroups": [
     {"id": "S1", "handle": "design", "name": "Design team", "users": ["UME", "UMIRA"]},
     {"id": "S2", "handle": "ops", "name": "Ops", "users": ["UMIRA"]}]},
 "users.getPresence": {"ok": true, "presence": "active", "online": true},
 "client.counts": {"ok": true, "channels": [], "ims": [], "mpims": []},
 "saved.list": {"ok": true, "saved_items": []},
 "subscriptions.thread.getView": {"ok": true, "threads": [], "total_unread_replies": 0},
 "commands.list": {"ok": true, "commands": [
     {"name": "/deploy", "desc": "Ship it", "usage": "[env]", "type": "app", "app_name": "Shipper"},
     {"name": "/shrug", "desc": "Server shrug", "type": "core"}]},
 "conversations.history": {"ok": true, "messages": [], "has_more": false}
})";

struct Env {
    model::Store                         store;
    net::Client                          client{app()};
    std::unique_ptr<slack::SlackBackend> be;
    std::vector<std::string>             errors;
    std::vector<std::pair<ConvRef, Ts>>  due;

    explicit Env(const std::string &extra = {}, bool session = true, bool reset = true) {
        base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "100");
        if (reset) {
            ctl("POST", "/_ctl/reset");
            set(kWorkspace);
            if (!extra.empty())
                set(extra);
        }
        slack::Credentials cr;
        cr.token          = session ? "xoxc-test" : "xoxp-test";
        cr.cookie         = session ? "xoxd-test" : "";
        cr.teamId         = "T1";
        be                = std::make_unique<slack::SlackBackend>(store, app(), client, cr);
        be->onError       = [this](const std::string &m) { errors.push_back(m); };
        be->onReminderDue = [this](ConvRef c, Ts ts) { due.emplace_back(c, ts); };
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
    bool history(ConvRef c) {
        bool called = false;
        be->loadHistory(c, 0, [&](bool, const std::string &) { called = true; });
        return pumpUntil([&] { return called; });
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

TEST("slack features: huddle rooms — mappers, the first history page, roster reloads") {
    model::Store   st;
    json::Document d;
    // A live room: its link and participants; nobody yet = its host.
    REQUIRE(d.parse(
        std::string_view(R"({"call_family": "huddle", "huddle_link": "https://h/1",
        "participants": [], "created_by": "UHOST", "date_start": 100, "date_end": 0})"),
        nullptr
    ));
    slack::mapjson::HuddleRoom h;
    REQUIRE(slack::mapjson::toHuddleRoom(d.root(), st, h));
    CHECK(h.active);
    CHECK_STR(h.link, "https://h/1");
    REQUIRE(h.participants.size() == 1);
    CHECK_STR(st.user(h.participants[0]).id, "UHOST");
    // date_end as a string still ends it; a call that is no huddle is nothing.
    REQUIRE(
        d.parse(std::string_view(R"({"call_family": "huddle", "date_end": "1758445200"})"), nullptr)
    );
    REQUIRE(slack::mapjson::toHuddleRoom(d.root(), st, h));
    CHECK(!h.active);
    REQUIRE(d.parse(std::string_view(R"({"call_family": "call"})"), nullptr));
    CHECK(!slack::mapjson::toHuddleRoom(d.root(), st, h));
    // A huddle_thread message: no Slackbot author, the frozen block gone,
    // the summary kept (participant_history once ended).
    REQUIRE(d.parse(
        std::string_view(R"({"type": "message", "subtype": "huddle_thread", "user": "USLACKBOT",
        "ts": "1758445000.000100", "text": "", "blocks": [{"type": "rich_text", "elements": []}],
        "room": {"call_family": "huddle", "date_start": 1758445000, "date_end": "1758448900",
                 "has_ended": true, "participants": [],
                 "participant_history": ["UME", "UMIRA"]}})"),
        nullptr
    ));
    const model::Message m = slack::mapjson::toMessage(d.root(), st);
    REQUIRE(m.isHuddle());
    CHECK(m.user == model::kNoUser);
    CHECK(m.extra->blocks.empty());
    CHECK(m.extra->huddle.ended);
    CHECK(m.extra->huddle.attendees.size() == 2);
    CHECK(m.extra->huddle.endSec - m.extra->huddle.startSec == 3900);
    CHECK_STR(m.text, "A huddle happened");

    if (!haveServer())
        return;
    // App keys without a socket: the roster reloads every minute there.
    Env e(
        R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
        {"type": "message", "subtype": "huddle_thread", "ts": "1800000000.000100", "text": "",
         "room": {"call_family": "huddle", "huddle_link": "https://app.slack.com/huddle/T1/C1",
                  "participants": ["UMIRA"], "date_start": 1800000000, "date_end": 0}}]}})",
        /*session=*/false
    );
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(e.history(c));
    const model::Conversation &x = e.store.conversation(c);
    CHECK(x.huddleActive);
    CHECK_STR(x.huddleLink, "https://app.slack.com/huddle/T1/C1");
    REQUIRE(x.huddleParticipants.size() == 1);
    CHECK_STR(e.store.user(x.huddleParticipants[0]).id, "UMIRA");
    // conversations.list carries no room: a reload keeps the live huddle.
    REQUIRE(pumpUntil([&] { return count("conversations.list") >= 2; }, 5000));
    pumpFor(50);
    CHECK(e.store.conversation(c).huddleActive);
}

TEST("slack features: slash commands — the list, built-ins, chat.command, DND") {
    if (!haveServer())
        return;
    Env e(R"({"chat.command?command=/broken": {"ok": false, "error": "no_such_thing"}})");
    REQUIRE(e.connect());
    REQUIRE(e.be->capabilities().slashCommands);
    const ConvRef general = e.conv("C1");
    // commands.list first, then the built-ins it doesn't have.
    REQUIRE(pumpUntil([&] { return e.be->commands(general).front().name == "deploy"; }));
    std::vector<std::string> names;
    for (const auto &c : e.be->commands(general)) {
        names.push_back(c.name);
        CHECK(c.local); // all run here, never posted as text
    }
    REQUIRE(names.size() >= 10);
    CHECK_STR(names[0], "deploy");
    CHECK_STR(names[1], "shrug");
    CHECK(std::count(names.begin(), names.end(), "shrug") == 1);
    CHECK(std::count(names.begin(), names.end(), "dnd") == 1);
    CHECK_STR(e.be->commands(general)[0].source, "App \xC2\xB7 Shipper");
    CHECK(e.be->commands(general)[0].app);

    // /shrug posts the shrug after the text.
    CHECK(e.be->runLocalCommand(general, 0, "shrug", "well").error.empty());
    REQUIRE(pumpUntil([&] { return count("chat.postMessage") == 1; }));
    CHECK_STR(
        std::string(Log().get("chat.postMessage")["form"]["text"].str()),
        "well \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF"
    );
    // A workspace command: chat.command with its text; a failure reaches the banner.
    e.be->runLocalCommand(general, 0, "deploy", "prod");
    REQUIRE(pumpUntil([&] { return count("chat.command") == 1; }));
    const Log   log;
    json::Value f = log.get("chat.command")["form"];
    CHECK_STR(std::string(f["command"].str()), "/deploy");
    CHECK_STR(std::string(f["text"].str()), "prod");
    CHECK_STR(std::string(f["channel"].str()), "C1");
    e.be->runLocalCommand(general, 0, "broken", "");
    REQUIRE(pumpUntil([&] { return !e.errors.empty(); }));
    CHECK_STR(e.errors.back(), "Command /broken failed: no_such_thing");
    // /dnd: minutes in the accepted forms, "off" ends it; my User follows.
    e.be->runLocalCommand(general, 0, "dnd", "1h 30m");
    REQUIRE(pumpUntil([&] { return count("dnd.setSnooze") == 1; }));
    CHECK_STR(std::string(Log().get("dnd.setSnooze")["form"]["num_minutes"].str()), "90");
    REQUIRE(pumpUntil([&] { return e.store.user(e.store.me).dnd; }));
    e.be->runLocalCommand(general, 0, "dnd", "off");
    REQUIRE(pumpUntil([&] { return count("dnd.endSnooze") == 1; }));
    REQUIRE(pumpUntil([&] { return !e.store.user(e.store.me).dnd; }));
    e.be->runLocalCommand(general, 0, "dnd", "45");
    REQUIRE(pumpUntil([&] { return count("dnd.setSnooze") == 2; }));
    CHECK_STR(std::string(Log().get("dnd.setSnooze", 1)["form"]["num_minutes"].str()), "45");
    // A bare /dnd and nonsense answer the usage at once.
    CHECK(e.be->runLocalCommand(general, 0, "dnd", "").error.rfind("Usage: /dnd", 0) == 0);
    CHECK(!e.be->runLocalCommand(general, 0, "dnd", "soon").error.empty());
    // /status with an emoji; /msg to someone: the DM opens, the rest goes there.
    e.be->runLocalCommand(general, 0, "status", ":tada: shipping");
    REQUIRE(pumpUntil([&] { return count("users.profile.set") == 1; }));
    const std::string profile(Log().get("users.profile.set")["form"]["profile"].str());
    CHECK(profile.find("\":tada:\"") != std::string::npos);
    CHECK(profile.find("\"shipping\"") != std::string::npos);
    CHECK_STR(
        e.be->runLocalCommand(general, 0, "msg", "@nobody hi").error, "No such user: @nobody"
    );
    e.be->runLocalCommand(general, 0, "msg", "@mira hello there");
    REQUIRE(pumpUntil([&] { return count("chat.postMessage") == 2; }));
    CHECK_STR(std::string(Log().get("chat.postMessage", 1)["form"]["channel"].str()), "D1");
    CHECK_STR(std::string(Log().get("chat.postMessage", 1)["form"]["text"].str()), "hello there");
    // /mute toggles the conversation (a local mute).
    e.be->runLocalCommand(general, 0, "mute", "");
    CHECK(e.store.conversation(general).muted);
    // In a thread: /shrug replies there, chat.command carries thread_ts
    // (the channel's ones above carry none).
    CHECK(Log().get("chat.command")["form"]["thread_ts"].str().empty());
    CHECK(Log().get("chat.postMessage")["form"]["thread_ts"].str().empty());
    const model::Ts root = 1800000000000100;
    e.be->runLocalCommand(general, root, "shrug", "");
    REQUIRE(pumpUntil([&] { return count("chat.postMessage") == 3; }));
    CHECK_STR(
        std::string(Log().get("chat.postMessage", 2)["form"]["thread_ts"].str()),
        "1800000000.000100"
    );
    e.be->runLocalCommand(general, root, "deploy", "staging");
    REQUIRE(pumpUntil([&] { return count("chat.command") == 3; }));
    const Log later; // f views into its document: it must outlive f's uses
    f = later.get("chat.command", 2)["form"];
    CHECK_STR(std::string(f["thread_ts"].str()), "1800000000.000100");
    CHECK_STR(std::string(f["channel"].str()), "C1");
}

TEST("slack features: bot buttons press through blocks.actions; legacy ones can't") {
    if (!haveServer())
        return;
    Env e(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
        {"type": "message", "ts": "1800000000.000100", "bot_id": "B42", "user": "UBOT",
         "text": "Approve?", "blocks": [
            {"type": "section", "block_id": "q", "text": {"type": "mrkdwn", "text": "Approve?"},
             "accessory": {"type": "button", "action_id": "more", "url": "https://x/more",
                           "text": {"type": "plain_text", "text": "More"}}},
            {"type": "actions", "block_id": "act1", "elements": [
                {"type": "button", "action_id": "yes", "value": "v1", "style": "primary",
                 "text": {"type": "plain_text", "text": "Yes"}},
                {"type": "static_select", "action_id": "pick"}]}],
         "attachments": [{"fallback": "old", "actions": [
            {"type": "button", "name": "legacy", "text": "Legacy"}]}]}]}})");
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(e.history(c));
    const Ts              ts = model::parseTs("1800000000.000100");
    const model::Message *m  = e.store.findMessage(c, ts);
    REQUIRE(m && m->extra);
    REQUIRE(m->extra->buttons.size() == 3);
    CHECK_STR(m->extra->buttons[0].url, "https://x/more");
    CHECK_STR(m->extra->buttons[1].id, "yes");
    CHECK(m->extra->buttons[1].style == model::Button::Style::Primary);
    CHECK(m->extra->buttons[2].id.empty()); // legacy
    // The legacy one is drawn in its attachment, which shows it, not the
    // fallback.
    CHECK(m->extra->buttons[1].attachment == 0);
    CHECK(m->extra->buttons[2].attachment == 1);
    REQUIRE(m->attachments().size() == 1);
    CHECK(m->attachments()[0].text.empty());
    CHECK_STR(m->extra->botId, "B42");

    bool        called = false, ok = false;
    std::string err;
    e.be->pressButton(c, ts, "yes", [&](bool o, const std::string &x) {
        called = true;
        ok     = o;
        err    = x;
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(ok);
    const Log   log;
    json::Value f = log.get("blocks.actions")["form"];
    CHECK_STR(std::string(f["service_id"].str()), "B42");
    const std::string actions(f["actions"].str()), container(f["container"].str());
    CHECK(actions.find("\"action_id\":\"yes\"") != std::string::npos);
    CHECK(actions.find("\"block_id\":\"act1\"") != std::string::npos);
    CHECK(actions.find("\"value\":\"v1\"") != std::string::npos);
    CHECK(actions.find("\"style\":\"primary\"") != std::string::npos);
    CHECK(container.find("\"channel_id\":\"C1\"") != std::string::npos);
    CHECK(container.find("\"message_ts\":\"1800000000.000100\"") != std::string::npos);
    // A legacy button: Slack doesn't let us.
    called = false;
    e.be->pressButton(c, ts, "", [&](bool o, const std::string &x) {
        called = true;
        ok     = o;
        err    = x;
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(!ok);
    CHECK_STR(err, model::Backend::kUnpressableButton);
    CHECK(count("blocks.actions") == 1);
}

TEST("slack features: the account's Slack theme from users.prefs.get") {
    if (!haveServer())
        return;
    Env e(
        R"({"users.prefs.get": {"ok": true, "prefs": {
        "ia_theme": "{\"primary\":{\"palette\":\"aubergine\"}}",
        "sidebar_theme_custom_values": "{\"column_bg\":\"#111111\",\"menu_bg\":\"#222222\",\"active_item\":\"#333333\",\"active_item_text\":\"#444444\",\"hover_item\":\"#555555\",\"text_color\":\"#666666\",\"active_presence\":\"#777777\",\"badge\":\"#888888\"}"}}})"
    );
    REQUIRE(e.connect());
    CHECK(e.be->capabilities().sidebarTheme);
    bool                         called = false;
    model::Backend::SidebarTheme t;
    std::string                  err;
    e.be->loadSidebarTheme([&](model::Backend::SidebarTheme x, std::string er) {
        called = true;
        t      = std::move(x);
        err    = std::move(er);
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(err.empty());
    CHECK_STR(t.iaTheme, "{\"primary\":{\"palette\":\"aubergine\"}}");
    CHECK_STR(t.legacyValues, "#111111,#222222,#333333,#444444,#555555,#666666,#777777,#888888");
    // OAuth tokens can't read it.
    Env o({}, false);
    CHECK(!o.be->capabilities().sidebarTheme);
}

TEST("slack features: a due reminder goes off once, with its preview; a week late is quiet") {
    if (!haveServer())
        return;
    const int64_t now = base::nowSecs();
    Env           e(
        str::concat(
            {R"({"saved.list": {"ok": true, "saved_items": [
        {"item_type": "message", "item_id": "C1", "ts": "1800000000.000100", "date_due": )",
             str::number(now - 1),
             R"(, "date_created": 1},
        {"item_type": "message", "item_id": "C1", "ts": "1800000000.000200", "date_due": )",
             str::number(now - 8 * 86400),
             R"(, "date_created": 1}]},
      "conversations.replies?ts=1800000000.000100": {"ok": true, "messages": [
        {"type": "message", "ts": "1800000000.000100", "user": "UMIRA", "text": "the report"}]}})"}
        )
    );
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(pumpUntil([&] { return !e.due.empty(); }, 5000));
    CHECK(e.due.size() == 1);
    CHECK(e.due[0].first == c && e.due[0].second == model::parseTs("1800000000.000100"));
    const model::Store::SavedItem *it = e.store.findSaved(c, e.due[0].second);
    REQUIRE(it);
    CHECK(it->fired);
    CHECK(it->previewed);
    CHECK_STR(it->text, "the report");
    // The week-late one is marked fired too, silently; nothing goes off again.
    const model::Store::SavedItem *late = e.store.findSaved(c, model::parseTs("1800000000.000200"));
    REQUIRE(late);
    CHECK(late->fired);
    pumpFor(100);
    CHECK(e.due.size() == 1);
}

TEST("slack features: the Threads entry — unread from the feed, read by markThreadRead") {
    if (!haveServer())
        return;
    Env e(R"({"subscriptions.thread.getView": {"ok": true, "total_unread_replies": 1, "threads": [
        {"root_msg": {"channel": "C1", "ts": "1800000000.000100", "user": "UME", "text": "root",
                      "last_read": "1800000000.000100"},
         "unread_replies": [{"ts": "1800000001.000100", "user": "UMIRA", "text": "reply",
                             "thread_ts": "1800000000.000100"}]}]}})");
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return e.store.unreadThreads() == 1; }, 5000));
    // Reading the thread up to that reply clears it.
    e.be->markThreadRead(
        e.conv("C1"), model::parseTs("1800000000.000100"), model::parseTs("1800000001.000100")
    );
    CHECK(e.store.unreadThreads() == 0);
    // The server's "all clear" clears whatever is left.
    set(
        R"({"subscriptions.thread.getView": {"ok": true, "total_unread_replies": 0, "threads": []}})"
    );
    e.store.setUnreadThreads(0);
    pumpFor(300);
    CHECK(e.store.unreadThreads() == 0);
}

TEST("slack features: on Grid the thread feed refused without team_id goes again with it") {
    if (!haveServer())
        return;
    // Issue #118: a Grid session token may resolve in the org.
    Env e(R"({"auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
                            "enterprise_id": "E1", "url": "https://lumen.slack.com/"},
              "subscriptions.thread.getView": {"ok": false, "error": "enterprise_is_restricted"},
              "subscriptions.thread.getView?team_id=T1": {"ok": true, "total_unread_replies": 0,
                "threads": [{"root_msg": {"channel": "C1", "ts": "1800000000.000100",
                                          "user": "UME", "text": "root"},
                             "latest_replies": []}]}})");
    REQUIRE(e.connect());
    // The poll's refusal and the page's may cross: neither gives up on it.
    for (int i = 0; i < 2; ++i) {
        bool                        called = false, ok = false;
        model::Backend::ThreadsView page;
        e.be->loadThreadsView({}, [&](bool o, model::Backend::ThreadsView p) {
            called = true;
            ok     = o;
            page   = std::move(p);
        });
        REQUIRE(pumpUntil([&] { return called; }, 5000));
        CHECK(ok);
        CHECK(page.threads.size() == 1);
        pumpFor(300); // a poll tick in between
    }
    // The read cursor goes with team_id too.
    e.be->markThreadRead(
        e.conv("C1"), model::parseTs("1800000000.000100"), model::parseTs("1800000000.000100")
    );
    REQUIRE(pumpUntil([&] { return count("subscriptions.thread.mark") == 1; }, 3000));
    CHECK_STR(Log().get("subscriptions.thread.mark")["form"]["team_id"].str(), "T1");
}

TEST("slack features: a thread feed refused in both forms reports Slack's code") {
    if (!haveServer())
        return;
    Env e(R"({"subscriptions.thread.getView": {"ok": false, "error": "not_allowed_token_type"},
              "subscriptions.thread.getTimestamps": {"ok": false, "error": "unknown_method"}})");
    REQUIRE(e.connect());
    for (int i = 0; i < 2; ++i) { // the second answers from the latch
        bool                        called = false, ok = true;
        model::Backend::ThreadsView page;
        e.be->loadThreadsView({}, [&](bool o, model::Backend::ThreadsView p) {
            called = true;
            ok     = o;
            page   = std::move(p);
        });
        REQUIRE(pumpUntil([&] { return called; }, 5000));
        CHECK(!ok);
        CHECK_STR(page.error, "unknown_method");
    }
    // Off Grid neither goes with team_id.
    const Log log;
    for (const char *m : {"subscriptions.thread.getView", "subscriptions.thread.getTimestamps"})
        for (int i = 0; i < log.count(m); ++i)
            CHECK(!log.get(m, i)["form"].has("team_id"));
}

// Issue #118: where getView is refused, the web client's getTimestamps
// lists the threads and conversations.replies fills each card.
TEST("slack features: the Threads page falls back to getTimestamps where getView is refused") {
    if (!haveServer())
        return;
    Env e(R"({"subscriptions.thread.getView": {"ok": false, "error": "unknown_method"},
      "subscriptions.thread.getTimestamps": {"ok": true, "subscriptions": [
        {"latest": "1800000002.000100", "last_read": "1800000001.000100", "channel_id": "C1",
         "date_created": 1800000000, "timestamp": "1800000000.000100", "priority": false},
        {"latest": "1800000101.000100", "last_read": "1800000101.000100", "channel_id": "C1",
         "date_created": 1800000100, "timestamp": "1800000100.000100", "priority": false},
        {"latest": "1800000201.000100", "last_read": "1800000200.000100", "channel_id": "CGONE",
         "date_created": 1800000200, "timestamp": "1800000200.000100", "priority": false}],
        "response_metadata": {"next_cursor": "bmV4dA=="}},
      "conversations.replies?ts=1800000000.000100": {"ok": true, "messages": [
        {"ts": "1800000000.000100", "user": "UME", "text": "root one", "reply_count": 2,
         "latest_reply": "1800000002.000100", "thread_ts": "1800000000.000100"},
        {"ts": "1800000002.000100", "user": "UMIRA", "text": "unread reply",
         "thread_ts": "1800000000.000100"}]},
      "conversations.replies?ts=1800000100.000100": [
        {"ok": true, "messages": [{"ts": "1800000101.000100", "user": "UMIRA",
                                   "text": "last reply", "thread_ts": "1800000100.000100"}]},
        {"ok": true, "messages": [{"ts": "1800000100.000100", "user": "UME", "text": "root two",
                                   "reply_count": 1, "thread_ts": "1800000100.000100"}]}]})");
    REQUIRE(e.connect());
    bool                        called = false, ok = false;
    model::Backend::ThreadsView page;
    e.be->loadThreadsView({}, [&](bool o, model::Backend::ThreadsView p) {
        called = true;
        ok     = o;
        page   = std::move(p);
    });
    REQUIRE(pumpUntil([&] { return called; }, 5000));
    REQUIRE(ok);
    // In the feed's order; the thread of a conversation we don't have is left out.
    REQUIRE(page.threads.size() == 2);
    CHECK_STR(page.threads[0].root.text, "root one");
    CHECK(page.threads[0].lastRead == model::parseTs("1800000001.000100"));
    REQUIRE(page.threads[0].latestReplies.size() == 1);
    CHECK_STR(page.threads[0].latestReplies[0].text, "unread reply");
    // A root the answer left out is asked for alone.
    CHECK_STR(page.threads[1].root.text, "root two");
    REQUIRE(page.threads[1].latestReplies.size() == 1);
    CHECK_STR(page.threads[1].latestReplies[0].text, "last reply");
    CHECK(page.hasMore);
    CHECK_STR(page.nextCursor, "bmV4dA==");
    // Unread: the replies past my read cursor; read: the newest one.
    const Log log;
    for (int i = 0; i < log.count("conversations.replies"); ++i) {
        const json::Value f = log.get("conversations.replies", i)["form"];
        if (f["ts"].str() == "1800000000.000100") {
            CHECK_STR(f["oldest"].str(), "1800000001.000100");
            CHECK_STR(f["inclusive"].str(), "0");
        } else if (f["ts"].str() == "1800000100.000100" && f.has("oldest")) {
            CHECK_STR(f["oldest"].str(), "1800000101.000100");
            CHECK_STR(f["inclusive"].str(), "1");
        }
    }
    CHECK_STR(
        log.get("subscriptions.thread.getTimestamps")["form"]["mode"].str(), "all_subscriptions"
    );
    // From now on the page goes straight to getTimestamps, the cursor with it.
    const int views = count("subscriptions.thread.getView");
    called          = false;
    e.be->loadThreadsView(page.nextCursor, [&](bool o, model::Backend::ThreadsView) {
        called = true;
        ok     = o;
    });
    REQUIRE(pumpUntil([&] { return called; }, 5000));
    CHECK(ok);
    CHECK(count("subscriptions.thread.getView") == views);
    bool      sawCursor = false;
    const Log after;
    for (int i = 0; i < after.count("subscriptions.thread.getTimestamps"); ++i)
        sawCursor |= after.get("subscriptions.thread.getTimestamps", i)["form"]["cursor"].str() ==
                     "bmV4dA==";
    CHECK(sawCursor);
}

TEST("slack features: a followed thread's reply notifies through the getTimestamps feed") {
    if (!haveServer())
        return;
    // The first round primes; the second moves the thread's latest, and only
    // then are its replies asked for.
    Env e(R"({"subscriptions.thread.getView": {"ok": false, "error": "unknown_method"},
      "subscriptions.thread.getTimestamps": [
        {"ok": true, "subscriptions": [
          {"latest": "1800000001.000100", "last_read": "1800000001.000100", "channel_id": "C1",
           "timestamp": "1800000000.000100"}]},
        {"ok": true, "subscriptions": [
          {"latest": "1800000002.000100", "last_read": "1800000001.000100", "channel_id": "C1",
           "timestamp": "1800000000.000100"}]}],
      "conversations.replies?ts=1800000000.000100": {"ok": true, "messages": [
        {"ts": "1800000000.000100", "user": "UME", "text": "root", "thread_ts": "1800000000.000100"},
        {"ts": "1800000002.000100", "user": "UMIRA", "text": "new one",
         "thread_ts": "1800000000.000100"}]}})");
    std::vector<std::string> arrived;
    const auto obs = e.store.observe(model::Store::kAnyConv, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Arrived && e.store.arrived())
            arrived.push_back(e.store.arrived()->text);
    });
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return !arrived.empty(); }, 5000));
    CHECK_STR(arrived[0], "new one");
    pumpFor(500); // more rounds of the same feed bring nothing more
    CHECK(arrived.size() == 1);
    CHECK(e.be->threadFollowed(e.conv("C1"), model::parseTs("1800000000.000100")));
    CHECK(e.store.unreadThreads() == 1);
    CHECK_STR(Log().get("conversations.replies")["form"]["oldest"].str(), "1800000001.000100");
    e.store.unobserve(obs);
}

TEST("slack features: a followed thread's reply notifies even when the thread isn't loaded") {
    if (!haveServer())
        return;
    // The first feed page primes; the second brings a new reply.
    Env e(
        R"({"subscriptions.thread.getView": [
        {"ok": true, "total_unread_replies": 0, "threads": [
          {"root_msg": {"channel": "C1", "ts": "1800000000.000100", "user": "UME", "text": "root"},
           "latest_replies": [{"ts": "1800000001.000100", "user": "UMIRA", "text": "old"}]}]},
        {"ok": true, "total_unread_replies": 1, "threads": [
          {"root_msg": {"channel": "C1", "ts": "1800000000.000100", "user": "UME", "text": "root",
                        "last_read": "1800000001.000100"},
           "latest_replies": [{"ts": "1800000001.000100", "user": "UMIRA", "text": "old"},
                              {"ts": "1800000002.000100", "user": "UMIRA", "text": "new one"}]}]}]})"
    );
    std::vector<std::string> arrived;
    const auto obs = e.store.observe(model::Store::kAnyConv, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Arrived && e.store.arrived())
            arrived.push_back(e.store.arrived()->text);
    });
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return !arrived.empty(); }, 5000));
    CHECK_STR(arrived[0], "new one");
    CHECK(arrived.size() == 1);
    CHECK(e.be->threadFollowed(e.conv("C1"), model::parseTs("1800000000.000100")));
    e.store.unobserve(obs);
}

TEST("slack features: banners — rate limits (throttled) and failed sends in words") {
    if (!haveServer())
        return;
    Env e(R"({"chat.postMessage?channel=C1": {"ok": false, "error": "is_archived"},
              "emoji.list": [{"ok": false, "error": "ratelimited", "__status": 429,
                              "__headers": {"Retry-After": "1"}},
                             {"ok": true, "emoji": {}}]})");
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] {
        for (const std::string &m : e.errors)
            if (m.find("rate-limiting") != std::string::npos)
                return true;
        return false;
    }));
    CHECK(e.errors.back().find("(emoji.list)") != std::string::npos);
    CHECK(e.errors.back().find("1 second") != std::string::npos);
    e.errors.clear();
    e.be->send(e.conv("C1"), "hello", 0, nullptr);
    REQUIRE(pumpUntil([&] { return !e.errors.empty(); }));
    CHECK_STR(e.errors.back(), "Couldn't send message: This conversation is archived.");
    CHECK(e.store.conversation(e.conv("C1")).messages.empty()); // the ghost is gone
}

TEST("slack features: a throttled users.list page waits unseen; earlier pages show at once") {
    if (!haveServer())
        return;
    Env        e(R"({"users.list": {"ok": true, "members": [
                 {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
                 {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira Okafor"}}],
               "response_metadata": {"next_cursor": "p2"}},
              "users.list?cursor=p2": [
                 {"ok": false, "error": "ratelimited", "__status": 429,
                  "__headers": {"Retry-After": "30"}},
                 {"ok": true, "members": [
                    {"id": "UJONAS", "name": "jonas", "profile": {"display_name": "Jonas"}}],
                  "response_metadata": {"next_cursor": ""}}]})");
    const auto named = [&](const char *id) {
        const model::UserRef r = e.store.findUser(id);
        return r != model::kNoUser && !e.store.user(r).placeholder;
    };
    // Page one is in while page two waits out its Retry-After (connect
    // settles only once the whole list is).
    bool settled = false;
    e.be->connect([&](bool, const std::string &) { settled = true; });
    REQUIRE(pumpUntil([&] { return named("UMIRA"); }, 3000));
    CHECK_FALSE(named("UJONAS"));
    REQUIRE(pumpUntil([&] { return settled && !e.be->connecting(); }, 5000));
    CHECK(named("UJONAS"));
    CHECK_STR(e.store.user(e.store.findUser("UJONAS")).displayName, "Jonas");
    for (const std::string &m : e.errors)
        CHECK(m.find("rate-limiting") == std::string::npos);
}

TEST("slack features: mentioned channels and user groups resolve; dead ids persist") {
    if (!haveServer())
        return;
    cache::WorkspaceCache::remove(app(), "slack:T1");
    {
        Env e(
            R"({"conversations.info?channel=C77": {"ok": true, "channel": {"id": "C77", "name": "secret"}},
                  "conversations.info?channel=C88": {"ok": false, "error": "channel_not_found"}})"
        );
        e.be->openCache(cache::WorkspaceCache::dirFor(app(), "slack:T1"));
        REQUIRE(e.connect());
        e.be->resolveChannel("C77");
        e.be->resolveChannel("C77"); // one lookup in flight
        e.be->resolveChannel("C88");
        REQUIRE(pumpUntil([&] {
            return e.store.channelName("C77") && e.store.channelName("C88");
        }));
        CHECK_STR(*e.store.channelName("C77"), "secret");
        CHECK(e.store.channelName("C88")->empty());
        CHECK(count("conversations.info") == 2);
        // Every group with its handle; mine among them.
        REQUIRE(pumpUntil([&] { return e.store.findUsergroup("S2") != nullptr; }));
        CHECK_STR(e.store.findUsergroup("S1")->handle, "design");
        CHECK(e.store.myGroups.size() == 1 && e.store.myGroups[0] == "S1");
        e.be->markDead("CDEAD");
        CHECK(e.be->isDead("CDEAD"));
    }
    // A restart: the dead id and the groups come back from the cache.
    Env again({}, true, false);
    again.be->openCache(cache::WorkspaceCache::dirFor(app(), "slack:T1"));
    CHECK(again.be->isDead("CDEAD"));
    REQUIRE(again.store.findUsergroup("S2"));
    CHECK_STR(again.store.findUsergroup("S2")->name, "Ops");
    again.be.reset();
    cache::WorkspaceCache::remove(app(), "slack:T1");
}

TEST("slack features: an OAuth callback delivered twice exchanges the code once") {
    if (!haveServer())
        return;
    plat::TestHooks *hooks = app().testHooks();
    if (!hooks)
        return;
    ctl("POST", "/_ctl/reset");
    // The first exchange answers late; a second one (the same code again)
    // would fail at once, end the flow — and the late answer would then land
    // on a destroyed flow (issue #96).
    set(R"({"oauth.v2.access": [
              {"ok": true, "authed_user": {"access_token": "xoxp-new"},
               "team": {"id": "T9", "name": "Nine"}, "__delay": 0.3},
              {"ok": false, "error": "invalid_code"}]})");
    net::Client client{app()};
    auto        flow = std::make_unique<slack::OAuthFlow>(
        app(), client, slack::AppConfig{"cid", "secret", "xapp-1"}
    );
    int         called = 0;
    std::string token, error;
    flow->start([&](slack::Credentials c, std::string err) {
        ++called;
        token = c.token;
        error = std::move(err);
        app().post([&] { flow.reset(); }); // as Accounts does (releaseLater)
    });
    std::string opened;
    REQUIRE(hooks->lastOpenedUrl(&opened));
    const std::string state = net::queryValue(opened.substr(opened.find('?')), "state");
    REQUIRE(!state.empty());
    const std::string url = "msga://oauth/callback?code=c1&state=" + state;
    CHECK(flow->handleCallback(url));
    CHECK_FALSE(flow->handleCallback(url)); // a second launch's copy
    REQUIRE(pumpUntil([&] { return called > 0; }));
    pumpFor(500);
    CHECK(called == 1);
    CHECK_STR(error, "");
    CHECK_STR(token, "xoxp-new");
    CHECK(count("oauth.v2.access") == 1);
}

#endif // !_WIN32

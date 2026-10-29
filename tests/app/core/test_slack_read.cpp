// SlackBackend's read side against the local fake Slack (tests/support/fake_slack.py
// via fake_slack_server.h): connect (roster, users, unread, stars, emoji,
// user groups), history pages, threads, the poll that delivers new
// messages, rate limits, auth errors, the Enterprise Grid fallback, and the
// JSON mappers. Nothing here talks to the real Slack. Delays are compressed
// (MSGA_SLACK_TEST_SPEEDUP) so the 5 s poll runs every 50 ms.
#include "app/slack/slack_backend.h"
#include "app/slack/slack_json.h"
#include "support/fake_slack_server.h"
#include "base/json.h"
#include "support/test.h"

#include <cstdlib>
#include <memory>
#include <string>

#ifndef _WIN32

namespace {

using fakeslack::app;
using fakeslack::ctl;
using fakeslack::pumpUntil;
using model::ConvRef;
using model::kNoConv;
using model::Ts;

// Standing answers ({key: answer-or-list}), see fake_slack.py /_ctl/set.
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
    // Calls of `method` whose form has field == value.
    int count(std::string_view method, std::string_view field, std::string_view value) const {
        int n = 0;
        for (json::Value r : doc.root())
            n += r["method"].str() == method && r["form"][field].str() == value;
        return n;
    }
    json::Value get(std::string_view method, int n = 0) const {
        for (json::Value r : doc.root())
            if (r["method"].str() == method && n-- == 0)
                return r;
        return {};
    }
};

// The workspace every connect test starts from: two pages of users, two of
// conversations (a channel, a muted channel, DMs incl. a Slack Connect peer
// users.list never lists, a group DM known only by its mpdm- name).
const char *kWorkspace = R"({
 "auth.test": {"ok": true, "user_id": "UME", "team_id": "T1", "team": "Lumen",
               "url": "https://lumen.slack.com/"},
 "users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
     {"id": "UMIRA", "name": "mira", "tz_offset": 3600,
      "profile": {"real_name": "Mira Okafor", "image_72": "https://a/mira72.png",
                  "status_emoji": ":palm_tree:", "status_text": "away"}},
     {"id": "UJONAS", "name": "jonas", "profile": {"display_name": "Jonas"}}],
   "response_metadata": {"next_cursor": "p2"}},
 "users.list?cursor=p2": {"ok": true, "members": [
     {"id": "UBOT", "name": "deploybot", "is_bot": true, "profile": {}}],
   "response_metadata": {"next_cursor": ""}},
 "users.conversations": {"ok": true, "channels": [
     {"id": "C1", "name": "general", "is_channel": true, "is_member": true,
      "topic": {"value": "Company news"}, "num_members": 12},
     {"id": "D1", "is_im": true, "user": "UMIRA"}],
   "response_metadata": {"next_cursor": "c2"}},
 "users.conversations?cursor=c2": {"ok": true, "channels": [
     {"id": "D2", "is_im": true, "user": "UEXT"},
     {"id": "G1", "is_mpim": true, "name": "mpdm-me--mira--jonas-1"},
     {"id": "C2", "name": "random", "is_channel": true, "is_member": true, "is_muted": true}],
   "response_metadata": {"next_cursor": ""}},
 "users.info?user=UEXT": {"ok": true, "user": {"id": "UEXT", "name": "ext",
     "profile": {"real_name": "Ext Partner"}}},
 "client.counts": {"ok": true,
   "channels": [{"id": "C1", "latest": "1700000100.000000", "last_read": "1700000000.000000",
                 "mention_count": 1, "has_unreads": true}],
   "ims": [{"id": "D1", "latest": "1700000050.000000", "last_read": "1700000000.000000",
            "dm_count": 2, "has_unreads": true}],
   "mpims": []},
 "stars.list": {"ok": true, "items": [{"type": "channel", "channel": "C2"},
                                      {"type": "message", "channel": "C1", "message": {}}]},
 "emoji.list": {"ok": true, "emoji": {"party": "https://e/party.png", "yay": "alias:party"}},
 "usergroups.list": {"ok": true, "usergroups": [{"id": "S1", "users": ["UJONAS", "UME"]},
                                                {"id": "S2", "users": ["UMIRA"]}]},
 "users.getPresence": {"ok": true, "presence": "active", "online": true},
 "conversations.history": {"ok": true, "messages": [], "has_more": false}
})";

// One test's world: the fake reset to `table`, a Store and a backend.
struct Env {
    model::Store                         store;
    net::Client                          client{app()};
    std::unique_ptr<slack::SlackBackend> be;
    std::string                          lostAuth;

    explicit Env(const char *table = kWorkspace, bool session = true) {
        base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "100");
        ctl("POST", "/_ctl/reset");
        if (table)
            set(table);
        slack::Credentials cr;
        cr.token       = session ? "xoxc-test" : "xoxp-test";
        cr.cookie      = session ? "xoxd-test" : "";
        cr.teamId      = "T1";
        be             = std::make_unique<slack::SlackBackend>(store, app(), client, cr);
        be->onAuthLost = [this](const std::string &e) { lostAuth = e; };
    }
    ~Env() {
        be.reset(); // no callback or timer of it runs afterwards
        base::test::unsetEnv("MSGA_SLACK_TEST_SPEEDUP");
    }
    bool connect(std::string *err = nullptr) {
        bool called = false, ok = false;
        be->connect([&](bool o, const std::string &e) {
            called = true;
            ok     = o;
            if (err)
                *err = e;
        });
        // done(true) comes with the conversations; these tests read the
        // users too, so wait for the whole load.
        return pumpUntil([&] { return called && !be->connecting(); }) && ok;
    }
    bool history(ConvRef c, Ts before) {
        bool called = false, ok = false;
        be->loadHistory(c, before, [&](bool o, const std::string &) {
            called = true;
            ok     = o;
        });
        return pumpUntil([&] { return called; }) && ok;
    }
    ConvRef            conv(const char *id) const { return store.findConversation(id); }
    const model::User &user(const char *id) const { return store.user(store.findUser(id)); }
    // A conversation by hand (the history/thread tests skip connect: no polls).
    ConvRef            addChannel(const char *id) {
        model::User me;
        me.id    = "UME";
        store.me = store.addUser(me);
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

} // namespace

TEST("slack read: connect fills users, conversations, unread, stars, emoji, groups") {
    if (!haveServer())
        return;
    Env e;
    e.store.setCustomEmoji("gone", "https://e/gone.png"); // cached, since removed
    REQUIRE(e.connect());
    CHECK_STR(e.store.workspaceName, "Lumen");
    CHECK_STR(e.store.user(e.store.me).id, "UME");
    // Both pages of users, mapped.
    const model::User &mira = e.user("UMIRA");
    CHECK_STR(mira.displayName, "Mira Okafor");
    CHECK_STR(mira.avatar, "https://a/mira72.png");
    CHECK_STR(mira.statusEmoji, "palm_tree");
    CHECK(mira.hasTz && mira.tzOffset == 3600);
    CHECK(e.user("UBOT").bot);
    // Both pages of conversations.
    REQUIRE(e.conv("C1") != kNoConv && e.conv("C2") != kNoConv && e.conv("G1") != kNoConv);
    const model::Conversation &general = e.store.conversation(e.conv("C1"));
    CHECK_STR(general.topic, "Company news");
    CHECK(general.memberCount == 12);
    CHECK(e.store.conversation(e.conv("C2")).muted);
    const model::Conversation &dm = e.store.conversation(e.conv("D1"));
    CHECK(dm.kind == model::ConvKind::Dm && dm.dmUser == e.store.findUser("UMIRA"));
    CHECK_STR(e.store.displayName(e.conv("D1")), "Mira Okafor");
    // The group DM's members come from its mpdm- name.
    CHECK(e.store.conversation(e.conv("G1")).members.size() == 3);
    CHECK_STR(e.store.displayName(e.conv("G1")), "Mira Okafor, Jonas");
    // client.counts seeds the badges: a DM's unread are all red badges.
    REQUIRE(pumpUntil([&] { return e.store.conversation(e.conv("D1")).unread == 2; }, 3000));
    CHECK(e.store.conversation(e.conv("D1")).mentions == 2);
    CHECK(general.unread == 1 && general.mentions == 1);
    CHECK(general.lastRead == model::parseTs("1700000000.000000"));
    CHECK(general.latest == model::parseTs("1700000100.000000"));
    // The DM peer users.list omits is resolved through users.info.
    REQUIRE(pumpUntil([&] { return !e.user("UEXT").placeholder; }, 3000));
    CHECK_STR(e.store.displayName(e.conv("D2")), "Ext Partner");
    // Stars (conversation stars only), custom emoji with aliases, my groups,
    // my presence.
    REQUIRE(pumpUntil([&] { return e.store.conversation(e.conv("C2")).starred; }, 3000));
    CHECK(!e.store.conversation(e.conv("C1")).starred);
    REQUIRE(pumpUntil([&] { return !e.store.emojiFor("yay").image.empty(); }, 3000));
    CHECK_STR(e.store.emojiFor("yay").image, "https://e/party.png");
    CHECK_FALSE(e.store.emojiFor("gone").resolved()); // emoji.list is the whole set
    REQUIRE(e.store.customEmojiNames().size() == 2);
    CHECK(e.store.customEmojiNames()[0] == "party" && e.store.customEmojiNames()[1] == "yay");
    REQUIRE(e.store.customEmojiImages().size() == 1);
    CHECK(e.store.customEmojiImages()[0].name == "party");
    REQUIRE(pumpUntil([&] { return e.store.myGroups.size() == 1; }, 3000));
    CHECK_STR(e.store.myGroups[0], "S1");
    REQUIRE(pumpUntil([&] { return e.be->selfPresence().loaded; }, 3000));
    CHECK(e.be->selfPresence().active && e.be->selfPresence().online);
    CHECK(e.store.user(e.store.me).active);
    // The listing parameters sent.
    Log               l;
    const json::Value list = l.get("users.conversations");
    CHECK_STR(list["form"]["types"].str(), "public_channel,private_channel,im,mpim");
    CHECK_STR(list["form"]["limit"].str(), "1000");
    CHECK_STR(list["form"]["exclude_archived"].str(), "true");
    CHECK_STR(list["form"]["team_id"].str(), "T1");
    CHECK_STR(l.get("users.conversations", 1)["form"]["cursor"].str(), "c2");
    CHECK_STR(l.get("users.list", 1)["form"]["cursor"].str(), "p2");
    CHECK_STR(l.get("usergroups.list")["form"]["include_users"].str(), "1");
    CHECK_STR(l.get("auth.test")["auth"].str(), "Bearer xoxc-test");
    CHECK_STR(l.get("auth.test")["cookie"].str(), "d=xoxd-test");
    const slack::SlackBackend::Capabilities caps = e.be->capabilities();
    CHECK(caps.threadsView && caps.messageReminders && caps.huddles && caps.memberList);
}

TEST("slack read: a users.list snapshot merges, never drops or blanks users") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return !e.user("UEXT").placeholder; }, 3000));
    // The next snapshot lacks Jonas and the external peer, and Mira's avatar.
    set(R"({"users.list": {"ok": true, "members": [
              {"id": "UME", "name": "me", "profile": {}},
              {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira O."}}],
            "response_metadata": {"next_cursor": ""}}})");
    // A new backend on the same Store: this run's list doesn't stand in.
    slack::Credentials cr;
    cr.token  = "xoxc-test";
    cr.cookie = "xoxd-test";
    cr.teamId = "T1";
    e.be      = std::make_unique<slack::SlackBackend>(e.store, app(), e.client, cr);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return !e.be->connecting(); }, 3000));
    CHECK_STR(e.user("UMIRA").displayName, "Mira O.");
    CHECK_STR(e.user("UMIRA").avatar, "https://a/mira72.png"); // kept
    CHECK(!e.user("UJONAS").placeholder);
    CHECK_STR(e.user("UJONAS").displayName, "Jonas");
    CHECK_STR(e.user("UEXT").displayName, "Ext Partner");
}

TEST("slack read: history pages arrive oldest first, hasMoreBefore follows has_more") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c = e.addChannel("C1");
    script("conversations.history", R"([
      {"ok": true, "has_more": true, "messages": [
        {"type": "message", "ts": "1700000300.000300", "bot_id": "B1", "username": "Deploys",
         "text": "fallback", "subtype": "bot_message",
         "bot_profile": {"name": "deploy", "icons": {"image_48": "https://b/48.png"}},
         "blocks": [{"type": "header", "text": {"type": "plain_text", "text": "Deployed <3"}},
                    {"type": "section", "text": {"type": "mrkdwn", "text": "build *42*"}}]},
        {"type": "message", "ts": "1700000200.000200", "user": "UMIRA", "text": "pic",
         "edited": {"user": "UMIRA"}, "thread_ts": "1700000200.000200", "reply_count": 2,
         "latest_reply": "1700000250.000000", "reply_users": ["UJONAS"],
         "reactions": [{"name": "tada", "count": 3, "users": ["UME", "UJONAS"]}],
         "files": [{"id": "F1", "name": "cat.png", "mimetype": "image/png", "pretty_type": "PNG",
                    "url_private": "https://files/cat.png", "thumb_360": "https://files/cat360.png",
                    "thumb_720": "https://files/cat720.png", "original_w": 1600, "original_h": 900,
                    "size": 12345}],
         "attachments": [{"from_url": "https://news/x", "title": "News", "service_name": "News",
                          "color": "3FCB8E", "text": "story", "thumb_url": "https://news/t.png",
                          "thumb_width": 80, "thumb_height": 60}]}]},
      {"ok": true, "has_more": false, "messages": [
        {"type": "message", "ts": "1700000100.000100", "user": "UME", "text": "first"}]}
    ])");
    REQUIRE(e.history(c, 0));
    const model::Conversation &conv = e.store.conversation(c);
    REQUIRE(conv.messages.size() == 2);
    CHECK(conv.hasMoreBefore);
    const model::Message &pic = conv.messages[0];
    CHECK(pic.ts == model::parseTs("1700000200.000200"));
    CHECK(pic.edited && pic.replyCount == 2 && !pic.isReply());
    CHECK(pic.latestReply == model::parseTs("1700000250.000000"));
    CHECK(pic.replyUsers.size() == 1 && pic.replyUsers[0] == e.store.findUser("UJONAS"));
    REQUIRE(pic.reactions.size() == 1);
    CHECK(pic.reactions[0].count == 3 && pic.reactions[0].users.size() == 2);
    REQUIRE(pic.files().size() == 1);
    CHECK_STR(pic.files()[0].path, "https://files/cat720.png");
    CHECK(pic.files()[0].isImage() && pic.files()[0].width == 1600);
    REQUIRE(pic.attachments().size() == 1);
    CHECK(pic.attachments()[0].linkPreview);
    CHECK_STR(pic.attachments()[0].color, "#3FCB8E");
    CHECK_STR(pic.attachments()[0].link, "https://news/x");
    CHECK_STR(pic.attachments()[0].image, "https://news/t.png");
    const model::Message &bot = conv.messages[1];
    CHECK_STR(bot.extra->botName, "Deploys");
    CHECK_STR(bot.extra->botAvatar, "https://b/48.png");
    CHECK_STR(bot.subtype(), "bot_message");
    CHECK_STR(bot.text, "*Deployed &lt;3*\nbuild *42*"); // the blocks, not the fallback
    // The older page: latest = the oldest we hold, exclusive.
    REQUIRE(e.history(c, conv.messages.front().ts));
    CHECK(conv.messages.size() == 3 && !conv.hasMoreBefore);
    CHECK_STR(conv.messages.front().text, "first");
    Log l;
    CHECK_STR(l.get("conversations.history")["form"]["limit"].str(), "50");
    CHECK(!l.get("conversations.history")["form"].has("latest"));
    CHECK_STR(l.get("conversations.history", 1)["form"]["latest"].str(), "1700000200.000200");
    CHECK_STR(l.get("conversations.history", 1)["form"]["inclusive"].str(), "false");
}

TEST("slack read: a thread loads every replies page and refreshes its root") {
    if (!haveServer())
        return;
    Env            e(nullptr);
    const ConvRef  c    = e.addChannel("C1");
    const Ts       root = model::parseTs("1700000200.000200");
    model::Message m;
    m.ts       = root;
    m.threadTs = root;
    m.text     = "root";
    std::vector<model::Message> page;
    page.push_back(std::move(m));
    e.store.addPage(c, std::move(page));
    script("conversations.replies", R"([
      {"ok": true, "has_more": true, "response_metadata": {"next_cursor": "r2"}, "messages": [
        {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
         "user": "UMIRA", "text": "root", "reply_count": 2, "latest_reply": "1700000400.000000",
         "reply_users": ["UJONAS", "UMIRA"]},
        {"type": "message", "ts": "1700000300.000000", "thread_ts": "1700000200.000200",
         "user": "UJONAS", "text": "one"}]},
      {"ok": true, "messages": [
        {"type": "message", "ts": "1700000400.000000", "thread_ts": "1700000200.000200",
         "user": "UMIRA", "text": "two", "subtype": "thread_broadcast"}]}
    ])");
    bool called = false, ok = false;
    e.be->loadThread(c, root, [&](bool o, const std::string &) {
        called = true;
        ok     = o;
    });
    REQUIRE(pumpUntil([&] { return called; }) && ok);
    const std::vector<model::Message> *replies = e.store.replies(c, root);
    REQUIRE(replies && replies->size() == 2);
    CHECK_STR((*replies)[0].text, "one");
    CHECK_STR((*replies)[1].text, "two");
    CHECK((*replies)[1].threadTs == root);
    const model::Message *r = e.store.findMessage(c, root);
    REQUIRE(r);
    CHECK(r->replyCount == 2 && r->replyUsers.size() == 2);
    CHECK(e.store.conversation(c).messages.size() == 1); // replies stay in the thread
    Log l;
    CHECK_STR(l.get("conversations.replies", 1)["form"]["cursor"].str(), "r2");
    CHECK_STR(l.get("conversations.replies")["form"]["ts"].str(), "1700000200.000200");
}

// The thread download relies on it: one failed page fails the whole load
// (no partial thread reported as complete), and a looping cursor ends.
TEST("slack read: a failed replies page fails the thread load; a looping cursor stops") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c    = e.addChannel("C1");
    const Ts      root = model::parseTs("1700000200.000200");
    script("conversations.replies", R"([
      {"ok": true, "has_more": true, "response_metadata": {"next_cursor": "r2"}, "messages": [
        {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
         "user": "UMIRA", "text": "root", "reply_count": 3},
        {"type": "message", "ts": "1700000300.000000", "thread_ts": "1700000200.000200",
         "user": "UJONAS", "text": "one"}]},
      {"ok": false, "error": "thread_not_found"}
    ])");
    bool        called = false, ok = true;
    std::string err;
    e.be->loadThread(c, root, [&](bool o, const std::string &er) {
        called = true;
        ok     = o;
        err    = er;
    });
    REQUIRE(pumpUntil([&] { return called; }));
    CHECK(!ok);
    CHECK_STR(err, "thread_not_found");
    const std::vector<model::Message> *replies = e.store.replies(c, root);
    CHECK(!replies || replies->empty()); // the first page is not kept as the thread
    CHECK(Log().count("conversations.replies") == 2);

    // A server repeating its next_cursor forever (the table's answer sticks).
    set(R"({"conversations.replies": {"ok": true, "messages": [],
              "response_metadata": {"next_cursor": "again"}}})");
    called = false;
    e.be->loadThread(c, root, [&](bool o, const std::string &er) {
        called = true;
        ok     = o;
        err    = er;
    });
    REQUIRE(pumpUntil([&] { return called; }, 30000));
    CHECK(!ok);
    CHECK_STR(err, "page_limit");
    CHECK(Log().count("conversations.replies") == 2 + 400);
}

TEST("slack read: the open chat's poll delivers new messages and deletions") {
    if (!haveServer())
        return;
    Env e;
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(e.history(c, 0));
    REQUIRE(pumpUntil([&] { return e.store.conversation(c).unread == 1; }, 3000)); // seeded
    e.be->setActiveConversation(c, 0);
    // Someone mentions me: the poll delivers it as a live message (badge up).
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000200.000000", "user": "UMIRA", "text": "hi <@UME>"},
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    const Ts hi = model::parseTs("1700000200.000000");
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, hi) != nullptr; }, 3000));
    CHECK(e.store.conversation(c).unread == 2);
    CHECK(e.store.conversation(c).mentions == 2);
    CHECK(e.store.conversation(c).latest == hi);
    // Edited elsewhere: the head page merges it in place.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000200.000000", "user": "UMIRA",
               "text": "hi <@UME>!", "edited": {"user": "UMIRA"}},
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, hi)->edited; }, 3000));
    CHECK_STR(e.store.findMessage(c, hi)->text, "hi <@UME>!");
    CHECK(e.store.conversation(c).unread == 2); // an edit is not news
    // Deleted elsewhere: gone from the head page, so gone here.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, hi) == nullptr; }, 3000));
}

TEST("slack read: a message deleted while its chat was closed goes on the first poll") {
    if (!haveServer())
        return;
    Env e;
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000300.000000", "user": "UMIRA", "text": "two"},
              {"type": "message", "ts": "1700000200.000000", "user": "UMIRA", "text": "one"},
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(e.history(c, 0));
    const Ts one = model::parseTs("1700000200.000000");
    REQUIRE(e.store.findMessage(c, one) != nullptr);
    // Deleted with no event reaching us; the chat is opened afterwards.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000300.000000", "user": "UMIRA", "text": "two"},
              {"type": "message", "ts": "1700000100.000000", "user": "UJONAS", "text": "old"}]}})");
    e.be->setActiveConversation(c, 0);
    REQUIRE(pumpUntil([&] { return e.store.findMessage(c, one) == nullptr; }, 3000));
    CHECK(e.store.findMessage(c, model::parseTs("1700000300.000000")) != nullptr);
    CHECK(e.store.findMessage(c, model::parseTs("1700000100.000000")) != nullptr);
}

TEST("slack read: hidden, the open chat polls slowly, client.counts brings it forward") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(e.history(c, 0));
    e.be->setActiveConversation(c, 0);
    e.be->setWindowVisible(false);
    fakeslack::pumpFor(100);
    // The open chat: 60 s instead of 5 s (600 ms here), and no presence rounds.
    Log before;
    fakeslack::pumpFor(1500);
    Log l;
    CHECK(l.count("conversations.history") - before.count("conversations.history") <= 4);
    CHECK(
        l.count("users.getPresence", "user", "UMIRA") ==
        before.count("users.getPresence", "user", "UMIRA")
    );
    // Something lands there: the counts snapshot moves and the chat is polled.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000300.000000", "user": "UMIRA", "text": "news"}]},
            "client.counts": {"ok": true,
              "channels": [{"id": "C1", "latest": "1700000300.000000",
                            "last_read": "1700000000.000000", "has_unreads": true}],
              "ims": [], "mpims": []}})");
    REQUIRE(pumpUntil(
        [&] { return e.store.findMessage(c, model::parseTs("1700000300.000000")) != nullptr; }, 3000
    ));
    // Shown again: the dots are asked for at once.
    const int presence = Log().count("users.getPresence", "user", "UMIRA");
    e.be->setWindowVisible(true);
    REQUIRE(pumpUntil(
        [&] { return Log().count("users.getPresence", "user", "UMIRA") > presence; }, 1000
    ));
}

TEST("slack read: a channel read elsewhere loses its badge with the next client.counts") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(pumpUntil([&] { return e.store.conversation(c).mentions == 1; }, 3000)); // seeded
    set(R"({"client.counts": {"ok": true,
              "channels": [{"id": "C1", "latest": "1700000100.000000",
                            "last_read": "1700000100.000000", "has_unreads": false}],
              "ims": [], "mpims": []}})");
    REQUIRE(pumpUntil(
        [&] {
            const model::Conversation &x = e.store.conversation(c);
            return x.unread == 0 && x.mentions == 0;
        },
        3000
    ));
    CHECK(e.store.conversation(c).lastRead == model::parseTs("1700000100.000000"));
}

TEST("slack read: seeing a read channel's newest message clears a badge left over") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c = e.addChannel("C1");
    set(R"({"conversations.history": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000100.000000", "user": "UMIRA", "text": "hi"}]}})");
    REQUIRE(e.history(c, 0));
    const Ts newest = model::parseTs("1700000100.000000");
    // Read up to the newest message, yet badged: thread replies counted on
    // the channel (issue #92), or a stale cached count.
    e.store.updateConversation(c, [&](model::Conversation &x) {
        x.lastRead = newest;
        x.unread = x.mentions = 5;
    });
    e.be->markRead(c, newest - 1); // not the newest: left alone
    CHECK(e.store.conversation(c).mentions == 5);
    e.be->markRead(c, newest);
    CHECK(e.store.conversation(c).unread == 0);
    CHECK(e.store.conversation(c).mentions == 0);
    fakeslack::pumpFor(200);
    CHECK(Log().count("conversations.mark") == 0); // nothing to tell Slack
}

TEST("slack read: a failed author lookup is not re-asked on every poll") {
    if (!haveServer())
        return;
    Env e;
    // Authors users.list doesn't know: a deleted account (user_not_found), one
    // users.info answers without a user, a bot bots.info can't find, and a
    // Slack Connect peer that resolves.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": false, "messages": [
              {"type": "message", "ts": "1700000400.000000", "user": "UGONE", "text": "a"},
              {"type": "message", "ts": "1700000300.000000", "user": "UBLANK", "text": "b"},
              {"type": "message", "ts": "1700000200.000000", "bot_id": "BGONE", "text": "c"},
              {"type": "message", "ts": "1700000100.000000", "user": "UPEER", "text": "d"}]},
            "users.info?user=UGONE": {"ok": false, "error": "user_not_found"},
            "users.info?user=UBLANK": {"ok": true},
            "users.info?user=UPEER": {"ok": true, "user": {"id": "UPEER", "name": "peer"}},
            "bots.info?bot=BGONE": {"ok": false, "error": "bot_not_found"}})");
    REQUIRE(e.connect());
    const ConvRef c = e.conv("C1");
    REQUIRE(e.history(c, 0));
    e.be->setActiveConversation(c, 0);
    // The open chat polls every 50 ms here (5 s × 1/100): ~30 polls.
    const int polls = Log().count("conversations.history");
    fakeslack::pumpFor(1500);
    Log l;
    CHECK(l.count("conversations.history") - polls >= 10);
    CHECK(l.count("users.info", "user", "UGONE") == 1);
    CHECK(l.count("users.info", "user", "UBLANK") == 1);
    CHECK(l.count("bots.info", "bot", "BGONE") == 1);
    CHECK(l.count("users.info", "user", "UPEER") == 1);
    CHECK_FALSE(e.user("UPEER").placeholder);
    CHECK(e.user("UGONE").placeholder);
}

TEST("slack read: a background call already queued is not queued twice") {
    if (!haveServer())
        return;
    Env e(nullptr);
    // A slow interactive call holds the paced lane.
    set(R"({"conversations.history": {"ok": true, "has_more": false, "messages": [],
                                      "__delay": 0.4}})");
    bool slow = false;
    e.be->readCallForTest(
        "conversations.history",
        "channel=C1",
        [&](const json::Document &, const std::string &) { slow = true; },
        false
    );
    int a = 0, b = 0, other = 0;
    e.be->readCallForTest(
        "users.getPresence",
        "user=UMIRA",
        [&](const json::Document &, const std::string &err) { a += err.empty(); },
        true
    );
    e.be->readCallForTest(
        "users.getPresence",
        "user=UJONAS",
        [&](const json::Document &, const std::string &err) { other += err.empty(); },
        true
    );
    e.be->readCallForTest(
        "users.getPresence",
        "user=UMIRA",
        [&](const json::Document &, const std::string &err) { b += err.empty(); },
        true
    );
    REQUIRE(pumpUntil([&] { return slow && a && b && other; }, 5000));
    fakeslack::pumpFor(100);
    Log l;
    CHECK(l.count("users.getPresence", "user", "UMIRA") == 1); // both callers answered by one
    CHECK(l.count("users.getPresence", "user", "UJONAS") == 1);
    CHECK(a == 1 && b == 1 && other == 1);
}

TEST("slack read: the channel browser's list brings the channels I'm not in, as non-members") {
    if (!haveServer())
        return;
    Env e;
    set(R"({"conversations.list": {"ok": true, "channels": [
              {"id": "C1", "name": "general", "is_channel": true, "is_member": true,
               "num_members": 99},
              {"id": "C7", "name": "watercooler", "is_channel": true, "is_member": false,
               "num_members": 40}],
            "response_metadata": {"next_cursor": ""}}})");
    REQUIRE(e.connect());
    CHECK(e.conv("C7") == kNoConv); // the roster is mine only
    CHECK(Log().count("conversations.list") == 0);
    e.be->loadChannelDirectory();
    REQUIRE(pumpUntil([&] { return e.conv("C7") != kNoConv; }, 3000));
    CHECK(!e.store.conversation(e.conv("C7")).member);
    CHECK(e.store.conversation(e.conv("C7")).memberCount == 40);
    // Mine stay the roster's copy.
    CHECK(e.store.conversation(e.conv("C1")).member);
    CHECK(e.store.conversation(e.conv("C1")).memberCount == 12);
    CHECK_STR(Log().get("conversations.list")["form"]["types"].str(), "public_channel");
    // Opened again soon after: nothing is asked.
    e.be->loadChannelDirectory();
    fakeslack::pumpFor(100);
    CHECK(Log().count("conversations.list") == 1);
}

TEST("slack read: a roster reload that changed nothing emits no Meta") {
    if (!haveServer())
        return;
    // App keys: no client.counts, so the roster reloads every minute (it is
    // the activity source there).
    Env e(kWorkspace, /*session=*/false);
    REQUIRE(e.connect());
    fakeslack::pumpFor(300); // the connect's own follow-ups settle
    int        meta = 0, roster = 0;
    const auto id    = e.store.observe(model::Store::kAnyConv, [&](const model::Change &ch) {
        meta += ch.kind == model::ChangeKind::Meta;
        roster += ch.kind == model::ChangeKind::Roster;
    });
    // Without push the roster reloads every 60 s (0.6 s here): wait for two.
    const int  lists = Log().count("users.conversations");
    REQUIRE(pumpUntil([&] { return Log().count("users.conversations") >= lists + 4; }, 5000));
    fakeslack::pumpFor(100);
    e.store.unobserve(id);
    CHECK(meta == 0);
    CHECK(roster == 0);
}

TEST("slack read: with client.counts the roster reloads when counts names a new id") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    fakeslack::pumpFor(300);
    // counts every 10 s (0.1 s here): nothing new, so no reload in ~15
    // snapshots (the fallback is 15 min, 9 s here).
    const int lists = Log().count("users.conversations");
    fakeslack::pumpFor(1500);
    CHECK(Log().count("client.counts") >= 5);
    CHECK(Log().count("users.conversations") == lists);
    // A new DM shows up in counts first: one reload brings it.
    set(R"({"client.counts": {"ok": true,
              "channels": [{"id": "C1", "latest": "1700000100.000000",
                            "last_read": "1700000000.000000", "mention_count": 1}],
              "ims": [{"id": "D1", "latest": "1700000050.000000", "last_read": "1700000000.000000"},
                      {"id": "D9", "latest": "1700000900.000000", "last_read": "0"}],
              "mpims": []},
            "users.conversations?cursor=c2": {"ok": true, "channels": [
              {"id": "D2", "is_im": true, "user": "UEXT"},
              {"id": "G1", "is_mpim": true, "name": "mpdm-me--mira--jonas-1"},
              {"id": "C2", "name": "random", "is_channel": true, "is_member": true},
              {"id": "D9", "is_im": true, "user": "UJONAS"}],
              "response_metadata": {"next_cursor": ""}}})");
    REQUIRE(pumpUntil([&] { return e.conv("D9") != kNoConv; }, 3000));
    fakeslack::pumpFor(300);
    const int after = Log().count("users.conversations", "cursor", "c2");
    CHECK(after >= 1);
    // An id the reload doesn't bring (not ours to list) asks only once.
    set(R"({"client.counts": {"ok": true, "channels": [{"id": "C1"}, {"id": "CZ"}],
              "ims": [{"id": "D1"}, {"id": "D9"}], "mpims": []}})");
    fakeslack::pumpFor(1500);
    const int lists2 = Log().count("users.conversations", "cursor", "c2");
    CHECK(lists2 <= after + 1);
    fakeslack::pumpFor(1000);
    CHECK(Log().count("users.conversations", "cursor", "c2") == lists2);
}

TEST("slack read: an open thread whose root left the head page asks for its root alone") {
    if (!haveServer())
        return;
    Env e;
    REQUIRE(e.connect());
    const ConvRef c    = e.conv("C1");
    const Ts      root = model::parseTs("1700000200.000200");
    // A long thread, its root long gone from the channel's head page.
    set(R"({"conversations.history?channel=C1": {"ok": true, "has_more": true, "messages": [
              {"type": "message", "ts": "1700009000.000000", "user": "UJONAS", "text": "later"}]},
            "conversations.replies?limit=1": {"ok": true, "has_more": true, "messages": [
              {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
               "user": "UMIRA", "text": "root", "reply_count": 1,
               "latest_reply": "1700000300.000000"}]},
            "conversations.replies": {"ok": true, "messages": [
              {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
               "user": "UMIRA", "text": "root", "reply_count": 1,
               "latest_reply": "1700000300.000000"},
              {"type": "message", "ts": "1700000300.000000", "thread_ts": "1700000200.000200",
               "user": "UJONAS", "text": "one"}]}})");
    bool loaded = false;
    e.be->loadThread(c, root, [&](bool, const std::string &) { loaded = true; });
    REQUIRE(pumpUntil([&] { return loaded; }));
    REQUIRE(e.store.replies(c, root) && e.store.replies(c, root)->size() == 1);
    e.be->setActiveConversation(c, root);
    // ~20 polls of the open chat: each asks for the root alone, none
    // re-reads the thread while latest_reply stays where we are.
    const int full0 = Log().count("conversations.replies", "limit", "50");
    fakeslack::pumpFor(1000);
    Log l;
    CHECK(l.count("conversations.replies", "limit", "1") >= 5);
    CHECK(l.count("conversations.replies", "limit", "50") == full0);
    // A reply lands: latest_reply moves, one full read delivers it.
    set(R"({"conversations.replies?limit=1": {"ok": true, "has_more": true, "messages": [
              {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
               "user": "UMIRA", "text": "root", "reply_count": 2,
               "latest_reply": "1700000400.000000"}]},
            "conversations.replies": {"ok": true, "messages": [
              {"type": "message", "ts": "1700000200.000200", "thread_ts": "1700000200.000200",
               "user": "UMIRA", "text": "root", "reply_count": 2,
               "latest_reply": "1700000400.000000"},
              {"type": "message", "ts": "1700000300.000000", "thread_ts": "1700000200.000200",
               "user": "UJONAS", "text": "one"},
              {"type": "message", "ts": "1700000400.000000", "thread_ts": "1700000200.000200",
               "user": "UMIRA", "text": "two"}]}})");
    REQUIRE(pumpUntil([&] { return e.store.replies(c, root)->size() == 2; }, 3000));
    CHECK_STR(e.store.replies(c, root)->back().text, "two");
    fakeslack::pumpFor(500);
    CHECK(Log().count("conversations.replies", "limit", "50") <= full0 + 2);
}

TEST("slack read: opening a chat fetches its head page once") {
    if (!haveServer())
        return;
    Env e;
    // Delays compressed 10× only: the open chat's poll every 500 ms, so a
    // loaded machine can't make the next regular poll look like a second
    // fetch.
    base::test::setEnv("MSGA_SLACK_TEST_SPEEDUP", "10");
    slack::Credentials cr;
    cr.token  = "xoxc-test";
    cr.cookie = "xoxd-test";
    cr.teamId = "T1";
    e.be.reset();
    e.be = std::make_unique<slack::SlackBackend>(e.store, app(), e.client, cr);
    REQUIRE(e.connect());
    const ConvRef c  = e.conv("C1");
    const int     h0 = Log().count("conversations.history", "channel", "C1");
    // What the shell does: the backend learns the chat is open, then the
    // list loads its head page; that load is the open's fetch, and the poll
    // waits its full gap after it.
    e.be->setActiveConversation(c, 0);
    REQUIRE(e.history(c, 0));
    fakeslack::pumpFor(150);
    CHECK(Log().count("conversations.history", "channel", "C1") == h0 + 1);
    // Then it polls as before.
    REQUIRE(pumpUntil(
        [&] { return Log().count("conversations.history", "channel", "C1") >= h0 + 2; }, 3000
    ));
}

TEST("slack read: a rate-limited call waits out Retry-After and retries") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c = e.addChannel("C1");
    script("conversations.history", R"([
      {"__status": 429, "__headers": {"Retry-After": "2"}, "ok": false, "error": "ratelimited"},
      {"ok": true, "has_more": false, "messages": [
        {"type": "message", "ts": "1700000100.000000", "user": "UME", "text": "made it"}]}
    ])");
    REQUIRE(e.history(c, 0));
    CHECK(e.store.conversation(c).messages.size() == 1);
    CHECK(Log().count("conversations.history") == 2);
}

TEST("slack read: a gateway page or a lost connection is retried, not the end") {
    if (!haveServer())
        return;
    Env           e(nullptr);
    const ConvRef c = e.addChannel("C1");
    script("conversations.history", R"([
      {"__status": 502, "__raw": "<html><body>502 Bad Gateway</body></html>"},
      {"__partial": true},
      {"__status": 503, "__raw": "upstream connect error"},
      {"__partial": true},
      {"ok": true, "has_more": false, "messages": [
        {"type": "message", "ts": "1700000100.000000", "user": "UME", "text": "made it"}]}
    ])");
    REQUIRE(e.history(c, 0));
    CHECK(e.store.conversation(c).messages.size() == 1);
    CHECK(Log().count("conversations.history") == 5);
}

TEST("slack read: dead credentials fail connect and report the loss") {
    if (!haveServer())
        return;
    Env e;
    script("auth.test", R"([{"ok": false, "error": "invalid_auth"}])");
    std::string err;
    CHECK(!e.connect(&err));
    CHECK_STR(err, "invalid_auth");
    CHECK(slack::SlackBackend::authError(err));
    REQUIRE(pumpUntil([&] { return !e.lostAuth.empty(); }, 2000));
    CHECK_STR(e.lostAuth, "invalid_auth");
    CHECK(Log().count("users.conversations") == 0);
}

TEST("slack read: Enterprise Grid lists through client.userBoot + im.list") {
    if (!haveServer())
        return;
    Env e;
    set(R"({"users.conversations": {"ok": false, "error": "enterprise_is_restricted"},
            "client.userBoot": {"ok": true, "channels": [
              {"id": "C1", "name": "general", "is_channel": true, "is_member": true},
              {"id": "C9", "name": "old", "is_channel": true, "is_archived": true}]},
            "im.list": {"ok": true, "ims": [{"id": "D1", "is_im": true, "user": "UMIRA"}],
                        "response_metadata": {"next_cursor": ""}}})");
    REQUIRE(e.connect());
    CHECK(e.conv("C1") != kNoConv && e.conv("D1") != kNoConv);
    CHECK(e.conv("C9") == kNoConv); // archived: left out, as exclude_archived did
    CHECK_STR(Log().get("im.list")["form"]["get_read_state"].str(), "true");
}

TEST("slack read: the Names preference — override × workspace default, as Slack decides") {
    using namespace slack::mapjson;
    const auto prefs = [](const char *text) {
        auto d = std::make_shared<json::Document>();
        CHECK(d->parse(std::string(text), nullptr));
        return d;
    };
    CHECK(namesOverride(prefs(R"({"display_real_names_override": 1})")->root()) == 1);
    CHECK(namesOverride(prefs(R"({"display_real_names_override": -1})")->root()) == -1);
    CHECK(namesOverride(prefs(R"({"display_real_names_override": 0})")->root()) == 0);
    CHECK(namesOverride(prefs(R"({"display_real_names_override": "-1"})")->root()) == -1);
    CHECK(namesOverride(prefs(R"({"display_real_names_override": 7})")->root()) == kNoPref);
    CHECK(namesOverride(prefs(R"({"display_display_names": true})")->root()) == kNoPref);
    CHECK(namesDefault(prefs(R"({"display_real_names": true})")->root()) == 1);
    CHECK(namesDefault(prefs(R"({"display_real_names": false})")->root()) == 0);
    CHECK(namesDefault(prefs(R"({})")->root()) == kNoPref);
    // Full names: override 1, or 0 with a full-names workspace; display
    // names: override -1, or 0 with a display-names one; else undecided.
    for (int d : {0, 1, kNoPref}) {
        CHECK(realNamesFrom(1, d) == 1);
        CHECK(realNamesFrom(-1, d) == 0);
        CHECK(realNamesFrom(kNoPref, d) == kNoPref);
    }
    CHECK(realNamesFrom(0, 1) == 1);
    CHECK(realNamesFrom(0, 0) == 0);
    CHECK(realNamesFrom(0, kNoPref) == kNoPref);
    // toUser keeps both names ("" is no name); displayName starts full-first.
    const auto u = toUser(prefs(R"({"id": "UMEG", "name": "meg",
                  "profile": {"real_name": " Meg Ryan ", "display_name": ""}})")
                              ->root());
    CHECK_STR(u.realName, "Meg Ryan");
    CHECK(u.profileName.empty());
    CHECK_STR(u.displayName, "Meg Ryan");
}

namespace {
// kWorkspace plus Meg, who has both names (Mira has a full name only,
// Jonas a display name only).
const char *kMeg = R"({"users.list": {"ok": true, "members": [
     {"id": "UME", "name": "me", "profile": {"real_name": "Me Myself"}},
     {"id": "UMIRA", "name": "mira", "profile": {"real_name": "Mira Okafor"}},
     {"id": "UJONAS", "name": "jonas", "profile": {"display_name": "Jonas"}},
     {"id": "UMEG", "name": "meg", "profile": {"real_name": "Meg Ryan", "display_name": "Meg"}}],
   "response_metadata": {"next_cursor": ""}}})";
} // namespace

TEST("slack read: names follow the user's Slack override, the local choice wins") {
    if (!haveServer())
        return;
    Env e;
    set(kMeg);
    set(R"({"users.prefs.get": {"ok": true, "prefs": {"display_real_names_override": -1}},
            "team.prefs.get": {"ok": true, "prefs": {"display_real_names": true}}})");
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return !e.store.realNames(); }, 3000));
    CHECK_STR(e.user("UMEG").displayName, "Meg");
    CHECK_STR(e.user("UMIRA").displayName, "Mira Okafor"); // no display name
    CHECK_STR(e.user("UJONAS").displayName, "Jonas");
    CHECK_STR(e.store.displayName(e.conv("D1")), "Mira Okafor");
    CHECK(Log().count("users.prefs.get") == 1);
    CHECK(Log().count("team.prefs.get") == 0); // the override decided
    // Settings → Names over Slack's, both ways, at once.
    e.be->setNamesMode(model::Backend::NamesMode::Full);
    CHECK(e.store.realNames());
    CHECK_STR(e.user("UMEG").displayName, "Meg Ryan");
    CHECK_STR(e.user("UJONAS").displayName, "Jonas"); // no full name
    e.be->setNamesMode(model::Backend::NamesMode::Display);
    CHECK_STR(e.user("UMEG").displayName, "Meg");
    e.be->setNamesMode(model::Backend::NamesMode::Service);
    CHECK(!e.store.realNames());
    CHECK(Log().count("users.prefs.get") == 1); // known already: not asked again
}

TEST("slack read: an override of 0 follows the workspace default (team.prefs.get)") {
    if (!haveServer())
        return;
    for (bool teamReal : {false, true}) {
        Env e;
        set(kMeg);
        set(std::string(
                R"({"users.prefs.get": {"ok": true, "prefs": {"display_real_names_override": 0}},
              "team.prefs.get": {"ok": true, "prefs": {"display_real_names": )"
            ) +
            (teamReal ? "true" : "false") + "}}}");
        REQUIRE(e.connect());
        REQUIRE(pumpUntil([&] { return Log().count("team.prefs.get") == 1; }, 3000));
        REQUIRE(pumpUntil([&] { return e.store.realNames() == teamReal; }, 3000));
        CHECK_STR(e.user("UMEG").displayName, teamReal ? "Meg Ryan" : "Meg");
    }
    // No pref in the answer (an old server): full names, nothing more asked.
    Env e;
    set(kMeg);
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("users.prefs.get") == 1; }, 3000));
    pumpUntil([] { return false; }, 200);
    CHECK(Log().count("team.prefs.get") == 0);
    CHECK(e.store.realNames());
    CHECK_STR(e.user("UMEG").displayName, "Meg Ryan");
}

TEST("slack read: Grid takes the workspace default from client.userBoot") {
    if (!haveServer())
        return;
    Env e;
    set(kMeg);
    set(R"({"users.conversations": {"ok": false, "error": "enterprise_is_restricted"},
            "client.userBoot": {"ok": true, "channels": [],
              "prefs": {"display_real_names_override": 0},
              "team": {"id": "T1", "prefs": {"display_real_names": false}}},
            "im.list": {"ok": true, "ims": [{"id": "D1", "is_im": true, "user": "UMIRA"}],
                        "response_metadata": {"next_cursor": ""}},
            "users.prefs.get": {"ok": true, "prefs": {"display_real_names_override": 0}},
            "team.prefs.get": {"ok": true, "prefs": {"display_real_names": true}}})");
    REQUIRE(e.connect());
    REQUIRE(pumpUntil([&] { return Log().count("users.prefs.get") == 1; }, 3000));
    pumpUntil([] { return false; }, 300);
    CHECK(!e.store.realNames());
    CHECK_STR(e.user("UMEG").displayName, "Meg");
    CHECK(Log().count("team.prefs.get") == 0); // userBoot had it
}

TEST("slack read: app keys never ask for the Names prefs; a local choice still applies") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/false);
    set(kMeg);
    REQUIRE(e.connect());
    pumpUntil([] { return false; }, 200);
    CHECK(Log().count("users.prefs.get") == 0 && Log().count("team.prefs.get") == 0);
    CHECK(e.store.realNames());
    CHECK_STR(e.user("UMEG").displayName, "Meg Ryan");
    e.be->setNamesMode(model::Backend::NamesMode::Display);
    CHECK_STR(e.user("UMEG").displayName, "Meg");
    e.be->setNamesMode(model::Backend::NamesMode::Service);
    CHECK_STR(e.user("UMEG").displayName, "Meg Ryan");
    CHECK(Log().count("users.prefs.get") == 0);
}

TEST("slack read: a local Names choice asks Slack nothing until switched back") {
    if (!haveServer())
        return;
    Env e;
    set(kMeg);
    set(R"({"users.prefs.get": {"ok": true, "prefs": {"display_real_names_override": -1}}})");
    e.be->setNamesMode(model::Backend::NamesMode::Full);
    REQUIRE(e.connect());
    pumpUntil([] { return false; }, 200);
    CHECK(Log().count("users.prefs.get") == 0);
    CHECK_STR(e.user("UMEG").displayName, "Meg Ryan");
    e.be->setNamesMode(model::Backend::NamesMode::Service);
    REQUIRE(pumpUntil([&] { return !e.store.realNames(); }, 3000));
    CHECK_STR(e.user("UMEG").displayName, "Meg");
    CHECK(Log().count("users.prefs.get") == 1);
}

TEST("slack read: app-key workspaces sweep DM activity instead of client.counts") {
    if (!haveServer())
        return;
    Env e(kWorkspace, /*session=*/false);
    set(
        R"({"conversations.info?channel=D1": {"ok": true, "channel": {"id": "D1", "is_im": true,
              "user": "UMIRA", "last_read": "1700000000.000000", "latest": "1700000500.000000"}}})"
    );
    REQUIRE(e.connect());
    REQUIRE(pumpUntil(
        [&] {
            return e.store.conversation(e.conv("D1")).latest == model::parseTs("1700000500.000000");
        },
        3000
    ));
    CHECK(Log().count("client.counts") == 0);
    CHECK(!e.be->capabilities().threadsView && !e.be->capabilities().messageReminders);
}

TEST("slack read: mappers — blocks, rich text, message_changed, conversations") {
    model::Store   store;
    json::Document d;
    REQUIRE(d.parse(
        std::string_view(
            R"({"subtype": "message_changed", "message": {
        "ts": "1.000001", "user": "U1", "text": "", "blocks": [{"type": "rich_text", "elements": [
          {"type": "rich_text_section", "elements": [
            {"type": "text", "text": "hey "}, {"type": "user", "user_id": "U2"},
            {"type": "text", "text": " bold ", "style": {"bold": true}},
            {"type": "link", "url": "https://x.io", "text": "x"},
            {"type": "emoji", "name": "wave", "skin_tone": 3}]},
          {"type": "rich_text_list", "style": "ordered", "elements": [
            {"type": "rich_text_section", "elements": [{"type": "text", "text": "a"}]},
            {"type": "rich_text_section", "elements": [{"type": "text", "text": "b"}]}]},
          {"type": "rich_text_preformatted", "elements": [{"type": "text", "text": "code"}]}]}]}})"
        ),
        nullptr
    ));
    const model::Message m = slack::mapjson::toMessage(d.root(), store);
    CHECK(m.ts == model::parseTs("1.000001"));
    CHECK_STR(
        m.text, "hey <@U2> *bold* <https://x.io|x>:wave::skin-tone-3:\n1. a\n2. b\n```code```"
    );
    // rich_text alongside a text keeps the text (the blocks mirror it).
    REQUIRE(d.parse(
        std::string_view(R"({"ts": "2.0", "text": "*raw*", "blocks": [
        {"type": "rich_text", "elements": []}]})"),
        nullptr
    ));
    CHECK_STR(slack::mapjson::toMessage(d.root(), store).text, "*raw*");
    // A flattened text (line breaks gone) gives way to the blocks' lines,
    // their raw & < > escaped like text.
    REQUIRE(d.parse(
        std::string_view(
            R"({"ts": "3.0", "text": "a &amp; b.  did:  • x -&gt; y • z  end", "blocks": [
        {"type": "rich_text", "elements": [
          {"type": "rich_text_section", "elements": [{"type": "text", "text": "a & b.\n\ndid:"}]},
          {"type": "rich_text_list", "style": "bullet", "indent": 0, "elements": [
            {"type": "rich_text_section", "elements": [{"type": "text", "text": "x -> y"}]},
            {"type": "rich_text_section", "elements": [{"type": "text", "text": "z"}]}]},
          {"type": "rich_text_section", "elements": [{"type": "text", "text": "end"}]}]}]})"
        ),
        nullptr
    ));
    CHECK_STR(
        slack::mapjson::toMessage(d.root(), store).text, "a &amp; b.\n\ndid:\n• x -&gt; y\n• z\nend"
    );
    REQUIRE(d.parse(
        std::string_view(R"({"id": "C5", "name": "dev", "is_private": true,
        "last_read": "10.0", "latest": {"ts": "20.0"}, "unread_count": 0,
        "notification_preference": "mentions",
        "properties": {"canvas": {"file_id": "F9"}}})"),
        nullptr
    ));
    const model::Conversation c = slack::mapjson::toConversation(d.root(), store);
    CHECK(c.kind == model::ConvKind::Private);
    CHECK(c.unread == 1); // latest > last_read stands in for a missing count
    CHECK(c.notify == model::NotifyLevel::Mentions);
    CHECK(!c.canvasTitle.empty());
    CHECK_STR(c.canvasId, "F9");
}

#endif // !_WIN32

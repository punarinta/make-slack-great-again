// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "backend/public_backend/json_mappers.h"

static QJsonObject obj(const char *json) {
    return QJsonDocument::fromJson(json).object();
}

static QJsonArray arr(const char *json) {
    return QJsonDocument::fromJson(json).array();
}

// ── toUser ──────────────────────────────────────────────────────────────────

TEST_CASE("toUser full profile", "[mappers][user]") {
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U123", "name": "alice.smith",
        "is_bot": false, "deleted": false,
        "profile": {
            "display_name": "Alice",
            "real_name": "Alice Smith",
            "image_72": "https://img.example.com/av.jpg"
        }
    })"));
    CHECK(u.id == UserId{"U123"});
    CHECK(u.name == "alice.smith");
    CHECK(u.displayName == "Alice");
    CHECK(u.avatarUrl == "https://img.example.com/av.jpg");
    CHECK(!u.isBot);
    CHECK(!u.isDeactivated);
}

TEST_CASE("toUser display_name empty falls back to real_name", "[mappers][user]") {
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U1", "name": "u1",
        "profile": {"display_name": "  ", "real_name": "Real Name", "image_72": ""}
    })"));
    CHECK(u.displayName == "Real Name");
}

TEST_CASE("toUser both names empty falls back to name field", "[mappers][user]") {
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U1", "name": "fallback",
        "profile": {"display_name": "", "real_name": "", "image_72": ""}
    })"));
    CHECK(u.displayName == "fallback");
}

TEST_CASE("toUser is_bot flag", "[mappers][user]") {
    auto u = JsonMappers::toUser(obj(R"({
        "id": "B1", "name": "slackbot", "is_bot": true, "deleted": false,
        "profile": {}
    })"));
    CHECK(u.isBot);
}

TEST_CASE("toUser deleted flag maps to isDeactivated", "[mappers][user]") {
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U2", "name": "ex", "deleted": true, "profile": {}
    })"));
    CHECK(u.isDeactivated);
}

// ── toConversation ───────────────────────────────────────────────────────────

TEST_CASE("toConversation public channel", "[mappers][conv]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "is_member": true, "last_read": "100.000", "unread_count": 3
    })"));
    CHECK(c.id == ConversationId{"C1"});
    CHECK(c.kind == ConvKind::PublicChannel);
    CHECK(c.name == "general");
    CHECK(c.isMember);
    CHECK(c.unread == 3);
    CHECK(c.lastRead == "100.000");
    CHECK(!c.dmUser.has_value());
}

TEST_CASE("toConversation private channel", "[mappers][conv]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "G1", "name": "secret",
        "is_private": true, "is_im": false, "is_mpim": false
    })"));
    CHECK(c.kind == ConvKind::PrivateChannel);
    CHECK(!c.dmUser.has_value());
}

TEST_CASE("toConversation IM sets kind and dmUser", "[mappers][conv]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "D1", "is_im": true, "is_mpim": false, "is_private": false,
        "user": "U456"
    })"));
    CHECK(c.kind == ConvKind::Im);
    REQUIRE(c.dmUser.has_value());
    CHECK(c.dmUser->value == "U456");
}

// ── toMessage ────────────────────────────────────────────────────────────────

TEST_CASE("toMessage basic fields", "[mappers][message]") {
    auto m = JsonMappers::toMessage(obj(R"({
        "ts": "123.456", "user": "U789", "text": "hello"
    })"));
    CHECK(m.ts == "123.456");
    CHECK(m.author == UserId{"U789"});
    CHECK(m.text.text == "hello");
    CHECK(!m.threadRoot.has_value());
    CHECK(!m.edited);
    CHECK(!m.subtype.has_value());
}

TEST_CASE("toMessage thread reply sets threadRoot", "[mappers][message]") {
    auto m = JsonMappers::toMessage(obj(R"({
        "ts": "200.000", "thread_ts": "100.000", "user": "U1", "text": ""
    })"));
    REQUIRE(m.threadRoot.has_value());
    CHECK(*m.threadRoot == "100.000");
}

TEST_CASE("toMessage thread root ts==thread_ts gives no threadRoot", "[mappers][message]") {
    auto m = JsonMappers::toMessage(obj(R"({
        "ts": "100.000", "thread_ts": "100.000", "user": "U1", "text": "",
        "reply_count": 5
    })"));
    CHECK(!m.threadRoot.has_value());
    CHECK(m.replyCount == 5);
}

TEST_CASE("toMessage message_changed unpacks nested message", "[mappers][message]") {
    auto m = JsonMappers::toMessage(obj(R"({
        "subtype": "message_changed",
        "message": {"ts": "50.000", "user": "U5", "text": "edited text"}
    })"));
    CHECK(m.ts == "50.000");
    CHECK(m.text.text == "edited text");
}

TEST_CASE("toMessage reactions", "[mappers][message]") {
    auto m = JsonMappers::toMessage(obj(R"({
        "ts": "1.0", "user": "U1", "text": "",
        "reactions": [
            {"name": "thumbsup", "count": 2, "users": ["U1", "U2"]}
        ]
    })"));
    REQUIRE(m.reactions.size() == 1);
    CHECK(m.reactions[0].name == "thumbsup");
    CHECK(m.reactions[0].count == 2);
    REQUIRE(m.reactions[0].users.size() == 2);
    CHECK(m.reactions[0].users[0] == UserId{"U1"});
    CHECK(m.reactions[0].users[1] == UserId{"U2"});
}

TEST_CASE("toMessage uses bot_id when user field absent", "[mappers][message]") {
    auto m = JsonMappers::toMessage(obj(R"({
        "ts": "1.0", "bot_id": "B123", "text": "bot says hi"
    })"));
    CHECK(m.author == UserId{"B123"});
}

TEST_CASE("toMessage edited flag set when 'edited' key present", "[mappers][message]") {
    auto m = JsonMappers::toMessage(obj(R"({
        "ts": "1.0", "user": "U1", "text": "updated",
        "edited": {"user": "U1", "ts": "2.0"}
    })"));
    CHECK(m.edited);
}

// ── toFile ────────────────────────────────────────────────────────────────────

TEST_CASE("toFile basic fields", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F001", "name": "report.pdf",
        "mimetype": "application/pdf", "pretty_type": "PDF",
        "url_private": "https://files.slack.com/report.pdf",
        "permalink": "https://workspace.slack.com/files/report",
        "thumb_360": "https://thumb.example.com/360.png",
        "original_w": 1920, "original_h": 1080,
        "size": 204800
    })"));
    CHECK(f.id          == "F001");
    CHECK(f.name        == "report.pdf");
    CHECK(f.mimeType    == "application/pdf");
    CHECK(f.prettyType  == "PDF");
    CHECK(f.urlPrivate  == "https://files.slack.com/report.pdf");
    CHECK(f.permalink   == "https://workspace.slack.com/files/report");
    CHECK(f.thumbUrl    == "https://thumb.example.com/360.png");
    CHECK(f.imageWidth  == 1920);
    CHECK(f.imageHeight == 1080);
    CHECK(f.size        == 204800);
}

TEST_CASE("toFile thumb_360 preferred over thumb_480", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "thumb_360": "https://360.png", "thumb_480": "https://480.png"
    })"));
    CHECK(f.thumbUrl == "https://360.png");
}

TEST_CASE("toFile falls back to thumb_480 when thumb_360 absent", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "thumb_480": "https://480.png"
    })"));
    CHECK(f.thumbUrl == "https://480.png");
}

TEST_CASE("toFile original dimensions preferred over thumb dimensions", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "original_w": 800, "original_h": 600,
        "thumb_360_w": 360, "thumb_360_h": 270
    })"));
    CHECK(f.imageWidth  == 800);
    CHECK(f.imageHeight == 600);
}

TEST_CASE("toFile falls back to thumb_360 dimensions", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "thumb_360_w": 360, "thumb_360_h": 270
    })"));
    CHECK(f.imageWidth  == 360);
    CHECK(f.imageHeight == 270);
}

TEST_CASE("toFile large size preserved as qint64", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({"id":"F1","size":3000000000})"));
    CHECK(f.size == 3000000000LL);
}

// ── toBlock ───────────────────────────────────────────────────────────────────

TEST_CASE("toBlock divider has typeStr only", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({"type":"divider"})"));
    CHECK(b.typeStr == "divider");
    CHECK(b.text.text.isEmpty());
    CHECK(b.imageUrl.isEmpty());
}

TEST_CASE("toBlock section with mrkdwn text", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "section",
        "text": {"type": "mrkdwn", "text": "*bold*"}
    })"));
    CHECK(b.typeStr == "section");
    CHECK(b.text.text == "bold");
    REQUIRE(b.text.entities.size() == 1);
    CHECK(b.text.entities[0].type == EntityType::Bold);
}

TEST_CASE("toBlock header with plain_text", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "header",
        "text": {"type": "plain_text", "text": "My Header"}
    })"));
    CHECK(b.typeStr        == "header");
    CHECK(b.text.text      == "My Header");
    CHECK(b.text.entities.empty());
}

TEST_CASE("toBlock image block", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "image",
        "image_url": "https://img.example.com/pic.png",
        "alt_text": "a picture"
    })"));
    CHECK(b.typeStr  == "image");
    CHECK(b.imageUrl == "https://img.example.com/pic.png");
    CHECK(b.altText  == "a picture");
    CHECK(b.text.text.isEmpty());
}

TEST_CASE("toBlock image block with title", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "image",
        "image_url": "https://img.example.com/pic.png",
        "alt_text": "alt",
        "title": {"type": "plain_text", "text": "Title text"}
    })"));
    CHECK(b.text.text == "Title text");
}

TEST_CASE("toBlock context concatenates text elements with separator", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "context",
        "elements": [
            {"type": "mrkdwn",     "text": "first"},
            {"type": "plain_text", "text": "second"}
        ]
    })"));
    CHECK(b.text.text == "first  second");
}

TEST_CASE("toBlock actions formats button labels", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "actions",
        "elements": [
            {"text": {"type": "plain_text", "text": "Approve"}},
            {"text": {"type": "plain_text", "text": "Deny"}}
        ]
    })"));
    CHECK(b.text.text == "[Approve]  [Deny]");
}

TEST_CASE("toBlock rich_text section plain text", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "rich_text",
        "elements": [{
            "type": "rich_text_section",
            "elements": [{"type": "text", "text": "hello"}]
        }]
    })"));
    CHECK(b.text.text == "hello");
    CHECK(b.text.entities.empty());
}

TEST_CASE("toBlock rich_text section bold inline", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "rich_text",
        "elements": [{
            "type": "rich_text_section",
            "elements": [{"type": "text", "text": "hi", "style": {"bold": true}}]
        }]
    })"));
    CHECK(b.text.text == "hi");
    REQUIRE(b.text.entities.size() == 1);
    CHECK(b.text.entities[0].type   == EntityType::Bold);
    CHECK(b.text.entities[0].offset == 0);
    CHECK(b.text.entities[0].length == 2);
}

TEST_CASE("toBlock rich_text user mention", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "rich_text",
        "elements": [{
            "type": "rich_text_section",
            "elements": [{"type": "user", "user_id": "U999"}]
        }]
    })"));
    CHECK(b.text.text == "@U999");
    REQUIRE(b.text.entities.size() == 1);
    CHECK(b.text.entities[0].type == EntityType::UserMention);
    CHECK(b.text.entities[0].data == "U999");
    CHECK(b.text.entities[0].length == 5);
}

TEST_CASE("toBlock rich_text link with label", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "rich_text",
        "elements": [{
            "type": "rich_text_section",
            "elements": [{"type": "link", "url": "https://example.com", "text": "click"}]
        }]
    })"));
    CHECK(b.text.text == "click");
    REQUIRE(b.text.entities.size() == 1);
    CHECK(b.text.entities[0].type == EntityType::Link);
    CHECK(b.text.entities[0].data == "https://example.com");
    CHECK(b.text.entities[0].length == 5);
}

TEST_CASE("toBlock rich_text link no label falls back to url", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "rich_text",
        "elements": [{
            "type": "rich_text_section",
            "elements": [{"type": "link", "url": "https://example.com"}]
        }]
    })"));
    CHECK(b.text.text == "https://example.com");
    REQUIRE(b.text.entities.size() == 1);
    CHECK(b.text.entities[0].type == EntityType::Link);
    CHECK(b.text.entities[0].length == 19);
}

TEST_CASE("toBlock rich_text preformatted", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "rich_text",
        "elements": [{
            "type": "rich_text_preformatted",
            "elements": [{"type": "text", "text": "code here"}]
        }]
    })"));
    CHECK(b.text.text == "code here");
    REQUIRE(b.text.entities.size() == 1);
    CHECK(b.text.entities[0].type   == EntityType::Pre);
    CHECK(b.text.entities[0].offset == 0);
    CHECK(b.text.entities[0].length == 9);
}

TEST_CASE("toBlock rich_text quote", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "rich_text",
        "elements": [{
            "type": "rich_text_quote",
            "elements": [{"type": "text", "text": "quoted"}]
        }]
    })"));
    CHECK(b.text.text == "quoted");
    REQUIRE(b.text.entities.size() == 1);
    CHECK(b.text.entities[0].type   == EntityType::Blockquote);
    CHECK(b.text.entities[0].offset == 0);
    CHECK(b.text.entities[0].length == 6);
}

TEST_CASE("toBlock rich_text bullet list", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "rich_text",
        "elements": [{
            "type": "rich_text_list",
            "style": "bullet",
            "elements": [
                {"type": "rich_text_section", "elements": [{"type": "text", "text": "alpha"}]},
                {"type": "rich_text_section", "elements": [{"type": "text", "text": "beta"}]}
            ]
        }]
    })"));
    CHECK(b.text.text == "• alpha\n• beta");
}

TEST_CASE("toBlock rich_text ordered list", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "rich_text",
        "elements": [{
            "type": "rich_text_list",
            "style": "ordered",
            "elements": [
                {"type": "rich_text_section", "elements": [{"type": "text", "text": "first"}]},
                {"type": "rich_text_section", "elements": [{"type": "text", "text": "second"}]}
            ]
        }]
    })"));
    CHECK(b.text.text == "1. first\n2. second");
}

// ── toAttachment ──────────────────────────────────────────────────────────────

TEST_CASE("toAttachment all fields", "[mappers][attachment]") {
    auto a = JsonMappers::toAttachment(obj(R"({
        "fallback": "fb", "color": "#36a64f", "pretext": "pre",
        "author_name": "Author", "title": "My title",
        "title_link": "https://link.example.com",
        "text": "plain body",
        "image_url": "https://img.example.com/img.png",
        "thumb_url": "https://img.example.com/thumb.png",
        "footer": "Posted via App"
    })"));
    CHECK(a.fallback   == "fb");
    CHECK(a.color      == "#36a64f");
    CHECK(a.pretext    == "pre");
    CHECK(a.authorName == "Author");
    CHECK(a.title      == "My title");
    CHECK(a.titleLink  == "https://link.example.com");
    CHECK(a.text.text  == "plain body");
    CHECK(a.imageUrl   == "https://img.example.com/img.png");
    CHECK(a.thumbUrl   == "https://img.example.com/thumb.png");
    CHECK(a.footer     == "Posted via App");
}

TEST_CASE("toAttachment text is parsed as mrkdwn", "[mappers][attachment]") {
    auto a = JsonMappers::toAttachment(obj(R"({"text": "<@U1|alice> check *this*"})"));
    CHECK(a.text.text == "alice check this");
    REQUIRE(a.text.entities.size() == 2);
    CHECK(a.text.entities[0].type == EntityType::UserMention);
    CHECK(a.text.entities[1].type == EntityType::Bold);
}

// ── toSearchResult ────────────────────────────────────────────────────────────

TEST_CASE("toSearchResult extracts conv from nested channel object", "[mappers][search]") {
    auto r = JsonMappers::toSearchResult(obj(R"({
        "channel": {"id": "C001", "name": "general"},
        "ts": "123.456", "user": "U1", "text": "found it"
    })"));
    CHECK(r.conv     == ConversationId{"C001"});
    CHECK(r.convName == "general");
    CHECK(r.msg.ts   == "123.456");
    CHECK(r.msg.text.text == "found it");
}

// ── Batch helpers ─────────────────────────────────────────────────────────────

TEST_CASE("toUsers skips entries with empty id", "[mappers][batch]") {
    auto users = JsonMappers::toUsers(arr(R"([
        {"id":"U1","name":"alice","profile":{"display_name":"Alice","real_name":"","image_72":""}},
        {"id":"",  "name":"bad", "profile":{}},
        {"id":"U2","name":"bob", "profile":{"display_name":"Bob","real_name":"","image_72":""}}
    ])"));
    REQUIRE(users.size() == 2);
    CHECK(users[0].id == UserId{"U1"});
    CHECK(users[1].id == UserId{"U2"});
}

TEST_CASE("toConversations skips entries with empty id", "[mappers][batch]") {
    auto convs = JsonMappers::toConversations(arr(R"([
        {"id":"C1","name":"general","is_private":false,"is_im":false,"is_mpim":false},
        {"id":"",  "name":"bad"},
        {"id":"C2","name":"random","is_private":false,"is_im":false,"is_mpim":false}
    ])"));
    REQUIRE(convs.size() == 2);
    CHECK(convs[0].id == ConversationId{"C1"});
    CHECK(convs[1].id == ConversationId{"C2"});
}

TEST_CASE("toMessages reverseOrder=true reverses the array", "[mappers][batch]") {
    auto msgs = JsonMappers::toMessages(arr(R"([
        {"ts":"1.000","user":"U1","text":"first"},
        {"ts":"2.000","user":"U1","text":"second"}
    ])"), true);
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].ts == "2.000");
    CHECK(msgs[1].ts == "1.000");
}

TEST_CASE("toMessages reverseOrder=false preserves array order", "[mappers][batch]") {
    auto msgs = JsonMappers::toMessages(arr(R"([
        {"ts":"1.000","user":"U1","text":"first"},
        {"ts":"2.000","user":"U1","text":"second"}
    ])"), false);
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].ts == "1.000");
    CHECK(msgs[1].ts == "2.000");
}

TEST_CASE("toSearchResults skips entries with empty conv id", "[mappers][batch]") {
    auto results = JsonMappers::toSearchResults(arr(R"([
        {"channel":{"id":"C1","name":"general"},"ts":"1.0","user":"U1","text":"hi"},
        {"channel":{"id":"",  "name":"bad"},    "ts":"2.0","user":"U1","text":"bye"}
    ])"));
    REQUIRE(results.size() == 1);
    CHECK(results[0].conv == ConversationId{"C1"});
}

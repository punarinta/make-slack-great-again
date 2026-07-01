// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "backend/slack/json_mappers.h"

using namespace slack;

static QJsonObject obj(const char *json) {
    return QJsonDocument::fromJson(json).object();
}

static QJsonArray arr(const char *json) {
    return QJsonDocument::fromJson(json).array();
}

// ── toUser ──────────────────────────────────────────────────────────────────

TEST_CASE("toUser full profile — real_name preferred over display_name", "[mappers][user]") {
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
    CHECK(u.displayName == "Alice Smith");
    CHECK(u.avatarUrl == "https://img.example.com/av.jpg");
    CHECK(!u.isBot);
    CHECK(!u.isDeactivated);
}

TEST_CASE(
    "toUser enterprise slug: real_name wins over AD-provisioned display_name", "[mappers][user]"
) {
    // Enterprise workspaces often auto-provision display_name from AD as a username
    // slug (e.g. "john.doe.dept") while real_name holds the human-readable full name.
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U1", "name": "john.doe.eng",
        "profile": {"display_name": "john.doe.eng", "real_name": "John Doe", "image_72": ""}
    })"));
    CHECK(u.displayName == "John Doe");
}

TEST_CASE("toUser real_name empty falls back to display_name", "[mappers][user]") {
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U1", "name": "u1",
        "profile": {"display_name": "Nickname", "real_name": "  ", "image_72": ""}
    })"));
    CHECK(u.displayName == "Nickname");
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

TEST_CASE("toUser profile-card fields: owner, title, tz_offset", "[mappers][user]") {
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U3", "name": "stefan", "is_owner": true, "tz_offset": 7200,
        "profile": {"real_name": "Stefan Möller", "title": "CTO"}
    })"));
    CHECK(u.isOwner);
    CHECK(u.isAdmin); // is_owner implies the admin role label fallback
    CHECK(u.title == "CTO");
    CHECK(u.hasTz);
    CHECK(u.tzOffset == 7200);
}

TEST_CASE("toUser missing tz_offset leaves hasTz false", "[mappers][user]") {
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U4", "name": "bot", "is_bot": true, "profile": {}
    })"));
    CHECK(!u.isOwner);
    CHECK(!u.hasTz);
    CHECK(u.tzOffset == 0);
}

TEST_CASE("toUser status_emoji simple — colons stripped", "[mappers][user][status]") {
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U1", "name": "u1",
        "profile": {"display_name": "Alice", "real_name": "", "image_72": "",
                    "status_emoji": ":palm_tree:", "status_text": "On vacation"}
    })"));
    CHECK(u.statusEmoji == "palm_tree");
    CHECK(u.statusText == "On vacation");
}

TEST_CASE("toUser status_emoji absent — empty string", "[mappers][user][status]") {
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U1", "name": "u1",
        "profile": {"display_name": "Alice", "real_name": "", "image_72": ""}
    })"));
    CHECK(u.statusEmoji.isEmpty());
    CHECK(u.statusText.isEmpty());
}

TEST_CASE(
    "toUser status_emoji skin-tone modifier stripped — sidebar regression",
    "[mappers][user][status]"
) {
    // Slack encodes a skin-toned emoji as ":baby::skin-tone-3:". After stripping
    // outer colons we get "baby::skin-tone-3". The skin-tone suffix must be removed
    // so that the base name "baby" resolves to the 👶 glyph instead of falling back
    // to the literal string ":baby::skin-tone-3:" which squeezes the user name out
    // of the conv-list row entirely.
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U1", "name": "u1",
        "profile": {"display_name": "Petter", "real_name": "", "image_72": "",
                    "status_emoji": ":baby::skin-tone-3:", "status_text": ""}
    })"));
    CHECK(u.statusEmoji == "baby");
}

TEST_CASE("toUser status_emoji without colons stored as-is", "[mappers][user][status]") {
    // Some clients omit the surrounding colons.
    auto u = JsonMappers::toUser(obj(R"({
        "id": "U1", "name": "u1",
        "profile": {"display_name": "Bob", "real_name": "", "image_72": "",
                    "status_emoji": "wave", "status_text": ""}
    })"));
    CHECK(u.statusEmoji == "wave");
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

TEST_CASE("toConversation is_muted true", "[mappers][conv]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "muted-channel",
        "is_private": false, "is_im": false, "is_mpim": false,
        "is_muted": true
    })"));
    CHECK(c.isMuted);
}

TEST_CASE("toConversation is_muted defaults to false", "[mappers][conv]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "normal",
        "is_private": false, "is_im": false, "is_mpim": false
    })"));
    CHECK(!c.isMuted);
}

TEST_CASE("toConversation active huddle: flag, link, participants", "[mappers][conv][huddle]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "room": {
            "call_family": "huddle", "has_ended": false, "date_end": 0,
            "huddle_link": "https://app.slack.com/huddle/T1/C1",
            "created_by": "UHOST", "participants": ["U1", "U2"]
        }
    })"));
    CHECK(c.huddleActive);
    CHECK(c.huddleLink == "https://app.slack.com/huddle/T1/C1");
    REQUIRE(c.huddleParticipants.size() == 2);
    CHECK(c.huddleParticipants[0] == UserId{"U1"});
}
TEST_CASE("toConversation ended huddle (has_ended) is not active", "[mappers][conv][huddle]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "room": { "call_family": "huddle", "has_ended": true, "date_end": 1700000000 }
    })"));
    CHECK(!c.huddleActive);
}
TEST_CASE(
    "toConversation ended huddle (string date_end) is not active", "[mappers][conv][huddle]"
) {
    // Slack sends date_end as a JSON string in some payloads; QJsonValue::toDouble
    // yields 0 for a string, which used to read as "still live" and stranded the
    // huddle banner forever after an end-edit arrived.
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "room": { "call_family": "huddle", "date_end": "1700000000" }
    })"));
    CHECK(!c.huddleActive);
}
TEST_CASE(
    "toConversation prewarmed huddle is active, falls back to host", "[mappers][conv][huddle]"
) {
    // A freshly-started "prewarmed" huddle has no participants yet but is live.
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "room": {
            "call_family": "huddle", "has_ended": false, "date_end": 0,
            "created_by": "UHOST", "participants": []
        }
    })"));
    CHECK(c.huddleActive);
    REQUIRE(c.huddleParticipants.size() == 1);
    CHECK(c.huddleParticipants[0] == UserId{"UHOST"}); // host shown until someone joins
}
TEST_CASE("toConversation third-party call is not a huddle", "[mappers][conv][huddle]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "room": { "call_family": "call", "date_end": 0, "participants": ["U1"] }
    })"));
    CHECK(!c.huddleActive);
}
TEST_CASE("toConversation no room means no huddle", "[mappers][conv][huddle]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false
    })"));
    CHECK(!c.huddleActive);
    CHECK(c.huddleParticipants.empty());
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

TEST_CASE("toConversation parses channel canvas properties", "[mappers][conv][canvas]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "properties": {"canvas": {"file_id": "F123ABC456", "is_empty": true,
                                  "quip_thread_id": "JAB1CDefGhI"}}
    })"));
    CHECK(c.canvasFileId == "F123ABC456");
    CHECK(c.canvasIsEmpty);
}

TEST_CASE(
    "toConversation finds free-team canvas tab under properties.tabs", "[mappers][conv][canvas]"
) {
    // Observed shape on a free workspace: no properties.canvas, the channel
    // canvas appears only as a tabs[] entry (verified against a real team).
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "test-channel-1",
        "is_private": false, "is_im": false, "is_mpim": false,
        "properties": {"tabs": [
            {"id": "Ct0AAA", "type": "bookmarks", "data": {}},
            {"id": "Ct0BBB", "type": "canvas",
             "data": {"file_id": "F0BAXNFLR5W", "shared_ts": "1781244767.317469"},
             "label": ""}
        ]}
    })"));
    CHECK(c.canvasFileId == "F0BAXNFLR5W");
    CHECK(!c.canvasIsEmpty);
}

TEST_CASE("toConversation prefers properties.canvas over tabs", "[mappers][conv][canvas]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "chan",
        "is_private": false, "is_im": false, "is_mpim": false,
        "properties": {
            "canvas": {"file_id": "F_MAIN", "is_empty": true},
            "tabs": [{"id": "Ct1", "type": "canvas", "data": {"file_id": "F_TAB"}}]
        }
    })"));
    CHECK(c.canvasFileId == "F_MAIN");
    CHECK(c.canvasIsEmpty);
}

TEST_CASE("toConversation without canvas leaves fields empty", "[mappers][conv][canvas]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false
    })"));
    CHECK(c.canvasFileId.isEmpty());
    CHECK(!c.canvasIsEmpty);
}

// ── toCanvasChanges ──────────────────────────────────────────────────────────

TEST_CASE("toCanvasChanges maps ops and document content", "[mappers][canvas]") {
    const auto arr = JsonMappers::toCanvasChanges({
        {.op = CanvasChange::Op::InsertAtEnd, .sectionId = {}, .markdown = "- [ ] task"},
        {.op = CanvasChange::Op::ReplaceSection, .sectionId = "temp:C:abc", .markdown = "## Done"},
        {.op = CanvasChange::Op::DeleteSection, .sectionId = "temp:C:xyz", .markdown = {}},
    });
    REQUIRE(arr.size() == 3);

    const auto a = arr.at(0).toObject();
    CHECK(a.value("operation").toString() == "insert_at_end");
    CHECK(!a.contains("section_id"));
    CHECK(a.value("document_content").toObject().value("type").toString() == "markdown");
    CHECK(a.value("document_content").toObject().value("markdown").toString() == "- [ ] task");

    const auto b = arr.at(1).toObject();
    CHECK(b.value("operation").toString() == "replace");
    CHECK(b.value("section_id").toString() == "temp:C:abc");

    const auto d = arr.at(2).toObject();
    CHECK(d.value("operation").toString() == "delete");
    CHECK(d.value("section_id").toString() == "temp:C:xyz");
    CHECK(!d.contains("document_content"));
}

TEST_CASE("toCanvasChanges rename uses title_content", "[mappers][canvas]") {
    const auto arr = JsonMappers::toCanvasChanges({
        {.op = CanvasChange::Op::Rename, .sectionId = {}, .markdown = "Project Status"},
    });
    REQUIRE(arr.size() == 1);

    const auto r = arr.at(0).toObject();
    CHECK(r.value("operation").toString() == "rename");
    CHECK(!r.contains("section_id"));
    CHECK(!r.contains("document_content"));
    CHECK(r.value("title_content").toObject().value("type").toString() == "markdown");
    CHECK(r.value("title_content").toObject().value("markdown").toString() == "Project Status");
}

TEST_CASE("toCanvasChanges whole-document replace omits section_id", "[mappers][canvas]") {
    const auto arr = JsonMappers::toCanvasChanges({
        {.op = CanvasChange::Op::ReplaceAll, .sectionId = {}, .markdown = "# Fresh\n\nbody"},
    });
    REQUIRE(arr.size() == 1);

    const auto r = arr.at(0).toObject();
    CHECK(r.value("operation").toString() == "replace");
    CHECK(!r.contains("section_id"));
    CHECK(r.value("document_content").toObject().value("markdown").toString() == "# Fresh\n\nbody");
}

TEST_CASE("toConversation unread uses unread_count when non-zero", "[mappers][conv][unread]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "busy",
        "is_private": false, "is_im": false, "is_mpim": false,
        "last_read": "100.000000",
        "latest": {"ts": "200.000000"},
        "unread_count": 5
    })"));
    CHECK(c.unread == 5);
}

TEST_CASE(
    "toConversation unread falls back to 1 when latestTs > lastRead and count is 0",
    "[mappers][conv][unread]"
) {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "channel",
        "is_private": false, "is_im": false, "is_mpim": false,
        "last_read": "100.000000",
        "latest": {"ts": "200.000000"},
        "unread_count": 0
    })"));
    CHECK(c.unread == 1);
}

TEST_CASE("toConversation unread is 0 when latestTs <= lastRead", "[mappers][conv][unread]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "read-channel",
        "is_private": false, "is_im": false, "is_mpim": false,
        "last_read": "200.000000",
        "latest": {"ts": "200.000000"},
        "unread_count": 0
    })"));
    CHECK(c.unread == 0);
}

TEST_CASE("toConversation unread is 0 when latestTs missing", "[mappers][conv][unread]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "empty",
        "is_private": false, "is_im": false, "is_mpim": false,
        "last_read": "100.000000",
        "unread_count": 0
    })"));
    CHECK(c.unread == 0);
}

TEST_CASE("toConversation unread is 0 when lastRead missing", "[mappers][conv][unread]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "fresh",
        "is_private": false, "is_im": false, "is_mpim": false,
        "latest": {"ts": "200.000000"},
        "unread_count": 0
    })"));
    CHECK(c.unread == 0);
}

TEST_CASE(
    "toConversation latestTs from IM string shape triggers fallback", "[mappers][conv][unread]"
) {
    // DMs return latest as a bare ts string, not an object
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "D1", "is_im": true, "is_mpim": false, "is_private": false,
        "user": "U1",
        "last_read": "100.000000",
        "latest": "200.000000",
        "unread_count": 0
    })"));
    CHECK(c.unread == 1);
}

TEST_CASE("toConversation mentionCount is mapped", "[mappers][conv][unread]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "mentions",
        "is_private": false, "is_im": false, "is_mpim": false,
        "unread_count": 3, "mention_count": 2
    })"));
    CHECK(c.unread == 3);
    CHECK(c.mentionCount == 2);
}

TEST_CASE("toConversation mentionCount defaults to 0 when absent", "[mappers][conv][unread]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "no-mentions",
        "is_private": false, "is_im": false, "is_mpim": false
    })"));
    CHECK(c.mentionCount == 0);
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

TEST_CASE("toMessage thread reply parses parent_user_id", "[mappers][message]") {
    auto m = JsonMappers::toMessage(obj(R"({
        "ts": "200.000", "thread_ts": "100.000", "user": "U2",
        "parent_user_id": "U1", "text": "reply"
    })"));
    CHECK(m.parentUserId == UserId{"U1"}); // author of the thread root
    // isFollowedThreadReply keys off this: U1's own thread got a reply from U2.
    CHECK(isFollowedThreadReply(m, UserId{"U1"}));
    CHECK_FALSE(isFollowedThreadReply(m, UserId{"U2"}));
}

TEST_CASE(
    "toMessage non-reply has no parent_user_id, is not a followed reply", "[mappers][message]"
) {
    auto m = JsonMappers::toMessage(obj(R"({
        "ts": "100.000", "user": "U1", "text": "top-level"
    })"));
    CHECK(m.parentUserId.value.isEmpty());
    CHECK_FALSE(isFollowedThreadReply(m, UserId{"U1"}));
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
    CHECK(f.id == "F001");
    CHECK(f.name == "report.pdf");
    CHECK(f.mimeType == "application/pdf");
    CHECK(f.prettyType == "PDF");
    CHECK(f.urlPrivate == "https://files.slack.com/report.pdf");
    CHECK(f.permalink == "https://workspace.slack.com/files/report");
    CHECK(f.thumbUrl == "https://thumb.example.com/360.png");
    CHECK(f.imageWidth == 1920);
    CHECK(f.imageHeight == 1080);
    CHECK(f.size == 204800);
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
    CHECK(f.imageWidth == 800);
    CHECK(f.imageHeight == 600);
}

TEST_CASE("toFile falls back to thumb_360 dimensions", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "thumb_360_w": 360, "thumb_360_h": 270
    })"));
    CHECK(f.imageWidth == 360);
    CHECK(f.imageHeight == 270);
}

TEST_CASE("toFile parses thumbnail ladder ascending", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "original_w": 2000, "original_h": 1500,
        "thumb_360": "https://360.png", "thumb_360_w": 360, "thumb_360_h": 270,
        "thumb_480": "https://480.png", "thumb_480_w": 480, "thumb_480_h": 360,
        "thumb_720": "https://720.png", "thumb_720_w": 720, "thumb_720_h": 540,
        "thumb_1024": "https://1024.png", "thumb_1024_w": 1024, "thumb_1024_h": 768
    })"));
    REQUIRE(f.thumbs.size() == 4);
    CHECK(f.thumbs[0] == FileThumb{360, 270, "https://360.png"});
    CHECK(f.thumbs[3] == FileThumb{1024, 768, "https://1024.png"});

    // previewUrl picks the smallest thumb covering the physical width…
    CHECK(f.previewUrl(400) == "https://480.png");
    CHECK(f.previewUrl(800) == "https://1024.png");
    // …and the largest one when nothing is big enough (never the original).
    CHECK(f.previewUrl(2000) == "https://1024.png");
}

TEST_CASE("File previewUrl falls back to thumbUrl then urlPrivate", "[mappers][file]") {
    File f;
    f.urlPrivate = "https://orig.png";
    CHECK(f.previewUrl(400) == "https://orig.png");
    f.thumbUrl = "https://legacy.png";
    CHECK(f.previewUrl(400) == "https://legacy.png");
}

TEST_CASE("toFile thumb width defaults to nominal side when _w absent", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "thumb_64": "https://64.png"
    })"));
    REQUIRE(f.thumbs.size() == 1);
    CHECK(f.thumbs[0].width == 64);
    CHECK(f.thumbs[0].url == "https://64.png");
}

TEST_CASE("toFile large size preserved as qint64", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({"id":"F1","size":3000000000})"));
    CHECK(f.size == 3000000000LL);
}

TEST_CASE("toFile PDF falls back to prerendered thumb_pdf", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "name": "report.pdf", "mimetype": "application/pdf",
        "thumb_pdf": "https://files.slack.com/thumb_pdf.png",
        "thumb_pdf_w": 909, "thumb_pdf_h": 1286
    })"));
    CHECK(f.thumbUrl == "https://files.slack.com/thumb_pdf.png");
    CHECK(f.imageWidth == 909);
    CHECK(f.imageHeight == 1286);
    CHECK(!f.isImage());
    CHECK(f.hasPreview());
}

TEST_CASE("toFile standard thumbs preferred over thumb_pdf", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "mimetype": "application/pdf",
        "thumb_360": "https://360.png", "thumb_360_w": 360, "thumb_360_h": 509,
        "thumb_pdf": "https://thumb_pdf.png", "thumb_pdf_w": 909, "thumb_pdf_h": 1286
    })"));
    CHECK(f.thumbUrl == "https://360.png");
    CHECK(f.imageWidth == 360);
    CHECK(f.imageHeight == 509);
    CHECK(f.hasPreview());
}

TEST_CASE("PDF without thumbnail has no preview", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "name": "report.pdf", "mimetype": "application/pdf",
        "url_private": "https://files.slack.com/report.pdf"
    })"));
    CHECK(!f.hasPreview());
}

TEST_CASE("non-PDF document ignores thumb_pdf for preview", "[mappers][file]") {
    auto f = JsonMappers::toFile(obj(R"({
        "id": "F1", "name": "deck.pptx",
        "mimetype": "application/vnd.openxmlformats-officedocument.presentationml.presentation",
        "thumb_pdf": "https://thumb_pdf.png"
    })"));
    CHECK(!f.hasPreview());
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
    CHECK(b.typeStr == "header");
    CHECK(b.text.text == "My Header");
    CHECK(b.text.entities.empty());
}

TEST_CASE("toBlock image block", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "image",
        "image_url": "https://img.example.com/pic.png",
        "alt_text": "a picture"
    })"));
    CHECK(b.typeStr == "image");
    CHECK(b.imageUrl == "https://img.example.com/pic.png");
    CHECK(b.altText == "a picture");
    CHECK(b.text.text.isEmpty());
    CHECK(b.imageWidth == 0);
    CHECK(b.imageHeight == 0);
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

TEST_CASE("toBlock image block keeps dimensions (Slack GIF picker shape)", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "image",
        "image_url": "https://media2.giphy.com/media/abc/giphy-downsized.gif",
        "alt_text": "a man is sweating",
        "title": {"type": "plain_text", "text": "GIF"},
        "image_width": 480,
        "image_height": 360
    })"));
    CHECK(b.typeStr == "image");
    CHECK(b.imageWidth == 480);
    CHECK(b.imageHeight == 360);
    CHECK(b.text.text == "GIF");
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

TEST_CASE("toBlock actions maps button elements", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "actions",
        "elements": [
            {"type": "button", "text": {"type": "plain_text", "text": "Approve"},
             "style": "primary"},
            {"type": "button", "text": {"type": "plain_text", "text": "Docs"},
             "url": "https://example.com/docs"},
            {"type": "static_select", "placeholder": {"type": "plain_text", "text": "Pick"}}
        ]
    })"));
    CHECK(b.text.text.isEmpty());
    REQUIRE(b.buttons.size() == 2);
    CHECK(b.buttons[0].text == "Approve");
    CHECK(b.buttons[0].style == "primary");
    CHECK(b.buttons[0].url.isEmpty());
    CHECK(b.buttons[1].text == "Docs");
    CHECK(b.buttons[1].url == "https://example.com/docs");
}

TEST_CASE("toBlock section accessory button", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "section",
        "text": {"type": "mrkdwn", "text": "details"},
        "accessory": {"type": "button", "text": {"type": "plain_text", "text": "View"}}
    })"));
    CHECK(b.text.text == "details");
    REQUIRE(b.buttons.size() == 1);
    CHECK(b.buttons[0].text == "View");
}

TEST_CASE("toBlock context parses mrkdwn elements", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "context",
        "elements": [
            {"type": "mrkdwn", "text": "see <https://example.com|docs>"}
        ]
    })"));
    CHECK(b.text.text == "see docs");
    REQUIRE(b.text.entities.size() == 1);
    CHECK(b.text.entities[0].type == EntityType::Link);
    CHECK(b.text.entities[0].data == "https://example.com");
}

TEST_CASE("toAttachment maps legacy actions buttons (string text)", "[mappers][attach]") {
    auto a = JsonMappers::toAttachment(obj(R"({
        "fallback": "You are unable to choose",
        "callback_id": "event_response",
        "actions": [
            {"type": "button", "name": "resp", "text": "Change Response"},
            {"type": "select", "name": "menu", "text": "Pick one"}
        ]
    })"));
    REQUIRE(a.buttons.size() == 1);
    CHECK(a.buttons[0].text == "Change Response");
    CHECK(a.buttons[0].url.isEmpty());
}

TEST_CASE("toAttachment text decodes escaped link URLs (calendar-bot shape)", "[mappers][attach]") {
    auto a = JsonMappers::toAttachment(obj(R"json({
        "text": "*<https://cal.example.com/event?eid=X&amp;ctz=UTC|Stand-Up>*\nRoom (4)"
    })json"));
    CHECK(a.text.text == "Stand-Up\nRoom (4)");
    REQUIRE(a.text.entities.size() == 2);
    CHECK(a.text.entities[0].type == EntityType::Bold);
    CHECK(a.text.entities[1].type == EntityType::Link);
    CHECK(a.text.entities[1].data == "https://cal.example.com/event?eid=X&ctz=UTC");
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

TEST_CASE("toBlock rich_text text run resolves embedded Slack tokens", "[mappers][block]") {
    // Outlook Calendar reminders arrive as a rich_text "text" element whose raw
    // text still carries <!date^…> and <url|label> tokens (Slack's text→rich_text
    // conversion doesn't structure them). They must resolve, not render literally.
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "rich_text",
        "elements": [{
            "type": "rich_text_section",
            "elements": [{
                "type": "text",
                "text": "<!date^1782903600^{time}|2:00 PM> <https://outlook.office365.com/owa?x=1|Ny event>"
            }]
        }]
    })"));
    CHECK_FALSE(b.text.text.contains("<!date"));
    CHECK(b.text.text.contains("Ny event"));
    CHECK_FALSE(b.text.text.contains("outlook.office365.com"));
    REQUIRE(b.text.entities.size() == 1);
    CHECK(b.text.entities[0].type == EntityType::Link);
    CHECK(b.text.entities[0].data == "https://outlook.office365.com/owa?x=1");
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
    CHECK(b.text.entities[0].type == EntityType::Bold);
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
    CHECK(b.text.entities[0].type == EntityType::Pre);
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
    CHECK(b.text.entities[0].type == EntityType::Blockquote);
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
    CHECK(a.fallback == "fb");
    CHECK(a.color == "#36a64f");
    CHECK(a.pretext == "pre");
    CHECK(a.authorName == "Author");
    CHECK(a.title == "My title");
    CHECK(a.titleLink == "https://link.example.com");
    CHECK(a.text.text == "plain body");
    CHECK(a.imageUrl == "https://img.example.com/img.png");
    CHECK(a.thumbUrl == "https://img.example.com/thumb.png");
    CHECK(a.footer == "Posted via App");
}

TEST_CASE("toAttachment image and thumb dimensions", "[mappers][attachment]") {
    auto a = JsonMappers::toAttachment(obj(R"({
        "image_url": "https://img.png", "image_width": 1200, "image_height": 630,
        "thumb_url": "https://thumb.png", "thumb_width": 360, "thumb_height": 189
    })"));
    CHECK(a.imageWidth == 1200);
    CHECK(a.imageHeight == 630);
    CHECK(a.thumbWidth == 360);
    CHECK(a.thumbHeight == 189);

    // Thumb when it covers the physical width, full image otherwise.
    CHECK(a.previewUrl(360) == "https://thumb.png");
    CHECK(a.previewUrl(800) == "https://img.png");
}

TEST_CASE("Attachment previewUrl with unknown thumb size keeps thumb", "[mappers][attachment]") {
    Attachment a;
    a.imageUrl = "https://img.png";
    a.thumbUrl = "https://thumb.png";
    CHECK(a.previewUrl(800) == "https://thumb.png");
    a.thumbUrl.clear();
    CHECK(a.previewUrl(800) == "https://img.png");
}

TEST_CASE("toAttachment text is parsed as mrkdwn", "[mappers][attachment]") {
    auto a = JsonMappers::toAttachment(obj(R"({"text": "<@U1|alice> check *this*"})"));
    CHECK(a.text.text == "alice check this");
    REQUIRE(a.text.entities.size() == 2);
    CHECK(a.text.entities[0].type == EntityType::UserMention);
    CHECK(a.text.entities[1].type == EntityType::Bold);
}

TEST_CASE("toAttachment fields parsed with mrkdwn values", "[mappers][attachment]") {
    auto a = JsonMappers::toAttachment(obj(R"({
        "fields": [
            {"title": "Type", "value": "Privacy violation", "short": true},
            {"title": "Code", "value": "<https://x.example.com|HITTA-1>", "short": true}
        ]
    })"));
    REQUIRE(a.fields.size() == 2);
    CHECK(a.fields[0].title == "Type");
    CHECK(a.fields[0].value.text == "Privacy violation");
    CHECK(a.fields[1].title == "Code");
    CHECK(a.fields[1].value.text == "HITTA-1");
    REQUIRE(a.fields[1].value.entities.size() == 1);
    CHECK(a.fields[1].value.entities[0].type == EntityType::Link);
    CHECK(a.fields[1].value.entities[0].data == "https://x.example.com");
}

TEST_CASE("toAttachment without fields leaves vector empty", "[mappers][attachment]") {
    auto a = JsonMappers::toAttachment(obj(R"({"text": "hi"})"));
    CHECK(a.fields.empty());
}

TEST_CASE("section block fields appended as lines with shifted entities", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "section",
        "text": {"type": "mrkdwn", "text": "header line"},
        "fields": [
            {"type": "mrkdwn", "text": "*Title:* first"},
            {"type": "mrkdwn", "text": "*Severity:* high"}
        ]
    })"));
    CHECK(b.text.text == "header line\nTitle: first\nSeverity: high");
    REQUIRE(b.text.entities.size() == 2);
    CHECK(b.text.entities[0].type == EntityType::Bold);
    // "header line\n" is 12 chars — first bold label starts right after it.
    CHECK(b.text.entities[0].offset == 12);
    CHECK(b.text.entities[1].offset == 12 + QString("Title: first\n").size());
}

TEST_CASE("section block with only fields and no text", "[mappers][block]") {
    auto b = JsonMappers::toBlock(obj(R"({
        "type": "section",
        "fields": [{"type": "plain_text", "text": "solo"}]
    })"));
    CHECK(b.text.text == "solo");
}

// ── toSearchResult ────────────────────────────────────────────────────────────

TEST_CASE("toSearchResult extracts conv from nested channel object", "[mappers][search]") {
    auto r = JsonMappers::toSearchResult(obj(R"({
        "channel": {"id": "C001", "name": "general"},
        "ts": "123.456", "user": "U1", "text": "found it"
    })"));
    CHECK(r.conv == ConversationId{"C001"});
    CHECK(r.convName == "general");
    CHECK(r.msg.ts == "123.456");
    CHECK(r.msg.text.text == "found it");
}

// ── toSelfPresence ────────────────────────────────────────────────────────────

TEST_CASE("toSelfPresence active with a connected client", "[mappers][presence]") {
    auto sp = JsonMappers::toSelfPresence(obj(R"({
        "ok": true, "presence": "active",
        "online": true, "auto_away": false, "manual_away": false,
        "connection_count": 2, "last_activity": 1700000000
    })"));
    CHECK(sp.loaded);
    CHECK(sp.active);
    CHECK(sp.online);
    CHECK(sp.connectionCount == 2);
    CHECK_FALSE(sp.phantomAway());
}

TEST_CASE("toSelfPresence away with zero connections is phantom away", "[mappers][presence]") {
    auto sp = JsonMappers::toSelfPresence(obj(R"({
        "ok": true, "presence": "away",
        "online": false, "auto_away": false, "manual_away": false,
        "connection_count": 0, "last_activity": 0
    })"));
    CHECK(sp.loaded);
    CHECK_FALSE(sp.active);
    CHECK_FALSE(sp.online);
    CHECK(sp.phantomAway());
}

TEST_CASE("toSelfPresence manual away is not phantom away", "[mappers][presence]") {
    auto sp = JsonMappers::toSelfPresence(obj(R"({
        "ok": true, "presence": "away",
        "online": false, "auto_away": false, "manual_away": true,
        "connection_count": 0, "last_activity": 0
    })"));
    CHECK(sp.manualAway);
    CHECK_FALSE(sp.phantomAway());
}

TEST_CASE("toSelfPresence idle auto-away while online is not phantom away", "[mappers][presence]") {
    auto sp = JsonMappers::toSelfPresence(obj(R"({
        "ok": true, "presence": "away",
        "online": true, "auto_away": true, "manual_away": false,
        "connection_count": 1, "last_activity": 1700000000
    })"));
    CHECK(sp.autoAway);
    CHECK(sp.online);
    CHECK_FALSE(sp.phantomAway());
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
    auto msgs = JsonMappers::toMessages(
        arr(R"([
        {"ts":"1.000","user":"U1","text":"first"},
        {"ts":"2.000","user":"U1","text":"second"}
    ])"),
        true
    );
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].ts == "2.000");
    CHECK(msgs[1].ts == "1.000");
}

TEST_CASE("toMessages reverseOrder=false preserves array order", "[mappers][batch]") {
    auto msgs = JsonMappers::toMessages(
        arr(R"([
        {"ts":"1.000","user":"U1","text":"first"},
        {"ts":"2.000","user":"U1","text":"second"}
    ])"),
        false
    );
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

// ── is_starred ────────────────────────────────────────────────────────────────

TEST_CASE("toConversation is_starred true", "[mappers][conv][star]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "is_starred": true
    })"));
    CHECK(c.isStarred == true);
}

TEST_CASE("toConversation is_starred absent defaults to false", "[mappers][conv][star]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false
    })"));
    CHECK(c.isStarred == false);
}

// ── notification_preference ───────────────────────────────────────────────────

TEST_CASE("toConversation notification_preference=everything → All", "[mappers][conv][notif]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "notification_preference": "everything"
    })"));
    CHECK(c.notifLevel == NotificationLevel::All);
}

TEST_CASE("toConversation notification_preference=mentions → Mentions", "[mappers][conv][notif]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "notification_preference": "mentions"
    })"));
    CHECK(c.notifLevel == NotificationLevel::Mentions);
}

TEST_CASE("toConversation notification_preference=nothing → Mute", "[mappers][conv][notif]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "notification_preference": "nothing"
    })"));
    CHECK(c.notifLevel == NotificationLevel::Mute);
}

TEST_CASE("toConversation notification_preference absent → Default", "[mappers][conv][notif]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false
    })"));
    CHECK(c.notifLevel == NotificationLevel::Default);
}

// ── Mpim members ──────────────────────────────────────────────────────────────

TEST_CASE("toConversation Mpim parses members array", "[mappers][conv][mpim]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "G1", "name": "mpdm-alice--bob--1",
        "is_mpim": true, "is_im": false, "is_private": true,
        "is_member": true,
        "members": ["U1", "U2", "U3"]
    })"));
    REQUIRE(c.kind == ConvKind::Mpim);
    REQUIRE(c.members.size() == 3);
    CHECK(c.members[0] == UserId{"U1"});
    CHECK(c.members[1] == UserId{"U2"});
    CHECK(c.members[2] == UserId{"U3"});
}

TEST_CASE("toConversation non-Mpim ignores members array", "[mappers][conv][mpim]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "general",
        "is_private": false, "is_im": false, "is_mpim": false,
        "members": ["U1", "U2"]
    })"));
    CHECK(c.members.empty());
}

// ── num_members / memberCount ─────────────────────────────────────────────────

TEST_CASE("toConversation num_members maps to memberCount", "[mappers][conv][members]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "big-channel",
        "is_private": false, "is_im": false, "is_mpim": false,
        "num_members": 42
    })"));
    CHECK(c.memberCount == 42);
}

TEST_CASE("toConversation num_members absent defaults to 0", "[mappers][conv][members]") {
    auto c = JsonMappers::toConversation(obj(R"({
        "id": "C1", "name": "channel",
        "is_private": false, "is_im": false, "is_mpim": false
    })"));
    CHECK(c.memberCount == 0);
}

// ── toSlashCommands ───────────────────────────────────────────────────────────

TEST_CASE("toSlashCommands parses an array of command objects", "[mappers][commands]") {
    auto cmds = JsonMappers::toSlashCommands(arr(R"([
        {"name": "/remind", "desc": "Set a reminder",
         "usage": "[what] [when]", "type": "core"},
        {"name": "deploy", "desc": "Deploy a service",
         "usage": "[service]", "type": "app", "app": "A012"}
    ])"));
    REQUIRE(cmds.size() == 2);
    CHECK(cmds[0].name == "remind"); // leading slash stripped
    CHECK(cmds[0].desc == "Set a reminder");
    CHECK(cmds[0].usage == "[what] [when]");
    CHECK(cmds[0].appId.isEmpty()); // core command — no app
    CHECK(cmds[1].name == "deploy");
    CHECK(cmds[1].appId == "A012");
}

TEST_CASE("toSlashCommands parses an object keyed by command name", "[mappers][commands]") {
    auto cmds = JsonMappers::toSlashCommands(obj(R"({
        "/shrug": {"name": "/shrug", "desc": "Shrug", "type": "core"}
    })"));
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].name == "shrug");
}

TEST_CASE("toSlashCommands tolerates junk input", "[mappers][commands]") {
    CHECK(JsonMappers::toSlashCommands(QJsonValue{}).empty());
    CHECK(JsonMappers::toSlashCommands(arr(R"([{"desc": "nameless"}, 42])")).empty());
}

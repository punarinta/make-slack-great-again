// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>
#include <QJsonObject>

#include "backend/teams/json_mappers.h"

using namespace teams;

namespace {
QJsonObject obj(const char *json) {
    return QJsonDocument::fromJson(json).object();
}
const TextEntity *find(const TextWithEntities &t, EntityType type) {
    for (const auto &e : t.entities)
        if (e.type == type)
            return &e;
    return nullptr;
}
} // namespace

// ── composite conversation id (channel = teamId|channelId) ───────────────────

TEST_CASE("channel conv id round-trips; chats stay bare", "[teams][convid]") {
    const auto cid = channelConvId("team-1", "19:abc@thread.tacv2");
    CHECK(cid == "team-1|19:abc@thread.tacv2");
    CHECK(isChannelConvId(cid));
    const auto [team, chan] = splitChannelConvId(cid);
    CHECK(team == "team-1");
    CHECK(chan == "19:abc@thread.tacv2");

    // A chat id has no '|' → not a channel.
    CHECK_FALSE(isChannelConvId("19:user1_user2@unq.gbl.spaces"));
}

// ── HTML body → TextWithEntities ─────────────────────────────────────────────

TEST_CASE("htmlToText extracts bold + link and decodes entities", "[teams][html]") {
    const auto t = JsonMappers::htmlToText(QStringLiteral(
        "<div>Hello <b>bold</b> and <a href=\"https://x.com\">link</a> &amp; more</div>"
    ));
    CHECK(t.text.trimmed() == "Hello bold and link & more");

    const auto *bold = find(t, EntityType::Bold);
    REQUIRE(bold != nullptr);
    CHECK(t.text.mid(bold->offset, bold->length) == "bold");

    const auto *link = find(t, EntityType::Link);
    REQUIRE(link != nullptr);
    CHECK(link->data == "https://x.com");
    CHECK(t.text.mid(link->offset, link->length) == "link");
}

TEST_CASE("htmlToText drops the trailing newline a closing block emits", "[teams][html]") {
    // <p>/<attachment> bodies used to leave a trailing '\n' → a blank line (an
    // ugly gap before the attachment chip). The text must end cleanly.
    const auto t = JsonMappers::htmlToText(
        QStringLiteral("<p>A post with an image attachment</p><attachment id=\"a1\"></attachment>")
    );
    CHECK(t.text == "A post with an image attachment");
}

TEST_CASE("htmlToText passes unknown tags' inner text through", "[teams][html]") {
    // <at> mentions aren't a handled tag — their label still appears as text.
    const auto t = JsonMappers::htmlToText(QStringLiteral("hi <at id=\"0\">Vlad</at>!"));
    CHECK(t.text == "hi Vlad!");
    CHECK(t.entities.empty());
}

// ── chatMessage → Message ────────────────────────────────────────────────────

TEST_CASE("toMessage maps id/date/author/text and groups reactions", "[teams][message]") {
    const auto m = JsonMappers::toMessage(obj(R"({
        "id": "1700000000001",
        "messageType": "message",
        "createdDateTime": "2026-06-22T19:15:25Z",
        "lastEditedDateTime": "2026-06-22T19:16:00Z",
        "body": { "contentType": "html", "content": "<p>hi there</p>" },
        "from": { "user": { "id": "user-A", "displayName": "Ann" } },
        "reactions": [
            { "reactionType": "like",  "user": { "user": { "id": "user-A" } } },
            { "reactionType": "like",  "user": { "user": { "id": "user-B" } } },
            { "reactionType": "heart", "user": { "user": { "id": "user-A" } } }
        ]
    })"));

    CHECK(m.ts == "1700000000001");
    CHECK(m.date > 0);
    CHECK(m.author.value == "user-A");
    CHECK(m.text.text.trimmed() == "hi there");
    CHECK(m.edited);

    REQUIRE(m.reactions.size() == 2);
    // Graph's legacy reaction enums are mapped to Slack shortcodes so they resolve
    // to a glyph (a raw ":like:" would render literally); "like" → "thumbsup".
    CHECK(m.reactions[0].name == "thumbsup");
    CHECK(m.reactions[0].count == 2);
    CHECK(m.reactions[1].name == "heart");
    CHECK(m.reactions[1].count == 1);
}

TEST_CASE("toMessage passes a Unicode-emoji reactionType through verbatim", "[teams][message]") {
    // Modern Graph returns reactionType as the emoji itself; keep it as-is so the
    // emoji resolver renders the glyph rather than a ":👍:" placeholder.
    const auto m = JsonMappers::toMessage(obj(R"({
        "id": "1700000000009",
        "messageType": "message",
        "createdDateTime": "2026-06-22T19:15:25Z",
        "body": { "contentType": "text", "content": "yo" },
        "from": { "user": { "id": "user-A" } },
        "reactions": [
            { "reactionType": "👍", "user": { "user": { "id": "user-A" } } }
        ]
    })"));
    REQUIRE(m.reactions.size() == 1);
    CHECK(m.reactions[0].name == QString::fromUtf8("👍"));
}

TEST_CASE("toMessage maps an app sender to botName and a reply to threadRoot", "[teams][message]") {
    const auto m = JsonMappers::toMessage(obj(R"({
        "id": "200",
        "messageType": "message",
        "replyToId": "100",
        "body": { "contentType": "text", "content": "plain reply" },
        "from": { "application": { "displayName": "Pipeline Bot" } }
    })"));
    CHECK(m.author.value.isEmpty());
    CHECK(m.botName == "Pipeline Bot");
    CHECK(m.text.text == "plain reply"); // contentType=text → no HTML parse
    REQUIRE(m.threadRoot.has_value());
    CHECK(*m.threadRoot == "100");
}

TEST_CASE("toMessage maps a reference attachment to a File chip", "[teams][message][attach]") {
    const auto m = JsonMappers::toMessage(obj(R"({
        "id": "300",
        "messageType": "message",
        "body": { "contentType": "html", "content": "<p>non-image attachment</p><attachment id=\"a1\"></attachment>" },
        "attachments": [
            { "id": "a1", "contentType": "reference", "name": "phone_bill.pdf",
              "contentUrl": "https://contoso.sharepoint.com/sites/x/phone_bill.pdf" }
        ]
    })"));
    CHECK(m.text.text.trimmed() == "non-image attachment"); // <attachment> tag dropped
    REQUIRE(m.files.size() == 1);
    CHECK(m.files[0].name == "phone_bill.pdf");
    CHECK(m.files[0].permalink == "https://contoso.sharepoint.com/sites/x/phone_bill.pdf");
    CHECK(m.files[0].mimeType == "application/pdf");
    CHECK(m.files[0].isPdf());
    CHECK(m.files[0].urlPrivate.isEmpty()); // SharePoint URL, not a Graph download
}

// ── chat / channel → Conversation ────────────────────────────────────────────

TEST_CASE("toChatConversation: 1:1 picks the other member as the DM", "[teams][conv]") {
    const auto c = JsonMappers::toChatConversation(
        obj(R"({
        "id": "19:dm@unq.gbl.spaces",
        "chatType": "oneOnOne",
        "members": [
            { "userId": "me-oid",   "displayName": "Me" },
            { "userId": "peer-oid", "displayName": "Peer Person" }
        ]
    })"),
        "me-oid"
    );
    CHECK(c.kind == ConvKind::Im);
    CHECK(c.name == "Peer Person");
    REQUIRE(c.dmUser.has_value());
    CHECK(c.dmUser->value == "peer-oid");
}

TEST_CASE("toChatConversation: group uses topic or member names", "[teams][conv]") {
    const auto c = JsonMappers::toChatConversation(
        obj(R"({
        "id": "19:grp@thread.v2",
        "chatType": "group",
        "topic": "Project X",
        "members": [
            { "userId": "me-oid", "displayName": "Me" },
            { "userId": "a-oid",  "displayName": "Aaron" }
        ]
    })"),
        "me-oid"
    );
    CHECK(c.kind == ConvKind::Mpim);
    CHECK(c.name == "Project X");
    CHECK(c.members.size() == 2);
}

TEST_CASE("toChannelConversation encodes the composite id and maps membership", "[teams][conv]") {
    const auto c = JsonMappers::toChannelConversation(
        obj(R"({
        "id": "19:chan@thread.tacv2",
        "displayName": "General",
        "membershipType": "standard"
    })"),
        "team-9",
        "Contoso Team"
    );
    CHECK(c.kind == ConvKind::PublicChannel);
    CHECK(c.name == "General");
    CHECK(c.description == "Contoso Team"); // parent team disambiguates
    CHECK(c.id.value == "team-9|19:chan@thread.tacv2");
    CHECK(isChannelConvId(c.id.value));

    const auto priv = JsonMappers::toChannelConversation(
        obj(R"({
        "id": "19:p@thread.tacv2", "displayName": "Secret", "membershipType": "private"
    })"),
        "team-9",
        "Contoso Team"
    );
    CHECK(priv.kind == ConvKind::PrivateChannel);
}

// ── reaction emoji mapping ───────────────────────────────────────────────────

TEST_CASE(
    "graphReactionType maps known Slack names and passes unknowns through", "[teams][react]"
) {
    CHECK(graphReactionType("thumbsup") == "like");
    CHECK(graphReactionType("+1") == "like");
    CHECK(graphReactionType("heart") == "heart");
    CHECK(graphReactionType("joy") == "laugh");
    CHECK(graphReactionType("rage") == "angry");
    CHECK(graphReactionType("rocket") == "rocket"); // unknown → unchanged
}

// ── inline images (hostedContents <img>) ─────────────────────────────────────

TEST_CASE("extractInlineImages pulls <img> src + dimensions; ignores other tags", "[teams][img]") {
    const auto imgs = extractInlineImages(QStringLiteral(
        "<p>see <img src=\"https://graph.microsoft.com/v1.0/teams/t/channels/c/messages/m/"
        "hostedContents/1/$value\" width=\"320\" height=\"240\"></p>"
        "<img src=\"https://graph.microsoft.com/hostedContents/2/$value\">"
    ));
    REQUIRE(imgs.size() == 2);
    CHECK(imgs[0].url.contains("hostedContents/1/$value"));
    CHECK(imgs[0].width == 320);
    CHECK(imgs[0].height == 240);
    CHECK(imgs[1].width == 0); // no dimensions on the second
    CHECK(extractInlineImages(QStringLiteral("<p>no images here</p>")).empty());
}

// ── presence ─────────────────────────────────────────────────────────────────

TEST_CASE("presenceActive maps Graph availability to the binary dot", "[teams][presence]") {
    CHECK(JsonMappers::presenceActive("Available"));
    CHECK(JsonMappers::presenceActive("AvailableIdle"));
    CHECK(JsonMappers::presenceActive("Busy"));
    CHECK(JsonMappers::presenceActive("DoNotDisturb"));
    CHECK_FALSE(JsonMappers::presenceActive("Away"));
    CHECK_FALSE(JsonMappers::presenceActive("BeRightBack"));
    CHECK_FALSE(JsonMappers::presenceActive("Offline"));
    CHECK_FALSE(JsonMappers::presenceActive("PresenceUnknown"));
}

// ── my profile ───────────────────────────────────────────────────────────────

TEST_CASE("toMyProfile pulls name/email/phone from /me", "[teams][profile]") {
    const auto p = JsonMappers::toMyProfile(obj(R"({
        "displayName": "Vladimir Osipov",
        "mail": "vladimir@contoso.com",
        "userPrincipalName": "vlad@contoso.onmicrosoft.com",
        "businessPhones": ["+370 600 00000"]
    })"));
    CHECK(p.displayName == "Vladimir Osipov");
    CHECK(p.email == "vladimir@contoso.com"); // mail preferred over UPN
    CHECK(p.phone == "+370 600 00000");       // falls back to first businessPhone
}

// ── search hit → SearchResult (conv derived from channelIdentity / chatId) ───

TEST_CASE("toSearchResult derives the conversation from a channel hit", "[teams][search]") {
    const auto r = JsonMappers::toSearchResult(obj(R"({
        "id": "1700000000999",
        "messageType": "message",
        "body": { "contentType": "text", "content": "found me" },
        "from": { "user": { "id": "user-A" } },
        "channelIdentity": { "teamId": "team-9", "channelId": "19:chan@thread.tacv2" }
    })"));
    CHECK(r.conv.value == "team-9|19:chan@thread.tacv2");
    CHECK(isChannelConvId(r.conv.value));
    CHECK(r.msg.ts == "1700000000999");
    CHECK(r.msg.text.text == "found me");
}

TEST_CASE("toSearchResult derives the conversation from a chat hit", "[teams][search]") {
    const auto r = JsonMappers::toSearchResult(obj(R"({
        "id": "200",
        "messageType": "message",
        "body": { "contentType": "text", "content": "dm hit" },
        "chatId": "19:dm@unq.gbl.spaces"
    })"));
    CHECK(r.conv.value == "19:dm@unq.gbl.spaces");
    CHECK_FALSE(isChannelConvId(r.conv.value));
}

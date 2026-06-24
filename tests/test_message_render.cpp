// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Tests for MsgRender helpers:
//   - resolveEmojiRich(): builtin names, workspace custom emojis, alias chains
//   - toHtml(): emoji entities render at Slack line-height size; custom emojis
//     resolve to <img> only when a map is available
//   - buildAttachHtml(): classic bot "fields" render as bold title + value
//   - lastReplyLabel(): "today at"/"yesterday at" wording

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDateTime>
#include <QTextDocument>

#include "ui/message_list/message_render.h"
#include "text/mrkdwn_parser.h"
#include "session/session.h"
#include "backend/backend.h"
#include "rpl/variable.h"

// ── Custom main (QApplication required for fonts/theme) ──────────────────────

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-message-render");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// ── resolveEmojiRich ──────────────────────────────────────────────────────────

static const QHash<QString, QString> kMap = {
    {"no-lunch", "https://emoji.slack-edge.com/T1/no-lunch/abc.jpg"},
    {"lunch-gone", "alias:no-lunch"},
    {"thumbs", "alias:thumbsup"},
    {"loop-a", "alias:loop-b"},
    {"loop-b", "alias:loop-a"},
};

TEST_CASE("resolveEmojiRich builtin name → unicode", "[render][emoji]") {
    const auto r = MsgRender::resolveEmojiRich("palm_tree", kMap);
    CHECK(r.unicode == QString::fromUtf8("\xF0\x9F\x8C\xB4")); // 🌴
    CHECK(r.imageUrl.isEmpty());
}

TEST_CASE("resolveEmojiRich custom emoji → image URL", "[render][emoji]") {
    const auto r = MsgRender::resolveEmojiRich("no-lunch", kMap);
    CHECK(r.unicode.isEmpty());
    CHECK(r.imageUrl == "https://emoji.slack-edge.com/T1/no-lunch/abc.jpg");
}

TEST_CASE("resolveEmojiRich alias to custom emoji follows chain", "[render][emoji]") {
    const auto r = MsgRender::resolveEmojiRich("lunch-gone", kMap);
    CHECK(r.imageUrl == "https://emoji.slack-edge.com/T1/no-lunch/abc.jpg");
}

TEST_CASE("resolveEmojiRich alias to builtin → unicode", "[render][emoji]") {
    const auto r = MsgRender::resolveEmojiRich("thumbs", kMap);
    CHECK(!r.unicode.isEmpty());
    CHECK(r.imageUrl.isEmpty());
}

TEST_CASE("resolveEmojiRich circular alias terminates with fallback", "[render][emoji]") {
    const auto r = MsgRender::resolveEmojiRich("loop-a", kMap);
    CHECK(r.unicode == ":loop-a:");
    CHECK(r.imageUrl.isEmpty());
}

TEST_CASE("resolveEmojiRich unknown name falls back to :name:", "[render][emoji]") {
    const auto r = MsgRender::resolveEmojiRich("definitely-not-an-emoji", kMap);
    CHECK(r.unicode == ":definitely-not-an-emoji:");
    CHECK(r.imageUrl.isEmpty());
}

TEST_CASE("resolveEmojiRich null session behaves like empty map", "[render][emoji]") {
    const auto r = MsgRender::resolveEmojiRich("no-lunch", static_cast<const Session *>(nullptr));
    CHECK(r.unicode == ":no-lunch:");
    CHECK(r.imageUrl.isEmpty());
}

TEST_CASE("resolveEmojiRich appends skin-tone modifier to builtin", "[render][emoji]") {
    const auto r = MsgRender::resolveEmojiRich("+1::skin-tone-3", kMap);
    // 👍 followed by the medium-light skin-tone modifier 🏼
    CHECK(r.unicode == QString::fromUtf8("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBC"));
    CHECK(r.imageUrl.isEmpty());
}

TEST_CASE("resolveEmojiRich skin-tone on aliased builtin", "[render][emoji]") {
    const auto r = MsgRender::resolveEmojiRich("thumbs::skin-tone-2", kMap);
    CHECK(r.unicode.startsWith(QString::fromUtf8("\xF0\x9F\x91\x8D"))); // 👍…
    CHECK(r.unicode.endsWith(QString::fromUtf8("\xF0\x9F\x8F\xBB")));   // …🏻
    CHECK(r.imageUrl.isEmpty());
}

TEST_CASE("resolveEmojiRich custom-image base ignores modifier", "[render][emoji]") {
    const auto r = MsgRender::resolveEmojiRich("no-lunch::skin-tone-4", kMap);
    CHECK(r.imageUrl == "https://emoji.slack-edge.com/T1/no-lunch/abc.jpg");
}

// ── toHtml emoji rendering ────────────────────────────────────────────────────

TEST_CASE("toHtml renders builtin emoji at Slack line-height size", "[render][emoji]") {
    const auto    twe  = MrkdwnParser::parse("hello :palm_tree:");
    const QString html = MsgRender::toHtml(twe, nullptr);
    CHECK(html.contains(QString::fromUtf8("\xF0\x9F\x8C\xB4")));
    CHECK(html.contains(QString("font-size:%1px").arg(MsgRender::inlineEmojiPx())));
}

TEST_CASE("toHtml without session leaves custom emoji as text", "[render][emoji]") {
    const auto    twe  = MrkdwnParser::parse("lunch? :no-lunch:");
    const QString html = MsgRender::toHtml(twe, nullptr);
    CHECK(html.contains(":no-lunch:"));
    CHECK(!html.contains("<img"));
}

// ── toHtml blockquote nesting cap ───────────────────────────────────────────

TEST_CASE("toHtml bounds nested blockquote tables (layout-hang guard)", "[render][quote]") {
    // A deep email reply chain (the IMAP backend builds one Blockquote entity per
    // '>' quote level) once produced 15 nested <table>s and hung QTextDocument
    // layout on the main thread (caught by the hang watchdog). toHtml must cap
    // nested-table emission no matter how deep the entities nest.
    TextWithEntities twe;
    twe.text = QStringLiteral("hello");
    for (int i = 0; i < 30; ++i)
        twe.entities.push_back(TextEntity{EntityType::Blockquote, 0, 5, QString()});
    const QString html = MsgRender::toHtml(twe, nullptr);
    CHECK(html.count("<table") <= 6); // bounded (cap 4 → ≤5 tables) regardless of 30-deep nesting
    CHECK(html.contains("hello"));    // content preserved
}

// ── toHtml code blocks ────────────────────────────────────────────────────────

TEST_CASE("toHtml renders ``` block as one full-width table, not <pre>", "[render][pre]") {
    const auto    twe  = MrkdwnParser::parse("can I add\n```\n  - A=1\n  - B=2\n```\nto compose?");
    const QString html = MsgRender::toHtml(twe, nullptr);
    CHECK(!html.contains("<pre"));
    CHECK(html.count("<table") == 1);
    CHECK(html.contains("white-space:pre-wrap"));
    // Newlines around the fence are absorbed by the block's own margin.
    CHECK(!html.contains("<br><table"));
    CHECK(!html.contains("</table><br>"));
    // Code lines stay raw newline-separated inside the pre-wrap cell, with the
    // trailing fence newline trimmed and leading indentation kept.
    CHECK(html.contains("  - A=1\n  - B=2</td>"));
}

TEST_CASE("codeBlockRects finds ``` tables but not blockquote tables", "[render][pre]") {
    const auto    twe = MrkdwnParser::parse("```\ncode here\n```\n> quoted line");
    QTextDocument doc;
    doc.setHtml(MsgRender::toHtml(twe, nullptr));
    doc.setTextWidth(400);
    CHECK(doc.toHtml().contains("quoted")); // blockquote table is present in the doc
    const auto rects = MsgRender::codeBlockRects(&doc);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].width() > 300); // full-width block
    CHECK(rects[0].height() > 10);
    CHECK(rects[0].top() >= 0);
}

// ── buildAttachHtml fields ────────────────────────────────────────────────────

TEST_CASE("buildAttachHtml renders fields as bold title + value", "[render][attachment]") {
    Attachment att;
    att.fields.push_back({"Severity", MrkdwnParser::parse("Exceptional")});
    const QString html = MsgRender::buildAttachHtml(att, nullptr);
    CHECK(html.contains("font-weight:bold"));
    CHECK(html.contains("Severity"));
    CHECK(html.contains("Exceptional"));
}

TEST_CASE("buildAttachHtml footer renders after fallback content", "[render][attachment]") {
    Attachment att;
    att.fallback       = "fb text";
    att.footer         = "via Bot";
    const QString html = MsgRender::buildAttachHtml(att, nullptr);
    CHECK(html.indexOf("fb text") >= 0);
    CHECK(html.indexOf("via Bot") > html.indexOf("fb text"));
}

TEST_CASE("toHtml renders a link nested in bold as <b><a>", "[render][nested]") {
    const auto    twe  = MrkdwnParser::parse("*<https://example.com/e|Stand-Up>*");
    const QString html = MsgRender::toHtml(twe, nullptr);
    CHECK(html.contains("<b><a href='https://example.com/e'"));
    CHECK(html.contains("Stand-Up</a></b>"));
}

TEST_CASE("toHtml renders marks nested inside a blockquote", "[render][nested]") {
    const auto    twe  = MrkdwnParser::parse("> *bold* word");
    const QString html = MsgRender::toHtml(twe, nullptr);
    CHECK(html.contains("<b>bold</b>"));
    CHECK(html.contains("<table")); // the quote's left-bar table survives
}

TEST_CASE("buildAttachHtml parses pretext as mrkdwn", "[render][attachment]") {
    Attachment att;
    att.pretext        = "_1 minute until this event_";
    const QString html = MsgRender::buildAttachHtml(att, nullptr);
    CHECK(html.contains("<i>1 minute until this event</i>"));
}

TEST_CASE("buildAttachHtml decodes HTML entities in title and footer", "[render][attachment]") {
    Attachment att;
    att.title          = "Q&amp;A";
    att.footer         = "Bits &amp; Bobs";
    const QString html = MsgRender::buildAttachHtml(att, nullptr);
    CHECK(html.contains("Q&amp;A"));    // decoded to "Q&A", then HTML-escaped once
    CHECK(!html.contains("&amp;amp;")); // not double-escaped
    CHECK(html.contains("Bits &amp; Bobs"));
}

TEST_CASE("buildAttachHtml renders legacy action buttons, skipping fallback", "[render][buttons]") {
    Attachment att;
    att.fallback = "You are unable to respond from here";
    att.buttons.push_back({"Change Response", "", ""});
    const QString html = MsgRender::buildAttachHtml(att, nullptr);
    CHECK(html.contains("Change Response"));
    CHECK(html.contains("<table"));             // button row
    CHECK(!html.contains("unable to respond")); // fallback replaced by buttons
    // Interactive-only button: anchor with the internal scheme, no target URL.
    CHECK(html.contains("<a href='" + MsgRender::kBotBtnAnchorPrefix));
    CHECK(MsgRender::botButtonUrlFromAnchor(MsgRender::kBotBtnAnchorPrefix + "0").isEmpty());
}

TEST_CASE("buildMsgHtml routes URL buttons through the botbtn anchor scheme", "[render][buttons]") {
    Message msg;
    Block   actions;
    actions.typeStr = "actions";
    actions.buttons.push_back({"Open Docs", "https://example.com/docs?a=1&b=2", ""});
    msg.blocks.push_back(actions);
    const QString html = MsgRender::buildMsgHtml(msg, nullptr);
    CHECK(html.contains("Open Docs"));
    CHECK(html.contains(MsgRender::kBotBtnAnchorPrefix + "url:"));
    CHECK(!html.contains("<a href='https://")); // raw URL hrefs would get link hover styling

    // The click handler resolves the original URL back out of the anchor.
    const int     hrefStart = html.indexOf("href='") + 6;
    const QString href      = html.mid(hrefStart, html.indexOf('\'', hrefStart) - hrefStart);
    CHECK(MsgRender::isBotButtonAnchor(href));
    CHECK(MsgRender::botButtonUrlFromAnchor(href) == "https://example.com/docs?a=1&b=2");
}

TEST_CASE(
    "botButtonRects finds one rect per button, codeBlockRects ignores them", "[render][buttons]"
) {
    Attachment att;
    att.text = MrkdwnParser::parse("pick one:");
    att.buttons.push_back({"Yes", "", ""});
    att.buttons.push_back({"No", "", ""});
    QTextDocument doc;
    doc.setHtml(MsgRender::buildAttachHtml(att, nullptr));
    doc.setTextWidth(400);
    const auto rects = MsgRender::botButtonRects(&doc);
    REQUIRE(rects.size() == 2);
    CHECK(rects[0].height() > 10);             // tall enough to look like a button
    CHECK(rects[1].left() > rects[0].right()); // side by side
    CHECK(MsgRender::codeBlockRects(&doc).isEmpty());
}

// ── Image blocks (Slack GIF picker / Giphy) ──────────────────────────────────

static Block gifBlock() {
    Block b;
    b.typeStr     = "image";
    b.imageUrl    = "https://media2.giphy.com/media/abc/giphy.gif";
    b.altText     = "a man is sweating";
    b.text        = {"GIF", {}};
    b.imageWidth  = 480;
    b.imageHeight = 360;
    return b;
}

TEST_CASE("buildMsgHtml without gif context renders image block alt text", "[render][gif]") {
    Message msg;
    msg.ts = "1718000000.000100";
    msg.blocks.push_back(gifBlock());
    const QString html = MsgRender::buildMsgHtml(msg, nullptr);
    CHECK(html.contains("a man is sweating"));
    CHECK(!html.contains("<img src='https://media2.giphy.com"));
}

TEST_CASE("buildMsgHtml with gif context embeds image + title toggle", "[render][gif]") {
    Message msg;
    msg.ts   = "1718000000.000100";
    msg.text = MrkdwnParser::parse("a man is sweating"); // fallback text duplicate
    msg.blocks.push_back(gifBlock());
    const QSet<QString>               collapsed;
    const MsgRender::GifRenderContext gif{msg.ts, &collapsed};
    const QString                     html = MsgRender::buildMsgHtml(msg, nullptr, &gif);
    CHECK(html.contains("<img src='https://media2.giphy.com/media/abc/giphy.gif'"));
    // 480×360 scaled into the 400×300 cap.
    CHECK(html.contains("width='400'"));
    CHECK(html.contains("height='300'"));
    // Title line is a collapse-toggle anchor.
    CHECK(html.contains(MsgRender::kGifToggleAnchorPrefix + msg.ts + "/b0"));
    CHECK(html.contains(">GIF&nbsp;<img src='" + MsgRender::kGifChevronExpandedRes));
    // The text field must NOT leak in below the image.
    CHECK(!html.contains("a man is sweating"));
}

TEST_CASE("buildMsgHtml collapsed image block keeps title, drops image", "[render][gif]") {
    Message msg;
    msg.ts = "1718000000.000100";
    msg.blocks.push_back(gifBlock());
    QSet<QString>                     collapsed{msg.ts + "/b0"};
    const MsgRender::GifRenderContext gif{msg.ts, &collapsed};
    const QString                     html = MsgRender::buildMsgHtml(msg, nullptr, &gif);
    CHECK(!html.contains("giphy.gif"));
    CHECK(html.contains(MsgRender::kGifChevronCollapsedRes));
}

TEST_CASE("buildAttachHtml with gif context embeds image and skips fallback", "[render][gif]") {
    Attachment att;
    att.fallback = "a man is sweating";
    att.blocks.push_back(gifBlock());
    const QSet<QString>               collapsed;
    const MsgRender::GifRenderContext gif{"1718000000.000100/a0", &collapsed};
    const QString                     html = MsgRender::buildAttachHtml(att, nullptr, &gif);
    CHECK(html.contains("giphy.gif"));
    CHECK(html.contains(MsgRender::kGifToggleAnchorPrefix + "1718000000.000100/a0/b0"));
    CHECK(!html.contains("a man is sweating"));
}

TEST_CASE("attachIsImageOnly true only for pure image-block attachments", "[render][gif]") {
    Attachment att;
    att.blocks.push_back(gifBlock());
    CHECK(MsgRender::attachIsImageOnly(att));
    att.title = "GIF from Giphy";
    CHECK(!MsgRender::attachIsImageOnly(att));
    CHECK(!MsgRender::attachIsImageOnly(Attachment{}));
}

// ── lastReplyLabel ────────────────────────────────────────────────────────────

TEST_CASE("lastReplyLabel uses 'today at' wording for today's ts", "[render][reply]") {
    const qint64  now   = QDateTime::currentSecsSinceEpoch();
    const QString label = MsgRender::lastReplyLabel(QString::number(now) + ".000100");
    CHECK(label.startsWith("today at "));
}

TEST_CASE("lastReplyLabel invalid ts → empty", "[render][reply]") {
    CHECK(MsgRender::lastReplyLabel("not-a-ts").isEmpty());
}

// ── Channel mention rendering ─────────────────────────────────────────────────

// Minimal Backend so a Session can be seeded with conversations.
namespace {
struct RenderStubBackend : Backend {
    rpl::variable<AuthState>                 _auth{AuthState::LoggedIn};
    rpl::variable<UserId>                    _me;
    rpl::variable<std::vector<Conversation>> _convs;
    rpl::variable<std::vector<User>>         _users;
    rpl::event_stream<Event>                 _events;

    rpl::producer<AuthState>                 authState() const override { return _auth.value(); }
    Capabilities                             capabilities() const override { return {}; }
    void                                     connectRealtime() override {}
    void                                     disconnectRealtime() override {}
    rpl::producer<UserId>                    loadMe() override { return _me.value(); }
    rpl::producer<std::vector<Conversation>> loadConversations() override { return _convs.value(); }
    rpl::producer<std::vector<User>>         loadUsers() override { return _users.value(); }
    rpl::producer<bool> loadPresence(UserId) override { return rpl::variable<bool>(false).value(); }
    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override {
        return rpl::variable<MessagePage>({}).value();
    }
    void sendMessage(ConversationId, OutgoingMessage) override {}
    void editMessage(ConversationId, Ts, TextWithEntities) override {}
    void deleteMessage(ConversationId, Ts) override {}
    void addReaction(ConversationId, Ts, QString) override {}
    void removeReaction(ConversationId, Ts, QString) override {}
    void markRead(ConversationId, Ts) override {}
    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override {
        return rpl::variable<std::vector<SearchResult>>({}).value();
    }
    rpl::producer<QHash<QString, QString>> loadEmojiList() override {
        return rpl::variable<QHash<QString, QString>>({}).value();
    }
    void uploadFiles(
        ConversationId,
        const QStringList &,
        const QString &,
        std::function<void(bool, QString)> = {}
    ) override {}
    void downloadFile(
        const QString &, std::function<void(QByteArray)>, std::function<void(QString)>
    ) override {}
    rpl::producer<Event> events() const override { return _events.events(); }
};

static Session *renderSession() {
    auto        *stub = new RenderStubBackend;
    Conversation c;
    c.id         = ConversationId{"C1"};
    c.name       = "general";
    c.kind       = ConvKind::PublicChannel;
    stub->_convs = std::vector<Conversation>{c};
    User u;
    u.id          = UserId{"U7"};
    u.name        = "alice";
    u.displayName = "Alice";
    stub->_users  = std::vector<User>{u};
    auto *session = new Session(std::unique_ptr<Backend>(stub), "T_TEST");
    session->start();
    return session;
}
} // namespace

TEST_CASE("toHtml renders a labeled channel link as its name", "[render][channel]") {
    const QString html = MsgRender::toHtml(MrkdwnParser::parse("see <#C1|general>"), nullptr);
    CHECK(html.contains("#general"));
    CHECK(!html.contains("#C1"));
}

TEST_CASE("toHtml without session falls back to the channel id", "[render][channel]") {
    // A bare link has no name part; with no cache to consult, the id shows.
    const QString html = MsgRender::toHtml(MrkdwnParser::parse("see <#C1>"), nullptr);
    CHECK(html.contains("#C1"));
}

TEST_CASE("toHtml resolves a bare channel link via the session", "[render][channel]") {
    auto         *session = renderSession();
    const QString html    = MsgRender::toHtml(MrkdwnParser::parse("see <#C1>"), session);
    CHECK(html.contains("#general"));
    CHECK(!html.contains("#C1"));
    delete session;
}

// ── notificationText (OS toast preview) ───────────────────────────────────────

TEST_CASE("notificationText converts builtin emoji codes to glyphs", "[render][notif]") {
    const QString out =
        MsgRender::notificationText(MrkdwnParser::parse("ship it :rocket:"), nullptr);
    CHECK(out == QString::fromUtf8("ship it 🚀"));
    CHECK(!out.contains(":rocket:"));
}

TEST_CASE("notificationText resolves a bare channel link via the session", "[render][notif]") {
    auto         *session = renderSession();
    const QString out     = MsgRender::notificationText(MrkdwnParser::parse("join <#C1>"), session);
    CHECK(out == "join #general");
    delete session;
}

TEST_CASE("notificationText keeps a labeled mention's name and drops the id", "[render][notif]") {
    // Without a session, the parser's baked label survives; the raw id never shows.
    const QString out =
        MsgRender::notificationText(MrkdwnParser::parse("hey <@U7|alice> ping"), nullptr);
    CHECK(out == "hey alice ping");
    CHECK(!out.contains("U7"));
}

TEST_CASE("notificationText resolves a bare mention via the session", "[render][notif]") {
    auto         *session = renderSession();
    const QString out     = MsgRender::notificationText(MrkdwnParser::parse("ping <@U7>"), session);
    CHECK(out == "ping @Alice");
    CHECK(!out.contains("U7"));
    delete session;
}

TEST_CASE("notificationText leaves unknown custom emoji as a code", "[render][notif]") {
    const QString out =
        MsgRender::notificationText(MrkdwnParser::parse("lunch? :no-lunch:"), nullptr);
    CHECK(out.contains(":no-lunch:"));
}

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

#include "ui/message_list/message_render.h"
#include "text/mrkdwn_parser.h"

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

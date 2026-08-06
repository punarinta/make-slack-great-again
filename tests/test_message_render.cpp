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

// ── email quoted-reply collapsing ───────────────────────────────────────────

TEST_CASE("buildMsgHtml strips an email reply's quoted trailer", "[render][quote]") {
    TextWithEntities twe;
    const QString    body   = QStringLiteral("Sounds good, shipping today.\n");
    const QString    quoted = QStringLiteral("On Mon, Alice wrote:\n") + QString(400, 'q');
    twe.text                = body + quoted;
    // A dominant trailing blockquote over the quoted region (>40% of the text,
    // ending in the tail) is what marks it as quoted history.
    twe.entities.push_back(
        TextEntity{EntityType::Blockquote, (int)body.size(), (int)quoted.size(), {}}
    );
    Message msg;
    msg.text = twe;
    msg.ts   = QStringLiteral("123.456");

    // Email (collapse on): only the sender's new content survives, quote dropped.
    const QString stripped = MsgRender::buildMsgHtml(msg, nullptr, nullptr, /*collapse=*/true);
    CHECK(stripped.contains("Sounds good"));
    CHECK(!stripped.contains("qqqq"));             // quoted history gone
    CHECK(!stripped.contains("Show quoted text")); // no toggle — not chat-like
    CHECK(!stripped.contains("<br"));              // trailing blank line trimmed (no gap)

    // Chat service (collapse off): quotes are intentional, keep everything.
    const QString chat = MsgRender::buildMsgHtml(msg, nullptr, nullptr, /*collapse=*/false);
    CHECK(chat.contains("qqqq"));
}

TEST_CASE("quotedTrailerCut catches a localized Outlook From:/Sent:/To: block", "[render][quote]") {
    TextWithEntities twe; // Chinese Outlook header block, no blockquote or '>' marks
    const QString    body = QStringLiteral(
        "Dear Nikita,\n\nThank you for your email.\n\n"
           "Best regards,\nLeo\n\n"
    );
    const QString quoted = QStringLiteral(
        "发件人: Nikita Bragin <branikita@gmail.com>\n"
        "发送时间: 2026年6月24日 23:37\n"
        "收件人: WowRobo - Leo Xiao <leo.xiao@wowrobo.com>\n"
        "主题: Re: reseller\n\nHello Leo, when do the orders ship?\n"
    );
    twe.text = body + quoted;
    Message msg;
    msg.text = twe;

    const QString stripped = MsgRender::buildMsgHtml(msg, nullptr, nullptr, /*collapse=*/true);
    CHECK(stripped.contains("Thank you for your email"));
    CHECK(!stripped.contains("Hello Leo, when do the orders ship")); // quoted original gone
    CHECK(!stripped.contains("branikita@gmail.com"));                // header block gone
}

TEST_CASE("buildMsgHtml collapses a plain-text email reply (> quotes)", "[render][quote]") {
    TextWithEntities twe; // text/plain path: no entities, quotes are literal '>' lines
    QString          body = QStringLiteral("Great, thank you!\n\nOn Wed, Jun 24 Leo wrote:\n");
    QString          quoted;
    for (int i = 0; i < 12; ++i)
        quoted += QStringLiteral("> quoted history line that is reasonably long here\n");
    twe.text = body + quoted;
    Message msg;
    msg.text = twe;

    const QString stripped = MsgRender::buildMsgHtml(msg, nullptr, nullptr, /*collapse=*/true);
    CHECK(stripped.contains("Great, thank you!"));
    CHECK(!stripped.contains("On Wed, Jun 24"));      // dangling "… wrote:" attribution gone
    CHECK(!stripped.contains("quoted history line")); // '>' history stripped
}

TEST_CASE(
    "quotedTrailerCut strips a '---' signature, attribution and quote together", "[render][quote]"
) {
    TextWithEntities twe; // Russian-locale Gmail attribution (no "wrote"), '---' signature
    twe.text = QStringLiteral(
                   "Hello, Leo, please let me know when shipping?\n\n"
                   "---\nNikita Bragin\nPhone: +374 55 934025\n\n"
                   "чт, 18 июн. 2026 г., 10:07 AM Nikita <branikita@gmail.com>:\n"
               ) +
               QString(300, 'z');
    Message msg;
    msg.text = twe;

    const QString out = MsgRender::buildMsgHtml(msg, nullptr, nullptr, /*collapse=*/true);
    CHECK(out.contains("Hello, Leo"));
    CHECK(!out.contains("Nikita Bragin")); // signature stripped
    CHECK(!out.contains("Phone"));
    CHECK(!out.contains("branikita@gmail.com")); // dangling attribution stripped
    CHECK(!out.contains("zzz"));                 // quoted body stripped
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

TEST_CASE("buildAttachHtml linked title substitutes emoji shortcodes", "[render][attachment]") {
    // CI bots (e.g. AWS CodePipeline via Amazon Q) put shortcodes in linked
    // titles; the anchor can't take full mrkdwn but emoji must still resolve.
    Attachment att;
    att.title          = ":white_check_mark: AWS CodePipeline Notification";
    att.titleLink      = "https://console.aws.example/pipeline";
    const QString html = MsgRender::buildAttachHtml(att, nullptr);
    CHECK(html.contains("<a href='https://console.aws.example/pipeline'"));
    CHECK(html.contains(QString::fromUtf8("✅"))); // ✅
    CHECK(!html.contains("white_check_mark"));
    CHECK(html.contains("AWS CodePipeline Notification"));
}

TEST_CASE("buildAttachHtml footer renders after fallback content", "[render][attachment]") {
    Attachment att;
    att.fallback       = "fb text";
    att.footer         = "via Bot";
    const QString html = MsgRender::buildAttachHtml(att, nullptr);
    CHECK(html.indexOf("fb text") >= 0);
    CHECK(html.indexOf("via Bot") > html.indexOf("fb text"));
}

TEST_CASE("buildAttachHtml renders a table block instead of the fallback", "[render][attachment]") {
    // Slack's newer table messages arrive as an attachment whose only content is
    // a "table" block, with fallback "[no preview available]" — the table must
    // render and the fallback must not.
    Attachment att;
    att.fallback = "[no preview available]";
    Block blk;
    blk.typeStr = "table";
    blk.tableRows.push_back({TextWithEntities{"Header", {}}, TextWithEntities{"", {}}});
    blk.tableRows.push_back({TextWithEntities{"ATC", {}}, TextWithEntities{"18.2", {}}});
    att.blocks.push_back(blk);
    const QString html = MsgRender::buildAttachHtml(att, nullptr);
    CHECK(html.contains("<table"));
    CHECK(html.contains("border-collapse:collapse"));
    CHECK(html.contains("Header"));
    CHECK(html.contains("ATC"));
    CHECK(html.contains("18.2"));
    CHECK(!html.contains("no preview available"));
}

static Block makeTable(int rows) {
    Block blk;
    blk.typeStr = "table";
    for (int i = 0; i < rows; ++i)
        blk.tableRows.push_back(
            {TextWithEntities{QString("r%1c0").arg(i), {}},
             TextWithEntities{QString("r%1c1").arg(i), {}}}
        );
    return blk;
}

TEST_CASE("tableBlockHtml caps inline tables at 10 rows and shades the last", "[render][table]") {
    const Block   blk    = makeTable(15);
    const QString capped = MsgRender::tableBlockHtml(blk, nullptr, MsgRender::kMaxInlineTableRows);
    CHECK(capped.count("<tr>") == 10);
    CHECK(capped.contains("r9c0"));
    CHECK(!capped.contains("r10c0"));
    // The 10th row is shaded as the "there's more" cue.
    CHECK(capped.count("color:") >= 2); // border-color + the shaded cells
    const int lastRow = capped.lastIndexOf("<tr>");
    CHECK(capped.indexOf("color:", lastRow) > 0);

    // Full render (the overlay): every row, nothing shaded.
    const QString full = MsgRender::tableBlockHtml(blk, nullptr, -1);
    CHECK(full.count("<tr>") == 15);
    CHECK(full.contains("r14c1"));
    const int fullLastRow = full.lastIndexOf("<tr>");
    CHECK(full.indexOf("color:", fullLastRow) < 0);
}

TEST_CASE("tableBlockHtml renders all rows when not truncated", "[render][table]") {
    const QString html = MsgRender::tableBlockHtml(makeTable(3), nullptr, 10);
    CHECK(html.count("<tr>") == 3);
    const int lastRow = html.lastIndexOf("<tr>");
    CHECK(html.indexOf("color:", lastRow) < 0); // no shading — nothing was cut
}

TEST_CASE("attachIsTableOnly true for a bare table attachment", "[render][table]") {
    Attachment att;
    att.fallback = "[no preview available]"; // fallback text doesn't count as content
    att.blocks.push_back(makeTable(3));
    CHECK(MsgRender::attachIsTableOnly(att));

    Attachment withTitle = att;
    withTitle.title      = "Report";
    CHECK(!MsgRender::attachIsTableOnly(withTitle));

    Attachment noTable;
    noTable.text = TextWithEntities{"plain", {}};
    CHECK(!MsgRender::attachIsTableOnly(noTable));
}

TEST_CASE("csvToTableBlock parses plain comma CSV", "[render][csv]") {
    const Block blk = MsgRender::csvToTableBlock("a,b,c\n1,2,3\n");
    CHECK(blk.typeStr == "table");
    REQUIRE(blk.tableRows.size() == 2);
    REQUIRE(blk.tableRows[0].size() == 3);
    CHECK(blk.tableRows[0][0].text == "a");
    CHECK(blk.tableRows[1][2].text == "3");
}

TEST_CASE("csvToTableBlock handles RFC 4180 quoting", "[render][csv]") {
    // Quoted fields may contain the delimiter, newlines and doubled quotes.
    const Block blk = MsgRender::csvToTableBlock(
        "name,notes\n\"Doe, Jane\",\"line1\nline2\"\nx,\"say \"\"hi\"\"\""
    );
    REQUIRE(blk.tableRows.size() == 3);
    CHECK(blk.tableRows[1][0].text == "Doe, Jane");
    CHECK(blk.tableRows[1][1].text == "line1\nline2");
    CHECK(blk.tableRows[2][1].text == "say \"hi\"");
}

TEST_CASE("csvToTableBlock strips BOM and handles CRLF + missing final newline", "[render][csv]") {
    const Block blk = MsgRender::csvToTableBlock(
        "\xEF\xBB\xBF"
        "a,b\r\nc,d"
    );
    REQUIRE(blk.tableRows.size() == 2);
    CHECK(blk.tableRows[0][0].text == "a"); // no U+FEFF prefix
    CHECK(blk.tableRows[1][1].text == "d"); // last line flushed without trailing newline
}

TEST_CASE("csvToTableBlock sniffs semicolon and tab delimiters", "[render][csv]") {
    const Block semi = MsgRender::csvToTableBlock("a;b;c\n1;2;3\n");
    REQUIRE(semi.tableRows.size() == 2);
    CHECK(semi.tableRows[0].size() == 3);
    CHECK(semi.tableRows[0][1].text == "b");

    const Block tabs = MsgRender::csvToTableBlock("a\tb\n1\t2\n");
    REQUIRE(tabs.tableRows.size() == 2);
    CHECK(tabs.tableRows[0].size() == 2);

    // A quoted comma in the first line doesn't fool the sniffer into commas.
    const Block quoted = MsgRender::csvToTableBlock("\"a,x\";b\n1;2\n");
    CHECK(quoted.tableRows[0].size() == 2);
    CHECK(quoted.tableRows[0][0].text == "a,x");
}

TEST_CASE("csvToTableBlock skips blank lines and empty input", "[render][csv]") {
    const Block blk = MsgRender::csvToTableBlock("a,b\n\n\nc,d\n\n");
    REQUIRE(blk.tableRows.size() == 2);
    CHECK(blk.tableRows[1][0].text == "c");

    CHECK(MsgRender::csvToTableBlock("").tableRows.empty());
    CHECK(MsgRender::csvToTableBlock("\n\n").tableRows.empty());
}

TEST_CASE("dataTableRects finds data tables but not code/quote/button tables", "[render][table]") {
    const auto    twe = MrkdwnParser::parse("```\ncode here\n```\n> quoted line");
    QTextDocument doc;
    doc.setHtml(
        MsgRender::toHtml(twe, nullptr) + MsgRender::tableBlockHtml(makeTable(4), nullptr, 10)
    );
    doc.setTextWidth(400);
    const auto rects = MsgRender::dataTableRects(&doc);
    REQUIRE(rects.size() == 1);
    CHECK(rects[0].height() > 10);
    // The code block is still detected separately (marker overlap would break
    // its chrome).
    CHECK(MsgRender::codeBlockRects(&doc).size() == 1);
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

TEST_CASE("buildAttachHtml resolves Slack tokens in the title", "[render][attachment]") {
    // Outlook Calendar reminders put <!date^…> ranges and the <url|label> event
    // link in the attachment *title* (no title_link) — they must render resolved,
    // not as literal tokens.
    Attachment att;
    att.title          = "<!date^1782976500^{time}|10:15 AM> - <!date^1782977400^{time}|10:30 AM> "
                         "<https://outlook.office365.com/owa/?itemid=AAk&amp;exvsurl=1&amp;"
                         "path=/calendar/item|Hitta Mer Stand-Up>";
    const QString html = MsgRender::buildAttachHtml(att, nullptr);
    CHECK(!html.contains("!date"));      // date tokens resolved to formatted times
    CHECK(!html.contains("1782976500")); // raw ts gone
    CHECK(html.contains("font-weight:bold"));
    CHECK(html.contains("Hitta Mer Stand-Up</a>")); // event link is clickable
    CHECK(html.contains("https://outlook.office365.com/owa/?itemid=AAk&amp;exvsurl=1"));
}

TEST_CASE(
    "buildAttachHtml title tokens collapse to text inside a title_link", "[render][attachment]"
) {
    Attachment att;
    att.title          = "Event <https://example.com/inner|details>";
    att.titleLink      = "https://example.com/outer";
    const QString html = MsgRender::buildAttachHtml(att, nullptr);
    // The whole title is one anchor to title_link; the embedded token collapses
    // to its label (anchors can't nest).
    CHECK(html.contains("<a href='https://example.com/outer'"));
    CHECK(html.contains("Event details</a>"));
    CHECK(!html.contains("example.com/inner"));
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
        std::optional<Ts>                  = std::nullopt,
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

// ── Message-link chips ────────────────────────────────────────────────────────

static const QString kPermalink = QStringLiteral(
    "https://cityteam.slack.com/archives/C1/"
    "p1786008939071009"
);

TEST_CASE("toHtml turns a bare message permalink into a chip", "[render][msglink]") {
    auto         *session = renderSession();
    const QString html    = MsgRender::toHtml(MrkdwnParser::parse("<" + kPermalink + ">"), session);
    // Clickable as an internal target, not as the raw URL: this is what makes the
    // click jump to the message instead of leaving for the browser.
    const auto    ref =
        MsgRender::messageRefFromAnchor(html.section("href='", 1, 1).section('\'', 0, 0));
    CHECK(ref.isValid());
    CHECK(ref.conv == "C1");
    CHECK(ref.ts == "1786008939.071009");
    // Reads as the conversation, not as a URL.
    CHECK(html.contains("#general"));
    CHECK(!html.contains("archives"));
    CHECK(html.contains(MsgRender::kMessageLinkIconRes));
    delete session;
}

TEST_CASE("a message-link chip keeps the thread root of a reply", "[render][msglink]") {
    auto         *session = renderSession();
    const QString url     = kPermalink + "?thread_ts=1786008900.000100&amp;cid=C1";
    const QString html    = MsgRender::toHtml(MrkdwnParser::parse("<" + url + ">"), session);
    const auto    ref =
        MsgRender::messageRefFromAnchor(html.section("href='", 1, 1).section('\'', 0, 0));
    REQUIRE(ref.isValid());
    CHECK(ref.threadTs == "1786008900.000100");
    delete session;
}

TEST_CASE("a permalink the author gave link text stays a plain link", "[render][msglink]") {
    // "<url|see this>" — the author's words are the message; replacing them with
    // a chip would delete what they wrote. Same rule the official client follows.
    auto         *session = renderSession();
    const QString html =
        MsgRender::toHtml(MrkdwnParser::parse("<" + kPermalink + "|see this>"), session);
    CHECK(html.contains("see this"));
    CHECK(html.contains("href='" + kPermalink));
    CHECK_FALSE(html.contains(MsgRender::kMessageAnchorPrefix));
    delete session;
}

TEST_CASE("an ordinary Slack URL is not a message link", "[render][msglink]") {
    auto         *session = renderSession();
    const QString html =
        MsgRender::toHtml(MrkdwnParser::parse("<https://cityteam.slack.com/home>"), session);
    CHECK_FALSE(html.contains(MsgRender::kMessageAnchorPrefix));
    delete session;
}

TEST_CASE("a link into another workspace still reads as a message", "[render][msglink]") {
    // C9 isn't a conversation of this workspace, so there is no name to show —
    // the chip must still say what it is (clicking it opens the permalink).
    auto         *session = renderSession();
    const QString url     = "https://other.slack.com/archives/C9/p1786008939071009";
    const QString html    = MsgRender::toHtml(MrkdwnParser::parse("<" + url + ">"), session);
    CHECK(html.contains(MsgRender::kMessageAnchorPrefix));
    CHECK_FALSE(html.contains("C9<"));
    delete session;
}

TEST_CASE("hasMessageLink finds a permalink inside an attachment", "[render][msglink]") {
    Message msg;
    msg.text = MrkdwnParser::parse("no link here");
    CHECK_FALSE(MsgRender::hasMessageLink(msg));
    Attachment att;
    att.text = MrkdwnParser::parse("<" + kPermalink + ">");
    msg.attachments.push_back(att);
    CHECK(MsgRender::hasMessageLink(msg));
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

// ── notificationPreview (whole-message toast body) ────────────────────────────

TEST_CASE("notificationPreview prefers the message text", "[render][notif]") {
    Message msg;
    msg.text = MrkdwnParser::parse("deploy finished");
    Attachment att;
    att.text = MrkdwnParser::parse("ignored");
    msg.attachments.push_back(att);
    CHECK(MsgRender::notificationPreview(msg, nullptr) == "deploy finished");
}

TEST_CASE(
    "notificationPreview flattens Block Kit blocks of a textless bot post", "[render][notif]"
) {
    // The CodePipeline/Amazon Q shape: no `text`, everything in blocks.
    Message msg;
    Block   header;
    header.typeStr = "header";
    header.text    = TextWithEntities{"Pipeline failed", {}};
    Block section;
    section.typeStr = "section";
    section.text    = MrkdwnParser::parse("stage *Deploy* on <#C1>");
    msg.blocks      = {header, section};

    auto         *session = renderSession();
    const QString out     = MsgRender::notificationPreview(msg, session);
    CHECK(out == QString::fromUtf8("Pipeline failed · stage Deploy on #general"));
    delete session;
}

TEST_CASE("notificationPreview summarises attachment title, text and fields", "[render][notif]") {
    Message    msg;
    Attachment att;
    att.title = "Build &amp; deploy";
    att.text  = MrkdwnParser::parse("failed after 3m");
    att.fields.push_back(AttachmentField{"Env &amp; region", MrkdwnParser::parse("prod")});
    msg.attachments.push_back(att);

    const QString out = MsgRender::notificationPreview(msg, nullptr);
    CHECK(out == QString::fromUtf8("Build & deploy · failed after 3m · Env & region: prod"));
}

TEST_CASE("notificationPreview falls back to the attachment fallback text", "[render][notif]") {
    Message    msg;
    Attachment att;
    att.fallback = "New build <https://ci.example/1|#1> ready";
    msg.attachments.push_back(att);
    // The fallback is mrkdwn: the link token resolves to its label, not raw markup.
    CHECK(MsgRender::notificationPreview(msg, nullptr) == "New build #1 ready");
}

TEST_CASE(
    "notificationPreview skips a placeholder fallback of a table attachment", "[render][notif]"
) {
    // Table messages carry "[no preview available]" as their fallback; the message
    // list drops it, and the toast must not surface it either.
    Message    msg;
    Attachment att;
    att.fallback = "[no preview available]";
    Block blk;
    blk.typeStr = "table";
    blk.tableRows.push_back({TextWithEntities{"Header", {}}});
    att.blocks.push_back(blk);
    msg.attachments.push_back(att);
    CHECK(MsgRender::notificationPreview(msg, nullptr).isEmpty());
}

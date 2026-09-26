// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// CanvasEmoji::expandInHtml — turns Slack's literal ":code:" shortcodes in the
// downloaded canvas HTML into rendered emoji (Unicode glyphs for built-ins,
// inline <img src="emoji:name"> for workspace custom emoji), without ever
// touching colons that live inside tags/attributes.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include "ui/canvas_page/canvas_display.h"
#include "ui/canvas_page/canvas_emoji.h"

#include <QApplication>
#include <QHash>
#include <QTextBlock>
#include <QTextDocument>

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-canvas-emoji");
    app.setOrganizationName("msga-test");
    return msga_test::runCatch(argc, argv);
}

using CanvasEmoji::expandInHtml;

namespace {
const QHash<QString, QString> kNoCustom;
}

TEST_CASE("built-in shortcodes become Unicode glyphs", "[canvas-emoji]") {
    const QString out = expandInHtml("<p>party :tada: time</p>", kNoCustom);
    CHECK(out.contains(QString::fromUtf8("\xF0\x9F\x8E\x89"))); // 🎉
    CHECK(!out.contains(":tada:"));
}

TEST_CASE("unknown shortcodes are left untouched", "[canvas-emoji]") {
    const QString in  = "<p>:definitely_not_an_emoji_xyz:</p>";
    const QString out = expandInHtml(in, kNoCustom);
    CHECK(out == in);
}

TEST_CASE("custom emoji become an emoji: <img>, not a raw URL", "[canvas-emoji]") {
    // A name with no built-in glyph — built-ins always win, so a custom emoji
    // only resolves when its name doesn't collide with a standard one.
    const QHash<QString, QString> map{{"partyparrot", "https://emoji.example.com/pp.gif"}};
    const QString                 out = expandInHtml("<p>:partyparrot: rocks</p>", map);
    CHECK(out.contains("<img src='emoji:partyparrot'"));
    // The wire-stable shortcode form must be used as the src — never the URL,
    // which would otherwise round-trip back to Slack as a markdown image.
    CHECK(!out.contains("emoji.example.com"));
    CHECK(!out.contains(":partyparrot:"));
}

TEST_CASE("alias to a built-in resolves to the glyph", "[canvas-emoji]") {
    const QHash<QString, QString> map{{"yay", "alias:tada"}};
    const QString                 out = expandInHtml("<p>:yay:</p>", map);
    CHECK(out.contains(QString::fromUtf8("\xF0\x9F\x8E\x89"))); // 🎉
}

TEST_CASE("alias to a custom emoji keeps the original name in the src", "[canvas-emoji]") {
    const QHash<QString, QString> map{
        {"blobwave", "alias:partyparrot"},
        {"partyparrot", "https://emoji.example.com/pp.gif"},
    };
    const QString out = expandInHtml("<p>:blobwave:</p>", map);
    // The alias name is what's stored — CanvasEdit resolves it at load time, and
    // saving it back yields ":blobwave:" (a valid Slack alias shortcode).
    CHECK(out.contains("<img src='emoji:blobwave'"));
}

TEST_CASE("colons inside tags and attributes are never expanded", "[canvas-emoji]") {
    // Section ids carry colons; a custom emoji literally named after the id's
    // tail must not be conjured out of an attribute.
    const QHash<QString, QString> map{{"tada", "https://emoji.example.com/x.gif"}};
    const QString                 in  = "<p id=\"temp:C:eBU8ce7\" style=\"color:red\">hi</p>";
    const QString                 out = expandInHtml(in, map);
    CHECK(out.contains("id=\"temp:C:eBU8ce7\""));
    CHECK(out.contains("style=\"color:red\""));
    CHECK(!out.contains("<img")); // nothing in the text run to expand
}

TEST_CASE("text emoji expand while the surrounding markup is preserved", "[canvas-emoji]") {
    const QString out = expandInHtml("<span style=\"color:red\">:tada:</span>", kNoCustom);
    CHECK(out.contains("style=\"color:red\""));
    CHECK(out.contains(QString::fromUtf8("\xF0\x9F\x8E\x89")));
    CHECK(!out.contains(":tada:"));
}

TEST_CASE("html with no colon is returned unchanged", "[canvas-emoji]") {
    const QString in = "<p>plain text, no codes</p>";
    CHECK(expandInHtml(in, kNoCustom) == in);
}

// ── CanvasDisplay (shared by the editor and the message-list preview) ────────

TEST_CASE("standard emoji pictures are not drawn twice", "[canvas-display]") {
    // Huddle-notes canvases carry <img data-is-slack>:code:</img>; the img is
    // void to Qt, so keeping it would show the picture AND the expanded code.
    const QString out = CanvasDisplay::prepareHtml(
        "<h1><control><img src=\"https://x/1f3a7.png\" alt=\"headphones\" data-is-slack "
        "style=\"width: 30px\">:headphones:</img></control> Huddle notes</h1>",
        nullptr
    );
    CHECK(!out.contains("<img"));
    CHECK(!out.contains("</img>"));
    CHECK(out.count(QString::fromUtf8("\xF0\x9F\x8E\xA7")) == 1); // 🎧, once
}

TEST_CASE("bare mention anchors become user anchors", "[canvas-display]") {
    const QString out = CanvasDisplay::prepareHtml("<p><a>@U6HQGJ6GZ</a> said</p>", nullptr);
    CHECK(out.contains("<a href=\"msga://user/U6HQGJ6GZ\">@U6HQGJ6GZ</a> said"));
}

TEST_CASE("lnk hyperlinks become anchors", "[canvas-display]") {
    const QString out =
        CanvasDisplay::prepareHtml("<p><lnk href=\"https://e.x/\">View</lnk></p>", nullptr);
    CHECK(out.contains("<a href=\"https://e.x/\">View</a>"));
}

TEST_CASE("canvas titles are decoded with codes and mentions resolved", "[canvas-display]") {
    CHECK(
        CanvasDisplay::title(":headphones: Notes with &lt;@U1&gt; and &lt;@U2|bob&gt;", nullptr) ==
        QString::fromUtf8("\xF0\x9F\x8E\xA7") + " Notes with @U1 and @bob"
    );
}

TEST_CASE("canvas headings get one shared size scale", "[canvas-display]") {
    CHECK(CanvasDisplay::headingPx(1, 15) == 23);
    CHECK(CanvasDisplay::headingPx(2, 15) == 20);
    CHECK(CanvasDisplay::headingPx(3, 15) == 17);
    CHECK(CanvasDisplay::headingPx(4, 15) == 15);

    QTextDocument doc;
    doc.setHtml("<h2>Summary</h2><p>body</p>");
    CanvasDisplay::styleHeadings(&doc, 15);
    const QTextBlock h = doc.begin();
    REQUIRE(h.blockFormat().headingLevel() == 2); // level kept for toMarkdown
    const QTextCharFormat cf = h.begin().fragment().charFormat();
    CHECK(cf.font().pixelSize() == 20);
    CHECK(cf.font().bold());
    CHECK(!cf.hasProperty(QTextFormat::FontSizeAdjustment));
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Unit tests for HTML email → TextWithEntities conversion (Phase 6).
#include <catch2/catch_test_macros.hpp>

#include "backend/imap/imap_html.h"

using namespace imap;

namespace {
// Find the first entity of a given type, or a sentinel with length -1.
TextEntity find(const TextWithEntities &t, EntityType type) {
    for (const auto &e : t.entities)
        if (e.type == type)
            return e;
    return TextEntity{type, 0, -1, {}};
}
QString span(const TextWithEntities &t, const TextEntity &e) {
    return e.length < 0 ? QString() : t.text.mid(e.offset, e.length);
}
} // namespace

TEST_CASE("html: entity decoding", "[imap][html]") {
    CHECK(decodeHtmlEntities("a &amp; b &lt;c&gt; &#39;q&#39; &#x41;") == "a & b <c> 'q' A");
    CHECK(decodeHtmlEntities("x&nbsp;y") == "x y"); // nbsp → normal space
}

TEST_CASE("html: Latin-1 named entities (accented letters) decode", "[imap][html]") {
    // Swedish / German / French letters common in real mail.
    CHECK(
        decodeHtmlEntities("ol&auml;st fr&aring;n f&ouml;rs&ouml;kt") ==
        QString::fromUtf8("oläst från försökt")
    );
    CHECK(
        decodeHtmlEntities("Gr&uuml;&szlig;e, na&iuml;ve caf&eacute;") ==
        QString::fromUtf8("Grüße, naïve café")
    );
    CHECK(decodeHtmlEntities("&euro;50 &copy;2026") == QString::fromUtf8("€50 ©2026"));
}

TEST_CASE("html: bold / italic spans land on the right text", "[imap][html]") {
    const auto t = htmlToEntities("<p>Hello <b>bold</b> and <i>italic</i></p>");
    CHECK(t.text == "Hello bold and italic");
    CHECK(span(t, find(t, EntityType::Bold)) == "bold");
    CHECK(span(t, find(t, EntityType::Italic)) == "italic");
}

TEST_CASE("html: links capture href as data", "[imap][html]") {
    const auto t    = htmlToEntities(R"(See <a href="https://x.com/p">the page</a> now)");
    const auto link = find(t, EntityType::Link);
    CHECK(span(t, link) == "the page");
    CHECK(link.data == "https://x.com/p");
}

TEST_CASE("html: scripts and styles are stripped", "[imap][html]") {
    const auto t =
        htmlToEntities("<style>.a{color:red}</style><p>Visible</p><script>alert(1)</script>");
    CHECK(t.text == "Visible");
}

TEST_CASE("html: paragraphs are separated by a blank line; whitespace collapses", "[imap][html]") {
    const auto t = htmlToEntities("<p>One</p><p>Two</p><div>Three\n   spaced</div>");
    CHECK(t.text == "One\n\nTwo\n\nThree spaced");
}

TEST_CASE("html: newlines — div=single, br=single, br+br / empty p = blank line", "[imap][html]") {
    // Gmail-style: divs are tight lines, blank lines come from <br><br> or <div><br></div>.
    CHECK(htmlToEntities("<div>a</div><div>b</div>").text == "a\nb");
    CHECK(htmlToEntities("a<br>b").text == "a\nb");
    CHECK(htmlToEntities("a<br><br>b").text == "a\n\nb");
    CHECK(htmlToEntities("<div>a</div><div><br></div><div>b</div>").text == "a\n\nb");
    // Runs of breaks are capped at 3 newlines (2 blank lines) — no giant gaps.
    CHECK(htmlToEntities("a<br><br><br>b").text == "a\n\n\nb");
    CHECK(htmlToEntities("a<br><br><br><br><br><br>b").text == "a\n\n\nb");
    // No leading/trailing blank lines.
    CHECK(htmlToEntities("<br><br><p>only</p><br><br>").text == "only");
}

TEST_CASE("html: zero-width filler between breaks doesn't open a gap", "[imap][html]") {
    // Marketing mail puts an invisible char (e.g. &zwnj;) on each spacer line;
    // dropped, the breaks collapse to the cap instead of one line per filler char.
    CHECK(htmlToEntities("a<br>&zwnj;<br>&zwnj;<br>&zwnj;<br>&zwnj;<br>b").text == "a\n\n\nb");
    const QString zw = QStringLiteral("x​‌‍y"); // ZWSP/ZWNJ/ZWJ
    CHECK(htmlToEntities(zw).text == "xy");           // dropped, no stray space
}

TEST_CASE("html: numeric / literal non-breaking spaces collapse like ASCII space", "[imap][html]") {
    // Spacer cells/lines filled with &#160; / &#xA0; / a raw U+00A0 must NOT count
    // as real text — otherwise they reset the blank-line cap and open a giant gap
    // (LinkedIn-style table layouts do exactly this). decodeHtmlEntities leaves
    // numeric NBSP as U+00A0, so the collapse must happen at the whitespace test.
    CHECK(htmlToEntities("a<br>&#160;<br>&#160;<br>&#160;<br>&#160;<br>b").text == "a\n\n\nb");
    CHECK(htmlToEntities("a<br>&#xA0;<br>&#xA0;<br>&#xA0;<br>&#xA0;<br>b").text == "a\n\n\nb");
    const QString nb = QStringLiteral("a  b"); // raw NBSP run → single space
    CHECK(htmlToEntities(nb).text == "a b");
    // Table rows that are pure NBSP spacers collapse entirely (rows are line-level
    // blocks and merge) instead of stacking one blank line per spacer row.
    CHECK(
        htmlToEntities(
            "<table><tr><td>x</td></tr><tr><td>&#160;</td></tr>"
            "<tr><td>&#160;</td></tr><tr><td>&#160;</td></tr>"
            "<tr><td>&#160;</td></tr><tr><td>y</td></tr></table>"
        )
            .text == "x\ny"
    );
}

TEST_CASE("html: empty inline elements between blocks can't stack a giant gap", "[imap][html]") {
    // An inline open (<a>, <b>, …) flushes the pending newlines; if it then yields
    // no visible text (a social-icon link wrapping a dropped <img>, an empty <b>),
    // each block boundary used to commit a fresh capped run and the runs stacked
    // into a huge void. The cap must hold against the newlines already in `text`.
    auto run = [](const QString &h) {
        const QString t      = htmlToEntities(h).text;
        int           maxRun = 0, cur = 0;
        for (QChar c : t) {
            if (c == '\n') {
                if (++cur > maxRun)
                    maxRun = cur;
            } else
                cur = 0;
        }
        return maxRun;
    };
    QString links, bolds;
    for (int i = 0; i < 9; ++i) {
        links += "<div><a href=\"x\"><img src=\"i.gif\"></a></div>";
        bolds += "<p><b></b></p>";
    }
    CHECK(run("a" + links + "b") <= 3);
    CHECK(run("a" + bolds + "b") <= 3);
    // The visible text still survives — only the blank gap is capped.
    CHECK(htmlToEntities("top" + links + "bottom").text.contains("top"));
    CHECK(htmlToEntities("top" + links + "bottom").text.contains("bottom"));
}

TEST_CASE("html: list items get bullets", "[imap][html]") {
    const auto t = htmlToEntities("<ul><li>first</li><li>second</li></ul>");
    CHECK(t.text.contains("• first"));
    CHECK(t.text.contains("• second"));
}

TEST_CASE("html: blockquote becomes a Blockquote entity", "[imap][html]") {
    const auto t = htmlToEntities("<blockquote>quoted reply</blockquote>");
    CHECK(span(t, find(t, EntityType::Blockquote)).contains("quoted reply"));
}

TEST_CASE("html: malformed / unclosed tags don't crash and still close entities", "[imap][html]") {
    const auto t = htmlToEntities("<b>unterminated bold <i>and italic");
    CHECK(t.text == "unterminated bold and italic");
    CHECK(find(t, EntityType::Bold).length > 0); // auto-closed at end
    CHECK(find(t, EntityType::Italic).length > 0);
}

TEST_CASE("html: headings render bold on their own line", "[imap][html]") {
    const auto t = htmlToEntities("<h1>Review your payment details</h1><p>Hi there</p>");
    CHECK(span(t, find(t, EntityType::Bold)) == "Review your payment details");
    CHECK(t.text.contains("\n")); // heading is a block → on its own line
}

TEST_CASE("html: display:none subtree is dropped (hidden preheader)", "[imap][html]") {
    const auto t = htmlToEntities(
        "<div style=\"display:none\">Hidden preheader text</div>"
        "<p>Visible body</p>"
    );
    CHECK(t.text == "Visible body");
    CHECK_FALSE(t.text.contains("preheader"));
}

TEST_CASE("html: hidden subtree skips nested same-name tags", "[imap][html]") {
    const auto t = htmlToEntities(
        "<div style=\"display:none\"><div>mobile</div><span>x</span></div>"
        "<div>desktop</div>"
    );
    CHECK(t.text == "desktop");
    CHECK_FALSE(t.text.contains("mobile"));
}

TEST_CASE("html: hidden attribute and visibility:hidden are dropped", "[imap][html]") {
    CHECK(htmlToEntities("<p hidden>nope</p><p>yes</p>").text == "yes");
    CHECK(htmlToEntities("<p style=\"visibility:hidden\">nope</p><p>yes</p>").text == "yes");
}

TEST_CASE("html: a hidden void element does not swallow the rest", "[imap][html]") {
    // <img display:none> has no close tag — must not start a skip.
    const auto t = htmlToEntities("<img src=\"x.gif\" style=\"display:none\"><p>still here</p>");
    CHECK(t.text.contains("still here"));
}

TEST_CASE("html: real-ish marketing snippet stays readable", "[imap][html]") {
    const auto t = htmlToEntities(
        "<html><head><title>x</title></head><body>"
        "<table><tr><td>Hi <strong>Vladimir</strong>,</td></tr></table>"
        "<p>Click <a href=\"https://l.com\">here</a> to confirm.</p>"
        "<img src=\"track.gif\" width=\"1\"></body></html>"
    );
    CHECK(t.text.contains("Hi Vladimir,"));
    CHECK(t.text.contains("Click here to confirm."));
    CHECK(span(t, find(t, EntityType::Bold)) == "Vladimir");
    CHECK(find(t, EntityType::Link).data == "https://l.com");
}

// ── normalizePlainText: the non-HTML body branch ──────────────────────────────

TEST_CASE("plain: simple body is passed through unchanged", "[imap][plain]") {
    CHECK(normalizePlainText("Hello\nworld") == "Hello\nworld");
    CHECK(normalizePlainText("a\n\nb") == "a\n\nb"); // one blank line kept
}

TEST_CASE("plain: runs of bare newlines cap at 3 (2 blank lines)", "[imap][plain]") {
    CHECK(normalizePlainText("a\n\n\nb") == "a\n\n\nb");
    CHECK(normalizePlainText("a\n\n\n\n\n\n\nb") == "a\n\n\nb");
}

TEST_CASE("plain: whitespace-only lines don't reset the blank-line cap", "[imap][plain]") {
    // The bug: a marketing mail pads the gap with lines that contain a space, so a
    // naive counter resets on the space and lets every newline through. Lines of
    // spaces/tabs must collapse exactly like bare blank lines.
    CHECK(normalizePlainText("a\n \n \n \n \nb") == "a\n\n\nb");
    CHECK(normalizePlainText("a\n\t\n  \n\t \nb") == "a\n\n\nb");
    // A genuinely huge spacer run still collapses to the cap.
    CHECK(normalizePlainText("a\n \n \n \n \n \n \n \n \nb") == "a\n\n\nb");
}

TEST_CASE("plain: trailing whitespace is stripped, leading indentation is kept", "[imap][plain]") {
    CHECK(normalizePlainText("line with trailing   \nnext") == "line with trailing\nnext");
    CHECK(normalizePlainText("head\n    indented body") == "head\n    indented body");
    // Inline whitespace within a content line is preserved verbatim.
    CHECK(normalizePlainText("col1\t\tcol2") == "col1\t\tcol2");
}

TEST_CASE("plain: zero-width filler and CR are dropped", "[imap][plain]") {
    CHECK(normalizePlainText("a\r\nb\r\nc") == "a\nb\nc"); // CRLF → LF
    // Every dropped code point: ZWSP, ZWNJ, ZWJ, word-joiner, BOM, soft-hyphen.
    QString zw = QStringLiteral("x");
    for (char16_t u : {0x200B, 0x200C, 0x200D, 0x2060, 0xFEFF, 0x00AD})
        zw += QChar(u);
    zw += QStringLiteral("y");
    CHECK(normalizePlainText(zw) == "xy");
}

TEST_CASE("plain: leading and trailing blank lines are trimmed", "[imap][plain]") {
    CHECK(normalizePlainText("\n\n \n  body \n \n\n") == "body");
}

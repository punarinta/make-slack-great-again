// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// CanvasDiff: section-level diffing between Slack's canvas HTML and the
// locally edited QTextDocument. The HTML fixture below is a verbatim server
// response (Nisdos workspace, June 2026) — if Slack changes the format, these
// tests document what we relied on.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ui/canvas_page/canvas_diff.h"

#include <QApplication>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-canvas-diff");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

using CanvasDiff::Chunk;

// Verbatim url_private response for a canvas created with:
//   # Probe title / paragraph / ## h2 / bullets / tasks / ordered /
//   quote / fenced code / 2×2 table / closing paragraph
// (bullet + task lists were merged into ONE <ul> by Slack; checkbox
// semantics are not present in the HTML at all).
static const char *kRichHtml =
    "<div class=\"quip-canvas-content\">"
    "<h1 id=\"temp:C:eBU896ed3cc4a752b3c3c5e98f80\">Probe title</h1>"
    "<p id=\"temp:C:eBU8ce74241e0d76a5df229181d3\" class=\"line\">Plain paragraph with "
    "<b>bold</b> text.</p>"
    "<h2 id=\"temp:C:eBU8fe207c584244e0696cdf57c2\">Heading two</h2>"
    "<div data-section-style='5' class=\"list-numbering-restart-at\" style=\"--indent0: 0\">"
    "<ul id='temp:C:eBU1b81a3b2d596a132e00b90392'>"
    "<li id='temp:C:eBUda25b2ee1015656307032a72d' value='1'>"
    "<span id=\"temp:C:eBUda25b2ee1015656307032a72d\">item one</span><br/></li>"
    "<li id='temp:C:eBUc7bfb2035544cf487c5ef1856'>"
    "<span id=\"temp:C:eBUc7bfb2035544cf487c5ef1856\">item two with <i>italic</i></span><br/></li>"
    "</ul></div>"
    "<div data-section-style='6' class=\"list-numbering-restart-at\" style=\"--indent0: 0\">"
    "<ul id='temp:C:eBU21f46e60d172903baafe33e19'>"
    "<li id='temp:C:eBUccd5a06d8e8c85f700f4d8c74' value='1'>"
    "<span id=\"temp:C:eBUccd5a06d8e8c85f700f4d8c74\">first</span><br/></li>"
    "<li id='temp:C:eBUad28ef96ec38a218a24cd5136'>"
    "<span id=\"temp:C:eBUad28ef96ec38a218a24cd5136\">second</span><br/></li>"
    "</ul></div>"
    "<p id=\"temp:C:eBUe898662f990e5b50afac8d546\" class=\"line\"></p>"
    "<blockquote><p id=\"temp:C:eBU1de6bc9d0c5b68efde0238abb\" class=\"line\">a quoted "
    "line</p></blockquote>"
    "<p id=\"temp:C:eBUf2b3f64af385184f2235bccc8\" class=\"prettyprint line\">code line "
    "1<br>code line 2</p>"
    "<table><tr><td><p id=\"temp:C:eBUf306bc02452b56894b1ee119f\" class=\"line\">a</p></td>"
    "<td><p id=\"temp:C:eBU3e71c141fde67cc99a13ba75d\" class=\"line\">b</p></td></tr>"
    "<tr><td><p id=\"temp:C:eBU8569112491a2d944ece9c7f07\" class=\"line\">1</p></td>"
    "<td><p id=\"temp:C:eBUf0edc4ed2df7ab3c57d9167ff\" class=\"line\">2</p></td></tr></table>"
    "<p id=\"temp:C:eBU17fe3538a0800e079ca7b6652\" class=\"line\">Last paragraph.</p>"
    "</div>";

static QString bodyHtml() {
    return CanvasDiff::splitTitleH1(QString::fromUtf8(kRichHtml), {"Probe title"}).second;
}

// ── splitTitleH1 ──────────────────────────────────────────────────────────────

TEST_CASE("splitTitleH1 strips the h1 matching the file title", "[canvas-diff]") {
    const auto [title, rest] =
        CanvasDiff::splitTitleH1(QString::fromUtf8(kRichHtml), {"Probe title"});
    CHECK(title == "Probe title");
    CHECK(!rest.contains("Probe title"));
    CHECK(rest.contains("Plain paragraph"));
}

TEST_CASE("splitTitleH1 keeps a content h1 that is not the title", "[canvas-diff]") {
    const auto [title, rest] = CanvasDiff::splitTitleH1(QString::fromUtf8(kRichHtml), {"Untitled"});
    CHECK(title.isEmpty());
    CHECK(rest.contains("Probe title")); // content heading, not the title field
}

// ── parseBaseChunks ───────────────────────────────────────────────────────────

TEST_CASE("parseBaseChunks reads the real canvas structure", "[canvas-diff]") {
    const auto chunks = CanvasDiff::parseBaseChunks(bodyHtml());
    REQUIRE(chunks.has_value());
    // 9 top-level sections, minus the empty spacer <p> (excluded on both
    // sides because Qt's HTML import drops empty paragraphs).
    REQUIRE(chunks->size() == 8);

    CHECK((*chunks)[0].kind == Chunk::Kind::Para);
    CHECK((*chunks)[0].id == "temp:C:eBU8ce74241e0d76a5df229181d3");
    CHECK((*chunks)[1].kind == Chunk::Kind::Heading);
    CHECK((*chunks)[2].kind == Chunk::Kind::List);
    CHECK((*chunks)[2].id == "temp:C:eBU1b81a3b2d596a132e00b90392"); // the <ul>, not the <div>
    CHECK((*chunks)[3].kind == Chunk::Kind::List);
    CHECK((*chunks)[4].kind == Chunk::Kind::Quote);
    CHECK((*chunks)[4].fragile);
    CHECK((*chunks)[5].kind == Chunk::Kind::Para); // code: <p class="prettyprint">
    CHECK((*chunks)[6].kind == Chunk::Kind::Table);
    CHECK((*chunks)[6].fragile);
    CHECK((*chunks)[7].md.contains("Last paragraph"));
}

TEST_CASE("parseBaseChunks rejects unknown structure", "[canvas-diff]") {
    CHECK(!CanvasDiff::parseBaseChunks("<hr><p id=\"x\">hi</p>").has_value());
    CHECK(!CanvasDiff::parseBaseChunks("stray text").has_value());
    CHECK(!CanvasDiff::parseBaseChunks("<p>no id</p>").has_value());
}

TEST_CASE("parseBaseChunks of empty body is empty", "[canvas-diff]") {
    const auto chunks = CanvasDiff::parseBaseChunks(QString());
    REQUIRE(chunks.has_value());
    CHECK(chunks->empty());
}

// ── Round-trip stability ──────────────────────────────────────────────────────

// The unchanged document must produce zero ops — this is the property that
// keeps autosave from rewriting (and thus clobbering) untouched sections.
TEST_CASE("unchanged document diffs to zero ops", "[canvas-diff]") {
    const auto base = CanvasDiff::parseBaseChunks(bodyHtml());
    REQUIRE(base.has_value());

    QTextDocument doc;
    doc.setHtml(bodyHtml());
    const auto current = CanvasDiff::documentChunks(&doc);

    REQUIRE(current.size() == base->size());
    for (size_t i = 0; i < current.size(); ++i) {
        INFO(
            "chunk " << i << " base=[" << (*base)[i].md.toStdString() << "] current=["
                     << current[i].md.toStdString() << "]"
        );
        CHECK(current[i].kind == (*base)[i].kind);
        CHECK(current[i].md == (*base)[i].md);
    }

    const auto ops = CanvasDiff::diff(*base, current);
    REQUIRE(ops.has_value());
    CHECK(ops->empty());
}

// ── Edits ─────────────────────────────────────────────────────────────────────

namespace {

QTextBlock findBlock(QTextDocument &doc, const QString &needle) {
    for (auto b = doc.begin(); b != doc.end(); b = b.next())
        if (b.text().contains(needle))
            return b;
    return {};
}

} // namespace

TEST_CASE("editing one paragraph yields one ReplaceSection", "[canvas-diff]") {
    const auto base = CanvasDiff::parseBaseChunks(bodyHtml());
    REQUIRE(base.has_value());

    QTextDocument doc;
    doc.setHtml(bodyHtml());
    auto block = findBlock(doc, "Last paragraph");
    REQUIRE(block.isValid());
    QTextCursor c(block);
    c.movePosition(QTextCursor::EndOfBlock);
    c.insertText(" Edited.");

    const auto ops = CanvasDiff::diff(*base, CanvasDiff::documentChunks(&doc));
    REQUIRE(ops.has_value());
    REQUIRE(ops->size() == 1);
    CHECK((*ops)[0].op == CanvasChange::Op::ReplaceSection);
    CHECK((*ops)[0].sectionId == "temp:C:eBU17fe3538a0800e079ca7b6652");
    CHECK((*ops)[0].markdown.contains("Edited."));
}

TEST_CASE("editing a list yields one ReplaceSection on the ul id", "[canvas-diff]") {
    const auto base = CanvasDiff::parseBaseChunks(bodyHtml());
    REQUIRE(base.has_value());

    QTextDocument doc;
    doc.setHtml(bodyHtml());
    auto block = findBlock(doc, "item one");
    REQUIRE(block.isValid());
    QTextCursor c(block);
    c.movePosition(QTextCursor::EndOfBlock);
    c.insertText(" tweaked");

    const auto ops = CanvasDiff::diff(*base, CanvasDiff::documentChunks(&doc));
    REQUIRE(ops.has_value());
    REQUIRE(ops->size() == 1);
    CHECK((*ops)[0].op == CanvasChange::Op::ReplaceSection);
    CHECK((*ops)[0].sectionId == "temp:C:eBU1b81a3b2d596a132e00b90392");
    CHECK((*ops)[0].markdown.contains("tweaked"));
    CHECK((*ops)[0].markdown.contains("item two")); // whole list is the section
}

TEST_CASE("appending a paragraph yields one insert after the last section", "[canvas-diff]") {
    const auto base = CanvasDiff::parseBaseChunks(bodyHtml());
    REQUIRE(base.has_value());

    QTextDocument doc;
    doc.setHtml(bodyHtml());
    QTextCursor c(&doc);
    c.movePosition(QTextCursor::End);
    c.insertBlock();
    c.setBlockFormat(QTextBlockFormat()); // plain paragraph, no inherited format
    c.setCharFormat(QTextCharFormat());
    c.insertText("Brand new closing line");

    const auto ops = CanvasDiff::diff(*base, CanvasDiff::documentChunks(&doc));
    REQUIRE(ops.has_value());
    REQUIRE(ops->size() == 1);
    CHECK((*ops)[0].op == CanvasChange::Op::InsertAfter);
    CHECK((*ops)[0].sectionId == "temp:C:eBU17fe3538a0800e079ca7b6652");
    CHECK((*ops)[0].markdown.contains("Brand new closing line"));
}

TEST_CASE("deleting a paragraph yields one DeleteSection", "[canvas-diff]") {
    const auto base = CanvasDiff::parseBaseChunks(bodyHtml());
    REQUIRE(base.has_value());

    QTextDocument doc;
    doc.setHtml(bodyHtml());
    // Backspace-style removal: select from the end of the previous block so
    // the deleted block merges into its predecessor (which keeps its format).
    auto block = findBlock(doc, "code line 1");
    REQUIRE(block.isValid());
    QTextCursor c(&doc);
    c.setPosition(block.position() - 1); // end of the preceding (quote) block
    c.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
    c.removeSelectedText();

    const auto ops = CanvasDiff::diff(*base, CanvasDiff::documentChunks(&doc));
    REQUIRE(ops.has_value());
    REQUIRE(ops->size() == 1);
    CHECK((*ops)[0].op == CanvasChange::Op::DeleteSection);
    CHECK((*ops)[0].sectionId == "temp:C:eBUf2b3f64af385184f2235bccc8");
}

TEST_CASE("editing a quote forces the whole-doc fallback", "[canvas-diff]") {
    const auto base = CanvasDiff::parseBaseChunks(bodyHtml());
    REQUIRE(base.has_value());

    QTextDocument doc;
    doc.setHtml(bodyHtml());
    auto block = findBlock(doc, "a quoted line");
    REQUIRE(block.isValid());
    QTextCursor c(block);
    c.movePosition(QTextCursor::EndOfBlock);
    c.insertText(" now different");

    CHECK(!CanvasDiff::diff(*base, CanvasDiff::documentChunks(&doc)).has_value());
}

TEST_CASE("empty base with new content is not diffable", "[canvas-diff]") {
    QTextDocument doc;
    doc.setPlainText("hello");
    CHECK(!CanvasDiff::diff({}, CanvasDiff::documentChunks(&doc)).has_value());
}

TEST_CASE("multiple inserts before an anchor keep their order", "[canvas-diff]") {
    const std::vector<Chunk> base = {
        {Chunk::Kind::Para, "id-a", false, "alpha"},
        {Chunk::Kind::Para, "id-b", false, "omega"},
    };
    const std::vector<Chunk> current = {
        {Chunk::Kind::Para, {}, false, "alpha"},
        {Chunk::Kind::Para, {}, false, "one"},
        {Chunk::Kind::Para, {}, false, "two"},
        {Chunk::Kind::Para, {}, false, "omega"},
    };
    const auto ops = CanvasDiff::diff(base, current);
    REQUIRE(ops.has_value());
    REQUIRE(ops->size() == 2);
    // insert_before a fixed anchor preserves natural order
    CHECK((*ops)[0].op == CanvasChange::Op::InsertBefore);
    CHECK((*ops)[0].sectionId == "id-b");
    CHECK((*ops)[0].markdown == "one");
    CHECK((*ops)[1].op == CanvasChange::Op::InsertBefore);
    CHECK((*ops)[1].sectionId == "id-b");
    CHECK((*ops)[1].markdown == "two");
}

TEST_CASE("trailing inserts after an anchor are emitted reversed", "[canvas-diff]") {
    const std::vector<Chunk> base = {
        {Chunk::Kind::Para, "id-a", false, "alpha"},
    };
    const std::vector<Chunk> current = {
        {Chunk::Kind::Para, {}, false, "alpha"},
        {Chunk::Kind::Para, {}, false, "one"},
        {Chunk::Kind::Para, {}, false, "two"},
    };
    const auto ops = CanvasDiff::diff(base, current);
    REQUIRE(ops.has_value());
    REQUIRE(ops->size() == 2);
    // insert_after the same anchor: reversed emission → natural document order
    CHECK((*ops)[0].op == CanvasChange::Op::InsertAfter);
    CHECK((*ops)[0].markdown == "two");
    CHECK((*ops)[1].op == CanvasChange::Op::InsertAfter);
    CHECK((*ops)[1].markdown == "one");
    CHECK((*ops)[0].sectionId == "id-a");
}

TEST_CASE("replace pairs with delete of extra base sections", "[canvas-diff]") {
    const std::vector<Chunk> base = {
        {Chunk::Kind::Para, "id-a", false, "keep"},
        {Chunk::Kind::Para, "id-b", false, "old one"},
        {Chunk::Kind::Para, "id-c", false, "old two"},
    };
    const std::vector<Chunk> current = {
        {Chunk::Kind::Para, {}, false, "keep"},
        {Chunk::Kind::Para, {}, false, "new single"},
    };
    const auto ops = CanvasDiff::diff(base, current);
    REQUIRE(ops.has_value());
    REQUIRE(ops->size() == 2);
    CHECK((*ops)[0].op == CanvasChange::Op::ReplaceSection);
    CHECK((*ops)[0].sectionId == "id-b");
    CHECK((*ops)[0].markdown == "new single");
    CHECK((*ops)[1].op == CanvasChange::Op::DeleteSection);
    CHECK((*ops)[1].sectionId == "id-c");
}

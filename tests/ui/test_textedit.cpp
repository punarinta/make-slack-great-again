#include "harness.h"

#include <cmath>

using namespace uitest;
using K = plat::Key;

namespace {

struct EditWin : Win {
    ui::TextEdit *edit;
    EditWin() : Win(400, 300) {
        auto *col = w->root().add<ui::View>();
        col->style().items(ui::Align::Stretch).justifyContent(ui::Justify::End);
        edit = col->add<ui::TextEdit>();
        frame();
        const ui::RectF r = edit->windowRect();
        click(r.x + r.w - 5, r.y + r.h / 2);
    }
    const uint32_t primary = plat::primaryMod();
#ifdef __APPLE__
    const uint32_t word = plat::ModAlt;
#else
    const uint32_t word = plat::ModCtrl;
#endif
};

std::string runsDesc(const ui::TextEdit &e) {
    std::string s;
    for (const auto &r : e.runs()) {
        s += std::to_string(r.start) + "-" + std::to_string(r.end) + ":" +
             std::to_string(int(r.format));
        if (!r.link.empty())
            s += "@" + std::string(r.link);
        s += " ";
    }
    return s;
}

} // namespace

TEST("textedit: typing, caret movement, backspace, delete") {
    EditWin t;
    REQUIRE(t.edit->focused());
    t.type("hello");
    CHECK_STR(t.edit->text(), "hello");
    CHECK(t.edit->caret() == 5);
    t.key(K::Left);
    t.key(K::Left);
    t.key(K::Backspace);
    CHECK_STR(t.edit->text(), "helo");
    CHECK(t.edit->caret() == 2);
    t.key(K::Delete);
    CHECK_STR(t.edit->text(), "heo");
    t.key(K::End);
    t.text("é ü");
    CHECK_STR(t.edit->text(), "heoé ü");
    t.key(K::Backspace); // one code point, two bytes
    CHECK_STR(t.edit->text(), "heoé ");
    t.key(K::Home);
    CHECK(t.edit->caret() == 0);
}

TEST("textedit: selection with shift, word jumps, select all replaces") {
    EditWin t;
    t.type("hello big world");
    t.chord(t.word, K::Left);
    CHECK(t.edit->caret() == 10);
    t.chord(t.word | plat::ModShift, K::Left);
    CHECK(t.edit->anchor() == 10 && t.edit->caret() == 6);
    CHECK_STR(t.edit->selectedText(), "big ");
    t.key(K::Right); // collapses to the selection end
    CHECK(t.edit->caret() == 10 && !t.edit->hasSelection());
    t.chord(t.word | plat::ModShift, K::Right);
    CHECK_STR(t.edit->selectedText(), "world");
    t.chord(t.word, K::Backspace);
    CHECK_STR(t.edit->text(), "hello big ");
    t.chord(t.primary, K::A);
    CHECK(t.edit->selectedText() == "hello big ");
    t.type("x");
    CHECK_STR(t.edit->text(), "x");
}

TEST("textedit: undo coalesces a typing run, redo restores") {
    EditWin t;
    t.type("hello world");
    t.key(K::Enter); // no onSubmit: newline (its own undo step)
    t.type("again");
    CHECK_STR(t.edit->text(), "hello world\nagain");
    t.chord(t.primary, K::Z);
    CHECK_STR(t.edit->text(), "hello world\n");
    t.chord(t.primary, K::Z);
    CHECK_STR(t.edit->text(), "hello world");
    t.chord(t.primary, K::Z);
    CHECK_STR(t.edit->text(), "");
    CHECK(!t.edit->canUndo());
    t.chord(t.primary | plat::ModShift, K::Z);
    CHECK_STR(t.edit->text(), "hello world");
    t.key(K::Backspace);
    t.key(K::Backspace);
    t.chord(t.primary, K::Z); // the two backspaces are one step
    CHECK_STR(t.edit->text(), "hello world");
    CHECK(t.edit->caret() == 11);
}

TEST("textedit: formats on selection and while typing, runs and undo") {
    EditWin t;
    t.type("make it bold");
    t.edit->setSelection(8, 12);
    t.edit->toggleFormat(ui::TextEdit::Bold); // the composer's Ctrl+B
    CHECK_STR(runsDesc(*t.edit), "0-8:0 8-12:1 ");
    CHECK(t.edit->formatActive(ui::TextEdit::Bold));
    CHECK_STR(t.edit->html(), "<b>bold</b>");
    t.chord(t.primary, K::Z);
    CHECK_STR(runsDesc(*t.edit), "0-12:0 ");
    // Typing format: toggle italic with no selection, type, toggle off.
    t.key(K::End);
    t.edit->toggleFormat(ui::TextEdit::Italic);
    t.type(" now");
    t.edit->toggleFormat(ui::TextEdit::Italic);
    t.type(" plain");
    CHECK_STR(runsDesc(*t.edit), "0-12:0 12-16:2 16-22:0 ");
    // Links.
    t.edit->setSelection(0, 4);
    t.edit->setLink("https://example.com");
    CHECK_STR(runsDesc(*t.edit), "0-4:0@https://example.com 4-12:0 12-16:2 16-22:0 ");
    t.edit->setSelection(4, 4);
    t.type("X"); // typing right after a link does not extend it
    CHECK_STR(runsDesc(*t.edit), "0-4:0@https://example.com 4-13:0 13-17:2 17-23:0 ");
}

TEST("textedit: copy puts text and html on the clipboard; paste keeps formats") {
    EditWin t;
    t.type("one two");
    t.edit->setSelection(4, 7);
    t.edit->toggleFormat(ui::TextEdit::Bold);
    t.edit->setSelection(0, 7);
    t.chord(t.primary, K::C);
    std::optional<std::string> html, plain;
    app().platform().requestClipboard("text/html", [&](auto v) { html = v; });
    app().platform().requestClipboard("text/plain;charset=utf-8", [&](auto v) { plain = v; });
    t.frame();
    REQUIRE(html && plain);
    CHECK_STR(*plain, "one two");
    CHECK_STR(*html, "one <b>two</b>");
    t.key(K::End);
    t.type(" ");
    t.chord(t.primary, K::V);
    t.frame();
    CHECK_STR(t.edit->text(), "one two one two");
    // The typed space continued the bold before it; the pasted fragment keeps its own.
    CHECK_STR(runsDesc(*t.edit), "0-4:0 4-8:1 8-12:0 12-15:1 ");
    // Plain paste (Shift) drops the formats.
    t.chord(t.primary | plat::ModShift, K::V);
    t.frame();
    CHECK_STR(t.edit->text(), "one two one twoone two");
    CHECK_STR(runsDesc(*t.edit), "0-4:0 4-8:1 8-12:0 12-22:1 ");
    // …the typing format carried on from the bold text before the caret,
    // which is what every editor does for pasted plain text.
    t.chord(t.primary, K::A);
    t.chord(t.primary, K::X);
    CHECK_STR(t.edit->text(), "");
}

TEST("textedit: pillLink tells a pill from a link") {
    EditWin t;
    CHECK(ui::TextEdit::pillLink("@U0ADA"));
    CHECK(ui::TextEdit::pillLink("<#C0ENG|eng>"));
    CHECK(ui::TextEdit::pillLink("<https://x.test/g.gif|GIF \xC2\xB7 cat>")); // a GIF pill
    CHECK_FALSE(ui::TextEdit::pillLink("https://x.test"));
    CHECK_FALSE(ui::TextEdit::pillLink({}));
}

TEST("textedit: a paste is marked as one while its text goes in") {
    EditWin t;
    app().platform().setClipboardText("pasted");
    bool sawPaste    = false;
    t.edit->onChange = [&] { sawPaste = sawPaste || t.edit->inPaste(); };
    t.chord(t.primary, K::V);
    t.frame();
    CHECK_STR(t.edit->text(), "pasted");
    CHECK(sawPaste); // the owner links a pasted URL at once
    CHECK_FALSE(t.edit->inPaste());
}

TEST("textedit: IME preedit shows inline, commit replaces it") {
    EditWin t;
    t.type("ab");
    t.key(K::Left);
    t.preedit("ni", 2);
    CHECK_STR(t.edit->text(), "ab");
    CHECK_STR(t.edit->preedit(), "ni");
    CHECK(t.edit->caret() == 1);
    t.preedit("nih", 3);
    CHECK_STR(t.edit->preedit(), "nih");
    t.key(K::Left); // the IME owns navigation while composing
    CHECK(t.edit->caret() == 1);
    t.preedit("");
    t.text("你好");
    CHECK_STR(t.edit->text(), "a你好b");
    CHECK(t.edit->preedit().empty());
    CHECK(t.edit->caret() == 1 + 6);
    // The candidate window follows the caret.
    const ui::RectF c = t.edit->caretRect();
    CHECK(c.h > 0);
}

TEST("textedit: Enter submits, Shift+Enter inserts a newline") {
    EditWin t;
    int     sent     = 0;
    t.edit->onSubmit = [&] {
        ++sent;
        t.edit->clear();
        return true;
    };
    t.type("hi");
    t.chord(plat::ModShift, K::Enter);
    t.type("there");
    CHECK_STR(t.edit->text(), "hi\nthere");
    t.key(K::Enter);
    CHECK(sent == 1 && t.edit->text().empty());
}

TEST("textedit: grows line by line up to maxLines, then scrolls inside") {
    EditWin t;
    t.edit->setMaxLines(3);
    t.frame();
    const float h1 = t.edit->frame().h;
    t.type("a");
    t.chord(plat::ModShift, K::Enter);
    t.type("b");
    t.frame();
    const float h2 = t.edit->frame().h;
    CHECK(h2 > h1);
    for (int i = 0; i < 6; ++i) {
        t.chord(plat::ModShift, K::Enter);
        t.type("c");
    }
    t.frame();
    const float h8 = t.edit->frame().h;
    CHECK(h8 < h1 * 3.5f);
    CHECK(near(h8 - h1, 2 * (h2 - h1), 1.5f));
    // The caret (last line) is visible inside the box.
    const ui::RectF c = t.edit->caretRect();
    CHECK(c.y >= 0 && c.y + c.h <= t.edit->frame().h + 0.5f);
}

TEST("textedit: click places the caret, double click selects a word, triple a line") {
    EditWin t;
    t.type("alpha beta gamma");
    const ui::RectF r = t.edit->windowRect();
    t.edit->setSelection(8, 8);
    const ui::RectF c8 = t.edit->caretRect(); // inside "beta"
    t.edit->setSelection(0, 0);
    t.clickN(r.x + c8.x + 1, r.y + c8.y + c8.h / 2, 1);
    CHECK(t.edit->caret() == 8);
    t.clickN(r.x + c8.x + 1, r.y + c8.y + c8.h / 2, 2);
    CHECK_STR(t.edit->selectedText(), "beta");
    t.clickN(r.x + c8.x + 1, r.y + c8.y + c8.h / 2, 3);
    CHECK_STR(t.edit->selectedText(), "alpha beta gamma");
    t.clickN(r.x + 2, r.y + c8.y + c8.h / 2, 1);
    t.clickN(r.x + c8.x + 1, r.y + c8.y + c8.h / 2, 1, plat::ModShift); // shift extends
    CHECK(t.edit->anchor() == 0 && t.edit->caret() == 8);
}

TEST("textedit: caret blinks on the system period while focused") {
    EditWin   t;
    const int ms = app().settings().caretBlinkMs;
    if (ms <= 0)
        return;
    t.frame();
    const int f0 = t.w->stats().frames;
    // Within ~2 periods the caret toggles at least once, repainting its rect only.
    CHECK(t.until([&] { return t.w->stats().frames > f0; }, 3000));
    CHECK(t.damageArea() < 20 * 40);
}

// An idle field stops waking the window: after ~10 s the caret stays on.
TEST("textedit: the caret stops blinking after a while idle, shown") {
    EditWin   t;
    const int ms = app().settings().caretBlinkMs;
    if (ms <= 0)
        return;
    t.frame();
    const double t0     = app().nowMs();
    int          frames = t.w->stats().frames, blinks = 0;
    double       quiet = t0; // since the last frame
    while (app().nowMs() < t0 + 13000 && app().nowMs() - quiet < 3.0 * ms) {
        app().pump(5);
        if (t.w->stats().frames != frames) {
            frames = t.w->stats().frames;
            quiet  = app().nowMs();
            ++blinks;
        }
    }
    CHECK(blinks >= 4);                       // it did blink
    CHECK(app().nowMs() - quiet >= 3.0 * ms); // and then went quiet
    CHECK(quiet - t0 > 8000);
    // Stopped on: the caret is painted.
    const ui::RectF r = t.edit->caretRect();
    gfx::Bitmap     bmp(int(t.edit->width()), int(t.edit->height()));
    gfx::Painter    p(bmp.view(), 1);
    t.edit->paint(p);
    const int x = int(std::round(r.x)), y = int(r.y + r.h / 2);
    REQUIRE(x >= 0 && x < bmp.width() && y >= 0 && y < bmp.height());
    CHECK(bmp.pixels()[size_t(y) * size_t(bmp.width()) + size_t(x)] == ui::color(ui::C::Caret));
    // Typing blinks again.
    t.text("x");
    const int f0 = t.w->stats().frames;
    CHECK(t.until([&] { return t.w->stats().frames > f0 + 1; }, 3000));
}

TEST("textedit: a popup that closes when its field loses focus survives deactivation") {
    Win   w(400, 300);
    auto  pop  = std::make_unique<ui::Popup>();
    auto *edit = pop->add<ui::TextEdit>();
    edit->style().size(200, 30);
    pop->setAnchor({20, 20, 10, 10}, ui::Popup::Place::Below);
    ui::Popup *p        = w.w->showPopup(std::move(pop));
    edit->onFocusChange = [p](bool on) {
        if (!on)
            p->close();
    };
    w.frame();
    edit->focus();
    REQUIRE(w.w->focusView() == edit);
    w.w->handle({.type = plat::EventType::FocusOut}); // the window loses the keyboard
    w.frame();
    CHECK(w.w->topPopup() == nullptr);
    CHECK(w.w->focusView() == nullptr);
}

TEST("textedit: an edit in a long text lays out only the paragraphs it touches") {
    EditWin     t;
    std::string doc;
    for (int i = 0; i < 300; ++i)
        doc += "paragraph " + std::to_string(i) + " with a few words to shape\n";
    t.edit->setText(doc);
    const uint32_t mid = uint32_t(doc.find("paragraph 150 ")) + 10;
    t.edit->setSelection(mid, mid);
    t.frame();
    auto builds = [&](auto &&step) {
        const size_t n = text::layoutBuilds();
        step();
        t.frame();
        return text::layoutBuilds() - n;
    };
    CHECK(builds([&] { t.text("x"); }) == 1);
    CHECK(builds([&] { t.key(K::Backspace); }) == 1);
    CHECK(builds([&] { t.chord(plat::ModShift, K::Enter); }) == 2); // split in two
    CHECK(builds([&] { t.key(K::Backspace); }) == 1);               // joined again
    CHECK(builds([&] { t.key(K::Down); }) == 0);
    CHECK(builds([&] { t.preedit("ni"); }) == 1);
    CHECK(builds([&] { t.preedit(""); }) == 1);
    CHECK_STR(t.edit->text(), doc);
}

// The paragraphs answer what one layout of the whole text would: caret
// boxes, Left / Right, Up / Down and clicks, around empty paragraphs (one
// as tall as its code-formatted break), wrapped lines and a trailing break.
TEST("textedit: paragraphs place the caret as one layout of the whole text") {
    EditWin t;
    t.edit->setMaxLines(0);
    const std::string doc = "alpha beta\n\nA longer paragraph that wraps over a few lines of the "
                            "field, with words enough to wrap twice at least.\ncode\n\nend\n";
    t.edit->setText(doc);
    // Code on "beta" and on the breaks after "code" (the empty line takes
    // its break's size), bold on "code".
    const uint32_t c0 = uint32_t(doc.find("code"));
    for (const auto &[a, b, f] :
         {std::tuple{6u, 10u, ui::TextEdit::Code},
          std::tuple{c0 + 4, c0 + 6, ui::TextEdit::Code},
          std::tuple{c0, c0 + 4, ui::TextEdit::Bold}}) {
        t.edit->setSelection(a, b);
        t.edit->toggleFormat(f);
    }
    t.frame();

    const text::Style    base = ui::font(ui::Font::Body, ui::C::Text);
    text::AttributedText at;
    for (const auto &r : t.edit->runs()) {
        text::Style st = base;
        if (r.format & ui::TextEdit::Bold)
            st.weight = text::Weight::Bold;
        if (r.format & ui::TextEdit::Code) {
            st.mono       = true;
            st.size       = std::max(10.f, base.size - 2);
            st.background = ui::color(ui::C::CodeBg);
        }
        at.append(std::string_view(doc).substr(r.start, r.end - r.start), st);
    }
    const ui::Style    &s = t.edit->currentStyle();
    text::LayoutOptions o;
    o.maxWidth     = std::max(1.f, t.edit->frame().w - s.pad.l - s.pad.r);
    const auto ref = text::Layout::build(at, o, t.edit->windowScale());
    REQUIRE(ref->lineCount() >= 8);

    const uint32_t n      = uint32_t(doc.size());
    int            misses = 0;
    for (uint32_t i = 0; i <= n; ++i) {
        if (i < n && (uint8_t(doc[i]) & 0xc0) == 0x80)
            continue;
        t.edit->setSelection(i, i);
        const ui::RectF c = t.edit->caretRect(), r = ref->caretRect(i);
        if (!near(c.x, r.x + s.pad.l, 0.01f) || !near(c.y, r.y + s.pad.t, 0.01f) ||
            !near(c.h, r.h, 0.01f))
            ++misses;
        auto moved = [&](K k, uint32_t want) {
            t.edit->setSelection(i, i);
            t.key(k);
            if (t.edit->caret() != want)
                ++misses;
        };
        moved(K::Left, ref->moveCaret(i, -1, 0));
        moved(K::Right, ref->moveCaret(i, 1, 0));
        const uint32_t up = ref->moveCaret(i, 0, -1), down = ref->moveCaret(i, 0, 1);
        moved(K::Up, up == i ? 0 : up);
        moved(K::Down, down == i ? n : down);
    }
    CHECK(misses == 0);
    // Clicks: left of, inside and right of each line.
    const ui::RectF wr = t.edit->windowRect();
    for (int li = 0; li < ref->lineCount(); ++li) {
        const float y = ref->baseline(li) - 2;
        for (const float x : {-5.f, 30.f, 120.f, o.maxWidth + 3}) {
            t.clickN(wr.x + s.pad.l + x, wr.y + s.pad.t + y, 1);
            t.frame(1);
            if (t.edit->caret() != ref->hitTest({x, y}).offset)
                ++misses;
        }
    }
    CHECK(misses == 0);
}

TEST("html: reader handles blocks, entities, links, whitespace") {
    std::string              text;
    std::vector<uint16_t>    fmt;
    std::vector<std::string> links;
    ui::rich::fromHtml(
        "<html><head><style>p{}</style></head><body>"
        "<p>Hi   <B>there</B></p>\n<p><a href=\"https://x.io/?a=1&amp;b=2\">link</a>"
        " &amp; <i>more</i>&nbsp;&#x263A;</p><!-- c --><pre>a  b\nc</pre></body>",
        &text,
        &fmt,
        &links
    );
    CHECK_STR(text, "Hi there\nlink & more \xe2\x98\xba\na  b\nc");
    REQUIRE(fmt.size() == text.size());
    CHECK(fmt[3] == ui::TextEdit::Bold);
    CHECK(fmt[9] == (1 << 8)); // "link"
    REQUIRE(links.size() == 1);
    CHECK_STR(links[0], "https://x.io/?a=1&b=2");
    CHECK(fmt[text.find("more")] == ui::TextEdit::Italic);
    CHECK(fmt[text.find("a  b")] == ui::TextEdit::Code);
}

TEST("html: Google Docs' normal-weight <b> wrapper is not bold; writer escapes") {
    std::string              text;
    std::vector<uint16_t>    fmt;
    std::vector<std::string> links;
    ui::rich::fromHtml(
        "<meta charset=\"utf-8\"><b style=\"font-weight:normal;\" id=\"docs\">"
        "<span style=\"font-weight:700\">Bold</span><span> plain</span></b>",
        &text,
        &fmt,
        &links
    );
    CHECK_STR(text, "Bold plain");
    CHECK(fmt[0] == ui::TextEdit::Bold && fmt[5] == 0);
    const std::vector<uint16_t> f(5, 0);
    CHECK_STR(ui::rich::toHtml("a<b&\n", f.data(), {}), "a&lt;b&amp;<br>");
}

TEST("html: the tag scanner and attribute reader, forgiving and strict") {
    using ui::rich::Tag;
    Tag                    t;
    const std::string_view h = "<A\nHREF = x.io/?a&amp;b>";
    CHECK(ui::rich::readTag(h, 0, &t) == h.size());
    CHECK_STR(std::string(t.name), "A");
    CHECK_FALSE(t.closing);
    CHECK_STR(std::string(ui::rich::tagAttr(t.text, "href")), "x.io/?a&amp;b"); // raw
    CHECK(ui::rich::tagAttr(t.text, "href", true).empty());
    CHECK(ui::rich::readTag("</b >", 0, &t) == 5);
    CHECK(t.closing);
    CHECK_STR(std::string(t.name), "b");
    // Comments (to their end, '>' inside), declarations: nothing to act on.
    const std::string_view c = "<!-- a > b -->x";
    CHECK(ui::rich::readTag(c, 0, &t) == c.size() - 1);
    CHECK(t.text.empty());
    CHECK(ui::rich::readTag("<!-- open", 0, &t) == 9);
    CHECK(ui::rich::readTag("<!DOCTYPE html>", 0, &t) == 15);
    CHECK(t.text.empty());
    CHECK(ui::rich::readTag("<a href", 0, &t) == std::string_view::npos);
    // Strict: exactly name="…", case and all.
    const std::string_view a = "div class=\"x y\" data-id='7' ID=\"u\" open=\"z";
    CHECK_STR(std::string(ui::rich::tagAttr(a, "class", true)), "x y");
    CHECK_STR(std::string(ui::rich::tagAttr(a, "data-id", true)), "7");
    CHECK(ui::rich::tagAttr(a, "id", true).empty()); // not "data-id", not "ID"
    CHECK_STR(std::string(ui::rich::tagAttr(a, "id")), "u");
    CHECK(ui::rich::tagAttr(a, "open", true).empty()); // never closed
    CHECK_STR(std::string(ui::rich::tagAttr(a, "open")), "z");
    // Skipped content: script/style always, head/title as asked.
    int depth = 0;
    ui::rich::readTag("<title>", 0, &t);
    CHECK_FALSE(ui::rich::skippedTag(t, &depth, false));
    CHECK(ui::rich::skippedTag(t, &depth, true));
    CHECK(depth == 1);
    ui::rich::readTag("<b>", 0, &t);
    CHECK(ui::rich::skippedTag(t, &depth, true)); // inside the title
    ui::rich::readTag("</TITLE>", 0, &t);
    CHECK(ui::rich::skippedTag(t, &depth, true));
    CHECK(depth == 0);
    ui::rich::readTag("</style>", 0, &t);
    CHECK(ui::rich::skippedTag(t, &depth, false));
    CHECK(depth == 0); // never below
    ui::rich::readTag("<b>", 0, &t);
    CHECK_FALSE(ui::rich::skippedTag(t, &depth, false));
}

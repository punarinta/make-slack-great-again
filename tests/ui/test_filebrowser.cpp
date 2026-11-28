// The in-app file chooser on plat's headless backend: listing and sorting,
// hidden files, filters, keyboard and mouse navigation, every mode's answer,
// and the Save overwrite question — against a throwaway folder tree.
#include "base/file.h"
#include "harness.h"

#include <cmath>
#include <cstdlib>
#include <optional>
#include <unistd.h>

using namespace uitest;
using Mode = plat::FileDialogDesc::Mode;

namespace {

// A folder tree: docs/ (report.txt, notes.md), pics/ (a.png, b.PNG),
// .secret/, .hidden.txt, zeta.txt, Alpha.txt.
struct Tree {
    std::string root;
    Tree() {
        root = base::test::makeTempDir("ui_filebrowser_");
        if (root.empty())
            root = "/tmp";
        for (const char *f :
             {"docs/report.txt",
              "docs/notes.md",
              "pics/a.png",
              "pics/b.PNG",
              "zeta.txt",
              "Alpha.txt",
              ".hidden.txt",
              ".secret/x"})
            file::writeAtomic(file::join(root, f), "data");
    }
    ~Tree() {
        for (const char *f :
             {"docs/report.txt",
              "docs/notes.md",
              "pics/a.png",
              "pics/b.PNG",
              "zeta.txt",
              "Alpha.txt",
              ".hidden.txt",
              ".secret/x",
              "new.txt",
              "docs",
              "pics",
              ".secret"})
            file::remove(file::join(root, f));
        file::remove(root);
    }
    std::string at(const char *rel) const { return file::join(root, rel); }
};

struct Run {
    Win                                     win{900, 640};
    ui::FileBrowser                        *b = nullptr;
    std::optional<std::vector<std::string>> got;
    explicit Run(plat::FileDialogDesc d) {
        b = ui::FileBrowser::show(*win.w, d, [this](std::vector<std::string> p) {
            got = std::move(p);
        });
        win.frame();
    }
    std::vector<std::string> names() const {
        std::vector<std::string> v;
        for (size_t i = 0; i < b->entryCount(); ++i)
            v.push_back(b->entry(i).name);
        return v;
    }
    // Window position of entry i's row (the list is the only ScrollView).
    ui::PointF rowPos(int i) const {
        const ui::View  *l = findList(b);
        const ui::PointF o = l->mapToWindow({0, 0});
        return {o.x + 60, o.y + 28.f * float(i) + 14};
    }
    static const ui::View *findList(const ui::View *v) {
        if (v->role() == ui::Role::List)
            return v;
        for (size_t i = 0; i < v->childCount(); ++i)
            if (const ui::View *f = findList(v->child(i)))
                return f;
        return nullptr;
    }
};

} // namespace

TEST("filebrowser: glob patterns") {
    CHECK(ui::FileBrowser::globMatch("*.png", "a.png"));
    CHECK(ui::FileBrowser::globMatch("*.png", "B.PNG"));
    CHECK_FALSE(ui::FileBrowser::globMatch("*.png", "a.png.txt"));
    CHECK(ui::FileBrowser::globMatch("*", ""));
    CHECK(ui::FileBrowser::globMatch("report-??.txt", "report-q3.txt"));
    CHECK_FALSE(ui::FileBrowser::globMatch("report-??.txt", "report-q.txt"));
    CHECK(ui::FileBrowser::globMatch("a*b*c", "aXXbYYc"));
}

TEST("filebrowser: lists folders first, hides dot files, keeps the card readable") {
    Tree t;
    Run  r({.mode = Mode::Open, .initialDir = t.root});
    CHECK_STR(r.b->dir(), t.root);
    const std::vector<std::string> want = {"docs", "pics", "Alpha.txt", "zeta.txt"};
    CHECK(r.names() == want);
    r.b->setShowHidden(true);
    const std::vector<std::string> all = {
        ".secret", "docs", "pics", ".hidden.txt", "Alpha.txt", "zeta.txt"
    };
    CHECK(r.names() == all);
    // Ctrl+H toggles it back (the list has focus).
    r.win.chord(plat::primaryMod(), plat::Key::H);
    CHECK(r.names() == want);
    CHECK_FALSE(r.b->showHidden());
}

namespace {
double luminance(gfx::Color c) {
    auto ch = [](uint32_t v) {
        const double s = double(v & 0xff) / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * ch(c >> 16) + 0.7152 * ch(c >> 8) + 0.0722 * ch(c);
}
double contrast(gfx::Color a, gfx::Color b) {
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}
} // namespace

// Issue #18: the old fallback dialog drew OS-palette text on a foreign
// background. Every colour here is a token on the card's own opaque
// background; check the pairs it uses are readable in both themes.
TEST("filebrowser: text on the card and list is readable in light and dark") {
    for (ui::ThemeMode m : {ui::ThemeMode::Light, ui::ThemeMode::Dark}) {
        app().setThemeMode(m);
        using ui::C;
        CHECK((ui::color(C::PopupBg) >> 24) == 0xff);
        CHECK((ui::color(C::InputBg) >> 24) == 0xff);
        CHECK(contrast(ui::color(C::Text), ui::color(C::PopupBg)) >= 7);
        CHECK(contrast(ui::color(C::Text), ui::color(C::InputBg)) >= 7);
        CHECK(contrast(ui::color(C::TextMuted), ui::color(C::InputBg)) >= 3);
        CHECK(contrast(ui::color(C::AccentText), ui::color(C::Accent)) >= 3);
    }
    app().setThemeMode(ui::ThemeMode::System);
}

TEST("filebrowser: keyboard opens folders, goes up, and accepts a file") {
    Tree t;
    Run  r({.mode = Mode::Open, .initialDir = t.root});
    r.win.key(plat::Key::Down); // docs
    CHECK(r.b->current() == 0);
    r.win.key(plat::Key::Enter);
    CHECK_STR(r.b->dir(), t.at("docs"));
    CHECK(r.b->entryCount() == 2); // notes.md, report.txt
    r.win.key(plat::Key::Backspace);
    CHECK_STR(r.b->dir(), t.root);
    CHECK(r.b->current() == 0); // back on "docs"
    CHECK(r.b->selected(0));
    // A letter jumps: "z" → zeta.txt; Enter accepts it.
    r.win.key(plat::Key::Z);
    CHECK(r.b->current() == 3);
    r.win.key(plat::Key::Enter);
    REQUIRE(r.got.has_value());
    CHECK(r.got->size() == 1 && (*r.got)[0] == t.at("zeta.txt"));
}

TEST("filebrowser: Escape cancels with no paths") {
    Tree t;
    Run  r({.mode = Mode::Open, .initialDir = t.root});
    r.win.key(plat::Key::Escape);
    REQUIRE(r.got.has_value());
    CHECK(r.got->empty());
    CHECK(r.win.w->topPopup() == nullptr);
}

TEST("filebrowser: mouse double click opens, several files with Ctrl") {
    Tree       t;
    Run        r({.mode = Mode::OpenMultiple, .initialDir = t.root});
    ui::PointF p = r.rowPos(1); // pics
    r.win.clickN(p.x, p.y, 1);
    r.win.clickN(p.x, p.y, 2);
    CHECK_STR(r.b->dir(), t.at("pics"));
    REQUIRE(r.b->entryCount() == 2);
    p = r.rowPos(0);
    r.win.clickN(p.x, p.y, 1);
    p = r.rowPos(1);
    r.win.clickN(p.x, p.y, 1, plat::primaryMod());
    CHECK(r.b->selected(0) && r.b->selected(1));
    CHECK(r.b->accept());
    REQUIRE(r.got.has_value());
    const std::vector<std::string> want = {t.at("pics/a.png"), t.at("pics/b.PNG")};
    CHECK(*r.got == want);
}

TEST("filebrowser: filters narrow the files, never the folders") {
    Tree t;
    Run  r(
        {.mode       = Mode::Open,
         .initialDir = t.at("pics"),
         .filters    = {{"Images", {"*.png"}}, {"Text", {"*.txt"}}}}
    );
    CHECK(r.b->entryCount() == 2); // a.png and b.PNG (case-insensitive)
    r.b->setFilter(1);
    CHECK(r.b->entryCount() == 0);
    r.b->setDir(t.root);
    const std::vector<std::string> want = {"docs", "pics", "Alpha.txt", "zeta.txt"};
    CHECK(r.names() == want);
}

TEST("filebrowser: save suggests the name, asks before replacing, adds the extension") {
    Tree t;
    {
        Run r(
            {.mode          = Mode::Save,
             .initialDir    = t.root,
             .suggestedName = "zeta.txt",
             .filters       = {{"Text", {"*.txt"}}}}
        );
        REQUIRE(r.b->nameField() != nullptr);
        CHECK_STR(r.b->nameField()->text(), "zeta.txt");
        CHECK(r.b->nameField()->focused());
        // zeta.txt exists: the first OK asks, the second replaces.
        CHECK_FALSE(r.b->accept());
        CHECK_FALSE(r.got.has_value());
        CHECK(r.b->accept());
        REQUIRE(r.got.has_value());
        CHECK(*r.got == std::vector<std::string>{t.at("zeta.txt")});
    }
    {
        Run r(
            {.mode          = Mode::Save,
             .initialDir    = t.root,
             .suggestedName = "old.txt",
             .filters       = {{"Text", {"*.txt"}}}}
        );
        // Typing replaces the selected stem; the filter adds ".txt".
        r.b->nameField()->setText("new");
        r.win.key(plat::Key::Enter);
        REQUIRE(r.got.has_value());
        CHECK(*r.got == std::vector<std::string>{t.at("new.txt")});
    }
    {
        // A typed folder name navigates instead of saving.
        Run r({.mode = Mode::Save, .initialDir = t.root, .suggestedName = "x.bin"});
        r.b->nameField()->setText("docs");
        CHECK_FALSE(r.b->accept());
        CHECK_STR(r.b->dir(), t.at("docs"));
        CHECK_STR(r.b->nameField()->text(), "x.bin");
    }
}

TEST("filebrowser: pick folder answers the selected or the shown folder") {
    Tree t;
    {
        Run                            r({.mode = Mode::PickFolder, .initialDir = t.root});
        const std::vector<std::string> want = {"docs", "pics"}; // no files
        CHECK(r.names() == want);
        r.win.key(plat::Key::End); // pics
        CHECK(r.b->accept());
        REQUIRE(r.got.has_value());
        CHECK(*r.got == std::vector<std::string>{t.at("pics")});
    }
    {
        Run r({.mode = Mode::PickFolder, .initialDir = t.at("docs")});
        CHECK(r.b->accept()); // nothing selected: the folder shown
        REQUIRE(r.got.has_value());
        CHECK(*r.got == std::vector<std::string>{t.at("docs")});
    }
}

TEST("filebrowser: an unreadable start falls back to home, a bad folder keeps the listing") {
    Tree t;
    Run  r({.mode = Mode::Open, .initialDir = t.at("no/such/dir")});
    CHECK_FALSE(r.b->dir().empty());
    CHECK(r.b->setDir(t.root));
    CHECK_FALSE(r.b->setDir(t.at("missing")));
    CHECK_STR(r.b->dir(), t.root);
    CHECK(r.b->entryCount() == 4);
}

// M9: file rows (narrower by their size column) keep their shaped names
// across repaints, and moving the pointer repaints only the rows whose
// hover fill changes.
TEST("filebrowser: hovering repaints two rows and reshapes no names") {
    Tree t;
    Run  r({.mode = Mode::Open, .initialDir = t.root});
    r.win.move(r.rowPos(2).x, r.rowPos(2).y); // Alpha.txt, a file
    r.win.frame();
    const size_t n0 = text::layoutBuilds();
    // Still pointer, a full repaint: nothing reshaped.
    r.win.w->damageAll();
    r.win.frame();
    CHECK(text::layoutBuilds() == n0);
    // Within the row: nothing to repaint.
    const int frames = r.win.w->stats().frames;
    r.win.move(r.rowPos(2).x + 30, r.rowPos(2).y + 3);
    CHECK(r.win.w->stats().frames == frames);
    // To the next row: the old and the new row, nothing else.
    r.win.move(r.rowPos(3).x, r.rowPos(3).y);
    const ui::View *list = Run::findList(r.b);
    const float     s    = r.win.w->scale();
    CHECK(r.win.damageArea() > 0);
    CHECK(r.win.damageArea() <= long(2 * 28 * s * list->width() * s) + 2);
    CHECK(text::layoutBuilds() == n0);
}

#include "harness.h"

using namespace uitest;

TEST("damage: changing one label repaints only its rect (plus its ink margin)") {
    Win   w(400, 300);
    auto *col = w.root().add<ui::View>();
    col->style().padding(10).spacing(12).items(ui::Align::Start);
    std::vector<ui::Label *> labels;
    std::vector<Box *>       boxes;
    for (int i = 0; i < 5; ++i) {
        auto *l = col->add<ui::Label>("Label " + std::to_string(i));
        l->style().size(200, 20); // fixed: a text change is a repaint, not a relayout
        labels.push_back(l);
        boxes.push_back(col->add<Box>(50, 10));
    }
    w.frame();
    for (Box *b : boxes)
        b->paints = 0;
    labels[2]->setText("Changed text");
    w.frame();
    const auto &d = w.w->stats().lastDamage;
    REQUIRE(d.size() == 1);
    // The label's rect grown by the margin glyph ink may reach (see Label).
    const ui::RectF r = labels[2]->damageRect();
    const float     s = w.w->scale();
    CHECK(labels[2]->paintOutset() > 0 && r.w > labels[2]->width());
    CHECK(d[0].x == int(r.x * s) && d[0].y == int(r.y * s));
    CHECK(d[0].w == int(r.w * s) && d[0].h == int(r.h * s));
    for (Box *b : boxes)
        if (!ui::overlaps(b->windowRect(), r))
            CHECK(b->paints == 0); // nothing outside the rect painted
    CHECK(boxes[0]->paints == 0 && boxes[4]->paints == 0);
}

TEST("damage: a bitmap filled in place repaints only the views showing it") {
    Win   w(400, 300);
    auto *col = w.root().add<ui::View>();
    col->style().padding(10).spacing(10).items(ui::Align::Start);
    auto  shared = std::make_shared<gfx::Bitmap>();
    auto  other  = std::make_shared<gfx::Bitmap>();
    auto *a      = col->add<ui::Image>();
    auto *b      = col->add<ui::Image>();
    auto *c      = col->add<ui::Image>();
    auto *gone   = col->add<ui::Image>();
    for (ui::Image *i : {a, b, c, gone})
        i->style().size(40, 40);
    a->setBitmap(shared);
    b->setBitmap(other);
    c->setBitmap(shared);
    gone->setBitmap(shared);
    gone->setVisible(false); // hidden: paints nothing, damages nothing
    w.frame();
    const int f0 = w.w->stats().frames;
    w.w->damageShowing({other});
    w.frame();
    CHECK(w.w->stats().frames == f0 + 1);
    const float s = w.w->scale();
    CHECK(w.damageArea() == long(40 * s) * long(40 * s)); // b alone
    w.w->damageShowing({shared});
    w.frame();
    CHECK(w.damageArea() == 2 * long(40 * s) * long(40 * s)); // a and c
    w.w->damageShowing({std::make_shared<gfx::Bitmap>()});    // nobody's
    w.frame();
    CHECK(w.w->stats().frames == f0 + 2);
}

TEST("damage: hover repaints only the button; idle frames paint nothing") {
    Win   w(400, 300);
    auto *col = w.root().add<ui::View>();
    col->style().padding(20).spacing(10).items(ui::Align::Start);
    auto *a = col->add<ui::Button>("First", ui::Button::Kind::Ghost);
    col->add<ui::Button>("Second", ui::Button::Kind::Ghost);
    w.frame();
    const int f0 = w.w->stats().frames;
    w.frame(5);
    CHECK(w.w->stats().frames == f0); // nothing dirty: no paint at all
    const ui::RectF r = a->windowRect();
    w.move(r.x + 3, r.y + 3);
    w.frame();
    CHECK(w.w->stats().frames == f0 + 1);
    CHECK(w.damageArea() == long(r.w * w.w->scale()) * long(r.h * w.w->scale()));
}

TEST("damage: moving a view damages old and new rects; paint culls the rest") {
    Win   w(400, 300);
    auto *row = w.root().add<ui::View>();
    row->style().row();
    auto *col = row->add<ui::View>();
    col->style().width(200).items(ui::Align::Start);
    auto *spacer = col->add<Box>(10, 10);
    auto *mover  = col->add<Box>(40, 40);
    auto *right  = row->add<ui::View>();
    right->style().flex(1).items(ui::Align::Start);
    std::vector<Box *> still;
    for (int i = 0; i < 20; ++i)
        still.push_back(right->add<Box>(100, 10));
    w.frame();
    for (Box *b : still)
        b->paints = 0;
    spacer->content = {10, 30};
    spacer->invalidateLayout();
    w.frame();
    CHECK(mover->frame().y == 30);
    const float s = w.w->scale();
    CHECK(w.damageArea() <= long((40 * 40 * 2 + 10 * 30) * s * s) + 100);
    for (Box *b : still)
        CHECK(b->paints == 0);
    CHECK(mover->paints > 0);
}

namespace plat::testing_internal {
void setHeadlessScale(Window &w, double s);
}

namespace {
std::vector<uint32_t> grab(Win &w) {
    const float           s  = w.w->scale();
    const int             pw = int(400 * s + 0.5f), ph = int(300 * s + 0.5f);
    std::vector<uint32_t> px(size_t(pw) * ph);
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x)
            hooks().readPixel(w.native(), x, y, &px[size_t(y) * pw + x]);
    return px;
}

struct TextRows : ui::VirtualList::Adapter {
    int                       count() const override { return 500; }
    std::unique_ptr<ui::View> create(int) override {
        auto row = std::make_unique<ui::View>();
        row->style().row().padding(10, 4).spacing(8).items(ui::Align::Center);
        auto *dot = row->add<ui::Badge>(1);
        (void)dot;
        row->add<ui::Label>();
        return row;
    }
    void bind(ui::View &row, int i) override {
        static_cast<ui::Badge *>(row.child(0))->setCount(i % 9 + 1);
        static_cast<ui::Label *>(row.child(1))
            ->setText("Row " + std::to_string(i) + (i % 3 ? " with some longer text" : ""));
    }
};
} // namespace

TEST("damage: a blit-scrolled frame equals a full repaint (scale 1 and 1.5)") {
    for (double scale : {1.0, 1.5}) {
        Win w(400, 300);
        plat::testing_internal::setHeadlessScale(w.native(), scale);
        TextRows rows;
        auto    *list = w.root().add<ui::VirtualList>(&rows);
        list->setBackground(ui::C::Surface);
        list->setStickToBottom(true);
        w.frame();
        for (int step : {-7, -13, -40, -3, -91, 25}) {
            list->scrollBy(float(step));
            w.frame();
        }
        if (w.damageArea() >= long(400 * 300 * scale * scale / 2))
            std::fprintf(stderr, "    scale %.1f: damage %ld\n", scale, w.damageArea());
        CHECK(w.damageArea() < long(400 * 300 * scale * scale / 2)); // it did blit
        const auto blitted = grab(w);
        w.w->damageAll();
        w.frame();
        const auto repainted = grab(w);
        size_t     diff      = 0;
        for (size_t i = 0; i < blitted.size(); ++i)
            diff += blitted[i] != repainted[i];
        if (diff)
            std::fprintf(stderr, "    scale %.1f: %zu pixels differ\n", scale, diff);
        CHECK(diff == 0);
    }
}

TEST("damage: ScrollView blits match a full repaint too (scale 1.5)") {
    Win w(400, 300);
    plat::testing_internal::setHeadlessScale(w.native(), 1.5);
    auto *sv = w.root().add<ui::ScrollView>();
    sv->setBackground(ui::C::Surface);
    sv->content()->style().padding(8).spacing(3);
    for (int i = 0; i < 60; ++i) {
        auto *row = sv->content()->add<ui::Clickable>();
        row->style().row().height(27).padding(10, 0).spacing(6).items(ui::Align::Center);
        row->add<ui::Label>("Channel " + std::to_string(i));
        if (i % 4 == 0)
            row->add<ui::Badge>(i + 1);
    }
    w.frame();
    for (float step : {33.3f, 7.7f, 120.f, -45.2f, 0.4f, 0.4f, 0.4f})
        sv->scrollBy(step), w.frame();
    CHECK(w.damageArea() < long(400 * 300 * 1.5 * 1.5 / 2));
    const auto blitted = grab(w);
    w.w->damageAll();
    w.frame();
    const auto repainted = grab(w);
    size_t     diff      = 0;
    for (size_t i = 0; i < blitted.size(); ++i)
        diff += blitted[i] != repainted[i];
    if (diff)
        std::fprintf(stderr, "    %zu pixels differ\n", diff);
    CHECK(diff == 0);
}

namespace {
// A badge-like view drawn over the list (a "jump to bottom" button, the
// messages hover toolbar): a later sibling of the scroll area.
struct Floater : ui::View {
    Floater() { setBackground(ui::C::Accent, 6); }
};
} // namespace

TEST("damage: what a damage-only presenter shows equals a full repaint while scrolling") {
    // Regression: the blit shifted pixels but only the repainted strip was
    // reported to endPaint, so X11/Wayland/Win32 (which present by damage,
    // and Wayland keeps its other buffers fresh by damage) showed stale rows.
    // Also: views painted over a scroll area were smeared by the blit.
    for (double scale : {1.0, 1.25, 1.5}) {
        Win w(400, 300);
        plat::testing_internal::setHeadlessScale(w.native(), scale);
        w.w->setVerify(true);
        auto *st = w.root().add<ui::View>();
        st->style().stack();
        TextRows rows;
        auto    *list = st->add<ui::VirtualList>(&rows);
        list->setBackground(ui::C::Surface);
        list->setStickToBottom(true);
        auto *fl = st->add<Floater>();
        fl->style().size(60, 24).alignSelf(ui::Align::Center);
        w.frame();
        auto pop = std::make_unique<ui::Popup>();
        pop->add<Box>(80, 40);
        pop->setAnchor({250, 40, 10, 10}, ui::Popup::Place::Below);
        w.w->showPopup(std::move(pop));
        w.frame();
        for (float step : {-7.f, -13.f, -40.f, -3.3f, -91.f, 25.f, -0.4f, -0.4f, -120.f, 60.f})
            list->scrollBy(step), w.frame();
        auto *sv = w.root().add<ui::ScrollView>();
        sv->style().margins(200, 0, 0, 0);
        for (int i = 0; i < 40; ++i)
            sv->content()->add<ui::Label>("Line " + std::to_string(i));
        w.frame();
        for (float step : {33.3f, 7.7f, 120.f, -45.2f, 0.4f})
            sv->scrollBy(step), w.frame();
        if (w.w->stats().verifyMismatches)
            std::fprintf(
                stderr,
                "    scale %.2f: %d mismatching frames, first at [%d,%d %dx%d]\n",
                scale,
                w.w->stats().verifyMismatches,
                w.w->stats().firstMismatch.x,
                w.w->stats().firstMismatch.y,
                w.w->stats().firstMismatch.w,
                w.w->stats().firstMismatch.h
            );
        CHECK(w.w->stats().verifiedFrames > 10);
        CHECK(w.w->stats().verifyMismatches == 0);
    }
}

TEST("damage: glyph ink outside a label's box is repainted when its text changes") {
    // Regression: "Jonas Weber" in the header — the J's hook and italic
    // overhang paint left/right of the label's frame; damage covered only the
    // frame, so a text change left stale ink next to it.
    Win w(400, 200);
    w.w->setVerify(true);
    auto *col = w.root().add<ui::View>();
    col->style().padding(40).items(ui::Align::Start);
    auto *l   = col->add<ui::Label>();
    auto  set = [&](const char *t) {
        text::AttributedText a;
        text::Style          st = ui::font(ui::Font::Title);
        st.size                 = 44;
        st.italic               = true;
        st.color                = ui::themed(ui::C::Text);
        a.append(t, st);
        l->setRichText(std::move(a));
        w.frame();
    };
    for (const char *t : {"Jonas fj", "Mira ff", "Jyf", "Lena fff", "Jonas fj"})
        set(t);
    CHECK(w.w->stats().verifyMismatches == 0);
}

namespace {
// Date-separator-like rows: a padded pill label centred in an odd height,
// which puts its text origin on a half physical pixel at 1.5x.
struct PillRows : ui::VirtualList::Adapter {
    int                       count() const override { return 300; }
    std::unique_ptr<ui::View> create(int) override {
        auto row = std::make_unique<ui::View>();
        row->style().stack().height(33).items(ui::Align::Center);
        auto *l = row->add<ui::Label>("", ui::Font::SmallBold);
        l->setBorder(ui::C::Border);
        l->setBackground(ui::C::Surface, 12);
        l->style().padding(11, 3);
        return row;
    }
    void bind(ui::View &row, int i) override {
        static_cast<ui::Label *>(row.child(0))->setText(i % 2 ? "Yesterday" : "Monday, Sep 28");
    }
};
} // namespace

TEST("damage: text in a scrolled row lands on the same pixels as a repaint (1.5x)") {
    // Regression: Label centred its text with a logical floor(); at 1.5x that
    // is a half physical pixel, where float noise flips the glyph row's
    // rounding, so a label blitted into place sat one pixel off the same
    // label repainted there ("Yesterday" pills jittering while scrolling).
    Win w(400, 300);
    plat::testing_internal::setHeadlessScale(w.native(), 1.5);
    w.w->setVerify(true);
    PillRows rows;
    auto    *list = w.root().add<ui::VirtualList>(&rows);
    list->setBackground(ui::C::Surface);
    list->setStickToBottom(true);
    w.frame();
    for (int i = 0; i < 60; ++i)
        list->scrollBy(-float(3 + (i * 7) % 29) - 0.35f), w.frame();
    CHECK(w.w->stats().verifyMismatches == 0);
}

TEST("hover: a tooltip appears after the pointer rests, and goes on press") {
    Win   w(400, 300);
    auto *col = w.root().add<ui::View>();
    col->style().items(ui::Align::Start).padding(20);
    auto *b = col->add<ui::IconButton>(gfx::Icon(0), "Bold");
    w.frame();
    const ui::RectF r = b->windowRect();
    w.move(r.x + 4, r.y + 4);
    CHECK(w.until([&] { return w.w->tooltip() != nullptr; }, 3000));
    CHECK(w.w->tooltip() && w.w->tooltip()->frame().y >= r.y + r.h);
    w.press();
    w.release();
    CHECK(w.w->tooltip() == nullptr);
}

TEST("theme: switching to dark restyles live without rebuilding the tree") {
    Win   w(200, 100);
    auto *l = w.root().add<ui::Label>("Hello");
    w.frame();
    uint32_t px = 0;
    REQUIRE(hooks().readPixel(w.native(), 199, 99, &px));
    CHECK((px & 0xffffff) == (ui::color(ui::C::WindowBg) & 0xffffff));
    const uint32_t light = px;
    app().setThemeMode(ui::ThemeMode::Dark);
    w.frame();
    CHECK(app().dark());
    REQUIRE(hooks().readPixel(w.native(), 199, 99, &px));
    CHECK(px != light);
    CHECK((px & 0xffffff) == (ui::color(ui::C::WindowBg) & 0xffffff));
    CHECK(w.root().child(0) == l); // same objects
    app().setThemeMode(ui::ThemeMode::Light);
    w.frame();
    REQUIRE(hooks().readPixel(w.native(), 199, 99, &px));
    CHECK(px == light);
    CHECK(ui::resolve(ui::themed(ui::C::Link)) == ui::color(ui::C::Link));
    CHECK(ui::resolve(0xff123456) == 0xff123456);
}

namespace {

// The window's pixels over `r` (window coordinates; the test windows are 1x).
std::vector<uint32_t> grab(Win &w, ui::RectF r) {
    std::vector<uint32_t> px;
    for (int y = int(r.y); y < int(r.bottom()); ++y)
        for (int x = int(r.x); x < int(r.right()); ++x) {
            uint32_t c = 0;
            hooks().readPixel(w.native(), x, y, &c);
            px.push_back(c);
        }
    return px;
}

} // namespace

TEST("damage: colour changes and hover recolour text, never reshape it") {
    Win   w(500, 300);
    auto *col = w.root().add<ui::View>();
    col->style().padding(20).spacing(10).items(ui::Align::Start);
    auto *label = col->add<ui::Label>("Sidebar row name", ui::Font::Body, ui::C::TextMuted);
    auto *rich  = col->add<ui::Label>();
    text::AttributedText t;
    t.append("rich ", ui::font(ui::Font::Body, ui::C::Text));
    t.append("link", ui::font(ui::Font::Body, ui::C::Link));
    rich->setRichText(t);
    auto *ghost = col->add<ui::Button>("Ghost", ui::Button::Kind::Ghost);
    auto *tab   = col->add<ui::Button>("Tab", ui::Button::Kind::Tab);
    auto *form = col->add<ui::Button>("Save", ui::Button::Kind::Secondary, ui::Button::Form::Small);
    auto *spin = col->add<ui::SpinBox>(14, 1, 99, " days");
    w.frame();
    const size_t n0 = text::layoutBuilds();
    // A label's colour (the sidebar's hover ink); a rich label's (unused).
    label->setColor(ui::C::Text);
    rich->setColor(ui::C::Danger);
    w.frame();
    // Hovering buttons, recolouring one, toggling a form button, focusing a
    // spin box (its number turns white on the accent).
    const ui::RectF g = ghost->windowRect();
    w.move(g.x + 5, g.y + 5);
    ghost->setTextColor(ui::C::Danger);
    w.frame();
    form->setEnabled(false);
    w.frame();
    form->setEnabled(true);
    spin->focus();
    w.frame();
    w.move(1, 1);
    CHECK(text::layoutBuilds() == n0);
    // The recoloured label paints what a label built in that colour does:
    // a font round trip drops its layout and builds the reference.
    const ui::RectF lr     = label->windowRect();
    const auto      before = grab(w, lr);
    label->setFont(ui::Font::BodyBold);
    w.frame();
    label->setFont(ui::Font::Body);
    w.frame();
    CHECK(text::layoutBuilds() > n0);
    CHECK(grab(w, lr) == before);
    // The same for the disabled form button.
    form->setEnabled(false);
    w.frame();
    const auto fb = grab(w, form->windowRect());
    form->setLabel("Other");
    w.frame();
    form->setLabel("Save");
    w.frame();
    CHECK(grab(w, form->windowRect()) == fb);
    // A tab still turns bold when checked (its font follows the state).
    const size_t n1 = text::layoutBuilds();
    tab->setChecked(true);
    w.frame();
    CHECK(text::layoutBuilds() == n1 + 1);
}

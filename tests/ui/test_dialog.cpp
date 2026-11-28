// The dialog kit: the titled ui::Dialog, the form Button and TextField, the date
// and time fields and check box, and the tooltip (above, arrowed, and at once where a view asks for
// it).
#include "harness.h"
#include "gfx/icons_generated.h"
#include "base/time.h"
#include "ui/controls.h"
#include "ui/datetime.h"
#include "base/time.h"

using namespace uitest;

namespace plat::testing_internal {
void setHeadlessScale(Window &w, double s);
}

namespace {

// A popup that closes another one as it is destroyed (a menu taking its
// submenu down with it).
struct Chained : ui::Popup {
    ui::Window *win   = nullptr;
    ui::Popup  *other = nullptr;
    bool       *gone  = nullptr;
    ~Chained() override {
        if (gone)
            *gone = true;
        if (win && other)
            win->closePopup(other);
    }
};

struct Tipped : ui::Clickable {
    bool now = false;
    Tipped() {
        style().size(40, 30);
        setTooltip("Forward message");
    }
    bool tooltipImmediate() const override { return now; }
};

} // namespace

TEST("popup: the titled dialog — 560-wide card, Escape, backdrop and buttons") {
    Win   w(1000, 700);
    int   accepted = 0, rejected = 0;
    auto  d   = std::make_unique<ui::Dialog>("Delete message");
    auto *del = ui::Dialog::makeButton("Delete", ui::Button::Kind::Danger);
    d->addButtonRow(del, ui::Dialog::makeButton("Cancel", ui::Button::Kind::Secondary));
    d->onAccepted   = [&] { ++accepted; };
    d->onRejected   = [&] { ++rejected; };
    ui::Dialog *raw = d.get();
    del->onClick    = [raw] { raw->accept(); };
    w.w->showPopup(std::move(d));
    w.frame();
    const ui::RectF card = raw->panel()->windowRect();
    CHECK(near(card.w, 560));                // clamp(W − 80, 480, 560)
    CHECK(near(card.x, (1000 - 560) / 2.f)); // centred
    CHECK(near(del->frame().h, 38));         // StyledButton Normal
    w.key(plat::Key::Escape);
    CHECK(rejected == 1 && w.w->topPopup() == nullptr);
    // A press on the backdrop rejects; the danger button accepts.
    auto d2        = std::make_unique<ui::Dialog>("Reminder", 440);
    d2->onRejected = [&] { ++rejected; };
    ui::Dialog *r2 = d2.get();
    w.w->showPopup(std::move(d2));
    w.frame();
    CHECK(near(r2->panel()->windowRect().w, 440));
    w.click(10, 10);
    CHECK(rejected == 2 && w.w->topPopup() == nullptr);
    auto  d3 = std::make_unique<ui::Dialog>("Delete message");
    auto *b3 = ui::Dialog::makeButton("Delete", ui::Button::Kind::Danger);
    d3->addButtonRow(b3, nullptr);
    d3->onAccepted = [&] { ++accepted; };
    ui::Dialog *r3 = d3.get();
    b3->onClick    = [r3] { r3->accept(); };
    w.w->showPopup(std::move(d3));
    w.frame();
    const ui::RectF br = b3->windowRect();
    w.click(br.x + 5, br.y + 5);
    CHECK(accepted == 1);
}

TEST("popup: a titled card taller than the window scrolls; its buttons stay reachable") {
    Win  w(800, 400);
    int  accepted = 0;
    auto d        = std::make_unique<ui::Dialog>("Profile");
    for (int i = 0; i < 12; ++i)
        d->content()->add<Box>(0, 60); // 12 × 60 + gaps: far past 400 − 80
    auto *ok = ui::Dialog::makeButton("Save", ui::Button::Kind::Primary);
    d->addButtonRow(ok, ui::Dialog::makeButton("Cancel", ui::Button::Kind::Secondary));
    d->onAccepted   = [&] { ++accepted; };
    ui::Dialog *raw = d.get();
    ok->onClick     = [raw] { raw->accept(); };
    w.w->showPopup(std::move(d));
    w.frame();
    const ui::RectF card = raw->panel()->windowRect();
    CHECK(card.h <= 400 - 80 + 0.5f); // height ≤ window − 80
    REQUIRE(raw->scroller() != nullptr);
    CHECK(raw->scroller()->canScroll());
    // Tab to Cancel (past the card's bottom) scrolls it into view
    // (focus-follows scrolling), then back to the top.
    w.key(plat::Key::Tab);
    CHECK(raw->scroller()->scrollOffset() > 0);
    raw->scroller()->scrollTo(0);
    w.frame();
    // Scrolled to the end, Save is inside the card and takes a click.
    w.move(card.x + card.w / 2, card.y + card.h / 2);
    for (int i = 0; i < 40 && !raw->scroller()->atEnd(); ++i) {
        hooks().injectScroll(w.native(), 0, 3); // notches down
        w.until([&] { return false; }, 30);
    }
    REQUIRE(raw->scroller()->atEnd());
    const ui::RectF br = ok->windowRect();
    CHECK(br.y + br.h <= card.y + card.h + 0.5f);
    w.click(br.x + 5, br.y + 5);
    CHECK(accepted == 1);
    // One that fits doesn't scroll, nor is it any taller than its content.
    auto  small = std::make_unique<ui::Dialog>("Reminder");
    auto *b     = ui::Dialog::makeButton("OK", ui::Button::Kind::Primary);
    small->addButtonRow(b, nullptr);
    ui::Dialog *s2 = small.get();
    w.w->showPopup(std::move(small));
    w.frame();
    CHECK_FALSE(s2->scroller()->canScroll());
    CHECK(s2->panel()->windowRect().h < 200);
}

TEST("popup: the tooltip sits above the target with its arrow; some show at once") {
    Win   w(400, 300);
    auto *t = w.root().add<Tipped>();
    t->style().alignSelf(ui::Align::Center);
    w.frame();
    const ui::RectF r = t->windowRect();
    t->now            = true;
    w.move(r.x + 5, r.y + 5);
    CHECK(w.w->tooltip() != nullptr); // no delay
    const ui::RectF tr = const_cast<ui::Popup *>(w.w->tooltip())->windowRect();
    CHECK(tr.y + tr.h <= r.y - 3);                  // above, 4 px gap
    CHECK(near(tr.x + tr.w / 2, r.x + r.w / 2, 1)); // centred on it
    w.move(390, 290);
    CHECK(w.w->tooltip() == nullptr);
    t->now = false;
    w.move(r.x + 5, r.y + 5);
    CHECK(w.w->tooltip() == nullptr); // the usual delay
    CHECK(w.until([&] { return w.w->tooltip() != nullptr; }));
}

TEST("popup: date and time fields step, type and clamp") {
    Win   w(500, 300);
    auto *col = w.root().add<ui::View>();
    col->style().padding(20).spacing(8);
    auto *date = col->add<ui::DateTimeField>(ui::DateTimeField::Kind::Date);
    auto *time = col->add<ui::DateTimeField>(ui::DateTimeField::Kind::Time);
    date->setMinimumDate(2026, 9, 30);
    date->setDate(2026, 9, 30);
    time->setTime(20, 45);
    w.frame();
    CHECK(near(date->frame().h, 32) && date->frame().w >= 240);
    date->focus();
    w.key(plat::Key::Down); // month back: below the minimum → clamped
    CHECK(date->month() == 9 && date->day() == 30);
    w.key(plat::Key::Up);
    CHECK(date->month() == 10);
    w.key(plat::Key::Right); // day
    w.key(plat::Key::Num5);
    CHECK(date->day() == 5);
    time->focus();
    w.key(plat::Key::Right); // minutes
    w.key(plat::Key::Up);
    CHECK(time->minute() == 46);
    w.key(plat::Key::Right); // AM/PM
    w.key(plat::Key::A);
    CHECK(time->hour() == 8);
    // Date and time ("MMM d, yyyy h:mm AP"): never below its minimum, which
    // is rounded up to the whole minute it shows.
    auto         *dt  = col->add<ui::DateTimeField>(ui::DateTimeField::Kind::DateTime);
    const int64_t min = base::fromLocal(2026, 9, 30, 20, 45, 30);
    dt->setMinimumValue(min);
    dt->setValue(min - 3600);
    CHECK(dt->value() == base::fromLocal(2026, 9, 30, 20, 46));
    dt->setValue(base::fromLocal(2026, 10, 2, 9, 5));
    dt->focus();
    w.key(plat::Key::Up); // Oct → Nov
    CHECK(dt->month() == 11);
    w.key(plat::Key::Right);
    w.key(plat::Key::Right); // year: four digits
    w.key(plat::Key::Num2);
    w.key(plat::Key::Num0);
    w.key(plat::Key::Num2);
    CHECK(dt->year() == 2026);
    w.key(plat::Key::Num7);
    CHECK(dt->year() == 2027);
    CHECK(dt->section() == 3); // on to the hour
    w.key(plat::Key::Down);
    CHECK(dt->hour() == 8);
    dt->setValue(min);
    dt->setSection(3);
    w.key(plat::Key::Down); // an hour before the minimum: clamped
    CHECK(dt->value() == base::fromLocal(2026, 9, 30, 20, 46));
    // The sections follow the date language's short date: yyyy/MM/dd.
    base::setDateLanguage("ja");
    date->setSection(0);
    date->step(1);
    CHECK(date->year() == 2027 && date->month() == 10);
    date->setSection(2);
    date->step(1);
    CHECK(date->day() == 6);
    base::setDateLanguage("en");
    // A check box toggles on click and Space.
    bool  on     = false;
    auto *cb     = col->add<ui::CheckBox>("Add a note with the original author and time");
    cb->onChange = [&](bool v) { on = v; };
    w.frame();
    const ui::RectF c = cb->windowRect();
    CHECK(near(c.h, 18)); // the box: 16 + the 1-px border
    w.click(c.x + 8, c.y + c.h / 2);
    CHECK(on && cb->checked());
    cb->focus();
    w.key(plat::Key::Space);
    CHECK(!on && !cb->checked());
}

// A check box label that fits is not reshaped for every width it is offered
// (a flex pass asks at the room, then at the width it got); one that has to
// wrap is, and wraps.
TEST("controls: a check box label reshapes only when its wrapping changes") {
    Win   w(600, 300);
    auto *col = w.root().add<ui::View>();
    col->style().items(ui::Align::Start);
    auto *cb = col->add<ui::CheckBox>("Remember my choice");
    w.frame();
    const float  h0 = cb->frame().h;
    const size_t n0 = text::layoutBuilds();
    for (float width : {500.f, 450.f, 300.f, 560.f}) {
        col->style().width(width);
        col->invalidateLayout();
        w.frame();
    }
    CHECK(text::layoutBuilds() == n0);
    col->style().width(80); // too narrow: wraps
    col->invalidateLayout();
    w.frame();
    CHECK(text::layoutBuilds() > n0);
    CHECK(cb->frame().h > h0);
}

TEST("controls: StyledLineEdit's sizes, leading icon and length counter") {
    Win   w(500, 300);
    auto *col = w.root().add<ui::View>();
    col->style().padding(20).spacing(8);
    auto *small = col->add<ui::TextField>("Paste a Slack theme");
    auto *norm  = col->add<ui::TextField>(
        "Filter threads", ui::TextField::Size::Normal, uint16_t(gfx::Icon::Search)
    );
    auto *name = col->add<ui::TextField>("Mira, Jonas", ui::TextField::Size::Normal);
    name->setMaxLength(5);
    w.frame();
    CHECK(near(small->frame().h, 30) && near(norm->frame().h, 38) && near(name->frame().h, 38));
    // The text starts after the icon (12 + 16 + 8, plus the line edit's own 3).
    CHECK(near(norm->edit().frame().x, 39) && near(small->edit().frame().x, 15));
    // A press on the frame focuses the edit.
    const ui::RectF r = norm->windowRect();
    w.click(r.x + 4, r.y + r.h / 2);
    CHECK(norm->edit().focused());
    // Input past the cap is cut.
    name->edit().focus();
    w.type("abcdefgh");
    CHECK(name->text() == "abcde");
}

// The label's capitals sit in the middle of the button: centring the line
// box instead put "Save" a couple of pixels low (the ascent above the caps
// is larger than the descent below the baseline).
TEST("controls: a form Button's label is vertically centred on its capitals") {
    for (double scale : {1.0, 1.5}) {
        Win w(300, 120);
        plat::testing_internal::setHeadlessScale(w.native(), scale);
        auto *col = w.root().add<ui::View>();
        col->style().padding(20).spacing(10).items(ui::Align::Start);
        auto *normal =
            col->add<ui::Button>("SHE", ui::Button::Kind::Primary, ui::Button::Form::Normal);
        auto *small =
            col->add<ui::Button>("SHE", ui::Button::Kind::Primary, ui::Button::Form::Small);
        w.frame();
        const float sc = w.w->scale();
        for (ui::Button *b : {normal, small}) {
            const ui::RectF r  = b->windowRect();
            const int       x0 = int(std::ceil(r.x * sc)), x1 = int((r.x + r.w) * sc);
            const int       y0 = int(std::ceil(r.y * sc)), y1 = int((r.y + r.h) * sc);
            uint32_t        bg = 0;
            hooks().readPixel(w.native(), (x0 + x1) / 2, y0 + 2, &bg);
            int top = -1, bottom = -1;
            for (int y = y0 + 2; y < y1 - 2; ++y)
                for (int x = x0 + 4; x < x1 - 4; ++x) {
                    uint32_t px = 0;
                    hooks().readPixel(w.native(), x, y, &px);
                    if ((px & 0xffffff) != (bg & 0xffffff)) {
                        if (top < 0)
                            top = y;
                        bottom = y;
                        break;
                    }
                }
            REQUIRE(top >= 0);
            const float above = float(top - y0), below = float(y1 - 1 - bottom);
            CHECK(std::abs(above - below) <= 1); // at most a device pixel of rounding
        }
    }
}

// The form style of ui::Button (dialogs, settings): its heights, paddings
// and fills in every state, and the grey fill of a disabled filled one. A
// pixel left of the label shows the fill (inside Secondary's inner stroke);
// a disabled one is matched against a disabled view in its expected fill
// (disabled views paint dimmed).
TEST("controls: a form Button's sizes and state fills") {
    using K = ui::Button::Kind;
    using C = ui::C;
    using F = ui::Button::Form;
    static const struct {
        K kind;
        C bg, hover, pressed, disabled;
    } kKinds[] = {
        {K::Primary, C::Accent, C::AccentHover, C::AccentPressed, C::FormHighlightStrong},
        {K::Secondary, C::FormBg, C::FormSunken, C::FormHighlightStrong, C::FormBg},
        {K::Danger, C::DangerFill, C::DangerFillHover, C::DangerFillHover, C::FormHighlightStrong},
        {K::Ghost,
         C::FormHighlight,
         C::FormHighlightStrong,
         C::FormHighlightStrong,
         C::FormHighlight},
    };
    for (ui::ThemeMode m : {ui::ThemeMode::Light, ui::ThemeMode::Dark}) {
        app().setThemeMode(m);
        for (double scale : {1.0, 1.5})
            for (const auto &k : kKinds)
                for (F form : {F::Small, F::Normal}) {
                    Win w(300, 140);
                    plat::testing_internal::setHeadlessScale(w.native(), scale);
                    auto *col = w.root().add<ui::View>();
                    col->style().padding(20).spacing(10).items(ui::Align::Start);
                    auto *b   = col->add<ui::Button>("Save", k.kind, form);
                    auto *ref = col->add<ui::View>();
                    ref->style().size(40, 20);
                    ref->setBackground(k.disabled);
                    ref->setEnabled(false);
                    w.frame();
                    const bool      small = form == F::Small;
                    const ui::RectF r = b->windowRect(), rr = ref->windowRect();
                    CHECK(b->frame().h == (small ? ui::kFormSmallH : ui::kFormNormalH));
                    CHECK(b->style().pad.l == (small ? 12 : 18));
                    const float sc = w.w->scale();
                    auto        at = [&](float x, float y) {
                        uint32_t px = 0;
                        hooks().readPixel(w.native(), int(x * sc), int(y * sc), &px);
                        return px;
                    };
                    const float x = r.x + 6, y = r.y + r.h / 2;
                    // Every fill token is opaque in both themes.
                    CHECK(at(x, y) == ui::color(k.bg));
                    w.move(x, y);
                    CHECK(at(x, y) == ui::color(k.hover));
                    w.press();
                    CHECK(at(x, y) == ui::color(k.pressed));
                    w.release();
                    b->setEnabled(false);
                    w.frame();
                    CHECK(at(x, y) == at(rr.x + 20, rr.y + 10));
                }
    }
    app().setThemeMode(ui::ThemeMode::System);
}

// M10: the calendar shapes its labels once; hovering days repaints only the
// two cells whose highlight changes and reshapes nothing, nor does a
// repaint; the focused date field's section paints white without a build.
TEST("popup: the calendar repaints hovered cells only and keeps its labels") {
    Win   w(500, 500);
    auto *col = w.root().add<ui::View>();
    col->style().padding(20);
    auto *date = col->add<ui::DateTimeField>(ui::DateTimeField::Kind::Date);
    date->setDate(2026, 10, 14);
    w.frame();
    date->focus();
    w.frame();
    const ui::RectF f = date->windowRect();
    w.click(f.right() - 7, f.y + f.h / 2); // the drop-down arrow
    ui::Popup *cal = w.w->topPopup();
    REQUIRE(cal != nullptr);
    const ui::RectF r  = cal->windowRect();
    // Grid rows 2 and 3 always hold days (cells are 32 px under a 30 px head).
    auto            at = [&](int col, int row) {
        return ui::PointF{r.x + 8 + 32 * float(col) + 16, r.y + 8 + 30 + 32 * float(1 + row) + 16};
    };
    w.move(at(1, 2).x, at(1, 2).y);
    const size_t n0 = text::layoutBuilds();
    w.w->damageAll();
    w.frame();
    CHECK(text::layoutBuilds() == n0);
    w.move(at(2, 2).x, at(2, 2).y);
    const float s = w.w->scale();
    CHECK(w.damageArea() > 0);
    CHECK(w.damageArea() <= long(2 * 32 * s * 32 * s));
    w.move(at(2, 3).x, at(2, 3).y);
    CHECK(w.damageArea() <= long(2 * 32 * s * 32 * s));
    CHECK(text::layoutBuilds() == n0);
}

// The length cap cuts what goes in as part of the edit itself: typing and
// pastes past it keep their undo step (the cut used to replace the text and
// wipe the history), and a paste in the middle keeps the tail.
TEST("controls: a length limit cuts typing and pastes, and undo still works") {
    Win   w(400, 200);
    auto *f = w.root().add<ui::TextField>("Name");
    f->setMaxLength(5);
    w.frame();
    ui::TextEdit &e = f->edit();
    e.focus();
    w.type("abc");
    w.type("defg");
    CHECK_STR(f->text(), "abcde");
    CHECK(e.undo());
    CHECK_STR(f->text(), "");
    CHECK(e.redo());
    CHECK_STR(f->text(), "abcde");
    // A paste into the middle: only what fits, the tail stays.
    f->setText("abc");
    e.setSelection(1, 1);
    e.insertText("12345");
    CHECK_STR(f->text(), "a12bc");
    CHECK(e.caret() == 3);
    CHECK(e.undo());
    CHECK_STR(f->text(), "abc");
    // Full: a key adds nothing and leaves the history alone.
    f->setText("vwxyz");
    w.type("q");
    CHECK_STR(f->text(), "vwxyz");
    CHECK(!e.canUndo());
    // Code points, not bytes; setText is cut too.
    f->setText("\xC3\xA5\xC3\xA4\xC3\xB6\xC3\xBC\xC3\xA9x");
    CHECK_STR(f->text(), "\xC3\xA5\xC3\xA4\xC3\xB6\xC3\xBC\xC3\xA9");
}

TEST("popup: a popup's destructor closing another one frees both") {
    Win   w(400, 300);
    bool  aGone = false, bGone = false;
    auto *b        = static_cast<Chained *>(w.w->showPopup(std::make_unique<Chained>()));
    b->gone        = &bGone;
    auto a         = std::make_unique<Chained>();
    a->win         = w.w.get();
    a->other       = b;
    a->gone        = &aGone;
    ui::Popup *raw = w.w->showPopup(std::move(a));
    w.frame();
    w.w->closePopup(raw);
    w.frame(); // the next frame frees the closed one, and what it closed
    CHECK(aGone);
    CHECK(bGone);
    CHECK(w.w->topPopup() == nullptr);
}

#include "harness.h"

using namespace uitest;

namespace {

// Records what reaches it.
struct Probe : Box {
    int         enters = 0, leaves = 0, downs = 0, ups = 0, menus = 0, drops = 0, keys = 0;
    bool        acceptDrop = false, handleDown = true;
    std::string dropText;
    using Box::Box;
    bool onEvent(ui::Event &e) override {
        switch (e.type) {
        case ui::EventType::PointerEnter:
            ++enters;
            return false;
        case ui::EventType::PointerLeave:
            ++leaves;
            return false;
        case ui::EventType::PointerDown:
            ++downs;
            return handleDown;
        case ui::EventType::PointerUp:
            ++ups;
            return true;
        case ui::EventType::ContextMenu:
            ++menus;
            return true;
        case ui::EventType::KeyDown:
            ++keys;
            return false;
        case ui::EventType::DropEnter:
        case ui::EventType::DropMove:
            if (!acceptDrop)
                return false;
            e.dropAction = plat::DropAction::Copy;
            return true;
        case ui::EventType::Drop:
            if (!acceptDrop)
                return false;
            ++drops;
            dropText = e.raw ? e.raw->text : "";
            return true;
        default:
            return false;
        }
    }
};

} // namespace

TEST("hit: deepest view, overlap order, hit-transparent, disabled") {
    Win   w(400, 300);
    auto *st = w.root().add<ui::View>();
    st->style().stack();
    auto *under = st->add<Probe>();
    auto *over  = st->add<Probe>();
    over->style().size(100, 100);
    over->style().alignSelf(ui::Align::Start);
    auto *inner = over->add<Probe>(40, 40);
    inner->style().alignSelf(ui::Align::Start);
    w.frame();
    w.move(10, 10);
    w.press();
    w.release();
    CHECK(inner->downs == 1 && over->downs == 0 && under->downs == 0);
    w.move(80, 80); // inside `over`, outside `inner`
    w.press();
    w.release();
    CHECK(over->downs == 1);
    over->setHitTransparent(true);
    w.move(80, 80);
    w.press();
    w.release();
    CHECK(under->downs == 1);
    inner->setEnabled(false);
    w.move(10, 10);
    w.press();
    w.release();
    CHECK(inner->downs == 1 && under->downs == 2);
}

TEST("hit: unhandled press bubbles to the parent; right click asks for a context menu") {
    Win   w(400, 300);
    auto *outer      = w.root().add<Probe>();
    auto *leaf       = outer->add<Probe>(50, 50);
    leaf->handleDown = false;
    leaf->style().alignSelf(ui::Align::Start);
    w.frame();
    w.click(10, 10);
    CHECK(leaf->downs == 1 && outer->downs == 1);
    outer->handleDown = false;
    w.click(10, 10, plat::Button::Right);
    CHECK(leaf->menus == 1 && outer->menus == 0);
}

TEST("hover: chain gets enter/leave, hovered() follows the pointer") {
    Win   w(400, 300);
    auto *row = w.root().add<ui::View>();
    row->style().row();
    auto *a = row->add<Probe>();
    a->style().width(100);
    auto *child = a->add<Probe>(50, 50);
    child->style().alignSelf(ui::Align::Start);
    auto *b = row->add<Probe>();
    b->style().width(100);
    w.frame();
    w.move(10, 10);
    CHECK(child->hovered() && a->hovered() && !b->hovered());
    CHECK(a->enters == 1 && child->enters == 1);
    w.move(80, 80); // still in a, not in child
    CHECK(a->hovered() && !child->hovered() && child->leaves == 1 && a->enters == 1);
    w.move(150, 10);
    CHECK(!a->hovered() && b->hovered() && a->leaves == 1);
}

TEST("hover: press captures, click only when released inside") {
    Win   w(400, 300);
    auto *col = w.root().add<ui::View>();
    col->style().items(ui::Align::Start);
    auto *btn    = col->add<ui::Button>("Press me");
    int   clicks = 0;
    btn->onClick = [&] { ++clicks; };
    w.frame();
    const ui::RectF r = btn->windowRect();
    w.click(r.x + 5, r.y + 5);
    CHECK(clicks == 1);
    w.move(r.x + 5, r.y + 5);
    w.press();
    CHECK(btn->pressed());
    w.move(300, 250); // dragged out while held: still captured
    CHECK(btn->pressed());
    w.release();
    CHECK(clicks == 1 && !btn->pressed());
}

TEST("hover: cursor per view, inherited by children") {
    Win   w(400, 300);
    auto *row = w.root().add<ui::View>();
    row->style().row();
    auto *a = row->add<Box>();
    a->style().width(100);
    a->setCursor(plat::Cursor::Hand);
    auto *inner = a->add<Box>(20, 20);
    inner->style().alignSelf(ui::Align::Start);
    auto *edit = row->add<ui::TextEdit>();
    edit->style().flex(1);
    w.frame();
    w.move(5, 5);
    CHECK(plat::testing_internal::headlessCursor(w.native()) == plat::Cursor::Hand);
    w.move(200, 10);
    CHECK(plat::testing_internal::headlessCursor(w.native()) == plat::Cursor::IBeam);
}

TEST("hover: clickables show the hand, the list around them and disabled ones don't") {
    Win   w(400, 300);
    auto *col = w.root().add<ui::View>();
    col->style().items(ui::Align::Start);
    auto *btn = col->add<ui::Button>("Go");
    btn->style().size(100, 30);
    auto *off = col->add<ui::Button>("Off");
    off->style().size(100, 30);
    off->setEnabled(false);
    auto *sections = col->add<ui::SectionList>(std::vector<std::string>{"One", "Two"});
    sections->style().size(200, 200);
    w.frame();
    const auto cursor = [&] { return plat::testing_internal::headlessCursor(w.native()); };
    w.move(50, 15);
    CHECK(cursor() == plat::Cursor::Hand);
    w.move(300, 15); // the empty column beside it
    CHECK(cursor() == plat::Cursor::Arrow);
    w.move(50, 45);
    CHECK(cursor() == plat::Cursor::Arrow);
    w.move(20, sections->frame().y + 10); // the first row
    CHECK(cursor() == plat::Cursor::Hand);
    w.move(20, sections->frame().y + 190); // below the last row
    CHECK(cursor() == plat::Cursor::Arrow);
}

TEST("focus: Tab and Shift+Tab traverse in tree order, skipping hidden and disabled") {
    Win   w(400, 300);
    auto *col = w.root().add<ui::View>();
    col->style().items(ui::Align::Start);
    auto *a = col->add<ui::Button>("A");
    auto *b = col->add<ui::Button>("B");
    auto *c = col->add<ui::Button>("C");
    auto *d = col->add<ui::Button>("D");
    w.frame();
    w.key(plat::Key::Tab);
    CHECK(w.w->focusView() == a && a->focused());
    w.key(plat::Key::Tab);
    CHECK(w.w->focusView() == b);
    c->setVisible(false);
    d->setEnabled(false);
    w.key(plat::Key::Tab);
    CHECK(w.w->focusView() == a); // wrapped around, past hidden c and disabled d
    w.chord(plat::ModShift, plat::Key::Tab);
    CHECK(w.w->focusView() == b);
    CHECK(w.w->focusVisible());
    // Enter on a focused button activates it.
    int n      = 0;
    b->onClick = [&] { ++n; };
    w.key(plat::Key::Enter);
    CHECK(n == 1);
}

TEST("focus: clicking a text field focuses it, clicking a button does not steal focus") {
    Win   w(400, 300);
    auto *col  = w.root().add<ui::View>();
    auto *edit = col->add<ui::TextEdit>();
    auto *btn  = col->add<ui::Button>("Bold");
    btn->style().alignSelf(ui::Align::Start);
    w.frame();
    const ui::RectF e = edit->windowRect(), b = btn->windowRect();
    w.click(e.x + 10, e.y + 5);
    CHECK(edit->focused());
    w.click(b.x + 5, b.y + 5);
    CHECK(edit->focused() && !btn->focused());
}

TEST("focus: a field inside a disabled container takes no typing and no focus") {
    Win   w(400, 300);
    auto *box  = w.root().add<ui::View>();
    auto *edit = box->add<ui::TextEdit>();
    w.frame();
    edit->focus();
    w.type("ab");
    CHECK_STR(edit->text(), "ab");
    // Disabling the container (the composer) drops the field's focus.
    box->setEnabled(false);
    CHECK(!edit->focused() && w.w->focusView() == nullptr);
    w.type("cd");
    CHECK_STR(edit->text(), "ab");
    // Nor can it be focused again, by code or by a click, while disabled.
    edit->focus();
    const ui::RectF e = edit->windowRect();
    w.click(e.x + 10, e.y + 5);
    CHECK(!edit->focused());
    w.type("ef");
    CHECK_STR(edit->text(), "ab");
    box->setEnabled(true);
    edit->focus();
    w.type("g");
    CHECK(edit->focused());
    CHECK_STR(edit->text(), "abg");
}

TEST("shortcut: primary modifier, removal, focused view first") {
    Win   w(400, 300);
    auto *probe = w.root().add<Probe>();
    probe->setFocusable(true);
    int       fired = 0;
    const int id    = w.w->addShortcut(plat::Key::K, ui::Window::kPrimary, [&] { ++fired; });
    w.frame();
    const uint32_t primary = plat::primaryMod();
    w.chord(primary, plat::Key::K);
    CHECK(fired == 1);
    w.key(plat::Key::K); // no modifier: no match
    CHECK(fired == 1);
    w.chord(primary | plat::ModShift, plat::Key::K); // extra modifier: no match
    CHECK(fired == 1);
    probe->focus();
    w.chord(primary, plat::Key::K);
    CHECK(probe->keys >= 1 && fired == 2); // the view saw it first, did not consume
    w.w->removeShortcut(id);
    w.chord(primary, plat::Key::K);
    CHECK(fired == 2);
}

TEST("popup: outside press closes and is swallowed; Escape closes; focus returns") {
    Win   w(400, 300);
    auto *col = w.root().add<ui::View>();
    col->style().items(ui::Align::Start);
    auto *btn    = col->add<ui::Button>("Under");
    int   clicks = 0;
    btn->onClick = [&] { ++clicks; };
    auto *edit   = col->add<ui::TextEdit>();
    edit->style().width(200);
    w.frame();
    edit->focus();
    auto p = std::make_unique<ui::Popup>();
    p->add<Box>(100, 60);
    p->setAnchor({200, 100, 10, 10});
    bool closed   = false;
    p->onClosed   = [&] { closed = true; };
    ui::Popup *pp = w.w->showPopup(std::move(p));
    w.frame();
    CHECK(w.w->topPopup() == pp);
    CHECK(pp->frame().x == 200 && pp->frame().y == 114); // below the anchor + gap
    CHECK(w.w->focusView() == pp);
    const ui::RectF b = btn->windowRect();
    w.click(b.x + 5, b.y + 5);
    CHECK(closed && clicks == 0 && w.w->topPopup() == nullptr);
    CHECK(w.w->focusView() == edit);
    // Escape closes the next one.
    auto q = std::make_unique<ui::Popup>();
    q->add<Box>(50, 50);
    q->setAnchor({10, 250, 10, 10}); // no room below: flips above
    ui::Popup *qq = w.w->showPopup(std::move(q));
    w.frame();
    CHECK(qq->frame().y + qq->frame().h <= 250);
    w.key(plat::Key::Escape);
    CHECK(w.w->topPopup() == nullptr);
}

TEST("menu: keyboard navigation skips separators and disabled items") {
    Win w(400, 300);
    w.frame();
    int                       chosen = -1;
    std::vector<ui::MenuItem> items;
    items.push_back({1, "Reply in thread"});
    items.push_back({0, {}, {}, ui::Button::kNoIcon, true, false, true}); // separator
    items.push_back({2, "Disabled", {}, ui::Button::kNoIcon, false});
    items.push_back({3, "Copy link", "Ctrl+L"});
    items.push_back({4, "Delete message"});
    ui::Menu *m = ui::Menu::show(*w.w, {50, 50, 0, 0}, items, [&](int id) { chosen = id; });
    w.frame();
    CHECK(w.w->focusView() == m);
    w.key(plat::Key::Down);
    CHECK(m->current() == 0);
    w.key(plat::Key::Down);
    CHECK(m->current() == 3);
    w.key(plat::Key::End);
    CHECK(m->current() == 4);
    w.key(plat::Key::Up);
    w.key(plat::Key::Enter);
    CHECK(chosen == 3);
    CHECK(w.w->topPopup() == nullptr);
    // Letter jump + mouse choose.
    chosen = -1;
    m      = ui::Menu::show(*w.w, {50, 50, 0, 0}, items, [&](int id) { chosen = id; });
    w.frame();
    w.key(plat::Key::D);
    CHECK(m->current() == 4); // "Disabled" is skipped
    const ui::RectF r = m->windowRect();
    w.move(r.x + 20, r.y + 6 + 5); // first row
    CHECK(m->current() == 0);
    w.press();
    w.release();
    CHECK(chosen == 1);
    // A context menu opens under the pointer: the release of the opening
    // right click must not choose the item it lands on.
    chosen = -1;
    m      = ui::Menu::show(
        *w.w,
        {r.x + 20, r.y + 11, 0, 0},
        items,
        [&](int id) { chosen = id; },
        ui::Popup::Place::Over
    );
    w.frame();
    hooks().injectButton(w.native(), plat::Button::Right, false);
    w.frame();
    CHECK(chosen == -1 && w.w->topPopup() == m);
}

TEST("hit: drag and drop target accepts by position") {
    Win   src(200, 200), dst(400, 300);
    auto *row = dst.root().add<ui::View>();
    row->style().row();
    auto *no = row->add<Probe>();
    no->style().width(200);
    auto *yes = row->add<Probe>();
    yes->style().flex(1);
    yes->acceptDrop = true;
    dst.frame();
    src.move(10, 10);
    hooks().injectButton(src.native(), plat::Button::Left, true);
    plat::DragDesc d;
    d.items = {{"text/plain;charset=utf-8", "dragged"}};
    REQUIRE(app().platform().startDrag(src.native(), d));
    hooks().injectPointerMove(dst.native(), {50, 50});  // over `no`
    hooks().injectPointerMove(dst.native(), {300, 50}); // over `yes`
    hooks().injectButton(dst.native(), plat::Button::Left, false);
    dst.frame();
    CHECK(yes->drops == 1 && yes->dropText == "dragged" && no->drops == 0);
}

// Hiding the focused view's ancestor moves the focus away as disabling it
// does: the flag goes, FocusOut arrives, the caret stops blinking (it kept
// repainting a hidden field every half period). Taking it out of the window
// or destroying it drops the focus and the flag without an event.
TEST("focus: hiding or removing the focused field's ancestor drops its focus and caret") {
    Win   w(400, 300);
    auto *box           = w.root().add<ui::View>();
    auto *edit          = box->add<ui::TextEdit>();
    int   outs          = 0;
    edit->onFocusChange = [&](bool on) { outs += !on; };
    w.frame();
    edit->focus();
    w.frame();
    REQUIRE(edit->focused());
    box->setVisible(false);
    w.frame();
    CHECK(!edit->focused() && w.w->focusView() == nullptr);
    CHECK(outs == 1);
    // No caret blink left behind: an idle window paints nothing more.
    const int    frames = w.w->stats().frames;
    const double until  = app().nowMs() + 2.5 * std::max(1, app().settings().caretBlinkMs);
    while (app().nowMs() < until)
        app().pump(5);
    CHECK(w.w->stats().frames == frames);
    // Shown again, it is just a field without focus.
    box->setVisible(true);
    w.frame();
    CHECK(!edit->focused());

    // Taken out of the window: no focus, no flag left behind.
    edit->focus();
    REQUIRE(edit->focused());
    std::unique_ptr<ui::View> owned = w.root().remove(box);
    CHECK(!edit->focused() && w.w->focusView() == nullptr);

    // Destroyed while focused: the focus is just gone.
    w.root().adopt(std::move(owned));
    w.frame();
    edit->focus();
    REQUIRE(edit->focused());
    w.root().clearChildren();
    w.frame();
    CHECK(w.w->focusView() == nullptr);
}

// The unread count the shell sets (App::setBadgeCount) reaches the platform:
// a dock or launcher badge, on a port its tray item's count (PORT_CAP_BADGE),
// read back through the test hooks where the backend can.
TEST("platform: the badge count reaches the platform") {
    plat::App &p = app().platform();
    p.setBadgeCount(7);
    if (hooks().badgeCount() < 0) { // not readable here; the call didn't crash
        p.setBadgeCount(0);
        return;
    }
    CHECK(hooks().badgeCount() == 7);
    p.setBadgeCount(150); // more than the 99 a tray draws: the count, all the same
    CHECK(hooks().badgeCount() == 150);
    p.setBadgeCount(0);
    CHECK(hooks().badgeCount() == 0);
}

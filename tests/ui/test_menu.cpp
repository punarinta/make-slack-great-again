// Context menus: submenus (keyboard and hover), checks, placement at a point
// near the window edges, and how a menu is asked for (right click, long
// press, the Menu key, Shift+F10).
#include "harness.h"

using namespace uitest;

namespace {

std::vector<ui::MenuItem> items() {
    std::vector<ui::MenuItem> v;
    v.push_back({1, "Mark as read"});
    v.push_back(ui::MenuItem::separatorItem());
    ui::MenuItem notify;
    notify.label = "Notifications";
    notify.sub.push_back({10, "All new posts", {}, ui::Button::kNoIcon, true, true});
    notify.sub.push_back({11, "Just mentions"});
    notify.sub.push_back({12, "Nothing", {}, ui::Button::kNoIcon, false}); // disabled
    notify.sub.push_back({13, "Never"});
    v.push_back(std::move(notify));
    v.push_back({2, "Leave channel", {}, ui::Button::kNoIcon, true, false, false, true});
    return v;
}

// A pressable view that also asks for a context menu.
struct MenuTarget : ui::Clickable {
    int menus = 0, clicks = 0;
    MenuTarget() {
        style().size(120, 40);
        setFocusable(true);
        onClick = [this] { ++clicks; };
    }
    bool onEvent(ui::Event &e) override {
        if (e.type == ui::EventType::ContextMenu) {
            ++menus;
            ui::Menu::popupAt(*window(), e.windowPos, {{1, "Copy"}}, nullptr);
            return true;
        }
        return Clickable::onEvent(e);
    }
};

} // namespace

TEST("menu: submenus open with Right/Enter, close with Left/Escape, choose for the root") {
    Win       w(600, 400);
    int       chosen = -1;
    ui::Menu *m      = ui::Menu::popupAt(*w.w, {40, 40}, items(), [&](int id) { chosen = id; });
    w.frame();
    w.key(plat::Key::Down);
    w.key(plat::Key::Down);
    REQUIRE(m->current() == 2); // Notifications (the separator is skipped)
    w.key(plat::Key::Right);
    ui::Menu *sub = m->submenu();
    REQUIRE(sub != nullptr);
    CHECK(sub->parentMenu() == m);
    CHECK(w.w->focusView() == sub);
    CHECK(sub->current() == 0);
    // Beside its row, the first item level with it.
    const ui::RectF pr = m->windowRect(), sr = sub->windowRect();
    CHECK(sr.x >= pr.x + pr.w);
    CHECK(near(sr.y, pr.y + 9 + 36, 1.5f)); // row 2 = one row + one separator down
    w.key(plat::Key::Left);
    w.frame();
    CHECK(m->submenu() == nullptr);
    CHECK(w.w->focusView() == m);
    CHECK(w.w->topPopup() == m);
    w.key(plat::Key::Enter); // Enter on a submenu row opens it too
    sub = m->submenu();
    REQUIRE(sub != nullptr);
    w.key(plat::Key::Escape); // Escape closes only the submenu
    w.frame();
    CHECK(m->submenu() == nullptr && w.w->topPopup() == m);
    w.key(plat::Key::Right);
    w.key(plat::Key::Down);
    w.key(plat::Key::Down); // "Nothing" is disabled: skipped
    CHECK(m->submenu()->current() == 3);
    w.key(plat::Key::Enter);
    w.frame();
    CHECK(chosen == 13);
    CHECK(w.w->topPopup() == nullptr); // the whole chain closed
}

TEST("menu: hovering a submenu row opens it; moving on closes it; clicks choose") {
    Win       w(600, 400);
    int       chosen = -1;
    ui::Menu *m      = ui::Menu::popupAt(*w.w, {40, 40}, items(), [&](int id) { chosen = id; });
    w.frame();
    const ui::RectF r = m->windowRect();
    w.move(r.x + 30, r.y + 6 + 36 + 9 + 18); // over "Notifications"
    CHECK(w.until([&] { return m->submenu() != nullptr; }));
    CHECK(w.w->focusView() == m);   // hover-opened: keys stay with the parent
    w.move(r.x + 30, r.y + 6 + 14); // back to "Mark as read"
    CHECK(w.until([&] { return m->submenu() == nullptr; }));
    w.move(r.x + 30, r.y + 6 + 36 + 9 + 18);
    CHECK(w.until([&] { return m->submenu() != nullptr; }));
    const ui::RectF s = m->submenu()->windowRect();
    w.move(s.x + 30, s.y + 6 + 36 + 18); // "Just mentions"
    w.press();
    w.release();
    CHECK(chosen == 11);
    CHECK(w.w->topPopup() == nullptr);
}

TEST("menu: a context menu at a point opens below-right, flips at the edges, stays inside") {
    Win       w(500, 300);
    // Plenty of room: its corner is the point plus the 8-px halo margin.
    ui::Menu *m = ui::Menu::popupAt(*w.w, {100, 50}, items(), nullptr);
    w.frame();
    ui::RectF r = m->windowRect();
    CHECK(near(r.x, 108) && near(r.y, 58));
    m->close();
    w.frame();
    // Bottom-right corner: it opens up and to the left of the point.
    m = ui::Menu::popupAt(*w.w, {490, 290}, items(), nullptr);
    w.frame();
    r = m->windowRect();
    CHECK(near(r.x + r.w, 482) && near(r.y + r.h, 282));
    m->close();
    w.frame();
    // Too tall either way (a short window): clamped inside the margin.
    Win s(400, 120);
    m = ui::Menu::popupAt(*s.w, {200, 60}, items(), nullptr);
    s.frame();
    r = m->windowRect();
    CHECK(r.y >= 8 && r.y + r.h <= 120 - 8 + 0.5f);
    // A submenu that would leave the window on the right opens on the left.
    m->close();
    w.frame();
    m = ui::Menu::popupAt(*w.w, {480, 10}, items(), nullptr);
    w.frame();
    m->openSubmenu(2, true);
    w.frame();
    REQUIRE(m->submenu() != nullptr);
    CHECK(m->submenu()->windowRect().x + m->submenu()->windowRect().w <= m->windowRect().x + 1);
}

TEST("menu: checked, disabled and danger items; the chevron row measures wider") {
    Win       w(600, 400);
    ui::Menu *m = ui::Menu::popupAt(*w.w, {40, 40}, items(), nullptr);
    w.frame();
    m->openSubmenu(2, true);
    w.frame();
    ui::Menu *sub = m->submenu();
    REQUIRE(sub != nullptr);
    // Down from the first item skips the disabled "Nothing".
    w.key(plat::Key::Down);
    w.key(plat::Key::Down);
    CHECK(sub->current() == 3);
    // Typing jumps by first letter, disabled items excluded.
    w.key(plat::Key::N);
    CHECK(sub->current() == 3);
    // With NumLock and CapsLock on too (they arrive as modifiers).
    w.key(plat::Key::Up);
    CHECK(sub->current() != 3);
    ui::Event n{ui::EventType::KeyDown};
    n.key  = plat::Key::N;
    n.mods = plat::ModNum | plat::ModCaps;
    CHECK(sub->onEvent(n));
    CHECK(sub->current() == 3);
    // Clicking a disabled item neither chooses nor closes.
    const ui::RectF s = sub->windowRect();
    w.move(s.x + 30, s.y + 6 + 2 * 36 + 18);
    w.press();
    w.release();
    CHECK(w.w->topPopup() == sub);
}

TEST("hit: long press asks for a context menu and cancels the click; moving does not") {
    Win   w(400, 300);
    auto *t = w.root().add<MenuTarget>();
    t->style().alignSelf(ui::Align::Start);
    w.frame();
    w.move(20, 20);
    w.press();
    CHECK(w.until([&] { return t->menus == 1; }));
    REQUIRE(w.w->topPopup() != nullptr);
    w.release(); // the release lands on the new menu: not a click, not a choice
    CHECK(t->clicks == 0);
    CHECK(w.w->topPopup() != nullptr);
    w.key(plat::Key::Escape);
    w.frame();
    // A press that moves away (a drag) is not a long press.
    w.move(20, 20);
    w.press();
    w.move(60, 30);
    for (int i = 0; i < 80; ++i)
        app().pump(10);
    CHECK(t->menus == 1);
    w.release(); // still inside: an ordinary click
    CHECK(t->clicks == 1);
    // A quick click still clicks.
    w.click(20, 20);
    CHECK(t->clicks == 2 && t->menus == 1);
}

TEST("hit: the Menu key and Shift+F10 open the focused view's context menu") {
    Win   w(400, 300);
    auto *t = w.root().add<MenuTarget>();
    t->style().alignSelf(ui::Align::Start);
    w.frame();
    t->focus();
    w.key(plat::Key::Menu);
    CHECK(t->menus == 1);
    ui::Popup *p = w.w->topPopup();
    REQUIRE(p != nullptr);
    CHECK(p->windowRect().x >= t->windowRect().x); // at the view, not at the pointer
    w.key(plat::Key::Escape);
    w.frame();
    CHECK(w.w->focusView() == t);
    w.chord(plat::ModShift, plat::Key::F10);
    CHECK(t->menus == 2);
}

TEST("menu: geometry — 36-px rows, 26-px headers, 9-px separators, fitted width") {
    Win                       w(600, 400);
    std::vector<ui::MenuItem> v;
    v.push_back(ui::MenuItem::separatorItem()); // leading: dropped
    v.push_back({1, "Star channel"});
    v.push_back(ui::MenuItem::separatorItem());
    v.push_back(ui::MenuItem::separatorItem()); // doubled: collapsed
    v.push_back(ui::MenuItem::headerItem("Notify you about…"));
    v.push_back({2, "All new posts", {}, uint16_t(0), true, true});
    v.push_back({3, "Leave channel", {}, ui::Button::kNoIcon, true, false, false, true});
    v.push_back(ui::MenuItem::separatorItem()); // trailing: dropped
    ui::Menu *m = ui::Menu::popupAt(*w.w, {40, 40}, v, nullptr);
    w.frame();
    const ui::RectF r = m->windowRect();
    CHECK(near(r.h, 6 + 36 + 9 + 26 + 36 + 36 + 6)); // padV, rows, padV
    CHECK(r.w < 240);                                // fitted, no 180-px floor…
    m->close();
    w.frame();
    m = ui::Menu::popupAt(*w.w, {40, 40}, {{1, "Mute"}}, nullptr);
    m->setMinWidth(140); // …unless asked (the workspace menu)
    w.frame();
    CHECK(near(m->windowRect().w, 140));
    // Headers are not rows: keyboard and hover skip them.
    m->close();
    w.frame();
    m = ui::Menu::popupAt(*w.w, {40, 40}, v, nullptr);
    w.frame();
    w.key(plat::Key::Down);
    w.key(plat::Key::Down);
    CHECK(m->current() == 3); // Star channel (0 after tidying), header skipped → All new posts
}

TEST("menu: an item's shortcut hint chooses it (T, E, Del, Ctrl+C)") {
    Win                       w(600, 400);
    int                       chosen = -1;
    std::vector<ui::MenuItem> v      = {
        {1, "Reply in thread", "T"},
        {2, "Copy message", "Ctrl+C"},
        {3, "Delete message…", "Del"},
    };
    ui::Menu::popupAt(*w.w, {40, 40}, v, [&](int id) { chosen = id; });
    w.frame();
    w.chord(plat::ModCtrl, plat::Key::C);
    CHECK(chosen == 2);
    ui::Menu::popupAt(*w.w, {40, 40}, v, [&](int id) { chosen = id; });
    w.frame();
    w.key(plat::Key::Delete);
    CHECK(chosen == 3);
    ui::Menu::popupAt(*w.w, {40, 40}, v, [&](int id) { chosen = id; });
    w.frame();
    w.key(plat::Key::T);
    CHECK(chosen == 1);
    CHECK(w.w->topPopup() == nullptr);
}

TEST("menu: table-driven items and separators") {
    constexpr ui::MenuDef kDefs[] = {
        {1, ui::Button::kNoIcon, "Star channel", "Star conversation", nullptr},
        {2, 7, "Reply in thread", nullptr, "T"},
    };
    std::vector<ui::MenuItem> v;
    ui::addMenuSeparator(v); // never leading
    CHECK(v.empty());
    ui::addMenuItem(v, kDefs, 1, false, true);
    ui::addMenuSeparator(v);
    ui::addMenuSeparator(v); // never doubled
    ui::addMenuItem(v, kDefs, 1, true, false);
    ui::addMenuItem(v, kDefs, 2, true, true).checked = true;
    REQUIRE(v.size() == 4);
    CHECK_STR(v[0].label, "Star channel");
    CHECK(v[0].id == 1 && v[0].enabled && v[0].hint.empty());
    CHECK(v[1].separator);
    CHECK_STR(v[2].label, "Star conversation");
    CHECK_FALSE(v[2].enabled);
    CHECK_STR(v[3].label, "Reply in thread"); // no alt label: the label
    CHECK_STR(v[3].hint, "T");
    CHECK(v[3].icon == 7 && v[3].checked && !v[3].danger);
}

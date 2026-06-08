// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for the ContextMenu extensions added in the context-menu session:
//   - addHeader(): non-clickable section labels
//   - addItem() with selected=true: checkmark + accent color
//   - Layout: per-item heights add up correctly
//   - Behavioral: clicking a header fires nothing; clicking an item fires its action
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QPixmap>
#include <QTest>

#include "ui/context_menu/context_menu.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-context-menu");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();
    return Catch::Session().run(argc, argv);
}

// ── Layout constants (mirror of ContextMenu's private section) ────────────────
// These must match the constexpr values defined in context_menu.h.
static constexpr int kShadow  = 8;
static constexpr int kPadV    = 6;
static constexpr int kItemH   = 36;
static constexpr int kHeaderH = 26;
static constexpr int kSepH    = 9;
static constexpr int kMinW    = 200;

// 'I' = normal item, 'H' = header, 'S' = separator
static int itemCenterY(const std::vector<char> &types, int idx) {
    int y = kShadow + kPadV;
    for (int j = 0; j < idx; ++j) {
        if (types[j] == 'H')
            y += kHeaderH;
        else if (types[j] == 'S')
            y += kSepH;
        else
            y += kItemH;
    }
    if (types[idx] == 'H')
        return y + kHeaderH / 2;
    else if (types[idx] == 'S')
        return y + kSepH / 2;
    else
        return y + kItemH / 2;
}

static int expectedHeight(const std::vector<char> &types) {
    int total = 0;
    for (char t : types) {
        if (t == 'H')
            total += kHeaderH;
        else if (t == 'S')
            total += kSepH;
        else
            total += kItemH;
    }
    return 2 * kShadow + 2 * kPadV + total;
}

// Creates a ContextMenu with WA_DeleteOnClose disabled so tests can query
// it after an action fires (which calls close() internally).
static ContextMenu *makeMenu() {
    auto *m = new ContextMenu();
    m->setAttribute(Qt::WA_DeleteOnClose, false);
    return m;
}

// Shows the menu and waits for the geometry to be set.
static void showMenu(ContextMenu *m, QPoint pos = QPoint(400, 400)) {
    m->popup(pos);
    QApplication::processEvents();
}

static void click(ContextMenu *m, QPoint pos) {
    QTest::mousePress(m, Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseRelease(m, Qt::LeftButton, Qt::NoModifier, pos);
    QApplication::processEvents();
}

// ── Height invariants ─────────────────────────────────────────────────────────

TEST_CASE("ContextMenu height: single item", "[context_menu][layout]") {
    auto *m = makeMenu();
    m->addItem("Action", [] {});
    showMenu(m);
    CHECK(m->height() == expectedHeight({'I'}));
    delete m;
}

TEST_CASE("ContextMenu height: header + item", "[context_menu][layout]") {
    auto *m = makeMenu();
    m->addHeader("Section");
    m->addItem("Action", [] {});
    showMenu(m);
    CHECK(m->height() == expectedHeight({'H', 'I'}));
    delete m;
}

TEST_CASE("ContextMenu height: header + item + separator + item", "[context_menu][layout]") {
    auto *m = makeMenu();
    m->addHeader("Section");
    m->addItem("First", [] {});
    m->addSeparator();
    m->addItem("Second", [] {});
    showMenu(m);
    CHECK(m->height() == expectedHeight({'H', 'I', 'S', 'I'}));
    delete m;
}

// ── Render without crash ──────────────────────────────────────────────────────

TEST_CASE("ContextMenu renders channel context menu without crash", "[context_menu][render]") {
    auto *m = makeMenu();
    m->addItem("Star channel", [] {});
    m->addSeparator();
    m->addHeader("Notify you about\xe2\x80\xa6"); // "…" as UTF-8
    m->addItem("All new posts", [] {}, false, ":/ui/bell.svg", false);
    m->addItem("Just mentions", [] {}, false, ":/ui/bell.svg", true);
    m->addItem("Mute and hide", [] {}, false, ":/ui/bell-off.svg", false);
    m->addSeparator();
    m->addItem("Leave channel", [] {}, /*destructive=*/true, ":/ui/log-out.svg");
    showMenu(m);
    QPixmap px(m->size());
    px.fill(Qt::transparent);
    m->render(&px);
    CHECK(!px.isNull());
    delete m;
}

TEST_CASE("ContextMenu renders MPDM context menu without crash", "[context_menu][render]") {
    auto *m = makeMenu();
    m->addItem("Star conversation", [] {});
    m->addSeparator();
    m->addHeader("Notify you about\xe2\x80\xa6");
    m->addItem("All new posts", [] {}, false, ":/ui/bell.svg", false);
    m->addItem("Just mentions", [] {}, false, ":/ui/bell.svg", false);
    m->addItem("Mute and hide", [] {}, false, ":/ui/bell-off.svg", true);
    m->addSeparator();
    m->addItem("Leave conversation", [] {}, /*destructive=*/true, ":/ui/log-out.svg");
    showMenu(m);
    QPixmap px(m->size());
    px.fill(Qt::transparent);
    m->render(&px);
    CHECK(!px.isNull());
    delete m;
}

// ── Header is not clickable ───────────────────────────────────────────────────

TEST_CASE("ContextMenu: clicking a header row fires no action", "[context_menu][header]") {
    auto *m     = makeMenu();
    bool  fired = false;
    m->addHeader("Section Header");
    m->addItem("Normal Item", [&fired] { fired = true; });
    showMenu(m);

    const int cx = kShadow + kMinW / 2;
    click(m, QPoint(cx, itemCenterY({'H', 'I'}, 0))); // click on header

    CHECK(!fired);
    delete m;
}

// ── Normal item fires its action ──────────────────────────────────────────────

TEST_CASE("ContextMenu: clicking a normal item fires its action", "[context_menu][item]") {
    auto *m     = makeMenu();
    bool  fired = false;
    m->addItem("Do Something", [&fired] { fired = true; });
    showMenu(m);

    const int cx = kShadow + kMinW / 2;
    click(m, QPoint(cx, itemCenterY({'I'}, 0)));

    CHECK(fired);
    delete m;
}

TEST_CASE(
    "ContextMenu: clicking below a header fires the item beneath it", "[context_menu][item]"
) {
    auto *m     = makeMenu();
    bool  fired = false;
    m->addHeader("Section");
    m->addItem("Action", [&fired] { fired = true; });
    showMenu(m);

    const int cx = kShadow + kMinW / 2;
    click(m, QPoint(cx, itemCenterY({'H', 'I'}, 1))); // click on item (index 1)

    CHECK(fired);
    delete m;
}

// ── Selected item still fires its action ─────────────────────────────────────

TEST_CASE(
    "ContextMenu: clicking a selected item fires its action", "[context_menu][item][selected]"
) {
    auto *m     = makeMenu();
    bool  fired = false;
    m->addItem("Active Level", [&fired] { fired = true; }, false, "", /*selected=*/true);
    showMenu(m);

    const int cx = kShadow + kMinW / 2;
    click(m, QPoint(cx, itemCenterY({'I'}, 0)));

    CHECK(fired);
    delete m;
}

TEST_CASE(
    "ContextMenu: only the clicked item fires when multiple items present", "[context_menu][item]"
) {
    auto *m        = makeMenu();
    int   firedIdx = -1;
    m->addItem("First", [&firedIdx] { firedIdx = 0; });
    m->addItem("Second", [&firedIdx] { firedIdx = 1; });
    m->addItem("Third", [&firedIdx] { firedIdx = 2; });
    showMenu(m);

    const int cx = kShadow + kMinW / 2;
    click(m, QPoint(cx, itemCenterY({'I', 'I', 'I'}, 1))); // click second item

    CHECK(firedIdx == 1);
    delete m;
}

// ── Destructive item fires its action ─────────────────────────────────────────

TEST_CASE(
    "ContextMenu: clicking a destructive item fires its action", "[context_menu][item][destructive]"
) {
    auto *m     = makeMenu();
    bool  fired = false;
    m->addItem("Leave channel", [&fired] { fired = true; }, /*destructive=*/true);
    showMenu(m);

    const int cx = kShadow + kMinW / 2;
    click(m, QPoint(cx, itemCenterY({'I'}, 0)));

    CHECK(fired);
    delete m;
}

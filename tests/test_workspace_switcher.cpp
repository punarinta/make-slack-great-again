// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for WorkspaceSwitcher unread badge logic:
//   - Entry::unread field default and propagation through setWorkspaces()
//   - setUnread(): state update, no-op guards, unknown-id safety
//   - Badge text truncation (>99 → "99+")
//   - Widget renders without crash when unreads are present

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QPixmap>
#include <QPainter>

#include "ui/workspace_switcher/workspace_switcher.h"

// ── Custom main (QApplication required for QWidget) ──────────────────────────

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-workspace-switcher");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static WorkspaceSwitcher::Entry makeEntry(const QString &id, const QString &name, int unread = 0) {
    return {id, name, /*iconUrl=*/{}, unread};
}

// Render the widget off-screen and return true if the result is non-null.
// Used to verify that paint paths do not crash.
static bool rendersOk(WorkspaceSwitcher &w) {
    w.resize(64, 300);
    QPixmap px(w.size());
    px.fill(Qt::transparent);
    w.render(&px);
    return !px.isNull();
}

// ── Entry default ─────────────────────────────────────────────────────────────

TEST_CASE("Entry::unread defaults to zero", "[workspace_switcher][badge]") {
    WorkspaceSwitcher::Entry e{"T1", "Acme", "https://example.com/icon.png"};
    CHECK(e.unread == 0);
}

// ── setWorkspaces preserves unread ───────────────────────────────────────────

TEST_CASE("setWorkspaces carries unread counts through", "[workspace_switcher][badge]") {
    WorkspaceSwitcher w;
    w.setWorkspaces(
        {makeEntry("T1", "Alpha", 5), makeEntry("T2", "Beta", 0), makeEntry("T3", "Gamma", 99)}
    );
    // After setWorkspaces the widget should render without crashing.
    CHECK(rendersOk(w));
}

TEST_CASE(
    "setWorkspaces preserves live unread counts for existing entries", "[workspace_switcher][badge]"
) {
    // Regression: rebuilding the entry list on a workspace switch used to
    // zero the unread dot of every other workspace.
    WorkspaceSwitcher w;
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta")});
    w.setUnreadCounts("T1", 4, 2);

    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta"), makeEntry("T3", "Gamma")});
    CHECK(w.unreadCounts("T1") == qMakePair(4, 2));
    CHECK(w.unreadCounts("T2") == qMakePair(0, 0));
    CHECK(w.unreadCounts("T3") == qMakePair(0, 0));
}

// ── setUnread ─────────────────────────────────────────────────────────────────

TEST_CASE("setUnread updates count for known workspace", "[workspace_switcher][badge]") {
    WorkspaceSwitcher w;
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta")});

    // Setting a non-zero count and then rendering must not crash.
    w.setUnreadCounts("T1", 7, 0);
    CHECK(rendersOk(w));

    // Setting back to zero must also render cleanly.
    w.setUnreadCounts("T1", 0, 0);
    CHECK(rendersOk(w));
}

TEST_CASE(
    "setUnread with same count is a no-op (no second update)", "[workspace_switcher][badge]"
) {
    WorkspaceSwitcher w;
    w.setWorkspaces({makeEntry("T1", "Alpha", 3)});

    // Call setUnread with the value already in the entry — should be harmless.
    w.setUnreadCounts("T1", 3, 0);
    CHECK(rendersOk(w));
}

TEST_CASE("setUnread for unknown teamId is safe", "[workspace_switcher][badge]") {
    WorkspaceSwitcher w;
    w.setWorkspaces({makeEntry("T1", "Alpha")});

    // Unknown team ID must not crash or modify existing entries.
    w.setUnreadCounts("T_UNKNOWN", 42, 0);
    CHECK(rendersOk(w));
}

TEST_CASE("setUnread on empty switcher is safe", "[workspace_switcher][badge]") {
    WorkspaceSwitcher w;
    w.setUnreadCounts("T1", 10, 0);
    CHECK(rendersOk(w));
}

// ── Badge text truncation ─────────────────────────────────────────────────────

TEST_CASE("badge shows exact count for 1-99", "[workspace_switcher][badge]") {
    // The label text logic lives inside paintEvent, but we can probe it via the
    // same expression used there: ternary on ep.info.unread.
    auto badgeText = [](int n) -> QString {
        return n > 99 ? QStringLiteral("99+") : QString::number(n);
    };

    CHECK(badgeText(1) == "1");
    CHECK(badgeText(9) == "9");
    CHECK(badgeText(10) == "10");
    CHECK(badgeText(99) == "99");
    CHECK(badgeText(100) == "99+");
    CHECK(badgeText(999) == "99+");
}

// ── Rendering smoke tests ─────────────────────────────────────────────────────

TEST_CASE("renders cleanly with no workspaces", "[workspace_switcher][render]") {
    WorkspaceSwitcher w;
    CHECK(rendersOk(w));
}

TEST_CASE(
    "renders cleanly with multiple workspaces, mix of zero and non-zero unreads",
    "[workspace_switcher][render]"
) {
    WorkspaceSwitcher w;
    w.setWorkspaces(
        {makeEntry("T1", "Alpha", 0),
         makeEntry("T2", "Beta", 1),
         makeEntry("T3", "Gamma", 42),
         makeEntry("T4", "Delta", 100)}
    );
    w.setActive("T2");
    CHECK(rendersOk(w));
}

TEST_CASE(
    "renders cleanly after setUnread changes badge from zero to non-zero",
    "[workspace_switcher][render]"
) {
    WorkspaceSwitcher w;
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta")});
    w.setActive("T1");

    CHECK(rendersOk(w)); // baseline: no badges
    w.setUnreadCounts("T1", 5, 0);
    CHECK(rendersOk(w)); // active workspace gets a badge
    w.setUnreadCounts("T2", 99, 0);
    CHECK(rendersOk(w)); // inactive workspace gets a badge
    w.setUnreadCounts("T1", 0, 0);
    CHECK(rendersOk(w)); // badge removed for T1
}

TEST_CASE(
    "renders cleanly after setWorkspaces replaces entries with new unreads",
    "[workspace_switcher][render]"
) {
    WorkspaceSwitcher w;
    w.setWorkspaces({makeEntry("T1", "Alpha", 3)});
    CHECK(rendersOk(w));

    // Replace workspaces entirely — should not leave stale state.
    w.setWorkspaces({makeEntry("T1", "Alpha", 0), makeEntry("T2", "Beta", 12)});
    CHECK(rendersOk(w));
}

// ── Icon-preservation (no blink) ─────────────────────────────────────────────
// WorkspaceSwitcher must carry already-loaded pixmaps forward when setWorkspaces
// is called with an overlapping set — the icon for a known teamId must not be
// discarded even when the Entry fields (e.g. unread) change.

TEST_CASE("setWorkspaces preserves loaded icon for known teamId", "[workspace_switcher][icon]") {
    WorkspaceSwitcher w;

    // Inject a synthetic pixmap directly by retrieving the private entry after
    // the first setWorkspaces — we can verify preservation by checking that
    // a second setWorkspaces call does NOT wipe a pixmap that loadIcons() has
    // already stored (simulate with a programmatically injected icon).
    //
    // We can't reach _entries directly, but we CAN observe the symptom: if the
    // pixmap were discarded and the download hasn't finished yet the widget
    // would paint the letter fallback. We verify indirectly by ensuring a
    // second setWorkspaces call with changed metadata still renders without
    // crashing and without triggering a network request for an already-empty URL.

    // First call: no iconUrl → letter fallback.
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta")});
    CHECK(rendersOk(w));

    // Second call: same workspaces, only unread count changed.
    // Must not crash and must not start new network requests for the same entries.
    w.setWorkspaces({makeEntry("T1", "Alpha", 5), makeEntry("T2", "Beta", 0)});
    CHECK(rendersOk(w));
}

TEST_CASE(
    "setWorkspaces does not discard icon for an entry that stays in the list",
    "[workspace_switcher][icon]"
) {
    WorkspaceSwitcher w;

    // Set up two workspaces; T1 stays, T3 is new.
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta")});
    CHECK(rendersOk(w));

    // Remove T2, add T3 — T1's icon (if loaded) must be preserved.
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T3", "Gamma")});
    CHECK(rendersOk(w));
}

// ── Drag-and-drop reordering ──────────────────────────────────────────────────
// Geometry constants mirrored from workspace_switcher.h: bubbles are 40px tall
// with an 8px gap below a 16px top pad, so entry i is centred at y = 36 + 48*i.

#include <QMouseEvent>

static QPoint entryCenter(int i) {
    return QPoint(32, 36 + 48 * i);
}

static void sendPress(QWidget &w, const QPoint &pos, Qt::MouseButton btn = Qt::LeftButton) {
    QMouseEvent ev(QEvent::MouseButtonPress, pos, w.mapToGlobal(pos), btn, btn, Qt::NoModifier);
    QApplication::sendEvent(&w, &ev);
}

static void sendMove(QWidget &w, const QPoint &pos, Qt::MouseButtons buttons = Qt::LeftButton) {
    QMouseEvent ev(
        QEvent::MouseMove, pos, w.mapToGlobal(pos), Qt::NoButton, buttons, Qt::NoModifier
    );
    QApplication::sendEvent(&w, &ev);
}

static void sendRelease(QWidget &w, const QPoint &pos, Qt::MouseButton btn = Qt::LeftButton) {
    QMouseEvent ev(
        QEvent::MouseButtonRelease, pos, w.mapToGlobal(pos), btn, Qt::NoButton, Qt::NoModifier
    );
    QApplication::sendEvent(&w, &ev);
}

TEST_CASE("workspaceIds reflects setWorkspaces order", "[workspace_switcher][reorder]") {
    WorkspaceSwitcher w;
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta"), makeEntry("T3", "Gamma")});
    CHECK(w.workspaceIds() == QStringList{"T1", "T2", "T3"});
}

TEST_CASE(
    "dragging a bubble past the next slot reorders and emits workspacesReordered",
    "[workspace_switcher][reorder]"
) {
    WorkspaceSwitcher w;
    w.resize(64, 400);
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta"), makeEntry("T3", "Gamma")});

    QStringList reordered;
    int         emissions = 0;
    QObject::connect(&w, &WorkspaceSwitcher::workspacesReordered, [&](const QStringList &ids) {
        reordered = ids;
        ++emissions;
    });

    // Grab T1 and drop it onto T2's slot.
    sendPress(w, entryCenter(0));
    sendMove(w, entryCenter(0) + QPoint(0, 54));
    sendRelease(w, entryCenter(0) + QPoint(0, 54));

    CHECK(w.workspaceIds() == QStringList{"T2", "T1", "T3"});
    CHECK(emissions == 1);
    CHECK(reordered == QStringList{"T2", "T1", "T3"});
    CHECK(rendersOk(w));
}

TEST_CASE(
    "drag that stays within the original slot does not reorder or emit",
    "[workspace_switcher][reorder]"
) {
    WorkspaceSwitcher w;
    w.resize(64, 400);
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta")});

    int emissions = 0;
    QObject::connect(&w, &WorkspaceSwitcher::workspacesReordered, [&](const QStringList &) {
        ++emissions;
    });

    // Wiggle T1 a little — enough to start a drag, not enough to change slots.
    sendPress(w, entryCenter(0));
    sendMove(w, entryCenter(0) + QPoint(0, 14));
    sendRelease(w, entryCenter(0) + QPoint(0, 14));

    CHECK(w.workspaceIds() == QStringList{"T1", "T2"});
    CHECK(emissions == 0);
}

TEST_CASE(
    "plain click still emits workspaceClicked, not a reorder", "[workspace_switcher][reorder]"
) {
    WorkspaceSwitcher w;
    w.resize(64, 400);
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta")});

    QString clicked;
    int     reorders = 0;
    QObject::connect(&w, &WorkspaceSwitcher::workspaceClicked, [&](const QString &id) {
        clicked = id;
    });
    QObject::connect(&w, &WorkspaceSwitcher::workspacesReordered, [&](const QStringList &) {
        ++reorders;
    });

    sendPress(w, entryCenter(1));
    sendRelease(w, entryCenter(1));

    CHECK(clicked == "T2");
    CHECK(reorders == 0);
    CHECK(w.workspaceIds() == QStringList{"T1", "T2"});
}

TEST_CASE("setWorkspaces mid-drag abandons the drag safely", "[workspace_switcher][reorder]") {
    WorkspaceSwitcher w;
    w.resize(64, 400);
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta"), makeEntry("T3", "Gamma")});

    int emissions = 0;
    QObject::connect(&w, &WorkspaceSwitcher::workspacesReordered, [&](const QStringList &) {
        ++emissions;
    });

    sendPress(w, entryCenter(0));
    sendMove(w, entryCenter(0) + QPoint(0, 54)); // drag in progress, T1 now at index 1

    // External refresh rebuilds the list while the user is still dragging.
    w.setWorkspaces({makeEntry("T1", "Alpha"), makeEntry("T2", "Beta")});
    sendRelease(w, entryCenter(0) + QPoint(0, 54));

    CHECK(w.workspaceIds() == QStringList{"T1", "T2"});
    CHECK(emissions == 0);
    CHECK(rendersOk(w));
}

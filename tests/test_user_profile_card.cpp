// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for UserProfileCard (mention-hover profile card):
//   - showFor() sizes and shows the card; grab() paints without crashing
//   - optional rows (role header, status, title, clock) change the height
//   - scheduleHide()/cancelHide()/hideNow() behavior
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QPixmap>
#include <QTest>

#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/user_profile_card/user_profile_card.h"

int main(int argc, char **argv) {
    // Parentless Qt::ToolTip windows can't map on Wayland (no transient parent)
    // and get hidden asynchronously — run offscreen for deterministic visibility.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-user-profile-card");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();
    return Catch::Session().run(argc, argv);
}

static User plainUser() {
    User u;
    u.id          = UserId{"U1"};
    u.name        = "stefan";
    u.displayName = "Stefan Möller";
    return u;
}

static const QRect kTarget(400, 400, 60, 18);

TEST_CASE("showFor sizes, shows and paints the card", "[profile-card]") {
    UserProfileCard card;
    card.showFor(plainUser(), QPixmap(), kTarget);

    CHECK(card.isVisible());
    CHECK(card.width() > 0);
    CHECK(card.height() > 0);
    CHECK(card.userId() == UserId{"U1"});

    const QPixmap rendered = card.grab();
    CHECK(!rendered.isNull());
    card.hideNow();
}

TEST_CASE("optional rows extend the card height", "[profile-card]") {
    UserProfileCard card;

    card.showFor(plainUser(), QPixmap(), kTarget);
    const int plainH = card.height();

    User owner    = plainUser();
    owner.isOwner = true;
    card.showFor(owner, QPixmap(), kTarget);
    const int ownerH = card.height();
    CHECK(ownerH > plainH); // role header strip added

    User full        = owner;
    full.title       = "CTO";
    full.statusText  = "On vacation";
    full.statusEmoji = "palm_tree";
    full.hasTz       = true;
    full.tzOffset    = 7200;
    card.showFor(full, QPixmap(), kTarget);
    CHECK(card.height() > ownerH); // status + title + local-time rows added
    CHECK(!card.grab().isNull());
    card.hideNow();
}

TEST_CASE("deactivated account hides the Message button", "[profile-card]") {
    UserProfileCard card;

    User gone          = plainUser();
    gone.isDeactivated = true;
    card.showFor(gone, QPixmap(), kTarget);
    const int deactivatedH = card.height();

    card.showFor(plainUser(), QPixmap(), kTarget);
    CHECK(card.height() > deactivatedH); // button row present for active accounts
    card.hideNow();
}

TEST_CASE("scheduleHide hides after the grace period; hideNow is immediate", "[profile-card]") {
    UserProfileCard card;
    card.showFor(plainUser(), QPixmap(), kTarget);
    CHECK(card.isVisible());

    card.scheduleHide();
    CHECK(card.isVisible()); // not hidden synchronously
    QTest::qWait(400);       // > kHideDelay (260 ms)
    CHECK(!card.isVisible());

    card.showFor(plainUser(), QPixmap(), kTarget);
    card.scheduleHide();
    card.cancelHide();
    QTest::qWait(400);
    CHECK(card.isVisible()); // cancel kept it open

    card.hideNow();
    CHECK(!card.isVisible());
}

TEST_CASE("setActive and updateAvatar repaint without crashing", "[profile-card]") {
    UserProfileCard card;
    card.showFor(plainUser(), QPixmap(), kTarget);

    card.setActive(true);
    CHECK(!card.grab().isNull());

    QPixmap avatar(72, 72);
    avatar.fill(Qt::darkCyan);
    card.updateAvatar(avatar);
    CHECK(!card.grab().isNull());
    card.hideNow();
}

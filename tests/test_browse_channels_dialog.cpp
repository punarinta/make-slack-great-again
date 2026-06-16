// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Tests for BrowseChannelsDialog (virtualized list rewrite):
//   - Construction smoke test
//   - Channel list populated from conversations (channels only, no DMs)
//   - People list populated from users (deactivated users excluded)
//   - Filter by name and description, case-insensitive; clear restores all
//   - Tab switching via tab buttons changes QStackedWidget index
//   - createChannelRequested signal fires on Create Channel button click
//   - channelActivated signal fires with the correct id on row activation
//   - userActivated signal fires with the correct id on row activation
//
// Population/filter assertions go through BrowseListView's count()/visibleCount()
// accessors, and activation through its onActivated hook (the same hook the
// virtual list invokes on a row click) — neither depends on widget geometry, so
// the tests run deterministically headless.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "backend/domain.h"
#include "ui/browse_channels_dialog/browse_channels_dialog.h"
#include "ui/browse_channels_dialog/browse_list_view.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-browse-channels");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();

    static QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

// ── Test data ──────────────────────────────────────────────────────────────────

static const Conversation kGeneral = {
    .id          = ConversationId{"C1"},
    .kind        = ConvKind::PublicChannel,
    .name        = "general",
    .description = "Company-wide updates",
    .isMember    = true,
    .memberCount = 20,
};
static const Conversation kRandom = {
    .id          = ConversationId{"C2"},
    .kind        = ConvKind::PublicChannel,
    .name        = "random",
    .description = "Off-topic chat",
    .isMember    = false,
    .memberCount = 10,
};
static const Conversation kSecret = {
    .id       = ConversationId{"G1"},
    .kind     = ConvKind::PrivateChannel,
    .name     = "secret",
    .isMember = true,
};
static const Conversation kDm = {
    .id     = ConversationId{"D1"},
    .kind   = ConvKind::Im,
    .dmUser = UserId{"U2"},
};
static const User kAlice = {UserId{"U1"}, "alice", "Alice Wonder", {}, false, false, false};
static const User kBob   = {UserId{"U2"}, "bob", "Bob Builder", {}, false, true, false};

// ── Helpers ────────────────────────────────────────────────────────────────────

static BrowseListView *channelList(QWidget *dlg) {
    return dlg->findChild<BrowseListView *>("browseChannelList");
}
static BrowseListView *peopleList(QWidget *dlg) {
    return dlg->findChild<BrowseListView *>("browsePeopleList");
}

// ── Construction ───────────────────────────────────────────────────────────────

TEST_CASE("BrowseChannelsDialog: constructs without crash", "[browse][smoke]") {
    BrowseChannelsDialog dlg({kGeneral}, {kAlice}, nullptr);
    CHECK(dlg.findChild<QLineEdit *>() != nullptr);
    CHECK(dlg.findChild<QStackedWidget *>() != nullptr);
    CHECK(dlg.findChildren<BrowseListView *>().size() == 2);
}

TEST_CASE("BrowseChannelsDialog: renders without crash", "[browse][smoke]") {
    // Includes a joined channel (kGeneral) and a person to exercise both
    // paint paths (channel "Joined" badge + people avatar).
    BrowseChannelsDialog dlg({kGeneral, kRandom, kSecret}, {kAlice, kBob}, nullptr);
    dlg.resize(800, 600);
    QPixmap px(dlg.size());
    px.fill(Qt::transparent);
    dlg.render(&px);
    CHECK(!px.isNull());
}

// ── Channel list population ────────────────────────────────────────────────────

TEST_CASE(
    "BrowseChannelsDialog: public and private channels appear in channel list", "[browse][channels]"
) {
    BrowseChannelsDialog dlg({kGeneral, kRandom, kSecret, kDm}, {kAlice}, nullptr);
    REQUIRE(channelList(&dlg) != nullptr);
    CHECK(channelList(&dlg)->count() == 3); // general, random, secret — not the DM
}

TEST_CASE("BrowseChannelsDialog: DMs are excluded from channel list", "[browse][channels]") {
    BrowseChannelsDialog dlg({kDm}, {kAlice}, nullptr);
    REQUIRE(channelList(&dlg) != nullptr);
    CHECK(channelList(&dlg)->count() == 0);
}

// ── People list population ─────────────────────────────────────────────────────

TEST_CASE("BrowseChannelsDialog: non-deactivated users appear in people list", "[browse][people]") {
    BrowseChannelsDialog dlg({kGeneral}, {kAlice, kBob}, nullptr);
    REQUIRE(peopleList(&dlg) != nullptr);
    CHECK(peopleList(&dlg)->count() == 2);
}

TEST_CASE(
    "BrowseChannelsDialog: deactivated users are excluded from people list", "[browse][people]"
) {
    User ghost          = kAlice;
    ghost.id            = UserId{"U99"};
    ghost.isDeactivated = true;
    BrowseChannelsDialog dlg({kGeneral}, {kAlice, ghost}, nullptr);
    REQUIRE(peopleList(&dlg) != nullptr);
    CHECK(peopleList(&dlg)->count() == 1);
}

// ── Filtering — channels ───────────────────────────────────────────────────────

TEST_CASE("BrowseChannelsDialog: filter by name hides non-matching channels", "[browse][filter]") {
    BrowseChannelsDialog dlg({kGeneral, kRandom, kSecret}, {kAlice}, nullptr);
    dlg.findChild<QLineEdit *>()->setText("general");
    CHECK(channelList(&dlg)->visibleCount() == 1);
}

TEST_CASE("BrowseChannelsDialog: channel filter is case-insensitive", "[browse][filter]") {
    BrowseChannelsDialog dlg({kGeneral, kRandom, kSecret}, {kAlice}, nullptr);
    dlg.findChild<QLineEdit *>()->setText("RANDOM");
    CHECK(channelList(&dlg)->visibleCount() == 1);
}

TEST_CASE("BrowseChannelsDialog: filter matches channel description", "[browse][filter]") {
    // kRandom.description = "Off-topic chat"
    BrowseChannelsDialog dlg({kGeneral, kRandom, kSecret}, {kAlice}, nullptr);
    dlg.findChild<QLineEdit *>()->setText("off-topic");
    CHECK(channelList(&dlg)->visibleCount() == 1);
}

TEST_CASE(
    "BrowseChannelsDialog: clearing channel filter restores all channels", "[browse][filter]"
) {
    BrowseChannelsDialog dlg({kGeneral, kRandom, kSecret}, {kAlice}, nullptr);
    auto                *ed = dlg.findChild<QLineEdit *>();
    ed->setText("rand");
    REQUIRE(channelList(&dlg)->visibleCount() == 1);
    ed->clear();
    CHECK(channelList(&dlg)->visibleCount() == 3);
}

TEST_CASE(
    "BrowseChannelsDialog: filter with no match hides all channel items", "[browse][filter]"
) {
    BrowseChannelsDialog dlg({kGeneral, kRandom, kSecret}, {kAlice}, nullptr);
    dlg.findChild<QLineEdit *>()->setText("zzz-no-match");
    CHECK(channelList(&dlg)->visibleCount() == 0);
}

// ── Filtering — people ────────────────────────────────────────────────────────

TEST_CASE("BrowseChannelsDialog: filter by name hides non-matching people", "[browse][filter]") {
    BrowseChannelsDialog dlg({kGeneral}, {kAlice, kBob}, nullptr);
    dlg.findChild<QLineEdit *>()->setText("alice");
    CHECK(peopleList(&dlg)->visibleCount() == 1);
}

TEST_CASE("BrowseChannelsDialog: people filter is case-insensitive", "[browse][filter]") {
    BrowseChannelsDialog dlg({kGeneral}, {kAlice, kBob}, nullptr);
    dlg.findChild<QLineEdit *>()->setText("BOB");
    CHECK(peopleList(&dlg)->visibleCount() == 1);
}

// ── Tab switching ──────────────────────────────────────────────────────────────

TEST_CASE(
    "BrowseChannelsDialog: Channels tab is active by default (stack index 0)", "[browse][tabs]"
) {
    BrowseChannelsDialog dlg({kGeneral}, {kAlice}, nullptr);
    CHECK(dlg.findChild<QStackedWidget *>()->currentIndex() == 0);
}

TEST_CASE("BrowseChannelsDialog: clicking People tab switches stack to index 1", "[browse][tabs]") {
    BrowseChannelsDialog dlg({kGeneral}, {kAlice}, nullptr);
    QPushButton         *peopleBtn = nullptr;
    for (auto *btn : dlg.findChildren<QPushButton *>()) {
        if (btn->isCheckable() && btn->text() == "People")
            peopleBtn = btn;
    }
    REQUIRE(peopleBtn != nullptr);
    peopleBtn->click();
    CHECK(dlg.findChild<QStackedWidget *>()->currentIndex() == 1);
}

TEST_CASE(
    "BrowseChannelsDialog: clicking Channels tab after People restores index 0", "[browse][tabs]"
) {
    BrowseChannelsDialog dlg({kGeneral}, {kAlice}, nullptr);
    QPushButton         *channelsBtn = nullptr, *peopleBtn = nullptr;
    for (auto *btn : dlg.findChildren<QPushButton *>()) {
        if (!btn->isCheckable())
            continue;
        if (btn->text() == "Channels")
            channelsBtn = btn;
        if (btn->text() == "People")
            peopleBtn = btn;
    }
    REQUIRE(channelsBtn != nullptr);
    REQUIRE(peopleBtn != nullptr);

    peopleBtn->click();
    REQUIRE(dlg.findChild<QStackedWidget *>()->currentIndex() == 1);
    channelsBtn->click();
    CHECK(dlg.findChild<QStackedWidget *>()->currentIndex() == 0);
}

// ── Signals ────────────────────────────────────────────────────────────────────

TEST_CASE(
    "BrowseChannelsDialog: Create Channel button emits createChannelRequested", "[browse][signals]"
) {
    BrowseChannelsDialog dlg({kGeneral}, {kAlice}, nullptr);
    bool                 fired = false;
    QObject::connect(&dlg, &BrowseChannelsDialog::createChannelRequested, [&] { fired = true; });

    // Identify the Create Channel button: non-checkable, non-flat
    QPushButton *createBtn = nullptr;
    for (auto *btn : dlg.findChildren<QPushButton *>()) {
        if (!btn->isCheckable() && !btn->isFlat())
            createBtn = btn;
    }
    REQUIRE(createBtn != nullptr);
    createBtn->click();
    CHECK(fired);
}

TEST_CASE(
    "BrowseChannelsDialog: channel activation emits channelActivated with correct id",
    "[browse][signals]"
) {
    // Items are sorted alphabetically: "general" (C1) sorts before "random" (C2).
    BrowseChannelsDialog dlg({kGeneral, kRandom}, {kAlice}, nullptr);
    ConversationId       received;
    QObject::connect(&dlg, &BrowseChannelsDialog::channelActivated, [&](ConversationId id) {
        received = id;
    });

    auto *list = channelList(&dlg);
    REQUIRE(list != nullptr);
    REQUIRE(list->visibleCount() == 2);
    REQUIRE(list->onActivated);
    list->onActivated(list->idAt(0)); // activate the first (sorted) row

    CHECK(received == ConversationId{"C1"}); // "general" sorts first
}

TEST_CASE(
    "BrowseChannelsDialog: people activation emits userActivated with correct id",
    "[browse][signals]"
) {
    // Items sorted alphabetically: "Alice Wonder" (U1) before "Bob Builder" (U2).
    BrowseChannelsDialog dlg({kGeneral}, {kAlice, kBob}, nullptr);
    UserId               received;
    QObject::connect(&dlg, &BrowseChannelsDialog::userActivated, [&](UserId id) { received = id; });

    auto *list = peopleList(&dlg);
    REQUIRE(list != nullptr);
    REQUIRE(list->visibleCount() == 2);
    REQUIRE(list->onActivated);
    list->onActivated(list->idAt(0)); // activate the first (sorted) row

    CHECK(received == UserId{"U1"}); // Alice sorts first
}

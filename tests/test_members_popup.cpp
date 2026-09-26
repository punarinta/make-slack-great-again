// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// MembersPopup: the header's list of who is in a channel or group DM.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>

#include "ui/browse_channels_dialog/browse_list_view.h"
#include "ui/members_popup/members_popup.h"
#include "ui/styled_line_edit/styled_line_edit.h"

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-members-popup");
    app.setOrganizationName("msga-test");
    return msga_test::runCatch(argc, argv);
}

namespace {

User person(const QString &id, const QString &name, const QString &displayName) {
    return User{.id = UserId{id}, .name = name, .displayName = displayName};
}

const std::vector<User> kPeople = {
    person("U2", "bob", "Bob Builder"),
    person("U1", "alice", "Alice Wonder"),
    person("U3", "carol", "Carol Singer"),
};

QLineEdit *searchOf(MembersPopup &popup) {
    auto *search = popup.findChild<StyledLineEdit *>("membersSearch");
    REQUIRE(search);
    return search->lineEdit();
}

BrowseListView *listOf(MembersPopup &popup) {
    auto *list = popup.findChild<BrowseListView *>("membersList");
    REQUIRE(list);
    return list;
}

QString titleOf(MembersPopup &popup) {
    auto *title = popup.findChild<QLabel *>("membersTitle");
    REQUIRE(title);
    return title->text();
}

void press(QWidget *w, int key) {
    QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

} // namespace

TEST_CASE("members are listed by name with the count in the title", "[members_popup]") {
    MembersPopup popup(nullptr);
    popup.open(QRect(100, 100, 40, 28), 3);
    CHECK(popup.visibleCount() == 0); // loading until the members arrive

    popup.setMembers(kPeople, UserId{"U1"});
    CHECK(popup.visibleCount() == 3);
    CHECK(titleOf(popup) == MembersPopup::tr("%Ln member(s)", "", 3));
    auto *list = listOf(popup);
    CHECK(list->idAt(0) == "U1");
    CHECK(list->idAt(1) == "U2");
    CHECK(list->idAt(2) == "U3");
}

TEST_CASE("deactivated accounts are left out", "[members_popup]") {
    auto people                 = kPeople;
    people.back().isDeactivated = true;
    MembersPopup popup(nullptr);
    popup.open(QRect(100, 100, 40, 28), 3);
    popup.setMembers(people, UserId{});
    CHECK(popup.visibleCount() == 2);
    CHECK(titleOf(popup) == MembersPopup::tr("%Ln member(s)", "", 2));
}

TEST_CASE("typing filters and Enter opens a DM with the first match", "[members_popup]") {
    MembersPopup        popup(nullptr);
    std::vector<UserId> activated;
    QObject::connect(&popup, &MembersPopup::memberActivated, [&](UserId u) {
        activated.push_back(u);
    });
    popup.open(QRect(100, 100, 40, 28), 3);
    popup.setMembers(kPeople, UserId{"U1"});

    searchOf(popup)->setText("car");
    CHECK(popup.visibleCount() == 1);
    press(searchOf(popup), Qt::Key_Return);
    REQUIRE(activated.size() == 1);
    CHECK(activated[0] == UserId{"U3"});
    CHECK_FALSE(popup.isVisible());
}

TEST_CASE("the handle finds a member too", "[members_popup]") {
    MembersPopup popup(nullptr);
    popup.open(QRect(100, 100, 40, 28), 3);
    popup.setMembers(kPeople, UserId{});
    searchOf(popup)->setText("bob");
    CHECK(popup.visibleCount() == 1);
    searchOf(popup)->setText("nobody");
    CHECK(popup.visibleCount() == 0);
}

TEST_CASE("an arriving list keeps the search already typed", "[members_popup]") {
    MembersPopup popup(nullptr);
    popup.open(QRect(100, 100, 40, 28), 3);
    searchOf(popup)->setText("ali");
    popup.setMembers(kPeople, UserId{});
    CHECK(popup.visibleCount() == 1);
}

TEST_CASE("a failed load shows the error, and reopening starts clean", "[members_popup]") {
    MembersPopup popup(nullptr);
    popup.open(QRect(100, 100, 40, 28), 3);
    popup.showError("Couldn't load the members (missing_scope).");
    CHECK(popup.visibleCount() == 0);
    auto *message = popup.findChild<QLabel *>("membersMessage");
    REQUIRE(message);
    CHECK(message->text().contains("missing_scope"));

    popup.hide();
    popup.open(QRect(100, 100, 40, 28), 3);
    CHECK_FALSE(message->text().contains("missing_scope"));
    popup.setMembers(kPeople, UserId{});
    CHECK(popup.visibleCount() == 3);
}

TEST_CASE("Escape closes the popup", "[members_popup]") {
    MembersPopup popup(nullptr);
    popup.open(QRect(100, 100, 40, 28), 3);
    REQUIRE(popup.isVisible());
    press(searchOf(popup), Qt::Key_Escape);
    CHECK_FALSE(popup.isVisible());
}

TEST_CASE("the panel fits the expected rows whole", "[members_popup]") {
    // Up to six rows show without a cut-off last one; more scroll.
    for (int count : {3, 6, 9}) {
        std::vector<User> people;
        for (int i = 0; i < count; ++i)
            people.push_back(person(QString("U%1").arg(i), QString("user%1").arg(i), {}));
        MembersPopup popup(nullptr);
        popup.open(QRect(100, 100, 40, 28), count);
        popup.setMembers(people, UserId{});
        QApplication::processEvents();
        auto *list = listOf(popup);
        INFO("count " << count << ", list height " << list->height());
        CHECK(list->height() == std::min(count, 6) * BrowseListView::rowHeight());
    }
}

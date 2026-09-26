// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// PopupTooltip press suppression: a press on a tooltipped target hides the
// tooltip, and it is not shown again for that target until the cursor leaves it.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QApplication>
#include <QTest>

#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme_manager.h"

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-popup-tooltip");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();
    return msga_test::runCatch(argc, argv);
}

namespace {

struct Fixture {
    QWidget       host;
    QWidget      *button = nullptr;
    PopupTooltip *tip    = nullptr;

    Fixture() {
        host.resize(400, 300);
        button = new QWidget(&host);
        button->setGeometry(150, 150, 40, 30);
        button->setMouseTracking(true);
        host.setMouseTracking(true);
        tip = new PopupTooltip(button);
        host.show();
        REQUIRE(QTest::qWaitForWindowExposed(&host));
    }

    QRect target() const { return QRect(button->mapToGlobal(QPoint(0, 0)), button->size()); }

    void hover() { tip->showAbove("Hint", target()); }
};

} // namespace

TEST_CASE("PopupTooltip: press on the target hides it and keeps it hidden", "[popup_tooltip]") {
    Fixture f;
    f.hover();
    REQUIRE(f.tip->isVisible());

    QTest::mousePress(f.button, Qt::LeftButton, Qt::NoModifier, QPoint(20, 15));
    CHECK_FALSE(f.tip->isVisible());
    QTest::mouseRelease(f.button, Qt::LeftButton, Qt::NoModifier, QPoint(20, 15));

    // Still over the pressed target: the call site re-issuing the hint is ignored.
    QTest::mouseMove(f.button, QPoint(22, 16));
    f.hover();
    CHECK_FALSE(f.tip->isVisible());

    // Leaving the target ends the suppression.
    QTest::mouseMove(&f.host, QPoint(10, 10));
    f.hover();
    CHECK(f.tip->isVisible());
}

TEST_CASE("PopupTooltip: a press before a delayed show suppresses it", "[popup_tooltip]") {
    Fixture f;
    QTest::mousePress(f.button, Qt::LeftButton, Qt::NoModifier, QPoint(20, 15));
    QTest::mouseRelease(f.button, Qt::LeftButton, Qt::NoModifier, QPoint(20, 15));
    f.hover();
    CHECK_FALSE(f.tip->isVisible());
}

TEST_CASE("PopupTooltip: a press elsewhere does not suppress this target", "[popup_tooltip]") {
    Fixture f;
    f.hover();
    QTest::mousePress(&f.host, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
    QTest::mouseRelease(&f.host, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
    CHECK_FALSE(f.tip->isVisible()); // any press dismisses a visible tooltip
    f.hover();
    CHECK(f.tip->isVisible());
}

TEST_CASE("PopupTooltip: showToast is not suppressed by the press", "[popup_tooltip]") {
    Fixture f;
    QTest::mousePress(f.button, Qt::LeftButton, Qt::NoModifier, QPoint(20, 15));
    QTest::mouseRelease(f.button, Qt::LeftButton, Qt::NoModifier, QPoint(20, 15));
    f.tip->showToast("Copied", f.target());
    CHECK(f.tip->isVisible());
}

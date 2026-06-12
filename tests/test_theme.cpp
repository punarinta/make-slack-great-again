// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-theme");
    app.setOrganizationName("msga-test");
    // ThemeManager persists via QSettings("msga", "msga") — redirect user-scope
    // storage so tests never touch the real config.
    static QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    return Catch::Session().run(argc, argv);
}

TEST_CASE("theme registry", "[theme]") {
    const auto &themes = Th::availableThemes();
    REQUIRE(themes.size() >= 2);
    CHECK(themes[0].id == "purple"); // default first

    const auto *purple = Th::themeById("purple");
    const auto *blue   = Th::themeById("blue");
    REQUIRE(purple != nullptr);
    REQUIRE(blue != nullptr);
    CHECK(purple == &Th::defaultTheme());

    CHECK(Th::themeById("does-not-exist") == nullptr);
}

TEST_CASE("blue retints chrome only; content surfaces are shared", "[theme]") {
    const auto &purple = *Th::themeById("purple");
    const auto &blue   = *Th::themeById("blue");

    // Chrome differs…
    CHECK(blue.nav.bg != purple.nav.bg);
    CHECK(blue.accent.def != purple.accent.def);
    CHECK(blue.titleBar.bg != purple.titleBar.bg);
    CHECK(blue.icon.accent == blue.accent.def);
    CHECK(blue.titleBar.bg == blue.nav.bg);

    // …content-side tokens must stay identical (copy-and-patch guarantee).
    CHECK(blue.text.primary == purple.text.primary);
    CHECK(blue.surface.content == purple.surface.content);
    CHECK(blue.message.codeBlockBg == purple.message.codeBlockBg);
    CHECK(blue.badge.mention == purple.badge.mention);
    CHECK(blue.fonts.base == purple.fonts.base);
}

TEST_CASE("ThemeManager switches, persists and ignores unknown ids", "[theme]") {
    auto &mgr = ThemeManager::instance();
    CHECK(mgr.themeId() == "purple"); // fresh settings → default

    QSignalSpy spy(&mgr, &ThemeManager::themeChanged);

    mgr.setThemeById("blue");
    CHECK(mgr.themeId() == "blue");
    CHECK(mgr.theme().nav.bg == Th::themeById("blue")->nav.bg);
    CHECK(spy.count() == 1);
    CHECK(QSettings("msga", "msga").value("appearance/theme").toString() == QStringLiteral("blue"));

    mgr.setThemeById("blue"); // no-op: already active
    CHECK(spy.count() == 1);

    mgr.setThemeById("does-not-exist"); // ignored
    CHECK(mgr.themeId() == "blue");
    CHECK(spy.count() == 1);

    mgr.setThemeById("purple");
    CHECK(mgr.themeId() == "purple");
    CHECK(spy.count() == 2);
    CHECK(
        QSettings("msga", "msga").value("appearance/theme").toString() == QStringLiteral("purple")
    );
}

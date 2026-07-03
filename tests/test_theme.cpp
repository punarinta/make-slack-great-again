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
    CHECK(themes[0].id == "purple");   // default first
    CHECK(themes[1].id == "charcoal"); // second slot in the picker

    const auto *purple   = Th::themeById("purple");
    const auto *charcoal = Th::themeById("charcoal");
    const auto *blue     = Th::themeById("blue");
    REQUIRE(purple != nullptr);
    REQUIRE(charcoal != nullptr);
    REQUIRE(blue != nullptr);
    CHECK(purple == &Th::defaultTheme());

    CHECK(Th::themeById("does-not-exist") == nullptr);
}

TEST_CASE("light themes retint chrome only; content surfaces are shared", "[theme]") {
    const auto &purple = *Th::themeById("purple");

    for (const auto &info : Th::availableThemes()) {
        if (info.id == QLatin1String("purple") || info.id == QLatin1String("charcoal"))
            continue; // charcoal is the dark theme — it retints content by design
        INFO("theme: " << info.id.toStdString());
        const auto &t = *info.theme;

        // Chrome differs…
        CHECK(t.nav.bg != purple.nav.bg);
        CHECK(t.accent.def != purple.accent.def);
        CHECK(t.titleBar.bg != purple.titleBar.bg);
        CHECK(t.icon.accent == t.accent.def);
        CHECK(t.titleBar.bg == t.nav.bg);

        // …content-side tokens must stay identical (copy-and-patch guarantee).
        CHECK(t.text.primary == purple.text.primary);
        CHECK(t.surface.content == purple.surface.content);
        CHECK(t.message.codeBlockBg == purple.message.codeBlockBg);
        CHECK(t.badge.mention == purple.badge.mention);
        CHECK(t.fonts.base == purple.fonts.base);
    }
}

TEST_CASE("charcoal is a coherent dark theme", "[theme]") {
    const auto &t = *Th::themeById("charcoal");

    // Dark surfaces, light text — and enough spread between them to read.
    CHECK(t.surface.content.lightnessF() < 0.2);
    CHECK(t.surface.raised.lightnessF() < 0.25);
    CHECK(t.surface.sunken.lightnessF() < t.surface.content.lightnessF());
    CHECK(t.text.primary.lightnessF() > 0.75);
    CHECK(t.text.secondary.lightnessF() > 0.5);
    CHECK(t.text.primary.lightnessF() - t.surface.content.lightnessF() > 0.5);

    // Alpha overlays must LIGHTEN on dark surfaces, not darken.
    CHECK(t.message.hover.lightnessF() > 0.9);
    CHECK(t.surface.highlight.lightnessF() > t.surface.content.lightnessF());

    // Sidebar depth: chats list darker than the content area, rail darkest —
    // the light themes' white-plate derivation must not flip this on dark.
    CHECK(t.nav.primary.lightnessF() < t.surface.content.lightnessF());
    CHECK(t.nav.bg.lightnessF() < t.nav.primary.lightnessF());

    // Filled controls stay visible: accent face vs the surfaces it sits on,
    // and its label vs the face.
    CHECK(t.accent.def.lightnessF() - t.surface.raised.lightnessF() > 0.1);
    CHECK(t.accent.text.lightnessF() - t.accent.def.lightnessF() > 0.4);

    // Content chrome that borders text follows the dark surfaces.
    CHECK(t.composer.bg.lightnessF() < 0.25);
    CHECK(t.message.codeBlockBg.lightnessF() < 0.25);
    CHECK(t.contextMenu.bg.lightnessF() < 0.25);
    CHECK(t.divider.def.lightnessF() < 0.4);

    // Structure/type scales stay shared with the base theme.
    const auto &purple = *Th::themeById("purple");
    CHECK(t.fonts.base == purple.fonts.base);
    CHECK(t.spacing.md == purple.spacing.md);
    CHECK(t.badge.mention == purple.badge.mention);
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

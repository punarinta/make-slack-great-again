// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Tests the pure string transform that produces the freedesktop .desktop body
// from the embedded template. The filesystem install path is Linux-only and
// side-effectful, so only buildDesktopEntry() — the part with logic worth
// pinning down — is covered here.
#include <catch2/catch_test_macros.hpp>

#include "util/desktop_integration.h"

#include <QString>

namespace {

// The template that ships in QRC (:/linux/msga.desktop).
const QString kTemplate = QStringLiteral(
    "[Desktop Entry]\n"
    "Name=MSGA\n"
    "Comment=Fast native Slack client\n"
    "Exec=msga %u\n"
    "Icon=msga\n"
    "Type=Application\n"
    "Categories=Network;InstantMessaging;\n"
    "StartupWMClass=msga\n"
    "MimeType=x-scheme-handler/msga;\n"
);

QString entry(const QString &exec, const QString &icon) {
    return QString::fromUtf8(DesktopIntegration::buildDesktopEntry(kTemplate, exec, icon));
}

} // namespace

TEST_CASE("buildDesktopEntry rewrites Exec to the runtime path + %u", "[desktop]") {
    const QString out = entry("/opt/msga/msga", "/x/icon.png");
    CHECK(out.contains("\nExec=/opt/msga/msga %u\n"));
    // The placeholder Exec must not survive.
    CHECK_FALSE(out.contains("\nExec=msga %u\n"));
}

TEST_CASE("buildDesktopEntry rewrites Icon to the installed path", "[desktop]") {
    const QString out =
        entry("/opt/msga/msga", "/home/u/.local/share/icons/hicolor/256x256/apps/msga.png");
    CHECK(out.contains("\nIcon=/home/u/.local/share/icons/hicolor/256x256/apps/msga.png\n"));
    CHECK_FALSE(out.contains("\nIcon=msga\n"));
}

TEST_CASE("buildDesktopEntry quotes an Exec path containing spaces", "[desktop]") {
    const QString out = entry("/home/My Apps/msga", "/x/icon.png");
    // Path is quoted; the %u field code stays outside the quotes.
    CHECK(out.contains("\nExec=\"/home/My Apps/msga\" %u\n"));
}

TEST_CASE("buildDesktopEntry preserves every other key", "[desktop]") {
    const QString out = entry("/opt/msga/msga", "/x/icon.png");
    CHECK(out.startsWith("[Desktop Entry]\n"));
    CHECK(out.contains("\nName=MSGA\n"));
    CHECK(out.contains("\nType=Application\n"));
    CHECK(out.contains("\nStartupWMClass=msga\n"));
    // The scheme handler line is what makes OAuth msga:// redirects route back
    // to the app — it must come through untouched.
    CHECK(out.contains("\nMimeType=x-scheme-handler/msga;\n"));
    // Trailing newline preserved (no spurious blank-line growth).
    CHECK(out.endsWith("MimeType=x-scheme-handler/msga;\n"));
}

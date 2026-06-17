// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QByteArray>
#include <QString>

// freedesktop ("XDG") launcher integration. On systems that support .desktop
// files (Linux/Ubuntu and other freedesktop desktops) this installs — or, when
// the executable moved or the template changed, reinstalls — a per-user
// launcher entry and its icon, and registers the msga:// scheme handler used by
// the OAuth redirect. The .desktop template and icon are compiled into the
// binary via QRC (:/linux/msga.desktop, :/icon.png), so a static build is fully
// self-contained. No-op on macOS/Windows and where no writable data dir exists.
namespace DesktopIntegration {

// Fire-and-forget; does its filesystem work off the GUI thread so it never adds
// to startup latency. Call once after the QApplication is up.
void installIfSupported();

// Builds the .desktop file body from the embedded template, overriding the
// Exec and Icon keys with the current runtime values (the Exec path is quoted
// if it contains spaces, per the Desktop Entry spec). Pure string logic,
// compiled on every platform and exercised by the test suite.
QByteArray
buildDesktopEntry(const QString &templateText, const QString &execPath, const QString &iconValue);

} // namespace DesktopIntegration

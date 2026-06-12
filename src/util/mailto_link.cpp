// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "mailto_link.h"
#include "clipboard.h"

#include <QDesktopServices>
#include <QProcess>
#include <QStandardPaths>

namespace MailtoLink {
namespace {

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
// On Linux QDesktopServices::openUrl only reports whether xdg-open was
// launched, not whether a mailto handler exists — without one, gio fails
// after the fact ("The specified location is not supported") and the click
// is a silent no-op. Query the handler registration up front instead.
bool queryLinuxHandler() {
    const QString xdgMime = QStandardPaths::findExecutable(QStringLiteral("xdg-mime"));
    if (xdgMime.isEmpty())
        return true; // can't tell — assume a handler and just try to open
    QProcess p;
    p.start(
        xdgMime,
        {QStringLiteral("query"),
         QStringLiteral("default"),
         QStringLiteral("x-scheme-handler/mailto")}
    );
    if (!p.waitForFinished(1500))
        return true;
    return !QString::fromUtf8(p.readAllStandardOutput()).trimmed().isEmpty();
}
#endif

bool systemHandlerAvailable() {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    static const bool available = queryLinuxHandler();
    return available;
#else
    // macOS/Windows: openUrl() itself reliably reports a missing handler.
    return true;
#endif
}

} // namespace

bool openOrCopy(const QUrl &url) {
    if (systemHandlerAvailable() && QDesktopServices::openUrl(url))
        return false;
    Clipboard::setText(address(url));
    return true;
}

} // namespace MailtoLink

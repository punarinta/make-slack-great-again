// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "util/desktop_integration.h"

#include <QStringList>

#if defined(Q_OS_LINUX)
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QThreadPool>
#endif

namespace DesktopIntegration {

QByteArray
buildDesktopEntry(const QString &templateText, const QString &execPath, const QString &iconValue) {
    // Quote the program path if it contains spaces — the Desktop Entry spec
    // reserves whitespace as an argument separator. The %u field code stays
    // outside the quotes so it's still recognised.
    QString exec = execPath;
    if (exec.contains(QLatin1Char(' ')))
        exec = QLatin1Char('"') + exec + QLatin1Char('"');

    // Rewrite the Exec/Icon keys to the live runtime values, leaving every
    // other line (Name, MimeType scheme handler, StartupWMClass, …) untouched.
    QStringList       out;
    const QStringList lines = templateText.split(QLatin1Char('\n'));
    out.reserve(lines.size());
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("Exec=")))
            out << QLatin1String("Exec=") + exec + QLatin1String(" %u");
        else if (line.startsWith(QLatin1String("Icon=")))
            out << QLatin1String("Icon=") + iconValue;
        else
            out << line;
    }
    return out.join(QLatin1Char('\n')).toUtf8();
}

#if defined(Q_OS_LINUX)
namespace {

constexpr auto kIconRelPath    = "icons/hicolor/256x256/apps/msga.png";
constexpr auto kDesktopRelPath = "applications/msga.desktop";

enum class Write { Error, Unchanged, Written };

// Writes `bytes` to `path` (creating parent dirs) only when the content would
// change, so an unchanged launcher never has its mtime bumped and we can avoid
// re-running update-desktop-database on every launch.
Write writeIfChanged(const QString &path, const QByteArray &bytes) {
    if (QFile existing(path); existing.open(QIODevice::ReadOnly)) {
        if (existing.readAll() == bytes)
            return Write::Unchanged;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return Write::Error;
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Write::Error;
    return out.write(bytes) == bytes.size() ? Write::Written : Write::Error;
}

QByteArray readResource(const char *path) {
    QFile f(QString::fromLatin1(path));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void doInstall() {
    // XDG_DATA_HOME (defaults to ~/.local/share). Empty means we can't resolve
    // a writable data dir — treat the OS as not supporting .desktop files.
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (data.isEmpty())
        return;

    // 1. Install the icon into the hicolor theme; an absolute path in Icon= is
    //    the most robust choice (no dependency on an icon-cache refresh).
    const QString    iconPath  = data + QLatin1Char('/') + QLatin1String(kIconRelPath);
    const QByteArray iconBytes = readResource(":/icon.png");
    const bool iconOk = !iconBytes.isEmpty() && writeIfChanged(iconPath, iconBytes) != Write::Error;

    // 2. Build the .desktop body from the embedded template with the current
    //    executable path and the icon we just installed.
    const QByteArray tmpl = readResource(":/linux/msga.desktop");
    if (tmpl.isEmpty())
        return;
    const QString    iconValue = iconOk ? iconPath : QStringLiteral("msga");
    const QByteArray entry     = buildDesktopEntry(
        QString::fromUtf8(tmpl), QCoreApplication::applicationFilePath(), iconValue
    );

    const QString desktopPath = data + QLatin1Char('/') + QLatin1String(kDesktopRelPath);
    if (writeIfChanged(desktopPath, entry) != Write::Written)
        return; // error, or already current — nothing to refresh

    // 3. The launcher changed: refresh the MIME/desktop database so the
    //    msga:// scheme handler and the entry are picked up. Best-effort and
    //    fully detached — a missing tool is fine.
    const QString udb = QStandardPaths::findExecutable(QStringLiteral("update-desktop-database"));
    if (!udb.isEmpty())
        QProcess::startDetached(udb, {QFileInfo(desktopPath).absolutePath()});
}

} // namespace
#endif // Q_OS_LINUX

void installIfSupported() {
#if defined(Q_OS_LINUX)
    // Off the GUI thread: small file writes plus spawning a helper must not add
    // to startup latency. The task captures nothing with surface lifetime.
    QThreadPool::globalInstance()->start([] { doInstall(); });
#endif
}

} // namespace DesktopIntegration

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "claude_code_auth.h"

#include "cc_roster.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace claude_code {

TokenStore::WorkspaceRecord toRecord(const Credentials &creds) {
    TokenStore::WorkspaceRecord rec;
    rec.key         = WorkspaceKey{kService, kWorkspaceId};
    rec.displayName = QStringLiteral("Claude Code");
    rec.iconUrl     = QStringLiteral("qrc:/claude_code_avatar.png");
    QJsonObject blob;
    blob[QStringLiteral("claudePath")] = creds.claudePath;
    rec.auth                           = QJsonDocument(blob).toJson(QJsonDocument::Compact);
    return rec;
}

Credentials fromRecord(const TokenStore::WorkspaceRecord &rec) {
    const QJsonObject blob = QJsonDocument::fromJson(rec.auth).object();
    Credentials       c;
    c.claudePath = blob.value(QStringLiteral("claudePath")).toString();
    // The CLI may have moved (reinstalled, switched installer) since the
    // workspace was added: fall back to looking again.
    if (c.claudePath.isEmpty() || !QFileInfo(c.claudePath).isExecutable())
        c.claudePath = findClaudeExecutable();
    return c;
}

QString findClaudeExecutable() {
#if defined(Q_OS_WIN)
    const QStringList names = {QStringLiteral("claude.exe"), QStringLiteral("claude.cmd")};
#else
    const QStringList names = {QStringLiteral("claude")};
#endif
    for (const auto &n : names)
        if (const QString p = QStandardPaths::findExecutable(n); !p.isEmpty())
            return p;
    // A GUI app often starts with a thinner PATH than a login shell, so also
    // try where the installers put the binary.
    const QString home  = QDir::homePath();
    QStringList   extra = {
        home + QStringLiteral("/.local/bin"),
        home + QStringLiteral("/.claude/local"),
        home + QStringLiteral("/.npm-global/bin"),
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/local/bin"),
    };
#if defined(Q_OS_WIN)
    extra << home + QStringLiteral("/AppData/Roaming/npm") << home + QStringLiteral("/.local/bin");
#endif
    for (const auto &n : names)
        if (const QString p = QStandardPaths::findExecutable(n, extra); !p.isEmpty())
            return p;
    return {};
}

void AuthStrategy::start() {
    const Paths paths = Paths::detect();
    if (!QFileInfo(paths.home).isDir()) {
        emit failed(
            QCoreApplication::translate(
                "claude_code",
                "Claude Code doesn't seem to be set up on this computer: %1 doesn't exist. Run "
                "`claude` once in a terminal, then add the workspace again."
            )
                .arg(QDir::toNativeSeparators(paths.home))
        );
        return;
    }
    // Without the CLI the workspace still shows every session; only starting
    // and continuing sessions from msga needs it.
    emit succeeded(toRecord(Credentials{findClaudeExecutable()}));
}

} // namespace claude_code

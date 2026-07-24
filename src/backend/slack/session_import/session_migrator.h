// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Converts existing Slack workspaces to session auth in bulk. The `d` cookie is
// per-user and domain-wide, so one cookie (taken from any already-session
// workspace) covers every workspace on that account. For each item it resolves
// the workspace domain via team.info (using the workspace's current token), then
// hands the resulting URLs to TokenDeriver to mint + validate xoxc tokens against
// the shared cookie. Widgets-free — the UI drives it and stores the results.
#pragma once

#include "backend/slack/session_import/session_types.h"
#include "backend/slack/slack_auth.h"

#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace slack::session {

class TokenDeriver;

class SessionMigrator : public QObject {
    Q_OBJECT
public:
    explicit SessionMigrator(QObject *parent = nullptr);

    // A workspace to convert. `currentToken` is whatever it authenticates with
    // today (an OAuth xoxp, or an xoxc if already session) — used only to look up
    // the workspace domain.
    struct Item {
        QString teamId;
        QString teamName;
        QString iconUrl;
        QString currentToken;
    };

    // Resolve each item's domain, then derive+validate a session token against
    // `cookie`. Emits finished() once. `converted` holds the workspaces that
    // succeeded (as ready-to-store session credentials).
    void run(const QString &cookie, const QList<Item> &items);

signals:
    void finished(const QList<slack::Credentials> &converted, const QString &error);

private:
    void resolveNext();

    QNetworkAccessManager *_nam;
    QString                _cookie;
    QList<Item>            _items;
    int                    _idx = 0;
    QList<TeamSession>     _resolved; // workspaces whose domain resolved, with URL + meta
    QString                _lastError;
    TokenDeriver          *_deriver = nullptr;
};

} // namespace slack::session

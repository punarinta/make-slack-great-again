// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Turns a Slack `d` session cookie (+ optional per-team hints) into validated
// per-workspace Credentials. For a candidate with no token, GETs the workspace
// boot page with the cookie and scrapes the embedded xoxc- api_token; then
// confirms every candidate with auth.test (and fetches the team icon via
// team.info). Widgets-free — the UI dialog drives it and stores the results.
#pragma once

#include "backend/slack/session_import/session_types.h"
#include "backend/slack/slack_auth.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;

namespace slack::session {

class TokenDeriver : public QObject {
    Q_OBJECT
public:
    explicit TokenDeriver(QObject *parent = nullptr);

    // Derive+validate all candidates against the given `d` cookie. Emits finished()
    // exactly once. Validated workspaces come back in `valid` (possibly fewer than
    // requested); `error` is set only when nothing could be validated at all.
    void run(const QString &cookie, const QList<TeamSession> &candidates);

signals:
    void finished(const QList<slack::Credentials> &valid, const QString &error);

private:
    void processNext();
    void deriveToken(const TeamSession &cand, std::function<void(QString)> onToken);
    void validate(const TeamSession &cand, const QString &token);
    void fetchIconThenCommit(slack::Credentials creds);
    void commit(slack::Credentials creds);

    QNetworkAccessManager    *_nam;
    QString                   _cookie;
    QList<TeamSession>        _queue;
    int                       _idx = 0;
    QList<slack::Credentials> _valid;
    QString                   _lastError;
};

} // namespace slack::session

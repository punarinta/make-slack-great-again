// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Microsoft Teams (Microsoft identity) OAuth 2.0 Authorization Code + PKCE auth
// strategy for a public client (no secret). Opens the browser; the OS delivers
// the msga:// callback to handleCallbackUri(). Lives below the Backend seam in
// teams:: — it knows the login.microsoftonline.com endpoints and the Graph scope
// list — and adapts its result to the neutral auth::AuthStrategy contract.
#pragma once

#include "auth/auth_strategy.h"
#include "backend/teams/teams_auth.h"

#include <QUrl>

namespace teams {

class OAuthFlow : public auth::AuthStrategy {
    Q_OBJECT
public:
    explicit OAuthFlow(AppConfig app, QObject *parent = nullptr);

    // Opens the browser. Emits succeeded() (neutral WorkspaceRecord) or failed()
    // once handleCallbackUri() delivers the code. Fails immediately (no browser)
    // if the app credentials are not configured.
    void start() override;

    void handleCallbackUri(const QUrl &uri) override;

private:
    void exchangeCode(const QString &code);
    void finish(
        const QString &accessToken,
        const QString &refreshToken,
        qint64         expiresAt,
        const QString &idToken
    );

    static QString scopes();
    static QString authorityBase(); // login.microsoftonline.com/organizations

    AppConfig _app;
    QString   _state;
    QString   _codeVerifier; // PKCE
};

} // namespace teams

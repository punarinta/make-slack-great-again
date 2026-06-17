// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Slack's OAuth v2 + PKCE auth strategy. Opens the browser; the OS delivers the
// msga:// callback to handleCallbackUri(). Lives below the Backend seam in
// slack:: — it knows slack.com endpoints, the Slack scope list and xoxp tokens —
// and adapts its result to the neutral auth::AuthStrategy contract.
#pragma once

#include "auth/auth_strategy.h"
#include "backend/slack/slack_auth.h"

#include <QUrl>

// Redirect URI registered verbatim in the Slack app's OAuth & Permissions settings.
static constexpr const char *kOAuthRedirectUri = "msga://oauth/callback";

namespace slack {

class OAuthFlow : public auth::AuthStrategy {
    Q_OBJECT
public:
    explicit OAuthFlow(AppConfig app, QObject *parent = nullptr);

    // Opens the browser. Emits succeeded() (neutral WorkspaceRecord) or failed()
    // once handleCallbackUri() delivers the code. If the app credentials are not
    // configured, fails immediately without opening a browser.
    void start() override;

    void handleCallbackUri(const QUrl &uri) override;

private:
    void exchangeCode(const QString &code);
    void fetchTeamInfo(
        const QString &xoxp,
        const QString &refreshToken,
        qint64         expiresAt,
        const QString &teamId,
        const QString &teamName
    );

    static QStringList userScopes();

    AppConfig _app;
    QString   _state;
    QString   _codeVerifier; // PKCE
};

} // namespace slack

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// OAuth v2 + PKCE flow for Slack user tokens.
// Opens the browser; the OS delivers the msga:// callback to handleUri().
#pragma once

#include <QObject>
#include <QUrl>

#include "network/web_api_client.h"
#include "token_store.h"

// Redirect URI registered verbatim in the Slack app's OAuth & Permissions settings.
static constexpr const char *kOAuthRedirectUri = "msga://oauth/callback";

class OAuthFlow : public QObject {
    Q_OBJECT
public:
    explicit OAuthFlow(const TokenStore::AppConfig &app, QObject *parent = nullptr);

    // Opens the browser. Emits done() or failed() once handleUri() delivers the code.
    void start();

public slots:
    // Called by SingleInstance when the OS delivers msga://oauth/callback?code=…
    void handleUri(const QUrl &uri);

signals:
    void done(TokenStore::Credentials creds);
    void failed(QString reason);

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

    TokenStore::AppConfig _app;
    WebApiClient          _client;
    QString               _state;
    QString               _codeVerifier; // PKCE
};

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Generic OAuth 2.0 authorization-code + PKCE flow with a loopback redirect:
// opens the browser, runs a one-shot HTTP listener on 127.0.0.1:<port>, then
// exchanges the code at the provider's token endpoint.
//
// Unlike backend/slack/oauth_flow.h (Slack, msga:// custom scheme via SingleInstance),
// LLM providers register http://localhost:<port> redirect URIs, so the
// callback arrives over a local TCP connection instead of the OS.
#pragma once

#include <QObject>
#include <QJsonObject>

class QTcpServer;

struct OAuthConfig {
    QString                        authorizeUrl;
    QString                        tokenUrl;
    QString                        clientId;
    QString                        scopes;       // space-separated
    quint16                        port = 0;     // loopback port the client is registered for
    QString                        callbackPath; // e.g. "/callback"
    // Extra query params some providers require on the authorize URL.
    QList<QPair<QString, QString>> extraAuthParams;
    // true → token request body is JSON; false → application/x-www-form-urlencoded
    bool                           jsonTokenRequest = false;
};

class OAuthLoopbackFlow : public QObject {
    Q_OBJECT
public:
    explicit OAuthLoopbackFlow(OAuthConfig cfg, QObject *parent = nullptr);

    // Opens the browser and starts listening. Emits done() or failed() once.
    void start();
    void cancel(); // stops the listener; no signal emitted

    // Refresh an access token. Standalone — does not require start().
    // Emits done(tokenResponse) / failed(reason) like the code exchange.
    void refresh(const QString &refreshToken);

    [[nodiscard]] QString redirectUri() const;

signals:
    void done(QJsonObject tokenResponse);
    void failed(QString reason);

private:
    void onNewConnection();
    void exchangeCode(const QString &code);
    void postTokenRequest(const QList<QPair<QString, QString>> &params);

    OAuthConfig _cfg;
    QTcpServer *_server = nullptr;
    QString     _state;
    QString     _codeVerifier; // PKCE
    bool        _finished = false;
};

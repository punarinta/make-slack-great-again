// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Shared auth lifecycle for LLM providers: OAuth (PKCE + loopback), API-key
// auth, token refresh, and credential persistence. Concrete providers supply
// the endpoints and the request/response wire format only.
#pragma once

#include "llm_provider.h"
#include "llm_token_store.h"
#include "oauth_loopback.h"

class QNetworkAccessManager;

class LlmProviderBase : public LlmProvider {
    Q_OBJECT
public:
    LlmProviderBase(QString id, QString displayName, QObject *parent = nullptr);

    [[nodiscard]] QString    id() const final { return _id; }
    [[nodiscard]] QString    displayName() const final { return _displayName; }
    [[nodiscard]] AuthState  authState() const final { return _state; }
    [[nodiscard]] AuthMethod authMethod() const final;
    [[nodiscard]] QString    accountLabel() const final { return _creds.accountLabel; }
    // OAuth is possible only when the subclass supplies a registered client.
    [[nodiscard]] bool supportsOAuth() const final { return !oauthConfig().clientId.isEmpty(); }

    void connectOAuth() final;
    void connectApiKey(const QString &key) final;
    void disconnectAccount() final;

    // Refreshes the OAuth token if it is about to expire, then calls sendChat().
    void chat(const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError) final;

protected:
    [[nodiscard]] virtual OAuthConfig oauthConfig() const = 0;
    // Display label (account email, …) extracted from a token response; may be empty.
    [[nodiscard]] virtual QString     accountLabelFromToken(const QJsonObject &token) const;
    // Perform the actual completion call. Credentials are valid when invoked.
    virtual void
    sendChat(const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError) = 0;

    [[nodiscard]] const LlmTokenStore::Credentials &creds() const { return _creds; }
    QNetworkAccessManager                          *nam();

private:
    void applyTokenResponse(const QJsonObject &token);
    void setState(AuthState s);

    QString                    _id;
    QString                    _displayName;
    AuthState                  _state = AuthState::Disconnected;
    LlmTokenStore::Credentials _creds;
    OAuthLoopbackFlow         *_flow = nullptr;
};

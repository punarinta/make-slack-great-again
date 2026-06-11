// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "llm_provider_base.h"

#include <QNetworkAccessManager>
#include <QDateTime>
#include <QJsonObject>

LlmProviderBase::LlmProviderBase(QString id, QString displayName, QObject *parent)
    : LlmProvider(parent), _id(std::move(id)), _displayName(std::move(displayName)) {
    _creds = LlmTokenStore::load(_id);
    _state = _creds.isConnected() ? AuthState::Connected : AuthState::Disconnected;
}

LlmProvider::AuthMethod LlmProviderBase::authMethod() const {
    if (!_creds.apiKey.isEmpty())
        return AuthMethod::ApiKey;
    if (!_creds.accessToken.isEmpty())
        return AuthMethod::OAuth;
    return AuthMethod::None;
}

QNetworkAccessManager *LlmProviderBase::nam() {
    if (!_nam)
        _nam = new QNetworkAccessManager(this);
    return _nam;
}

QString LlmProviderBase::accountLabelFromToken(const QJsonObject &) const {
    return {};
}

void LlmProviderBase::setState(AuthState s) {
    if (_state == s)
        return;
    _state = s;
    emit authStateChanged();
}

void LlmProviderBase::connectOAuth() {
    if (!supportsOAuth()) {
        emit authFailed(
            tr("%1 does not offer sign-in for third-party apps — use an API key").arg(_displayName)
        );
        return;
    }
    if (_state == AuthState::Connecting)
        return;
    if (_flow) {
        _flow->cancel();
        _flow->deleteLater();
    }
    _flow = new OAuthLoopbackFlow(oauthConfig(), this);
    setState(AuthState::Connecting);

    connect(_flow, &OAuthLoopbackFlow::done, this, [this](const QJsonObject &token) {
        _flow->deleteLater();
        _flow = nullptr;
        applyTokenResponse(token);
        setState(AuthState::Connected);
    });
    connect(_flow, &OAuthLoopbackFlow::failed, this, [this](const QString &reason) {
        _flow->deleteLater();
        _flow = nullptr;
        setState(_creds.isConnected() ? AuthState::Connected : AuthState::Disconnected);
        emit authFailed(reason);
    });
    _flow->start();
}

void LlmProviderBase::connectApiKey(const QString &key) {
    const QString trimmed = key.trimmed();
    if (trimmed.isEmpty()) {
        emit authFailed(tr("API key is empty"));
        return;
    }
    if (_flow) { // abandon a half-finished OAuth attempt
        _flow->cancel();
        _flow->deleteLater();
        _flow = nullptr;
    }
    _creds              = {};
    _creds.apiKey       = trimmed;
    // Masked form for the UI; never display the full key back.
    _creds.accountLabel = trimmed.length() > 10
                              ? trimmed.left(5) + QStringLiteral("…") + trimmed.right(4)
                              : QStringLiteral("API key");
    LlmTokenStore::save(_id, _creds);
    setState(AuthState::Connected);
    // setState is a no-op when already Connected (e.g. key replaced) — but the
    // label changed, so make sure the UI refreshes either way.
    emit authStateChanged();
}

void LlmProviderBase::disconnectAccount() {
    if (_flow) {
        _flow->cancel();
        _flow->deleteLater();
        _flow = nullptr;
    }
    _creds = {};
    LlmTokenStore::clear(_id);
    setState(AuthState::Disconnected);
}

void LlmProviderBase::applyTokenResponse(const QJsonObject &token) {
    const QString refresh = token.value("refresh_token").toString();
    _creds.apiKey.clear();
    _creds.accessToken = token.value("access_token").toString();
    if (!refresh.isEmpty()) // refresh responses may omit a new refresh token
        _creds.refreshToken = refresh;
    const qint64 expiresIn = token.value("expires_in").toVariant().toLongLong();
    _creds.expiresAt       = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
    const QString label    = accountLabelFromToken(token);
    if (!label.isEmpty())
        _creds.accountLabel = label;
    else if (_creds.accountLabel.isEmpty())
        _creds.accountLabel = tr("Connected");
    LlmTokenStore::save(_id, _creds);
}

void LlmProviderBase::chat(
    const Llm::Request &req, Llm::OnResponse onResponse, Llm::OnError onError
) {
    if (_state != AuthState::Connected) {
        if (onError)
            onError(tr("%1 is not connected").arg(_displayName));
        return;
    }

    const bool needsRefresh = _creds.apiKey.isEmpty() && _creds.expiresAt > 0 &&
                              QDateTime::currentSecsSinceEpoch() > _creds.expiresAt - 60 &&
                              !_creds.refreshToken.isEmpty();
    if (!needsRefresh) {
        sendChat(req, std::move(onResponse), std::move(onError));
        return;
    }

    auto *flow = new OAuthLoopbackFlow(oauthConfig(), this);
    connect(
        flow,
        &OAuthLoopbackFlow::done,
        this,
        [this, flow, req, onResponse = std::move(onResponse), onError = std::move(onError)](
            const QJsonObject &token
        ) mutable {
            flow->deleteLater();
            applyTokenResponse(token);
            sendChat(req, std::move(onResponse), std::move(onError));
        }
    );
    connect(flow, &OAuthLoopbackFlow::failed, this, [this, flow, onError](const QString &reason) {
        flow->deleteLater();
        if (onError)
            onError(tr("Token refresh failed: %1").arg(reason));
    });
    flow->refresh(_creds.refreshToken);
}

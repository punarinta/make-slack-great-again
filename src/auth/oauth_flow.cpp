// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "oauth_flow.h"

#include <QDesktopServices>
#include <QUrl>
#include <QUrlQuery>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QJsonDocument>
#include <QJsonObject>

OAuthFlow::OAuthFlow(const TokenStore::AppConfig &app, QObject *parent)
    : QObject(parent), _app(app), _client(this) {}

QStringList OAuthFlow::userScopes() {
    return {
        "channels:history", "groups:history",  "im:history",  "mpim:history",       "channels:read",
        "groups:read",      "im:read",         "mpim:read",   "users:read",         "team:read",
        "emoji:read",       "reactions:read",  "files:read",  "users.profile:read", "search:read",
        "chat:write",       "reactions:write", "files:write", "stars:write",
    };
}

void OAuthFlow::start() {
    // PKCE: 32 random bytes → base64url (43 chars, within RFC 7636's 43–128 range)
    QByteArray verifierBytes(32, '\0');
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(verifierBytes.data()), 8);
    _codeVerifier = QString::fromLatin1(
        verifierBytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
    );

    const QByteArray challenge =
        QCryptographicHash::hash(_codeVerifier.toLatin1(), QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

    _state = QString::number(QRandomGenerator::global()->generate64(), 16);

    QUrl      url("https://slack.com/oauth/v2/authorize");
    QUrlQuery q;
    q.addQueryItem("client_id", _app.clientId);
    q.addQueryItem("user_scope", userScopes().join(','));
    q.addQueryItem("redirect_uri", kOAuthRedirectUri);
    q.addQueryItem("state", _state);
    q.addQueryItem("code_challenge", QString::fromLatin1(challenge));
    q.addQueryItem("code_challenge_method", "S256");
    url.setQuery(q);

    QDesktopServices::openUrl(url);
}

void OAuthFlow::handleUri(const QUrl &uri) {
    // Expected: msga://oauth/callback?code=…&state=…
    if (uri.scheme() != "msga" || uri.host() != "oauth" || uri.path() != "/callback")
        return;

    QUrlQuery q(uri.query());
    if (q.hasQueryItem("error")) {
        emit failed(q.queryItemValue("error"));
        return;
    }
    if (q.queryItemValue("state") != _state) {
        emit failed("state_mismatch");
        return;
    }
    exchangeCode(q.queryItemValue("code"));
}

void OAuthFlow::exchangeCode(const QString &code) {
    QUrlQuery params;
    params.addQueryItem("client_id", _app.clientId);
    params.addQueryItem("client_secret", _app.clientSecret);
    params.addQueryItem("code", code);
    params.addQueryItem("redirect_uri", kOAuthRedirectUri);
    params.addQueryItem("code_verifier", _codeVerifier); // PKCE

    auto           *nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl("https://slack.com/api/oauth.v2.access"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    auto *reply = nam->post(req, params.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam] {
        reply->deleteLater();
        nam->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(reply->errorString());
            return;
        }
        auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (!obj.value("ok").toBool()) {
            emit failed(obj.value("error").toString("unknown"));
            return;
        }
        auto user = obj.value("authed_user").toObject();
        auto team = obj.value("team").toObject();
        fetchTeamInfo(
            user.value("access_token").toString(),
            user.value("refresh_token").toString(), // non-empty when token rotation enabled
            team.value("id").toString(),
            team.value("name").toString()
        );
    });
}

void OAuthFlow::fetchTeamInfo(
    const QString &xoxp, const QString &refreshToken, const QString &teamId, const QString &teamName
) {
    auto           *nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl("https://slack.com/api/team.info"));
    req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(xoxp).toUtf8());
    auto *reply = nam->get(req);
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, reply, nam, xoxp, refreshToken, teamId, teamName] {
            reply->deleteLater();
            nam->deleteLater();
            QString iconUrl;
            if (reply->error() == QNetworkReply::NoError) {
                auto root = QJsonDocument::fromJson(reply->readAll()).object();
                auto icon = root.value("team").toObject().value("icon").toObject();
                iconUrl   = icon.value("image_88").toString();
            }
            emit done(TokenStore::Credentials{xoxp, teamId, teamName, iconUrl, refreshToken});
        }
    );
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "oauth_flow.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrlQuery>

namespace slack {

OAuthFlow::OAuthFlow(AppConfig app, QObject *parent)
    : auth::AuthStrategy(parent), _app(std::move(app)) {}

QStringList OAuthFlow::userScopes() {
    // NOTE: tokens issued before a scope was added here lack it until the user
    // signs in to the workspace again; the affected calls fail with
    // missing_scope and the UI shows a re-auth hint.
    return {
        "channels:history",    "groups:history",     "im:history",    "mpim:history",
        "channels:read",       "groups:read",        "im:read",       "mpim:read",
        "users:read",          "team:read",          "emoji:read",    "reactions:read",
        "files:read",          "users.profile:read", "search:read",   "chat:write",
        "reactions:write",     "files:write",        "stars:write",   "channels:write",
        "groups:write",        "mpim:write",         "im:write",      "users:write",
        "users.profile:write", "dnd:write",          "canvases:read", "canvases:write",
    };
}

void OAuthFlow::start() {
    if (_app.clientId.isEmpty()) {
        emit failed(
            QCoreApplication::translate(
                "slack::OAuthFlow",
                "App credentials are not configured.\n\n"
                "Copy credentials.cmake.example to credentials.cmake, "
                "fill in your Slack app credentials, and rebuild."
            )
        );
        return;
    }

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

void OAuthFlow::handleCallbackUri(const QUrl &uri) {
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
        auto         user      = obj.value("authed_user").toObject();
        auto         team      = obj.value("team").toObject();
        const qint64 expiresIn = user.value("expires_in").toInteger(0);
        const qint64 expiresAt = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
        fetchTeamInfo(
            user.value("access_token").toString(),
            user.value("refresh_token").toString(),
            expiresAt,
            team.value("id").toString(),
            team.value("name").toString()
        );
    });
}

void OAuthFlow::fetchTeamInfo(
    const QString &xoxp,
    const QString &refreshToken,
    qint64         expiresAt,
    const QString &teamId,
    const QString &teamName
) {
    auto           *nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl("https://slack.com/api/team.info"));
    req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(xoxp).toUtf8());
    auto *reply = nam->get(req);
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, reply, nam, xoxp, refreshToken, expiresAt, teamId, teamName] {
            reply->deleteLater();
            nam->deleteLater();
            QString iconUrl;
            if (reply->error() == QNetworkReply::NoError) {
                auto root = QJsonDocument::fromJson(reply->readAll()).object();
                auto icon = root.value("team").toObject().value("icon").toObject();
                iconUrl   = icon.value("image_88").toString();
            }
            // Encode the Slack token into the neutral record's opaque blob — the
            // UI above the seam never touches slack::Credentials.
            emit succeeded(
                slack::toRecord(
                    slack::Credentials{xoxp, teamId, teamName, iconUrl, refreshToken, expiresAt}
                )
            );
        }
    );
}

} // namespace slack

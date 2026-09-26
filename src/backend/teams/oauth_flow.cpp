// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "oauth_flow.h"

#include "network/oauth_pkce.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

// Same custom-scheme redirect the OS routes back to the app (registered verbatim
// in the Entra app's "Mobile and desktop applications" platform).
static constexpr const char *kOAuthRedirectUri = "msga://oauth/callback";

namespace teams {

OAuthFlow::OAuthFlow(AppConfig app, QObject *parent)
    : auth::AuthStrategy(parent), _app(std::move(app)) {}

QString OAuthFlow::authorityBase() {
    // "organizations" = any work/school (Entra) tenant; Teams has no consumer API,
    // so personal Microsoft accounts are excluded. The signed-in user picks their
    // account, so home-realm discovery routes to the right tenant.
    return QStringLiteral("https://login.microsoftonline.com/organizations");
}

QString OAuthFlow::scopes() {
    // Delegated Graph scopes (space-separated, v2 endpoint). Several require
    // tenant-admin consent under Microsoft's managed default policy — an
    // un-consented tenant surfaces an admin-consent error the UI explains.
    return QStringLiteral(
        "openid profile offline_access "
        "User.Read User.ReadBasic.All User.ReadWrite "
        "Team.ReadBasic.All Channel.ReadBasic.All "
        "Chat.Read Chat.ReadWrite "
        "ChannelMessage.Read.All ChannelMessage.Send "
        "Presence.Read.All Presence.ReadWrite Files.ReadWrite.All"
    );
}

void OAuthFlow::start() {
    if (_app.clientId.isEmpty()) {
        // The client ID can be pasted at runtime (Settings → System → "Microsoft
        // Teams"), so lead with that — editing credentials.cmake only applies to
        // your own build.
        emit failed(
            QCoreApplication::translate(
                "teams::OAuthFlow",
                "No Microsoft Teams app is set up yet.\n\n"
                "Open Settings → System, find “Microsoft Teams”, and paste your Entra app's "
                "client ID. Building msga yourself? Put it in credentials.cmake as "
                "MSGA_TEAMS_CLIENT_ID instead and rebuild."
            )
        );
        return;
    }

    const auto pkce = net::oauth::makePkce();
    _codeVerifier   = pkce.verifier;
    _state          = pkce.state;

    QUrl      url(authorityBase() + QStringLiteral("/oauth2/v2.0/authorize"));
    QUrlQuery q;
    q.addQueryItem("client_id", _app.clientId);
    q.addQueryItem("response_type", "code");
    q.addQueryItem("redirect_uri", kOAuthRedirectUri);
    q.addQueryItem("response_mode", "query");
    q.addQueryItem("scope", scopes());
    q.addQueryItem("state", _state);
    q.addQueryItem("code_challenge", QString::fromLatin1(pkce.challenge));
    q.addQueryItem("code_challenge_method", "S256");
    q.addQueryItem("prompt", "select_account");
    url.setQuery(q);

    QDesktopServices::openUrl(url);
}

void OAuthFlow::handleCallbackUri(const QUrl &uri) {
    // Expected: msga://oauth/callback?code=…&state=…
    if (uri.scheme() != "msga" || uri.host() != "oauth" || uri.path() != "/callback")
        return;

    const auto cb = net::oauth::parseCallback(QUrlQuery(uri.query()), _state);
    if (!cb.error.isEmpty()) {
        // Surface the admin-consent case clearly; Entra returns error=
        // access_denied / consent_required with a description.
        emit failed(cb.errorDescription.isEmpty() ? cb.error : cb.errorDescription);
        return;
    }
    exchangeCode(cb.code);
}

void OAuthFlow::exchangeCode(const QString &code) {
    QUrlQuery params;
    params.addQueryItem("client_id", _app.clientId);
    params.addQueryItem("grant_type", "authorization_code");
    params.addQueryItem("code", code);
    params.addQueryItem("redirect_uri", kOAuthRedirectUri);
    params.addQueryItem("code_verifier", _codeVerifier); // PKCE (no client_secret — public client)
    params.addQueryItem("scope", scopes());

    const QUrl tokenUrl(authorityBase() + QStringLiteral("/oauth2/v2.0/token"));
    net::oauth::postForm(this, tokenUrl, params, [this](QNetworkReply *reply) {
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (obj.contains("error")) {
            const QString desc = obj.value("error_description").toString();
            emit          failed(desc.isEmpty() ? obj.value("error").toString("unknown") : desc);
            return;
        }
        const qint64 expiresIn = obj.value("expires_in").toInteger(0);
        const qint64 expiresAt = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
        finish(
            obj.value("access_token").toString(),
            obj.value("refresh_token").toString(),
            expiresAt,
            obj.value("id_token").toString()
        );
    });
}

namespace {
// Decode a JWT payload (the middle segment) without verifying the signature —
// we only read non-sensitive claims (tid, oid) from a token Microsoft just
// issued to us over TLS.
QJsonObject jwtPayload(const QString &jwt) {
    const auto parts = jwt.split(QLatin1Char('.'));
    if (parts.size() < 2)
        return {};
    const auto payload = QByteArray::fromBase64(parts[1].toLatin1(), QByteArray::Base64UrlEncoding);
    return QJsonDocument::fromJson(payload).object();
}
} // namespace

void OAuthFlow::finish(
    const QString &accessToken,
    const QString &refreshToken,
    qint64         expiresAt,
    const QString &idToken
) {
    const auto    claims   = jwtPayload(idToken);
    const QString tenantId = claims.value("tid").toString();
    const QString userId   = claims.value("oid").toString();
    // Fallback workspace name from the UPN domain; replaced by the org's real
    // displayName once /organization returns.
    const QString upn      = claims.value("preferred_username").toString();
    const QString domain   = upn.contains('@') ? upn.section('@', 1) : upn;

    // Look up the organization's display name (and skip icon for now — the org
    // photo needs a separate, optional call). Falls back to the UPN domain.
    auto           *nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QStringLiteral("https://graph.microsoft.com/v1.0/organization")));
    req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(accessToken).toUtf8());
    auto *reply = nam->get(req);
    connect(
        reply,
        &QNetworkReply::finished,
        this,
        [this, reply, nam, accessToken, refreshToken, expiresAt, tenantId, userId, domain] {
            reply->deleteLater();
            nam->deleteLater();
            QString orgName = domain;
            if (reply->error() == QNetworkReply::NoError) {
                const auto arr =
                    QJsonDocument::fromJson(reply->readAll()).object().value("value").toArray();
                if (!arr.isEmpty()) {
                    const auto name = arr.first().toObject().value("displayName").toString();
                    if (!name.isEmpty())
                        orgName = name;
                }
            }
            emit succeeded(
                teams::toRecord(
                    teams::Credentials{
                        accessToken,
                        tenantId,
                        orgName,
                        /*iconUrl*/ {},
                        refreshToken,
                        expiresAt,
                        userId
                    }
                )
            );
        }
    );
}

} // namespace teams

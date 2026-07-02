// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "oauth_loopback.h"

#include "network/form_urlencode.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QDesktopServices>
#include <QUrl>
#include <QUrlQuery>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QCoreApplication>

OAuthLoopbackFlow::OAuthLoopbackFlow(OAuthConfig cfg, QObject *parent)
    : QObject(parent), _cfg(std::move(cfg)) {}

QString OAuthLoopbackFlow::redirectUri() const {
    return QStringLiteral("http://localhost:%1%2").arg(_cfg.port).arg(_cfg.callbackPath);
}

void OAuthLoopbackFlow::start() {
    _finished = false;

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

    _server = new QTcpServer(this);
    if (!_server->listen(QHostAddress::LocalHost, _cfg.port)) {
        emit failed(
            tr("Could not listen on port %1: %2").arg(_cfg.port).arg(_server->errorString())
        );
        _server->deleteLater();
        _server = nullptr;
        return;
    }
    connect(_server, &QTcpServer::newConnection, this, &OAuthLoopbackFlow::onNewConnection);

    QUrl      url(_cfg.authorizeUrl);
    QUrlQuery q;
    q.addQueryItem("response_type", "code");
    q.addQueryItem("client_id", _cfg.clientId);
    q.addQueryItem("redirect_uri", redirectUri());
    q.addQueryItem("scope", _cfg.scopes);
    q.addQueryItem("state", _state);
    q.addQueryItem("code_challenge", QString::fromLatin1(challenge));
    q.addQueryItem("code_challenge_method", "S256");
    for (const auto &[k, v] : _cfg.extraAuthParams)
        q.addQueryItem(k, v);
    url.setQuery(q);

    QDesktopServices::openUrl(url);
}

void OAuthLoopbackFlow::cancel() {
    _finished = true;
    if (_server) {
        _server->close();
        _server->deleteLater();
        _server = nullptr;
    }
}

void OAuthLoopbackFlow::onNewConnection() {
    auto *sock = _server->nextPendingConnection();
    if (!sock)
        return;

    connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
        // Only the request line matters: "GET /callback?code=…&state=… HTTP/1.1"
        if (!sock->canReadLine())
            return;
        const QByteArray        line  = sock->readLine();
        const QList<QByteArray> parts = line.split(' ');
        const QString target = parts.size() >= 2 ? QString::fromLatin1(parts[1]) : QString();
        const QUrl    url("http://localhost" + target);

        const QString    page = tr("You can close this window and return to msga.");
        const QByteArray body =
            ("<html><body style=\"font-family:sans-serif;padding:40px\">" + page.toUtf8() +
             "</body></html>");
        sock->write(
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body
        );
        sock->flush();
        sock->disconnectFromHost();

        if (_finished || url.path() != _cfg.callbackPath)
            return;
        _finished = true;
        _server->close();

        QUrlQuery q(url.query());
        if (q.hasQueryItem("error")) {
            emit failed(q.queryItemValue("error"));
            return;
        }
        if (q.queryItemValue("state") != _state) {
            emit failed("state_mismatch");
            return;
        }
        exchangeCode(q.queryItemValue("code"));
    });
    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
}

void OAuthLoopbackFlow::exchangeCode(const QString &code) {
    postTokenRequest({
        {"grant_type", "authorization_code"},
        {"code", code},
        {"redirect_uri", redirectUri()},
        {"client_id", _cfg.clientId},
        {"code_verifier", _codeVerifier},
        {"state", _state},
    });
}

void OAuthLoopbackFlow::refresh(const QString &refreshToken) {
    postTokenRequest({
        {"grant_type", "refresh_token"},
        {"refresh_token", refreshToken},
        {"client_id", _cfg.clientId},
    });
}

void OAuthLoopbackFlow::postTokenRequest(const QList<QPair<QString, QString>> &paramsIn) {
    QList<QPair<QString, QString>> params = paramsIn;
    if (!_cfg.clientSecret.isEmpty()) // Google "Desktop app" clients require it
        params.append({QStringLiteral("client_secret"), _cfg.clientSecret});

    auto           *nam = new QNetworkAccessManager(this);
    QNetworkRequest req((QUrl(_cfg.tokenUrl)));

    QByteArray payload;
    if (_cfg.jsonTokenRequest) {
        QJsonObject obj;
        for (const auto &[k, v] : params)
            obj[k] = v;
        payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    } else {
        QUrlQuery q;
        for (const auto &[k, v] : params)
            q.addQueryItem(k, v);
        payload = net::formUrlEncode(q);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    }

    auto *reply = nam->post(req, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam] {
        reply->deleteLater();
        nam->deleteLater();
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            const QString detail = obj.value("error_description")
                                       .toString(obj.value("error").toString(reply->errorString()));
            emit failed(detail);
            return;
        }
        if (!obj.contains("access_token")) {
            emit failed(obj.value("error").toString("no_access_token"));
            return;
        }
        emit done(obj);
    });
}

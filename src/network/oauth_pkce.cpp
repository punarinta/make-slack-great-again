// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "oauth_pkce.h"

#include "network/form_urlencode.h"

#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

namespace net::oauth {

Pkce makePkce() {
    Pkce       pkce;
    // PKCE: 32 random bytes → base64url (43 chars, within RFC 7636's 43–128 range)
    QByteArray verifierBytes(32, '\0');
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(verifierBytes.data()), 8);
    pkce.verifier = QString::fromLatin1(
        verifierBytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
    );
    pkce.challenge = QCryptographicHash::hash(pkce.verifier.toLatin1(), QCryptographicHash::Sha256)
                         .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    pkce.state     = QString::number(QRandomGenerator::global()->generate64(), 16);
    return pkce;
}

Callback parseCallback(const QUrlQuery &q, const QString &expectedState) {
    Callback cb;
    if (q.hasQueryItem("error")) {
        cb.error            = q.queryItemValue("error");
        cb.errorDescription = q.queryItemValue("error_description");
        return cb;
    }
    if (q.queryItemValue("state") != expectedState) {
        cb.error = QStringLiteral("state_mismatch");
        return cb;
    }
    cb.code = q.queryItemValue("code");
    return cb;
}

void post(
    QObject                             *ctx,
    const QNetworkRequest               &req,
    const QByteArray                    &payload,
    std::function<void(QNetworkReply *)> done
) {
    auto *nam   = new QNetworkAccessManager(ctx);
    auto *reply = nam->post(req, payload);
    QObject::connect(reply, &QNetworkReply::finished, ctx, [reply, nam, done = std::move(done)] {
        reply->deleteLater();
        nam->deleteLater();
        done(reply);
    });
}

void postForm(
    QObject                             *ctx,
    const QUrl                          &url,
    const QUrlQuery                     &params,
    std::function<void(QNetworkReply *)> done
) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    post(ctx, req, net::formUrlEncode(params), std::move(done));
}

} // namespace net::oauth

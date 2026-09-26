// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// The OAuth 2.0 authorization-code + PKCE plumbing every sign-in flow shares:
// the Slack and Teams strategies (msga:// redirect delivered by the OS) and
// OAuthLoopbackFlow (http://localhost redirect, used for IMAP XOAUTH2). Each
// flow keeps its own endpoints, scopes, redirect and result handling.
#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

class QNetworkReply;
class QNetworkRequest;
class QObject;
class QUrl;
class QUrlQuery;

namespace net::oauth {

// A fresh PKCE pair (RFC 7636, S256) plus the anti-CSRF `state` value.
struct Pkce {
    QString    verifier;  // 32 random bytes, base64url (43 chars)
    QByteArray challenge; // base64url(SHA-256(verifier))
    QString    state;
};
Pkce makePkce();

// The redirect's query, checked in the order every flow uses: a provider
// `error` wins, then a `state` that isn't ours ("state_mismatch"), else `code`.
struct Callback {
    QString code;
    QString error;            // empty = success
    QString errorDescription; // the provider's error_description, if any
};
Callback parseCallback(const QUrlQuery &query, const QString &expectedState);

// POST `payload` on a throwaway QNetworkAccessManager and hand the finished
// reply to `done` (delivered in `ctx`'s thread, dropped once `ctx` is gone).
// The reply and its manager are deleted after `done` returns.
void post(
    QObject                             *ctx,
    const QNetworkRequest               &req,
    const QByteArray                    &payload,
    std::function<void(QNetworkReply *)> done
);
// post() with an application/x-www-form-urlencoded body.
void postForm(
    QObject                             *ctx,
    const QUrl                          &url,
    const QUrlQuery                     &params,
    std::function<void(QNetworkReply *)> done
);

} // namespace net::oauth

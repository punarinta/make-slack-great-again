// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QQueue>
#include <functional>

class QHttpMultiPart;

// Simple Slack Web API client.
// All calls share a single in-flight slot to stay well under rate limits.
// On HTTP 429, honors Retry-After then re-queues the failed call.
//
// Transport failures are classified and retried automatically:
//  - Safe (the request never reached Slack — connection refused, DNS, TLS
//    handshake): retried with backoff, for every call.
//  - Ambiguous (the request may have been processed but the response was
//    lost — timeout, connection dropped mid-flight, HTTP 5xx): retried for
//    idempotent calls; non-idempotent calls (callNonIdempotent) instead get
//    onError(kConnectionLost) so the caller can reconcile against server
//    state before resending — blind resends duplicate messages.
//  - Anything else fails the call as before.
// Retries are unlimited (the queue simply waits out an offline stretch);
// the first retry is immediate (stale kept-alive connection case), then
// delays double from 1 s up to 60 s.
class WebApiClient : public QObject {
    Q_OBJECT
public:
    static const QString kBaseUrl;
    // onError value for an ambiguous transport failure of a non-idempotent
    // call: the request MAY have been processed — reconcile, don't resend.
    static const QString kConnectionLost;

    explicit WebApiClient(QObject *parent = nullptr);

    void               setToken(const QString &token);
    [[nodiscard]] bool hasToken() const;

    // Override base URL per instance (tests only — production uses kBaseUrl).
    void setBaseUrl(const QString &url);

    // Pre-warm the TLS connection so the first API call skips the handshake.
    void preWarm(const QString &host);

    using OnSuccess = std::function<void(QJsonObject)>;
    using OnError   = std::function<void(QString)>;

    // Called when the server returns token_expired.  The handler must obtain a
    // new token, call setToken(), then invoke the supplied done(success) callback
    // to resume (true) or abort (false) the queue.
    using OnTokenExpired = std::function<void(std::function<void(bool)>)>;
    void setOnTokenExpired(OnTokenExpired fn);

    // Single-call: fires onSuccess with the full response object.
    // quietErrors: suppress the generic Slack-error warning — for call sites
    // that expect routine failures and do their own logging.
    void call(
        const QString &method,
        QUrlQuery      params,
        OnSuccess      onSuccess,
        OnError        onError     = {},
        bool           quietErrors = false
    );

    // Like call(), but for methods whose effect must not be applied twice
    // (chat.postMessage & co). Ambiguous transport failures are NOT retried;
    // onError(kConnectionLost) is fired instead.
    void callNonIdempotent(
        const QString &method, QUrlQuery params, OnSuccess onSuccess, OnError onError = {}
    );

    // Paginated load: fires onPage for each page's array items, then onDone.
    // Follows next_cursor until exhausted.
    void paginate(
        const QString                  &method,
        const QString                  &arrayKey,
        QUrlQuery                       params,
        std::function<void(QJsonArray)> onPage,
        std::function<void()>           onDone,
        OnError                         onError = {}
    );

    // POST a Slack API method with a JSON body (for calls that take JSON, e.g.
    // files.completeUploadExternal). Goes through the rate-limit queue.
    // Treated as non-idempotent (JSON-body methods create things) — ambiguous
    // transport failures surface as kConnectionLost instead of being retried.
    void postJson(
        const QString &method, const QJsonObject &body, OnSuccess onSuccess, OnError onError = {}
    );

    // Tests only: shrink the retry backoff / transfer timeout.
    void setRetryBaseDelayMs(int ms) { _retryBaseDelayMs = ms; }
    void setTransferTimeoutMs(int ms) { _transferTimeoutMs = ms; }

    // Raw POST of a body to an external URL (Slack file upload URL — the docs
    // require POST, raw bytes allowed). No auth header. Bypasses the API queue.
    void rawPost(
        const QUrl &url, const QByteArray &data, std::function<void()> onDone, OnError onError = {}
    );

    // POST multipart/form-data to a Slack API method with the auth token (for
    // users.setPhoto — file upload). Takes ownership of `parts`. Bypasses the
    // rate-limit queue; onSuccess fires with the parsed ok:true response.
    void postMultipart(
        const QString &method, QHttpMultiPart *parts, OnSuccess onSuccess, OnError onError = {}
    );

    // GET an arbitrary URL with the auth token set. For downloading private files.
    // Bypasses the API queue.
    void downloadUrl(const QUrl &url, std::function<void(QByteArray)> onData, OnError onError = {});

private:
    struct PendingCall {
        QString     method;
        QUrlQuery   params;
        QJsonObject jsonBody; // non-empty → POST JSON instead of GET with params
        OnSuccess   onSuccess;
        OnError     onError;
        bool        quietErrors      = false; // skip the generic Slack-error warning
        bool        idempotent       = true;  // safe to resend after an ambiguous failure
        int         transportRetries = 0;     // transport-failure retries so far
    };

    void              enqueue(PendingCall c);
    void              tryNext();
    void              execute(const PendingCall &c);
    void              handleReply(QNetworkReply *reply, PendingCall c);
    // 0 for the first retry (stale kept-alive connection — a fresh one
    // usually succeeds immediately), then base*2^n capped at one minute.
    [[nodiscard]] int retryDelayMs(int attempt) const;

    QNetworkAccessManager *_nam;
    QString                _token;
    QString                _baseUrl{kBaseUrl};
    QQueue<PendingCall>    _queue;
    bool                   _inflight = false;
    bool           _throttled = false; // true while waiting out a Retry-After or token refresh
    int            _retryBaseDelayMs  = 1000;
    int            _transferTimeoutMs = 30000; // abort if no bytes move for this long
    OnTokenExpired _onTokenExpired;
};

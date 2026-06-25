// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QUrlQuery>
#include <functional>

namespace net {

// Protocol-agnostic queued HTTP client — the transport mechanics shared by any
// HTTP-based backend (Slack today, Teams next). Holds NO API semantics: no base
// URL convention, no `ok:false` envelope, no pagination. A concrete API client
// (e.g. slack::WebApiClient) subclasses this and implements handleResponse() to
// interpret a transport-successful reply.
//
// What lives here (generic by construction):
//  - a single in-flight slot + FIFO queue (keeps callers well under rate limits)
//  - a no-progress transfer timeout (a dead connection can't hang the queue)
//  - transport-failure classification: Safe (request never reached the server —
//    refused/DNS/TLS) retries any call; Ambiguous (timeout/dropped/5xx) retries
//    only idempotent calls and fails non-idempotent ones with kConnectionLost so
//    the caller can reconcile before resending; Fatal fails the call
//  - exponential backoff (first retry immediate, then 1s doubling to 60s)
//  - the idempotent-vs-non-idempotent POST handling (Qt silently retransmits a
//    GET whose connection died mid-flight, double-applying write methods)
//  - HTTP 429 Retry-After handling (a transport-standard backpressure signal)
//  - a token-refresh pause/resume mechanism (the *trigger* is API-specific and
//    lives in the subclass; the queue choreography is generic)
class HttpQueue : public QObject {
    Q_OBJECT
public:
    // onError value for an ambiguous transport failure of a non-idempotent
    // call: the request MAY have been processed — reconcile, don't resend.
    static const QString kConnectionLost;

    explicit HttpQueue(QObject *parent = nullptr);

    void               setToken(const QString &token) { _token = token; }
    [[nodiscard]] bool hasToken() const { return !_token.isEmpty(); }

    // The API base URL every queued method is appended to (tests override it).
    void setBaseUrl(const QString &url) { _baseUrl = url; }

    // Pre-warm the TLS connection so the first call skips the handshake.
    void preWarm(const QString &host) { _nam->connectToHostEncrypted(host, 443); }

    using OnSuccess = std::function<void(QJsonObject)>;
    using OnError   = std::function<void(QString)>;

    // Called when the server returns the API's "auth expired" signal. The
    // handler obtains a new token, calls setToken(), then invokes done(success)
    // to resume (true) or abort (false) the queue. The subclass decides *when*
    // to invoke it (see pauseForTokenRefresh).
    using OnTokenExpired = std::function<void(std::function<void(bool)>)>;
    void setOnTokenExpired(OnTokenExpired fn) { _onTokenExpired = std::move(fn); }

    // Fail every queued (not-yet-sent) call with `error`, emptying the queue.
    // The in-flight call (if any) is left alone. See the long note at the .cpp.
    void failAllPending(const QString &error);

    // Tests only: shrink the retry backoff / transfer timeout.
    void setRetryBaseDelayMs(int ms) { _retryBaseDelayMs = ms; }
    void setTransferTimeoutMs(int ms) { _transferTimeoutMs = ms; }

    // Raw POST of a body to an external URL. No auth header. Bypasses the queue.
    void rawPost(
        const QUrl &url, const QByteArray &data, std::function<void()> onDone, OnError onError = {}
    );

    // GET an arbitrary URL with the auth token set. Bypasses the queue.
    void downloadUrl(const QUrl &url, std::function<void(QByteArray)> onData, OnError onError = {});

signals:
    // A call hit HTTP 429 and was requeued for `retryAfterSecs`. The call still
    // completes (transparently retried), so this is informational — the UI can
    // surface a transient "rate-limited" notice. `method` is the API method that
    // was throttled (e.g. "users.getPresence").
    void rateLimited(const QString &method, int retryAfterSecs);

protected:
    struct PendingCall {
        QString     method;
        QUrlQuery   params;
        QJsonObject jsonBody; // non-empty → POST JSON instead of GET with params
        OnSuccess   onSuccess;
        OnError     onError;
        bool        quietErrors      = false; // skip the generic error warning
        bool        idempotent       = true;  // safe to resend after an ambiguous failure
        int         transportRetries = 0;     // transport/transient retries so far
    };

    void enqueue(PendingCall c);
    void tryNext();

    // Interpret a reply that completed without a transport error. The base
    // class has already handled 429 and transport failures; `obj` is the parsed
    // JSON body. Default: fire onSuccess(obj) and advance the queue. Subclasses
    // override to apply their API's success/error envelope.
    virtual void handleResponse(const QJsonObject &obj, PendingCall c);

    // ── Primitives for handleResponse overrides ─────────────────────────────
    // Requeue `c` at the FRONT, pause the queue for delayMs, then resume.
    void              requeueWithDelay(PendingCall c, int delayMs);
    // Pause, requeue `c`, run the token-refresh handler, then resume on success
    // or drain the queue with `error` on failure.
    void              pauseForTokenRefresh(PendingCall c, const QString &error);
    // Exponential backoff for retry attempt n (0/1 → immediate, then base*2^n
    // capped at 60s).
    [[nodiscard]] int retryDelayMs(int attempt) const;
    // Resume after a self-managed pause (clears throttle, pumps the queue).
    void              advance();

    [[nodiscard]] QNetworkAccessManager *nam() const { return _nam; }
    [[nodiscard]] const QString         &token() const { return _token; }
    [[nodiscard]] const QString         &baseUrl() const { return _baseUrl; }
    [[nodiscard]] bool hasTokenExpiredHandler() const { return static_cast<bool>(_onTokenExpired); }

private:
    void execute(const PendingCall &c);
    void handleReply(QNetworkReply *reply, PendingCall c);

    QNetworkAccessManager *_nam;
    QString                _token;
    QString                _baseUrl;
    QQueue<PendingCall>    _queue;
    bool                   _inflight          = false;
    bool                   _throttled         = false; // waiting out a backoff / refresh
    int                    _retryBaseDelayMs  = 1000;
    int                    _transferTimeoutMs = 30000;
    OnTokenExpired         _onTokenExpired;
};

} // namespace net

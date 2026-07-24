// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "http_queue.h"

#include "form_urlencode.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QDebug>

#include <limits>

namespace net {

const QString HttpQueue::kConnectionLost = "connection_lost";

namespace {

// Did the failed request definitely never get processed by the server (Safe to
// resend anything), possibly get processed with only the response lost
// (Ambiguous — resending a non-idempotent call could duplicate its effect), or
// fail in a way retrying won't fix (Fatal)?
enum class FailureClass { Safe, Ambiguous, Fatal };

FailureClass classifyTransportError(QNetworkReply::NetworkError e) {
    switch (e) {
    // The connection was never established — nothing reached the server.
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::SslHandshakeFailedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
        return FailureClass::Safe;
    // The request may have been sent and processed; only the response is known
    // to be missing. Includes HTTP 5xx — servers document that parts of an
    // operation can succeed before an internal error is raised.
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError: // transfer-timeout abort
    case QNetworkReply::ProtocolFailure:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::InternalServerError:
    case QNetworkReply::ServiceUnavailableError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::UnknownProxyError:
    case QNetworkReply::UnknownServerError:
        return FailureClass::Ambiguous;
    default:
        return FailureClass::Fatal;
    }
}

} // namespace

HttpQueue::HttpQueue(QObject *parent) : QObject(parent), _nam(new QNetworkAccessManager(this)) {}

void HttpQueue::applyAuth(QNetworkRequest &req) const {
    req.setRawHeader("Authorization", ("Bearer " + _token).toUtf8());
    // Session auth requires the `d` cookie on every call; the token alone is
    // rejected. Sent verbatim (the stored value already carries its `xoxd-` prefix).
    if (!_cookie.isEmpty()) {
        req.setRawHeader("Cookie", ("d=" + _cookie).toUtf8());
        // QNAM's default cookie handling (CookieLoadControlAttribute == Automatic)
        // rebuilds the Cookie header from its own (empty) jar and clobbers the one
        // set above — so without this the cookie never goes out and Slack answers
        // invalid_auth. Manual mode tells QNAM to leave our header alone (and not
        // to harvest Set-Cookie into the jar).
        req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    }
}

void HttpQueue::failAllPending(const QString &error) {
    // Drain into a local queue first: an onError handler may legitimately
    // re-enqueue (a retry), and we must not loop over our own _queue while it's
    // being appended to.
    QQueue<PendingCall> pending;
    pending.swap(_queue);
    _throttled = false; // nothing left to wait for
    while (!pending.isEmpty()) {
        auto c = pending.dequeue();
        if (c.onError)
            c.onError(error);
    }
}

void HttpQueue::rawPost(
    const QUrl &url, const QByteArray &data, std::function<void()> onDone, OnError onError
) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );
    req.setTransferTimeout(_transferTimeoutMs);
    auto *reply = _nam->post(req, data);
    connect(reply, &QNetworkReply::finished, this, [reply, onDone, onError]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (onError)
                onError(reply->errorString());
        } else {
            if (onDone)
                onDone();
        }
    });
}

void HttpQueue::downloadUrl(
    const QUrl &url, std::function<void(QByteArray)> onData, OnError onError
) {
    QNetworkRequest req(url);
    applyAuth(req);
    req.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );
    req.setTransferTimeout(_transferTimeoutMs);
    auto *reply = _nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, onData, onError]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (onError)
                onError(reply->errorString());
        } else {
            if (onData)
                onData(reply->readAll());
        }
    });
}

void HttpQueue::enqueue(PendingCall c) {
    _queue.enqueue(std::move(c));
    tryNext();
}

void HttpQueue::tryNext() {
    if (_inflight || _throttled || _queue.isEmpty())
        return;
    // Run the first eligible queued call whose method isn't in a 429 cooldown. A
    // throttled method is skipped (not blocking), so other methods keep flowing;
    // same-method calls keep their relative order because we always take the
    // earliest eligible one.
    //
    // Two lanes, Normal before Background. Interactive work must never wait behind
    // the paced background sweep: we only dip into the Background lane when NO
    // Normal call is queued at all — a Normal call that's merely cooling down on a
    // 429 still parks the background lane so it isn't overtaken.
    const qint64 now           = QDateTime::currentMSecsSinceEpoch();
    int          idx           = -1;
    qint64       soonest       = std::numeric_limits<qint64>::max();
    bool         normalWaiting = false;
    for (int i = 0; i < _queue.size(); ++i) {
        if (_queue.at(i).priority != Priority::Normal)
            continue;
        normalWaiting        = true;
        const qint64 readyAt = _methodReadyAtMs.value(_queue.at(i).method, 0);
        if (readyAt <= now) {
            idx = i;
            break;
        }
        soonest = std::min(soonest, readyAt);
    }
    if (idx < 0 && !normalWaiting) {
        for (int i = 0; i < _queue.size(); ++i) {
            // Everything here is Background. Gate on both the method's 429 cooldown
            // and the global pace so the lane trickles out at ≤ one call per
            // _backgroundPaceMs.
            const qint64 methodReady = _methodReadyAtMs.value(_queue.at(i).method, 0);
            const qint64 readyAt     = std::max(methodReady, _backgroundReadyAtMs);
            if (readyAt <= now) {
                idx = i;
                break;
            }
            soonest = std::min(soonest, readyAt);
        }
    }
    if (idx < 0) {
        // Nothing runnable right now — wake when the soonest cooldown/pace clears.
        if (soonest != std::numeric_limits<qint64>::max())
            scheduleWake(soonest);
        return;
    }
    // Arm the pace gate before dispatching a Background call, so the next one waits
    // out the interval no matter how quickly this reply lands.
    if (_queue.at(idx).priority == Priority::Background && _backgroundPaceMs > 0)
        _backgroundReadyAtMs = now + _backgroundPaceMs;
    _inflight = true;
    execute(_queue.takeAt(idx));
}

void HttpQueue::scheduleWake(qint64 targetMs) {
    if (_scheduledWakeMs >= 0 && _scheduledWakeMs <= targetMs)
        return; // a wake at or before this instant is already pending
    _scheduledWakeMs = targetMs;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int delay = static_cast<int>(qBound<qint64>(qint64(0), targetMs - now, qint64(3600'000)));
    QTimer::singleShot(delay, this, [this, targetMs] {
        if (_scheduledWakeMs == targetMs)
            _scheduledWakeMs = -1;
        tryNext();
    });
}

void HttpQueue::advance() {
    tryNext();
}

void HttpQueue::requeueWithDelay(PendingCall c, int delayMs) {
    _throttled = true;
    _queue.prepend(std::move(c));
    QTimer::singleShot(delayMs, this, [this] {
        _throttled = false;
        tryNext();
    });
}

void HttpQueue::execute(const PendingCall &c) {
    QUrl            url(_baseUrl + c.method);
    QNetworkRequest req;
    applyAuth(req);
    req.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );
    // Without this a dead connection hangs the single-slot queue indefinitely
    // (and the message that's "sending" stays translucent forever). The timer
    // resets whenever bytes move, so it only fires on a genuinely stuck call.
    req.setTransferTimeout(_transferTimeoutMs);

    QNetworkReply *reply = nullptr;
    if (!c.jsonBody.isEmpty()) {
        // POST with JSON body
        url.setQuery(QUrlQuery{});
        req.setUrl(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = _nam->post(req, QJsonDocument(c.jsonBody).toJson(QJsonDocument::Compact));
    } else if (!c.idempotent) {
        // POST with form body. Qt's HTTP stack transparently retransmits GET
        // requests when a connection dies mid-flight (HTTP deems GET safe to
        // repeat) — for a write method that re-applies the call and, for
        // chat.postMessage, duplicates the message. POSTs are never
        // auto-retransmitted, so the ambiguity surfaces here and is handled by
        // the classification in handleReply instead of inside Qt.
        url.setQuery(QUrlQuery{});
        req.setUrl(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        reply = _nam->post(req, net::formUrlEncode(c.params));
    } else {
        // GET with query params. Slack parses query strings with form rules
        // (literal '+' = space), so these need formUrlEncode too — QUrl keeps
        // the %2B escapes intact.
        url.setQuery(QString::fromUtf8(net::formUrlEncode(c.params)), QUrl::TolerantMode);
        req.setUrl(url);
        reply = _nam->get(req);
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, c]() mutable {
        handleReply(reply, std::move(c));
    });
}

void HttpQueue::handleReply(QNetworkReply *reply, PendingCall c) {
    reply->deleteLater();
    _inflight = false;

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (httpStatus == 429) {
        int retryAfter = reply->rawHeader("Retry-After").toInt();
        retryAfter     = qMax(retryAfter, 1);
        qDebug() << "HttpQueue: rate-limited on" << c.method << "retrying in" << retryAfter << "s";
        emit rateLimited(c.method, retryAfter);
        // Per-method backpressure (Slack throttles per method): only THIS method
        // waits out Retry-After. Requeue it at the front so it stays ahead of any
        // later same-method calls, mark its cooldown, then pump — other methods
        // run immediately instead of blocking behind the throttled one.
        _methodReadyAtMs[c.method] =
            QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(retryAfter) * 1000;
        _queue.prepend(std::move(c));
        tryNext();
        return;
    }

    // The server responded with a definitive client-error status (4xx). Qt flags
    // this as reply->error(), but it is NOT a transport failure — the body carries
    // the API's error envelope. Hand it to handleResponse so an HTTP-status API
    // (e.g. Microsoft Graph's @odata.error / 401 token refresh) can interpret it,
    // rather than classifying it as a retryable transport error. Slack never takes
    // this path (its errors are HTTP 200 + ok:false), so its behavior is unchanged.
    // 5xx and genuine transport failures (no HTTP status) fall through to the
    // classification/retry logic below.
    if (httpStatus >= 400 && httpStatus < 500) {
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        handleResponse(obj, std::move(c));
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        const FailureClass cls = classifyTransportError(reply->error());
        const bool         retryable =
            cls == FailureClass::Safe || (cls == FailureClass::Ambiguous && c.idempotent);
        if (retryable) {
            c.transportRetries++;
            // A kept-alive connection that the server (or a suspend cycle)
            // silently killed surfaces as RemoteHostClosedError / ProtocolFailure
            // on reuse — drop the cached connections so the retry runs on a fresh
            // one.
            if (c.transportRetries == 1)
                _nam->clearConnectionCache();
            const int delay = retryDelayMs(c.transportRetries);
            qDebug() << "HttpQueue: transport error" << reply->errorString() << "on" << c.method
                     << "— retry" << c.transportRetries << "in" << delay << "ms";
            requeueWithDelay(std::move(c), delay);
            return;
        }
        qWarning() << "HttpQueue error:" << reply->errorString() << "on" << c.method;
        if (c.onError)
            c.onError(cls == FailureClass::Ambiguous ? kConnectionLost : reply->errorString());
        tryNext();
        return;
    }

    const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
    handleResponse(obj, std::move(c));
}

void HttpQueue::handleResponse(const QJsonObject &obj, PendingCall c) {
    // Generic default: any reply we received is a success. API-specific clients
    // override this to apply their success/error envelope.
    if (c.onSuccess)
        c.onSuccess(obj);
    tryNext();
}

void HttpQueue::pauseForTokenRefresh(PendingCall c, const QString &error) {
    _throttled = true;
    _queue.prepend(c);
    _onTokenExpired([this, error](bool success) {
        _throttled = false;
        if (success) {
            tryNext();
        } else {
            // Drain the queue with errors so callers aren't stuck.
            while (!_queue.isEmpty()) {
                auto failed = _queue.dequeue();
                if (failed.onError)
                    failed.onError(error);
            }
        }
    });
}

int HttpQueue::retryDelayMs(int attempt) const {
    if (attempt <= 1)
        return 0; // first retry is immediate (a fresh socket usually recovers at once)
    constexpr int kMaxDelayMs = 60'000;
    int           d           = _retryBaseDelayMs;
    for (int i = 2; i < attempt && d < kMaxDelayMs; ++i)
        d *= 2;
    return qMin(d, kMaxDelayMs);
}

} // namespace net

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "web_api_client.h"

#include <QHttpMultiPart>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QTimer>
#include <QDebug>

const QString WebApiClient::kBaseUrl        = "https://slack.com/api/";
const QString WebApiClient::kConnectionLost = "connection_lost";

namespace {

// Did the failed request definitely never get processed by the server (Safe
// to resend anything), possibly get processed with only the response lost
// (Ambiguous — resending a non-idempotent call could duplicate its effect),
// or fail in a way retrying won't fix (Fatal)?
enum class FailureClass { Safe, Ambiguous, Fatal };

FailureClass classifyTransportError(QNetworkReply::NetworkError e) {
    switch (e) {
    // The connection was never established — nothing reached Slack.
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::SslHandshakeFailedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
        return FailureClass::Safe;
    // The request may have been sent and processed; only the response is
    // known to be missing. Includes HTTP 5xx — Slack documents that parts of
    // an operation can succeed before an internal error is raised.
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

// Slack-level failures (HTTP 200 + ok:false) that are transient server-side
// hiccups rather than a problem with the request itself. Slack documents these
// as "likely a transient issue on our end" — safe to retry an idempotent call,
// same class as an ambiguous transport failure.
bool isTransientSlackError(const QString &err) {
    return err == "internal_error" || err == "service_unavailable" || err == "fatal_error";
}

// Unlike a transport failure (genuinely offline → unlimited queueing is correct,
// the queue just drains when the network returns), a Slack-level "transient"
// error is an HTTP 200 with ok:false — Slack received and rejected the request.
// When it persists it is NOT a passing blip: the request itself is the problem
// (e.g. users.getPresence is rate-limited, "not intended for frequent/bulk use",
// and returns internal_error for ineligible users such as bot/app or deactivated
// accounts). Retrying forever just hammers a failing endpoint, so bound it and
// surface the error after ~1 min of backoff (1+2+4+8+16+32 s).
constexpr int kMaxTransientSlackRetries = 6;

} // namespace

WebApiClient::WebApiClient(QObject *parent)
    : QObject(parent), _nam(new QNetworkAccessManager(this)) {}

void WebApiClient::setToken(const QString &token) {
    _token = token;
}
bool WebApiClient::hasToken() const {
    return !_token.isEmpty();
}
void WebApiClient::setOnTokenExpired(OnTokenExpired fn) {
    _onTokenExpired = std::move(fn);
}

void WebApiClient::failAllPending(const QString &error) {
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

void WebApiClient::setBaseUrl(const QString &url) {
    _baseUrl = url;
}

void WebApiClient::preWarm(const QString &host) {
    _nam->connectToHostEncrypted(host, 443);
}

void WebApiClient::call(
    const QString &method, QUrlQuery params, OnSuccess onSuccess, OnError onError, bool quietErrors
) {
    enqueue({method, std::move(params), {}, std::move(onSuccess), std::move(onError), quietErrors});
}

void WebApiClient::callNonIdempotent(
    const QString &method, QUrlQuery params, OnSuccess onSuccess, OnError onError
) {
    PendingCall c{method, std::move(params), {}, std::move(onSuccess), std::move(onError)};
    c.idempotent = false;
    enqueue(std::move(c));
}

void WebApiClient::postJson(
    const QString &method, const QJsonObject &body, OnSuccess onSuccess, OnError onError
) {
    PendingCall c{method, {}, body, std::move(onSuccess), std::move(onError)};
    c.idempotent = false;
    enqueue(std::move(c));
}

void WebApiClient::rawPost(
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

void WebApiClient::postMultipart(
    const QString &method, QHttpMultiPart *parts, OnSuccess onSuccess, OnError onError
) {
    QNetworkRequest req(QUrl(_baseUrl + method));
    req.setRawHeader("Authorization", ("Bearer " + _token).toUtf8());
    req.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );
    req.setTransferTimeout(_transferTimeoutMs);
    auto *reply = _nam->post(req, parts);
    parts->setParent(reply); // multipart lives as long as the request
    connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (onError)
                onError(reply->errorString());
            return;
        }
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (!obj.value("ok").toBool()) {
            if (onError)
                onError(obj.value("error").toString("unknown"));
            return;
        }
        if (onSuccess)
            onSuccess(obj);
    });
}

void WebApiClient::downloadUrl(
    const QUrl &url, std::function<void(QByteArray)> onData, OnError onError
) {
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", ("Bearer " + _token).toUtf8());
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

void WebApiClient::paginate(
    const QString                  &method,
    const QString                  &arrayKey,
    QUrlQuery                       params,
    std::function<void(QJsonArray)> onPage,
    std::function<void()>           onDone,
    OnError                         onError
) {
    if (!params.hasQueryItem("limit"))
        params.addQueryItem("limit", "200");

    struct Ctx {
        WebApiClient                   *self;
        QString                         method;
        QString                         arrayKey;
        QUrlQuery                       params;
        std::function<void(QJsonArray)> onPage;
        std::function<void()>           onDone;
        OnError                         onError;
        std::function<void(QUrlQuery)>  loadPage;
    };
    auto ctx = std::make_shared<Ctx>(Ctx{
        this, method, arrayKey, params, std::move(onPage), std::move(onDone), std::move(onError)
    });

    // loadPage is a member of Ctx but captures only a *weak_ptr* to it, so it
    // never forms a strong self-cycle. Liveness across async pages rides on the
    // in-flight call's success/error closures below, which hold a shared_ptr:
    // each page schedules the next call (capturing a fresh shared ref) before
    // the current closure returns, so a strong owner always exists while the
    // chain is running. When the chain ends — normally, via error, or because
    // the client is torn down mid-flight and the pending call's closures are
    // released — the last shared ref drops and the whole Ctx (plus the partial
    // accumulator it captures) is freed. A *strong* self-capture here would
    // instead outlive an interrupted pagination forever, leaking the entire Ctx
    // graph with no GC root (an all-indirect LeakSanitizer report). call()
    // routes both transport failures and Slack `ok:false` responses to the
    // error handler, so that closure is the single place every failed page
    // lands.
    std::weak_ptr<Ctx> weak = ctx;
    ctx->loadPage           = [weak](QUrlQuery p) {
        auto ctx = weak.lock();
        if (!ctx)
            return;
        ctx->self->call(
            ctx->method,
            p,
            [ctx](QJsonObject resp) {
                // call() only invokes onSuccess on `ok:true`, so resp is always ok here.
                auto arr = resp.value(ctx->arrayKey).toArray();
                if (!arr.isEmpty())
                    ctx->onPage(arr);

                auto cursor =
                    resp.value("response_metadata").toObject().value("next_cursor").toString();
                if (!cursor.isEmpty()) {
                    auto next = ctx->params;
                    next.removeQueryItem("cursor");
                    next.addQueryItem("cursor", cursor);
                    ctx->loadPage(next);
                } else {
                    ctx->onDone();
                }
            },
            [ctx](QString err) {
                if (ctx->onError)
                    ctx->onError(err);
            }
        );
    };
    ctx->loadPage(params);
}

void WebApiClient::enqueue(PendingCall c) {
    _queue.enqueue(std::move(c));
    tryNext();
}

void WebApiClient::tryNext() {
    if (_inflight || _throttled || _queue.isEmpty())
        return;
    _inflight = true;
    execute(_queue.dequeue());
}

void WebApiClient::execute(const PendingCall &c) {
    QUrl            url(_baseUrl + c.method);
    QNetworkRequest req;
    req.setRawHeader("Authorization", ("Bearer " + _token).toUtf8());
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
        // repeat) — for a Slack write method that re-applies the call and,
        // for chat.postMessage, duplicates the message. POSTs are never
        // auto-retransmitted, so the ambiguity surfaces here and is handled
        // by the classification in handleReply instead of inside Qt.
        url.setQuery(QUrlQuery{});
        req.setUrl(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        reply = _nam->post(req, c.params.toString(QUrl::FullyEncoded).toUtf8());
    } else {
        // GET with query params
        url.setQuery(c.params);
        req.setUrl(url);
        reply = _nam->get(req);
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, c]() mutable {
        handleReply(reply, std::move(c));
    });
}

void WebApiClient::handleReply(QNetworkReply *reply, PendingCall c) {
    reply->deleteLater();
    _inflight = false;

    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 429) {
        int retryAfter = reply->rawHeader("Retry-After").toInt();
        retryAfter     = qMax(retryAfter, 1);
        qDebug() << "WebApiClient: rate-limited, retrying in" << retryAfter << "s";
        _throttled = true;
        _queue.prepend(c); // put back at front
        QTimer::singleShot(retryAfter * 1000, this, [this] {
            _throttled = false;
            tryNext();
        });
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        const FailureClass cls = classifyTransportError(reply->error());
        const bool         retryable =
            cls == FailureClass::Safe || (cls == FailureClass::Ambiguous && c.idempotent);
        if (retryable) {
            c.transportRetries++;
            // A kept-alive connection that the server (or a suspend cycle)
            // silently killed surfaces as RemoteHostClosedError /
            // ProtocolFailure on reuse — drop the cached connections so the
            // retry runs on a fresh one.
            if (c.transportRetries == 1)
                _nam->clearConnectionCache();
            const int delay = retryDelayMs(c.transportRetries);
            qDebug() << "WebApiClient: transport error" << reply->errorString() << "on" << c.method
                     << "— retry" << c.transportRetries << "in" << delay << "ms";
            _throttled = true;
            _queue.prepend(std::move(c));
            QTimer::singleShot(delay, this, [this] {
                _throttled = false;
                tryNext();
            });
            return;
        }
        qWarning() << "WebApiClient error:" << reply->errorString() << "on" << c.method;
        if (c.onError)
            c.onError(cls == FailureClass::Ambiguous ? kConnectionLost : reply->errorString());
        tryNext();
        return;
    }

    auto doc = QJsonDocument::fromJson(reply->readAll());
    auto obj = doc.object();

    if (!obj.value("ok").toBool()) {
        auto err = obj.value("error").toString("unknown");

        // A transient Slack-side error on an idempotent call: ride out a brief
        // blip with backoff instead of leaking a dropped call to the caller.
        // Retried at qDebug, not qWarning, so it doesn't spam the log. Bounded
        // (kMaxTransientSlackRetries) — once exhausted we fall through to the
        // normal error path so a *persistent* failure (a down/blocked endpoint,
        // or a request Slack keeps rejecting) surfaces instead of looping.
        if (c.idempotent && isTransientSlackError(err) &&
            c.transportRetries < kMaxTransientSlackRetries) {
            c.transportRetries++;
            const int delay = retryDelayMs(c.transportRetries);
            qDebug() << "WebApiClient: transient Slack error" << err << "on" << c.method
                     << "— retry" << c.transportRetries << "in" << delay << "ms";
            _throttled = true;
            _queue.prepend(std::move(c));
            QTimer::singleShot(delay, this, [this] {
                _throttled = false;
                tryNext();
            });
            return;
        }

        if (!c.quietErrors)
            qWarning() << "WebApiClient Slack error:" << err << "on" << c.method
                       << "| needed:" << obj.value("needed").toString()
                       << "| provided:" << obj.value("provided").toString();
        if (err == "token_expired" && _onTokenExpired) {
            qDebug() << "WebApiClient: token_expired on" << c.method
                     << "— pausing queue, re-queuing call";
            _throttled = true;
            _queue.prepend(c);
            _onTokenExpired([this](bool success) {
                _throttled = false;
                if (success) {
                    tryNext();
                } else {
                    // Drain queue with errors so callers aren't stuck
                    while (!_queue.isEmpty()) {
                        auto failed = _queue.dequeue();
                        if (failed.onError)
                            failed.onError("token_expired");
                    }
                }
            });
        } else {
            if (c.onError)
                c.onError(err);
            tryNext();
        }
        return;
    }

    if (c.onSuccess)
        c.onSuccess(obj);
    tryNext();
}

int WebApiClient::retryDelayMs(int attempt) const {
    if (attempt <= 1)
        return 0; // first retry is immediate (a fresh socket usually recovers at once)
    constexpr int kMaxDelayMs = 60'000;
    int           d           = _retryBaseDelayMs;
    for (int i = 2; i < attempt && d < kMaxDelayMs; ++i)
        d *= 2;
    return qMin(d, kMaxDelayMs);
}

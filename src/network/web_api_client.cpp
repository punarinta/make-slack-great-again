// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "web_api_client.h"

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

    // loadPage holds a shared_ptr to its own ctx (self-reference cycle that
    // keeps the pagination alive across async pages); it MUST be cleared on
    // every exit or the whole Ctx — and the partial accumulator it captures —
    // leaks. call() routes both transport failures and Slack `ok:false`
    // responses to the error handler, so this wrapper is the single place every
    // failed page lands: break the cycle, then forward the error.
    auto onPageError = [ctx](QString err) {
        ctx->loadPage = {};
        if (ctx->onError)
            ctx->onError(err);
    };
    ctx->loadPage = [ctx, onPageError](QUrlQuery p) {
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
                    ctx->loadPage = {};
                    ctx->onDone();
                }
            },
            onPageError
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
        return 0;
    constexpr int kMaxDelayMs = 60'000;
    int           d           = _retryBaseDelayMs;
    for (int i = 2; i < attempt && d < kMaxDelayMs; ++i)
        d *= 2;
    return qMin(d, kMaxDelayMs);
}

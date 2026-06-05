// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "web_api_client.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QTimer>
#include <QDebug>

const QString WebApiClient::kBaseUrl = "https://slack.com/api/";

WebApiClient::WebApiClient(QObject *parent)
    : QObject(parent)
    , _nam(new QNetworkAccessManager(this))
{}

void WebApiClient::setToken(const QString &token) { _token = token; }
bool WebApiClient::hasToken() const { return !_token.isEmpty(); }

void WebApiClient::call(const QString &method, QUrlQuery params,
                        OnSuccess onSuccess, OnError onError) {
    enqueue({ method, std::move(params), {}, std::move(onSuccess), std::move(onError) });
}

void WebApiClient::postJson(const QString &method, const QJsonObject &body,
                             OnSuccess onSuccess, OnError onError) {
    enqueue({ method, {}, body, std::move(onSuccess), std::move(onError) });
}

void WebApiClient::rawPut(const QUrl &url, const QByteArray &data,
                           std::function<void()> onDone, OnError onError) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    auto *reply = _nam->put(req, data);
    connect(reply, &QNetworkReply::finished, this, [reply, onDone, onError]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (onError) onError(reply->errorString());
        } else {
            if (onDone) onDone();
        }
    });
}

void WebApiClient::downloadUrl(const QUrl &url,
                                std::function<void(QByteArray)> onData,
                                OnError onError) {
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", ("Bearer " + _token).toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    auto *reply = _nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, onData, onError]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (onError) onError(reply->errorString());
        } else {
            if (onData) onData(reply->readAll());
        }
    });
}

void WebApiClient::paginate(const QString &method, const QString &arrayKey,
                             QUrlQuery params,
                             std::function<void(QJsonArray)> onPage,
                             std::function<void()> onDone,
                             OnError onError) {
    if (!params.hasQueryItem("limit"))
        params.addQueryItem("limit", "200");

    // Recursive lambda via shared_ptr so it can call itself.
    struct Ctx {
        WebApiClient *self;
        QString       method;
        QString       arrayKey;
        QUrlQuery     params;
        std::function<void(QJsonArray)> onPage;
        std::function<void()>           onDone;
        OnError                         onError;
    };
    auto ctx = std::make_shared<Ctx>(Ctx{
        this, method, arrayKey, params,
        std::move(onPage), std::move(onDone), std::move(onError)
    });

    std::function<void(QUrlQuery)> loadPage;
    loadPage = [ctx, loadPage](QUrlQuery p) mutable {
        ctx->self->call(ctx->method, p,
            [ctx, loadPage](QJsonObject resp) mutable {
                if (!resp.value("ok").toBool()) {
                    if (ctx->onError)
                        ctx->onError(resp.value("error").toString("unknown"));
                    return;
                }
                auto arr = resp.value(ctx->arrayKey).toArray();
                if (!arr.isEmpty())
                    ctx->onPage(arr);

                auto cursor = resp.value("response_metadata")
                                  .toObject()
                                  .value("next_cursor").toString();
                if (!cursor.isEmpty()) {
                    auto next = ctx->params;
                    next.removeQueryItem("cursor");
                    next.addQueryItem("cursor", cursor);
                    loadPage(next);
                } else {
                    ctx->onDone();
                }
            },
            ctx->onError
        );
    };
    loadPage(params);
}

void WebApiClient::enqueue(PendingCall c) {
    _queue.enqueue(std::move(c));
    tryNext();
}

void WebApiClient::tryNext() {
    if (_inflight || _throttled || _queue.isEmpty()) return;
    _inflight = true;
    execute(_queue.dequeue());
}

void WebApiClient::execute(const PendingCall &c) {
    QUrl url(kBaseUrl + c.method);
    QNetworkRequest req;
    req.setRawHeader("Authorization", ("Bearer " + _token).toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = nullptr;
    if (!c.jsonBody.isEmpty()) {
        // POST with JSON body
        url.setQuery(QUrlQuery{});
        req.setUrl(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = _nam->post(req, QJsonDocument(c.jsonBody).toJson(QJsonDocument::Compact));
    } else {
        // GET with query params
        url.setQuery(c.params);
        req.setUrl(url);
        reply = _nam->get(req);
    }

    connect(reply, &QNetworkReply::finished,
            this, [this, reply, c]() mutable { handleReply(reply, std::move(c)); });
}

void WebApiClient::handleReply(QNetworkReply *reply, PendingCall c) {
    reply->deleteLater();
    _inflight = false;

    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 429) {
        int retryAfter = reply->rawHeader("Retry-After").toInt();
        retryAfter = qMax(retryAfter, 1);
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
        qWarning() << "WebApiClient error:" << reply->errorString();
        if (c.onError) c.onError(reply->errorString());
        tryNext();
        return;
    }

    auto doc = QJsonDocument::fromJson(reply->readAll());
    auto obj = doc.object();

    if (!obj.value("ok").toBool()) {
        auto err = obj.value("error").toString("unknown");
        qWarning() << "WebApiClient Slack error:" << err << "on" << c.method;
        if (c.onError) c.onError(err);
        tryNext();
        return;
    }

    if (c.onSuccess) c.onSuccess(obj);
    tryNext();
}

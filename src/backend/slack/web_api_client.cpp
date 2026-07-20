// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "web_api_client.h"

#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDebug>

namespace slack {

const QString  WebApiClient::kBaseUrl        = "https://slack.com/api/";
const QString &WebApiClient::kConnectionLost = net::HttpQueue::kConnectionLost;

namespace {

// Slack-level failures (HTTP 200 + ok:false) that are transient server-side
// hiccups rather than a problem with the request itself. Slack documents these
// as "likely a transient issue on our end" — safe to retry an idempotent call.
bool isTransientSlackError(const QString &err) {
    return err == "internal_error" || err == "service_unavailable" || err == "fatal_error";
}

// Unlike a transport failure (genuinely offline → unlimited queueing is correct),
// a Slack-level "transient" error is an HTTP 200 with ok:false — Slack received
// and rejected the request. When it persists it is NOT a passing blip: the
// request itself is the problem (e.g. users.getPresence rate-limited, or queried
// for an ineligible bot/deactivated user). Retrying forever just hammers a
// failing endpoint, so bound it and surface the error after ~1 min of backoff.
constexpr int kMaxTransientSlackRetries = 6;

} // namespace

WebApiClient::WebApiClient(QObject *parent) : net::HttpQueue(parent) {
    setBaseUrl(kBaseUrl);
}

void WebApiClient::call(
    const QString &method, QUrlQuery params, OnSuccess onSuccess, OnError onError, bool quietErrors
) {
    enqueue({method, std::move(params), {}, std::move(onSuccess), std::move(onError), quietErrors});
}

void WebApiClient::callBackground(
    const QString &method, QUrlQuery params, OnSuccess onSuccess, OnError onError, bool quietErrors
) {
    PendingCall c{
        method, std::move(params), {}, std::move(onSuccess), std::move(onError), quietErrors
    };
    c.priority = Priority::Background;
    enqueue(std::move(c));
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

void WebApiClient::postMultipart(
    const QString &method, QHttpMultiPart *parts, OnSuccess onSuccess, OnError onError
) {
    QNetworkRequest req(QUrl(baseUrl() + method));
    req.setRawHeader("Authorization", ("Bearer " + token()).toUtf8());
    req.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );
    auto *reply = nam()->post(req, parts);
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
    // released — the last shared ref drops and the whole Ctx is freed. A
    // *strong* self-capture here would instead outlive an interrupted
    // pagination forever (an all-indirect LeakSanitizer report).
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

void WebApiClient::handleResponse(const QJsonObject &obj, PendingCall c) {
    if (!obj.value("ok").toBool()) {
        const auto err = obj.value("error").toString("unknown");

        // A transient Slack-side error on an idempotent call: ride out a brief
        // blip with backoff instead of leaking a dropped call to the caller.
        // Bounded — a persistent failure surfaces instead of looping forever.
        if (c.idempotent && isTransientSlackError(err) &&
            c.transportRetries < kMaxTransientSlackRetries) {
            c.transportRetries++;
            const int delay = retryDelayMs(c.transportRetries);
            qDebug() << "WebApiClient: transient Slack error" << err << "on" << c.method
                     << "— retry" << c.transportRetries << "in" << delay << "ms";
            requeueWithDelay(std::move(c), delay);
            return;
        }

        if (!c.quietErrors)
            qWarning() << "WebApiClient Slack error:" << err << "on" << c.method
                       << "| needed:" << obj.value("needed").toString()
                       << "| provided:" << obj.value("provided").toString();

        if (err == "token_expired" && hasTokenExpiredHandler()) {
            qDebug() << "WebApiClient: token_expired on" << c.method
                     << "— pausing queue, re-queuing call";
            pauseForTokenRefresh(std::move(c), "token_expired");
        } else {
            if (c.onError)
                c.onError(err);
            advance();
        }
        return;
    }

    if (c.onSuccess)
        c.onSuccess(obj);
    advance();
}

} // namespace slack

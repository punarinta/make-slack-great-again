// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "graph_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QDebug>

namespace teams {

const QString GraphClient::kBaseUrl = "https://graph.microsoft.com/v1.0/";

GraphClient::GraphClient(QObject *parent) : net::HttpQueue(parent) {
    setBaseUrl(kBaseUrl);
}

void GraphClient::get(
    const QString &path, QUrlQuery params, OnSuccess onSuccess, OnError onError, bool quietErrors
) {
    enqueue({path, std::move(params), {}, std::move(onSuccess), std::move(onError), quietErrors});
}

void GraphClient::postJson(
    const QString &path, const QJsonObject &body, OnSuccess onSuccess, OnError onError
) {
    PendingCall c{path, {}, body, std::move(onSuccess), std::move(onError)};
    c.idempotent = false;
    enqueue(std::move(c));
}

void GraphClient::patchJson(
    const QString &path, const QJsonObject &body, OnSuccess onSuccess, OnError onError
) {
    QNetworkRequest req(QUrl(kBaseUrl + path));
    req.setRawHeader("Authorization", ("Bearer " + token()).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );
    auto *reply =
        nam()->sendCustomRequest(req, "PATCH", QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        reply->deleteLater();
        const int  status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto data   = reply->readAll();
        // A transport failure (no HTTP status) — surface the Qt error string.
        if (reply->error() != QNetworkReply::NoError && status == 0) {
            if (onError)
                onError(reply->errorString());
            return;
        }
        const auto obj = QJsonDocument::fromJson(data).object();
        if (obj.contains(QStringLiteral("error"))) {
            if (onError)
                onError(obj.value(QStringLiteral("error")).toObject().value("code").toString());
            return;
        }
        if (onSuccess)
            onSuccess(obj); // empty object on 204 No Content
    });
}

void GraphClient::putBinary(
    const QString    &path,
    const QByteArray &bytes,
    const QByteArray &contentType,
    OnSuccess         onSuccess,
    OnError           onError
) {
    QNetworkRequest req(QUrl(kBaseUrl + path));
    req.setRawHeader("Authorization", ("Bearer " + token()).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    req.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );
    auto *reply = nam()->sendCustomRequest(req, "PUT", bytes);
    connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        reply->deleteLater();
        const int  status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto data   = reply->readAll();
        if (reply->error() != QNetworkReply::NoError && status == 0) {
            if (onError)
                onError(reply->errorString());
            return;
        }
        const auto obj = QJsonDocument::fromJson(data).object();
        if (obj.contains(QStringLiteral("error"))) {
            if (onError)
                onError(obj.value(QStringLiteral("error")).toObject().value("code").toString());
            return;
        }
        if (onSuccess)
            onSuccess(obj);
    });
}

void GraphClient::paginate(
    const QString                  &path,
    QUrlQuery                       params,
    std::function<void(QJsonArray)> onPage,
    std::function<void()>           onDone,
    OnError                         onError
) {
    struct Ctx {
        GraphClient                            *self;
        std::function<void(QJsonArray)>         onPage;
        std::function<void()>                   onDone;
        OnError                                 onError;
        // (path, params) for the *next* page; updated from @odata.nextLink.
        std::function<void(QString, QUrlQuery)> loadPage;
    };
    auto ctx =
        std::make_shared<Ctx>(Ctx{this, std::move(onPage), std::move(onDone), std::move(onError)});

    // weak self-capture (see slack::WebApiClient::paginate for the cycle-leak
    // rationale): liveness rides on the in-flight call's closures, which hold a
    // strong ref; a strong self-capture would outlive an interrupted pagination.
    std::weak_ptr<Ctx> weak = ctx;
    ctx->loadPage           = [weak](QString p, QUrlQuery q) {
        auto ctx = weak.lock();
        if (!ctx)
            return;
        ctx->self->get(
            p,
            q,
            [ctx](QJsonObject resp) {
                const auto arr = resp.value("value").toArray();
                if (!arr.isEmpty())
                    ctx->onPage(arr);
                const auto next = resp.value("@odata.nextLink").toString();
                if (next.startsWith(kBaseUrl)) {
                    // nextLink is an absolute URL; split into path + query
                    // relative to the base so execute()'s baseUrl+path holds.
                    const QString rel  = next.mid(kBaseUrl.size());
                    const int     qpos = rel.indexOf(QLatin1Char('?'));
                    if (qpos < 0)
                        ctx->loadPage(rel, {});
                    else
                        ctx->loadPage(rel.left(qpos), QUrlQuery(rel.mid(qpos + 1)));
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
    ctx->loadPage(path, params);
}

void GraphClient::handleResponse(const QJsonObject &obj, PendingCall c) {
    if (obj.contains(QStringLiteral("error"))) {
        const auto err  = obj.value(QStringLiteral("error")).toObject();
        const auto code = err.value(QStringLiteral("code")).toString();

        if (code == QLatin1String("InvalidAuthenticationToken") && hasTokenExpiredHandler()) {
            qDebug() << "GraphClient: token expired on" << c.method << "— refreshing";
            pauseForTokenRefresh(std::move(c), code);
            return;
        }
        if (!c.quietErrors)
            qWarning() << "GraphClient error:" << code << "|"
                       << err.value(QStringLiteral("message")).toString() << "on" << c.method;
        if (c.onError)
            c.onError(code.isEmpty() ? QStringLiteral("graph_error") : code);
        advance();
        return;
    }

    if (c.onSuccess)
        c.onSuccess(obj);
    advance();
}

} // namespace teams

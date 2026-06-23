// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "network/http_queue.h"

namespace teams {

// Microsoft Graph client. Builds on net::HttpQueue (transport mechanics, queue,
// retries, 429, the 4xx→handleResponse routing) and adds Graph-specific
// semantics: the graph.microsoft.com/v1.0 base URL, the `@odata.error` envelope,
// the `InvalidAuthenticationToken` → token-refresh trigger, and `@odata.nextLink`
// pagination.
//
// Graph signals errors with real HTTP status codes (not Slack's HTTP 200 +
// ok:false), so error interpretation lives in handleResponse, which the neutral
// core now reaches for 4xx replies too.
class GraphClient : public net::HttpQueue {
    Q_OBJECT
public:
    static const QString kBaseUrl;

    explicit GraphClient(QObject *parent = nullptr);

    // GET a Graph path (relative to kBaseUrl, e.g. "me/joinedTeams"); idempotent.
    void
    get(const QString &path,
        QUrlQuery      params,
        OnSuccess      onSuccess,
        OnError        onError     = {},
        bool           quietErrors = false);

    // POST a JSON body to a Graph path (e.g. "chats/{id}/messages"); treated as
    // non-idempotent (Graph offers no idempotency key for sends). An empty body
    // still POSTs (e.g. a no-body action like .../softDelete).
    void postJson(
        const QString &path, const QJsonObject &body, OnSuccess onSuccess, OnError onError = {}
    );

    // PATCH a JSON body to a Graph path (e.g. edit a message). The shared queue
    // models only GET/POST and PATCH is low-frequency, so this bypasses the queue
    // with a direct request (mirrors slack::WebApiClient::postMultipart). onSuccess
    // fires with the parsed body, or an empty object on a 204 No Content.
    void patchJson(
        const QString &path, const QJsonObject &body, OnSuccess onSuccess, OnError onError = {}
    );

    // PUT raw bytes to a Graph path with a content type (e.g. set the profile
    // photo at me/photo/$value). Bypasses the queue like patchJson. onSuccess
    // fires with the parsed body, or empty on a 200/204 with no body.
    void putBinary(
        const QString    &path,
        const QByteArray &bytes,
        const QByteArray &contentType,
        OnSuccess         onSuccess,
        OnError           onError = {}
    );

    // Paginated GET: fires onPage for each page's `value` array, following
    // `@odata.nextLink` (an absolute URL) until exhausted, then onDone.
    void paginate(
        const QString                  &path,
        QUrlQuery                       params,
        std::function<void(QJsonArray)> onPage,
        std::function<void()>           onDone,
        OnError                         onError = {}
    );

protected:
    void handleResponse(const QJsonObject &obj, PendingCall c) override;
};

} // namespace teams

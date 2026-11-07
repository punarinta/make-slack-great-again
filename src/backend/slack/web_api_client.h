// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "network/http_queue.h"

class QHttpMultiPart;

namespace slack {

// Slack Web API client. Builds on net::HttpQueue (transport mechanics, queue,
// retries, 429) and adds the Slack-specific semantics: the slack.com/api/* base
// URL, the `ok:false` response envelope, transient-error codes, `xoxp` bearer
// specifics, and `response_metadata.next_cursor` pagination.
//
// Slack offers NO public idempotency key, so write methods go as POST
// (callNonIdempotent / postJson): an ambiguous transport failure of one of
// those surfaces as kConnectionLost rather than being blindly retried, letting
// the caller reconcile against server state before resending.
class WebApiClient : public net::HttpQueue {
    Q_OBJECT
public:
    static const QString  kBaseUrl;
    // Re-exported from net::HttpQueue for call sites that compare against it.
    static const QString &kConnectionLost;

    explicit WebApiClient(QObject *parent = nullptr);

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

    // Like call(), but enqueued on the low-priority background lane: it yields to
    // every Normal call and is paced by setBackgroundPaceMs. For bulk sweeps (the
    // per-DM conversations.info resync/activity passes) that must not crowd out
    // interactive requests or trip a rate-limit tier. Defaults to quietErrors —
    // routine sweep failures shouldn't spam the log.
    void callBackground(
        const QString &method,
        QUrlQuery      params,
        OnSuccess      onSuccess,
        OnError        onError     = {},
        bool           quietErrors = true
    );

    // Like call(), but for methods whose effect must not be applied twice
    // (chat.postMessage & co). Ambiguous transport failures are NOT retried;
    // onError(kConnectionLost) is fired instead.
    void callNonIdempotent(
        const QString &method, QUrlQuery params, OnSuccess onSuccess, OnError onError = {}
    );

    // Paginated load: fires onPage for each page's array items, then onDone.
    // Follows next_cursor until exhausted.
    // Walks a cursor-paginated method, handing each page to onPage.
    //
    // maxPages BOUNDS THE WALK, and that bound is a safety mechanism, not a
    // nicety. Chasing next_cursor to exhaustion is fine on an ordinary
    // workspace but catastrophic on Enterprise Grid, where users.list
    // enumerates the entire company directory: Slack reads the resulting burst
    // as scraping and revokes the session outright — measured dying on the 4th
    // page, after 3000 records (scripts/slack-session-probe.py, phase 5).
    // Stopping early is not a failure: onDone still fires with whatever arrived,
    // and unresolved users are filled in on demand via users.info.
    void paginate(
        const QString                  &method,
        const QString                  &arrayKey,
        QUrlQuery                       params,
        std::function<void(QJsonArray)> onPage,
        std::function<void()>           onDone,
        OnError                         onError = {},
        int                             maxPages = kDefaultMaxPages
    );

    // Generous blanket cap: enough for any realistic single-workspace listing,
    // low enough that a runaway cursor can't turn into an abuse signal.
    static constexpr int kDefaultMaxPages = 20;

    // POST a Slack API method with a JSON body (e.g. files.completeUploadExternal).
    // Goes through the rate-limit queue; treated as non-idempotent.
    void postJson(
        const QString &method, const QJsonObject &body, OnSuccess onSuccess, OnError onError = {}
    );

    // POST multipart/form-data to a Slack API method with the auth token (for
    // users.setPhoto). Takes ownership of `parts`. Bypasses the queue; onSuccess
    // fires with the parsed ok:true response.
    void postMultipart(
        const QString &method, QHttpMultiPart *parts, OnSuccess onSuccess, OnError onError = {}
    );

protected:
    void handleResponse(const QJsonObject &obj, PendingCall c) override;
};

} // namespace slack

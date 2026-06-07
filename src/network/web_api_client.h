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

// Simple Slack Web API client.
// All calls share a single in-flight slot to stay well under rate limits.
// On HTTP 429, honors Retry-After then re-queues the failed call.
class WebApiClient : public QObject {
    Q_OBJECT
public:
    static const QString kBaseUrl;

    explicit WebApiClient(QObject *parent = nullptr);

    void setToken(const QString &token);
    [[nodiscard]] bool hasToken() const;

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
    void call(const QString &method,
              QUrlQuery     params,
              OnSuccess     onSuccess,
              OnError       onError = {});

    // Paginated load: fires onPage for each page's array items, then onDone.
    // Follows next_cursor until exhausted.
    void paginate(const QString &method,
                  const QString &arrayKey,
                  QUrlQuery      params,
                  std::function<void(QJsonArray)> onPage,
                  std::function<void()>            onDone,
                  OnError                          onError = {});

    // POST a Slack API method with a JSON body (for calls that take JSON, e.g.
    // files.completeUploadExternal). Goes through the rate-limit queue.
    void postJson(const QString &method, const QJsonObject &body,
                  OnSuccess onSuccess, OnError onError = {});

    // Raw PUT to an external URL (e.g. Slack file upload S3 URL). No auth header.
    // Bypasses the API queue.
    void rawPut(const QUrl &url, const QByteArray &data,
                std::function<void()> onDone,
                OnError onError = {});

    // GET an arbitrary URL with the auth token set. For downloading private files.
    // Bypasses the API queue.
    void downloadUrl(const QUrl &url,
                     std::function<void(QByteArray)> onData,
                     OnError onError = {});

private:
    struct PendingCall {
        QString     method;
        QUrlQuery   params;
        QJsonObject jsonBody; // non-empty → POST JSON instead of GET with params
        OnSuccess   onSuccess;
        OnError     onError;
    };

    void enqueue(PendingCall c);
    void tryNext();
    void execute(const PendingCall &c);
    void handleReply(QNetworkReply *reply, PendingCall c);

    QNetworkAccessManager *_nam;
    QString  _token;
    QQueue<PendingCall> _queue;
    bool _inflight  = false;
    bool _throttled = false; // true while waiting out a Retry-After or token refresh
    OnTokenExpired _onTokenExpired;
};

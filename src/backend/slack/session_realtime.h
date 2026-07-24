// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Per-workspace realtime for session (xoxc/cookie) auth, using Slack's classic
// RTM WebSocket (rtm.connect → wss URL). This is the USER-level push channel the
// browser client uses — completely separate from app-level Socket Mode (xapp).
// Unlike Socket Mode's single shared socket, each session workspace owns its own
// RTM socket (its own user token), so this is created per PublicBackend and fires
// straight into that backend's event stream (no multi-workspace fan-out).
//
// RTM frames are bare Slack event objects, so they go through the same
// slack::normalizeSlackEvent() as Socket Mode. There are no envelopes to ack.
#pragma once

#include "backend/domain.h"
#include "rpl/event_stream.h"

#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QWebSocket;

namespace slack {

class SessionRealtime : public QObject {
    Q_OBJECT
public:
    // `events` is the owning backend's stream; must outlive this object.
    SessionRealtime(
        QString token, QString cookie, rpl::event_stream<Event> *events, QObject *parent = nullptr
    );
    ~SessionRealtime() override;

    void start(); // idempotent; first call opens the socket
    void stop();

    // Session safety-timer hooks (mirror SocketModeRealtime): ensureConnected()
    // reconnects only if dropped (no-op while healthy); reconnectNow() forces a
    // fresh socket when the Session detected the stream missed messages.
    void ensureConnected();
    void reconnectNow();

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &text);

private:
    void openAndConnect(); // rtm.connect → wss URL
    void connectWs(const QUrl &url);
    void teardownConnection();
    void scheduleReconnect();
    void sendPing();

    QString                   _token;
    QString                   _cookie;
    rpl::event_stream<Event> *_events; // non-owning (the backend's stream)
    QNetworkAccessManager    *_nam;
    QWebSocket               *_ws               = nullptr;
    QNetworkReply            *_openReply        = nullptr;
    QTimer                   *_ping             = nullptr;
    bool                      _connecting       = false;
    bool                      _started          = false;
    bool                      _stopped          = false;
    bool                      _hadHello         = false;
    bool                      _reconnectPending = false; // a backoff reconnect is scheduled
    int                       _reconnectMs      = 1000;  // exponential backoff, cap 30s
    int                       _pingId           = 0;
    // Wall-clock ms the live socket connected (0 = not connected). The backoff is
    // reset only after a DURABLE connection (≥ _stableMs) — otherwise a socket
    // Slack drops seconds after connect would reset to a 1 s retry and pin us in a
    // reconnect storm (the exact bug the first cut had). Mirrors SocketModeRealtime.
    qint64                    _connectedSinceMs = 0;
    static constexpr int      _stableMs         = 30'000;
    QUrl                      _rtmConnectUrl{"https://slack.com/api/rtm.connect"};
};

} // namespace slack

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/event_stream.h"

#include <QObject>
#include <QWebSocket>
#include <QNetworkAccessManager>
#include <QUrl>
#include <vector>

class QTimer;

// Connects to Slack's Socket Mode over WebSocket.
// Requires an xapp- app-level token with the connections:write scope.
// Calls apps.connections.open to get a one-use wss URL, then maintains
// a persistent WebSocket connection, acking envelopes and normalizing
// incoming Events for the Backend event stream.
//
// The connection is app-level: Slack delivers events for EVERY workspace the
// app is installed in over this one socket (and round-robins events between
// sockets if several are open for the same app). One instance is therefore
// shared by all workspace backends; each backend registers its event stream
// as a sink and every normalized event is broadcast to all sinks. Sinks
// (Sessions) ignore events for conversations/users they don't know.
class SocketModeRealtime : public QObject {
    Q_OBJECT
public:
    explicit SocketModeRealtime(QString xappToken, QObject *parent = nullptr);
    ~SocketModeRealtime() override;

    // Idempotent — the first call opens the connection.
    void start();
    void stop();

    // Register/unregister a backend's event stream. Events are broadcast to
    // all registered sinks.
    void addSink(rpl::event_stream<Event> *events);
    void removeSink(rpl::event_stream<Event> *events);

    // Subscribe to presence events for the given user IDs on behalf of `sink`.
    // The union of all sinks' IDs is sent immediately if connected, or queued
    // for the next connect.
    void subscribePresence(rpl::event_stream<Event> *sink, QStringList userIds);

    // ── Test seams ──────────────────────────────────────────────────────────
    // Point the apps.connections.open handshake at a local server.
    void setConnectionsOpenUrlForTest(const QUrl &url) { _openUrl = url; }
    // Speed up the liveness watchdog so tests don't wait tens of seconds.
    void setWatchdogTimingForTest(int watchdogMs, int staleMs);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &text);

private:
    void                 openAndConnect();
    void                 connectWs(const QUrl &url);
    void                 ack(const QString &envelopeId);
    void                 scheduleReconnect();
    // Tear down the current socket and reconnect immediately (no backoff). Used
    // by the liveness watchdog when the connection has gone silently dead.
    void                 forceReconnect();
    // Watchdog: pings the socket and forces a reconnect if it has gone silent.
    void                 checkLiveness();
    // Record that the socket is alive right now (wall clock).
    void                 touchActivity();
    // Wire up QNetworkInformation so we reconnect the instant the OS reports the
    // network is reachable again (e.g. right after wake), instead of waiting for
    // the watchdog deadline. Best-effort: no-op if no backend is available.
    void                 setupReachabilityWatch();
    void                 sendPresenceSub();
    std::optional<Event> normalizeSlackEvent(const QJsonObject &event);
    // Extra huddle-state event for a huddle_thread message (start) or its edit
    // (end); additive — does not replace the message's normal event.
    std::optional<Event> huddleEventFor(const QJsonObject &event);

    QString                                 _xappToken;
    std::vector<rpl::event_stream<Event> *> _sinks; // non-owning
    QNetworkAccessManager                  *_nam;
    QWebSocket                             *_ws             = nullptr;
    bool                                    _started        = false;
    bool                                    _stopped        = false;
    int                                     _reconnectMs    = 1000; // exponential backoff, max 30s
    // Liveness watchdog. A sleeping laptop severs the TCP connection silently —
    // QWebSocket can be left half-open ("connected" but dead) so `disconnected`
    // never fires and we'd never reconnect. The watchdog pings the socket and,
    // if no frame/pong has arrived within a deadline, forces a reconnect.
    // `_lastActivityMs` is WALL-CLOCK (QDateTime), not monotonic: monotonic time
    // pauses during system suspend, so a suspend gap would be invisible to it;
    // wall clock makes the post-wake staleness obvious.
    QTimer                                 *_watchdog       = nullptr;
    qint64                                  _lastActivityMs = 0;
    int                                     _watchdogMs     = 20000;
    int                                     _staleMs        = 50000;
    bool                                    _reachabilityWatched = false;
    // apps.connections.open endpoint; overridable for tests.
    QUrl                                    _openUrl{"https://slack.com/api/apps.connections.open"};
    // users to track, per sink; the union is re-sent on every connect
    QHash<rpl::event_stream<Event> *, QStringList> _presenceIds;
};

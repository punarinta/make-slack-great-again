// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/event_stream.h"

#include <QObject>
#include <QWebSocket>
#include <QNetworkAccessManager>
#include <vector>

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

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &text);

private:
    void                 openAndConnect();
    void                 connectWs(const QUrl &url);
    void                 ack(const QString &envelopeId);
    void                 scheduleReconnect();
    void                 sendPresenceSub();
    std::optional<Event> normalizeSlackEvent(const QJsonObject &event);

    QString                                 _xappToken;
    std::vector<rpl::event_stream<Event> *> _sinks; // non-owning
    QNetworkAccessManager                  *_nam;
    QWebSocket                             *_ws          = nullptr;
    bool                                    _started     = false;
    bool                                    _stopped     = false;
    int                                     _reconnectMs = 1000; // exponential backoff, max 30s
    // users to track, per sink; the union is re-sent on every connect
    QHash<rpl::event_stream<Event> *, QStringList> _presenceIds;
};

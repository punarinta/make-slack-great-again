// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/event_stream.h"

#include <QObject>
#include <QWebSocket>
#include <QNetworkAccessManager>

// Connects to Slack's Socket Mode over WebSocket.
// Requires an xapp- app-level token with the connections:write scope.
// Calls apps.connections.open to get a one-use wss URL, then maintains
// a persistent WebSocket connection, acking envelopes and normalizing
// incoming Events for the Backend event stream.
class SocketModeRealtime : public QObject {
    Q_OBJECT
public:
    explicit SocketModeRealtime(
        QString xappToken,
        rpl::event_stream<Event> *events,
        QObject *parent = nullptr);
    ~SocketModeRealtime() override;

    void start();
    void stop();

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &text);

private:
    void openAndConnect();
    void connectWs(const QUrl &url);
    void ack(const QString &envelopeId);
    void scheduleReconnect();
    std::optional<Event> normalizeSlackEvent(const QJsonObject &event);

    QString                  _xappToken;
    rpl::event_stream<Event> *_events;   // non-owning
    QNetworkAccessManager    *_nam;
    QWebSocket               *_ws         = nullptr;
    bool                      _stopped    = false;
    int                       _reconnectMs = 1000; // exponential backoff, max 30s
};

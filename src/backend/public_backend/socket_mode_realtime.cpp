// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "socket_mode_realtime.h"
#include "json_mappers.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QDebug>

#include <algorithm>

SocketModeRealtime::SocketModeRealtime(
    QString xappToken,
    rpl::event_stream<Event> *events,
    QObject *parent)
    : QObject(parent)
    , _xappToken(std::move(xappToken))
    , _events(events)
    , _nam(new QNetworkAccessManager(this))
{}

SocketModeRealtime::~SocketModeRealtime() {
    stop();
}

void SocketModeRealtime::start() {
    openAndConnect();
}

void SocketModeRealtime::stop() {
    _stopped = true;
    if (_ws) {
        _ws->abort();
        _ws->deleteLater();
        _ws = nullptr;
    }
}

// ── Connection setup ──────────────────────────────────────────────────────────

void SocketModeRealtime::openAndConnect() {
    QNetworkRequest req(QUrl("https://slack.com/api/apps.connections.open"));
    req.setRawHeader("Authorization", ("Bearer " + _xappToken).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");

    auto *reply = _nam->post(req, QByteArray{});
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (_stopped) return;

        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (!obj.value("ok").toBool()) {
            qWarning() << "Socket Mode: apps.connections.open error:"
                       << obj.value("error").toString();
            scheduleReconnect();
            return;
        }
        _reconnectMs = 1000; // reset backoff on a successful handshake
        connectWs(QUrl(obj.value("url").toString()));
    });
}

void SocketModeRealtime::connectWs(const QUrl &url) {
    if (_ws) {
        _ws->abort();
        _ws->deleteLater();
    }
    _ws = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this);
    connect(_ws, &QWebSocket::connected,            this, &SocketModeRealtime::onConnected);
    connect(_ws, &QWebSocket::disconnected,         this, &SocketModeRealtime::onDisconnected);
    connect(_ws, &QWebSocket::textMessageReceived,  this, &SocketModeRealtime::onTextMessage);
    _ws->open(url);
}

void SocketModeRealtime::scheduleReconnect() {
    qDebug() << "Socket Mode: reconnecting in" << _reconnectMs << "ms";
    QTimer::singleShot(_reconnectMs, this, [this] {
        if (!_stopped) openAndConnect();
    });
    _reconnectMs = std::min(_reconnectMs * 2, 30000);
}

// ── WebSocket event handlers ──────────────────────────────────────────────────

void SocketModeRealtime::subscribePresence(QStringList userIds) {
    _presenceIds = std::move(userIds);
    sendPresenceSub();
}

void SocketModeRealtime::sendPresenceSub() {
    if (!_ws || _ws->state() != QAbstractSocket::ConnectedState || _presenceIds.isEmpty())
        return;
    QJsonArray ids;
    for (const auto &id : _presenceIds) ids.append(id);
    _ws->sendTextMessage(QJsonDocument(QJsonObject{
        {"type", "presence_sub"},
        {"ids",  ids}
    }).toJson(QJsonDocument::Compact));
}

void SocketModeRealtime::onConnected() {
    qDebug() << "Socket Mode: connected";
    sendPresenceSub();
}

void SocketModeRealtime::onDisconnected() {
    qDebug() << "Socket Mode: disconnected";
    if (!_stopped) scheduleReconnect();
}

void SocketModeRealtime::onTextMessage(const QString &text) {
    const auto envelope = QJsonDocument::fromJson(text.toUtf8()).object();
    const auto type     = envelope.value("type").toString();

    if (type == "hello") {
        qDebug() << "Socket Mode: hello received";
        return;
    }

    if (type == "disconnect") {
        qDebug() << "Socket Mode: server requested disconnect";
        _ws->close();
        return; // onDisconnected will trigger reconnect
    }

    if (type == "events_api") {
        const auto envelopeId = envelope.value("envelope_id").toString();
        ack(envelopeId); // always ack first

        const auto payload = envelope.value("payload").toObject();
        const auto event   = payload.value("event").toObject();

        if (auto ev = normalizeSlackEvent(event)) {
            _events->fire(std::move(*ev));
        }
    }
}

void SocketModeRealtime::ack(const QString &envelopeId) {
    const QJsonObject obj{{"envelope_id", envelopeId}};
    _ws->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// ── Event normalization ───────────────────────────────────────────────────────

std::optional<Event> SocketModeRealtime::normalizeSlackEvent(const QJsonObject &ev) {
    const auto type    = ev.value("type").toString();
    const auto subtype = ev.value("subtype").toString();

    if (type == "message") {
        if (subtype == "message_deleted") {
            return EvMessageDeleted{
                ConversationId{ev.value("channel").toString()},
                ev.value("deleted_ts").toString()
            };
        }
        if (subtype == "message_changed" || subtype == "message_replied") {
            return EvMessageChanged{
                ConversationId{ev.value("channel").toString()},
                JsonMappers::toMessage(ev.value("message").toObject())
            };
        }
        // Plain message or bot_message
        return EvMessageNew{
            ConversationId{ev.value("channel").toString()},
            JsonMappers::toMessage(ev)
        };
    }

    if (type == "reaction_added") {
        const auto item = ev.value("item").toObject();
        return EvReactionAdded{
            ConversationId{item.value("channel").toString()},
            item.value("ts").toString(),
            ev.value("reaction").toString(),
            UserId{ev.value("user").toString()}
        };
    }

    if (type == "reaction_removed") {
        const auto item = ev.value("item").toObject();
        return EvReactionRemoved{
            ConversationId{item.value("channel").toString()},
            item.value("ts").toString(),
            ev.value("reaction").toString(),
            UserId{ev.value("user").toString()}
        };
    }

    // channel_marked, group_marked, im_marked, mpim_marked all have the same shape
    if (type == "channel_marked" || type == "group_marked" ||
        type == "im_marked"      || type == "mpim_marked") {
        return EvConvMarked{
            ConversationId{ev.value("channel").toString()},
            ev.value("ts").toString(),
            ev.value("unread_count_display").toInt()
        };
    }

    if (type == "user_typing") {
        return EvTyping{
            ConversationId{ev.value("channel").toString()},
            UserId{ev.value("user").toString()}
        };
    }

    if (type == "presence_change") {
        return EvPresenceChanged{
            UserId{ev.value("user").toString()},
            ev.value("presence").toString() == "active"
        };
    }

    if (type == "dnd_updated_user") {
        return EvDndChanged{
            UserId{ev.value("user").toString()},
            ev.value("dnd_status").toObject().value("dnd_enabled").toBool()
        };
    }

    if (type == "channel_created") {
        return EvChannelCreated{
            JsonMappers::toConversation(ev.value("channel").toObject())
        };
    }

    if (type == "member_joined_channel") {
        return EvMemberJoined{
            ConversationId{ev.value("channel").toString()},
            UserId{ev.value("user").toString()}
        };
    }

    return std::nullopt;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "socket_mode_realtime.h"
#include "json_mappers.h"

#include <QAbstractSocket>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QDateTime>
#include <QNetworkInformation>
#include <QDebug>

#include <algorithm>

namespace slack {

SocketModeRealtime::SocketModeRealtime(QString xappToken, QObject *parent)
    : QObject(parent), _xappToken(std::move(xappToken)), _nam(new QNetworkAccessManager(this)) {}

SocketModeRealtime::~SocketModeRealtime() {
    stop();
}

void SocketModeRealtime::start() {
    if (_started)
        return;
    _started = true;
    if (!_watchdog) {
        // Slack sends frequent traffic (server pings every few seconds) and we
        // additionally ping on every tick, so a connection that produces nothing
        // for ~2.5 ticks (_staleMs) is genuinely dead — not merely idle.
        _watchdog = new QTimer(this);
        _watchdog->setInterval(_watchdogMs);
        connect(_watchdog, &QTimer::timeout, this, &SocketModeRealtime::checkLiveness);
        _watchdog->start();
    }
    setupReachabilityWatch();
    openAndConnect();
}

void SocketModeRealtime::setWatchdogTimingForTest(int watchdogMs, int staleMs) {
    _watchdogMs = watchdogMs;
    _staleMs    = staleMs;
    if (_watchdog)
        _watchdog->setInterval(_watchdogMs);
}

void SocketModeRealtime::addSink(rpl::event_stream<Event> *events) {
    if (std::find(_sinks.begin(), _sinks.end(), events) == _sinks.end())
        _sinks.push_back(events);
}

void SocketModeRealtime::removeSink(rpl::event_stream<Event> *events) {
    _sinks.erase(std::remove(_sinks.begin(), _sinks.end(), events), _sinks.end());
    if (_presenceIds.remove(events))
        sendPresenceSub();
}

void SocketModeRealtime::stop() {
    _stopped = true;
    teardownConnection();
}

void SocketModeRealtime::teardownConnection() {
    if (_openReply) {
        // Detach first: the `finished` handler bails when _openReply no longer
        // matches the reply it captured, so the aborted handshake can't go on to
        // open a socket.
        auto *r    = _openReply;
        _openReply = nullptr;
        r->abort();
    }
    if (_ws) {
        // Drop the socket's signals before aborting so its `disconnected` /
        // `errorOccurred` can't re-enter our slots and queue a competing reconnect.
        disconnect(_ws, nullptr, this, nullptr);
        _ws->abort();
        _ws->deleteLater();
        _ws = nullptr;
    }
    _connecting = false;
}

// ── Connection setup ──────────────────────────────────────────────────────────

void SocketModeRealtime::openAndConnect() {
    // One connection cycle at a time. Reconnect can be triggered concurrently
    // from several places (a real `disconnected`, the liveness watchdog, the
    // reachability watcher); without this guard those race into multiple
    // simultaneous handshakes and thus multiple live Slack sockets. Slack
    // delivers each event to only ONE of an app's sockets, so the ones the app
    // isn't actively reading silently swallow a share of events.
    if (_stopped || _connecting)
        return;
    _connecting = true;

    QNetworkRequest req(_openUrl);
    req.setRawHeader("Authorization", ("Bearer " + _xappToken).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    // A hung handshake must not wedge the single-flight guard forever; on
    // timeout `finished` fires with an error and we back off and retry.
    req.setTransferTimeout(15000);

    _openReply = _nam->post(req, QByteArray{});
    connect(_openReply, &QNetworkReply::finished, this, [this, reply = _openReply] {
        reply->deleteLater();
        if (_openReply != reply)
            return; // superseded by a teardown (stop/forceReconnect) — ignore
        _openReply = nullptr;
        if (_stopped) {
            _connecting = false;
            return;
        }

        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (!obj.value("ok").toBool()) {
            qWarning() << "Socket Mode: apps.connections.open error:"
                       << obj.value("error").toString();
            _connecting = false;
            scheduleReconnect();
            return;
        }
        _reconnectMs = 1000; // reset backoff on a successful handshake
        // _connecting stays set until the socket connects (onConnected) or the
        // attempt fails (onDisconnected / errorOccurred), so a stray reconnect
        // trigger in the meantime can't open a second socket.
        connectWs(QUrl(obj.value("url").toString()));
    });
}

void SocketModeRealtime::connectWs(const QUrl &url) {
    if (_ws) {
        // Drop the old socket's signals BEFORE aborting so its abort-triggered
        // `disconnected` / `errorOccurred` can't re-enter our slots and queue a
        // competing reconnect (matches teardownConnection / forceReconnect).
        disconnect(_ws, nullptr, this, nullptr);
        _ws->abort();
        _ws->deleteLater();
    }
    _ws = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this);
    connect(_ws, &QWebSocket::connected, this, &SocketModeRealtime::onConnected);
    connect(_ws, &QWebSocket::disconnected, this, &SocketModeRealtime::onDisconnected);
    connect(_ws, &QWebSocket::textMessageReceived, this, &SocketModeRealtime::onTextMessage);
    // A failed handshake/connect would otherwise leave _connecting stuck true
    // (QWebSocket emits no `disconnected` for a socket that never connected),
    // wedging the single-flight guard. Release it and back off.
    connect(_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!_ws || sender() != _ws)
            return; // a stale/superseded socket — ignore
        if (_ws->state() == QAbstractSocket::ConnectedState)
            return; // transient error on a live socket; `disconnected` owns real drops
        _connecting = false;
        if (!_stopped)
            scheduleReconnect();
    });
    // A pong is proof the socket is still alive even when the workspace is quiet.
    connect(_ws, &QWebSocket::pong, this, [this](quint64, const QByteArray &) { touchActivity(); });
    _ws->open(url);
}

void SocketModeRealtime::scheduleReconnect() {
    qDebug() << "Socket Mode: reconnecting in" << _reconnectMs << "ms";
    QTimer::singleShot(_reconnectMs, this, [this] {
        if (!_stopped)
            openAndConnect();
    });
    _reconnectMs = std::min(_reconnectMs * 2, 30000);
}

void SocketModeRealtime::ensureConnected() {
    if (!_started || _stopped)
        return;
    if (_connecting)
        return; // a connect cycle is already underway — let it finish
    if (_ws && _ws->state() == QAbstractSocket::ConnectedState)
        return; // healthy
    // Started but not connected and nothing in flight: a reconnect stalled
    // (e.g. backoff scheduled far out, or the single-flight guard never
    // cleared). Kick a fresh connect now.
    qDebug() << "Socket Mode: ensureConnected — not connected, reconnecting";
    openAndConnect();
}

void SocketModeRealtime::reconnectNow() {
    if (!_started || _stopped)
        return;
    forceReconnect();
}

void SocketModeRealtime::forceReconnect() {
    if (_stopped)
        return;
    qWarning() << "Socket Mode: connection went silent — forcing reconnect";
    // Abort the current socket AND any in-flight handshake, and clear the
    // single-flight guard, so openAndConnect below starts exactly one fresh
    // connection rather than racing a half-built one.
    teardownConnection();
    _reconnectMs = 1000; // fresh start, no inherited backoff
    openAndConnect();
}

void SocketModeRealtime::checkLiveness() {
    if (_stopped || !_ws || _ws->state() != QAbstractSocket::ConnectedState)
        return; // not connected: onDisconnected / scheduleReconnect own recovery
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (_lastActivityMs && now - _lastActivityMs > _staleMs) {
        // No frame and no pong for the whole deadline — the socket is a zombie
        // (typical after a laptop sleep severs TCP silently). Reconnect.
        forceReconnect();
        return;
    }
    // Probe: a healthy server answers with a pong, which refreshes activity.
    _ws->ping();
}

void SocketModeRealtime::touchActivity() {
    _lastActivityMs = QDateTime::currentMSecsSinceEpoch();
}

void SocketModeRealtime::setupReachabilityWatch() {
    if (_reachabilityWatched)
        return;
    if (!QNetworkInformation::loadDefaultBackend())
        return; // platform without a reachability backend — watchdog still covers us
    auto *ni = QNetworkInformation::instance();
    if (!ni || !(ni->supportedFeatures() & QNetworkInformation::Feature::Reachability))
        return;
    _reachabilityWatched = true;
    connect(
        ni,
        &QNetworkInformation::reachabilityChanged,
        this,
        [this](QNetworkInformation::Reachability reachability) {
            if (_stopped || !_started)
                return;
            if (reachability != QNetworkInformation::Reachability::Online)
                return; // only act when the network comes back
            if (!_ws || _ws->state() != QAbstractSocket::ConnectedState) {
                // Network just returned and we're not connected — recover now
                // rather than waiting out the backoff or the watchdog deadline.
                qDebug() << "Socket Mode: network reachable — reconnecting now";
                forceReconnect();
            } else {
                // Connection may have silently died across the transition;
                // probe it so the watchdog reacts immediately if it's stale.
                _ws->ping();
            }
        }
    );
}

// ── WebSocket event handlers ──────────────────────────────────────────────────

void SocketModeRealtime::subscribePresence(rpl::event_stream<Event> *sink, QStringList userIds) {
    _presenceIds[sink] = std::move(userIds);
    sendPresenceSub();
}

void SocketModeRealtime::sendPresenceSub() {
    if (!_ws || _ws->state() != QAbstractSocket::ConnectedState || _presenceIds.isEmpty())
        return;
    QJsonArray    ids;
    QSet<QString> seen;
    for (const auto &list : _presenceIds)
        for (const auto &id : list)
            if (!seen.contains(id)) {
                seen.insert(id);
                ids.append(id);
            }
    _ws->sendTextMessage(QJsonDocument(
                             QJsonObject{{"type", "presence_sub"}, {"ids", ids}}
    ).toJson(QJsonDocument::Compact));
}

void SocketModeRealtime::onConnected() {
    qDebug() << "Socket Mode: connected";
    _connecting = false; // cycle complete — future reconnects may proceed
    touchActivity();
    sendPresenceSub();
}

void SocketModeRealtime::onDisconnected() {
    if (sender() && sender() != _ws)
        return; // a stale/superseded socket's late disconnect — ignore
    _connecting = false;
    if (_ws)
        qDebug() << "Socket Mode: disconnected — close code" << _ws->closeCode() << "reason"
                 << _ws->closeReason();
    else
        qDebug() << "Socket Mode: disconnected";
    if (!_stopped)
        scheduleReconnect();
}

void SocketModeRealtime::onTextMessage(const QString &text) {
    touchActivity(); // any frame proves the socket is alive
    const auto envelope = QJsonDocument::fromJson(text.toUtf8()).object();
    const auto type     = envelope.value("type").toString();

    if (type == "hello") {
        qDebug() << "Socket Mode: hello received";
        if (_hadHello) {
            // Session re-established after a gap. Slack does not replay missed
            // events, so tell every backend/UI to backfill (refetch history /
            // conversation badges). Iterate a copy: a handler may remove a sink.
            const auto sinks = _sinks;
            for (auto *sink : sinks)
                if (std::find(_sinks.begin(), _sinks.end(), sink) != _sinks.end())
                    sink->fire_copy(Event{EvRealtimeReconnected{}});
        }
        _hadHello = true;
        return;
    }

    if (type == "disconnect") {
        const auto reason = envelope.value("reason").toString();
        // Slack sends two flavours. "warning" is a heads-up that this socket
        // will be recycled *soon* — Slack expects us to KEEP handling events on
        // it until it actually closes (or a refresh_requested follows), so we
        // must not tear it down here. "refresh_requested" (and "too_many_-
        // websockets" / anything else) is the real signal to reconnect now.
        if (reason == "warning") {
            qDebug() << "Socket Mode: disconnect warning (recycle pending) — keeping socket open";
            return;
        }
        qDebug() << "Socket Mode: server requested disconnect — reason" << reason;
        _ws->close();
        return; // onDisconnected will trigger reconnect
    }

    if (type == "events_api") {
        const auto envelopeId = envelope.value("envelope_id").toString();
        ack(envelopeId); // always ack first

        const auto payload = envelope.value("payload").toObject();
        const auto event   = payload.value("event").toObject();

        // Broadcast to every workspace backend — sinks ignore events for
        // conversations/users they don't know (IDs are globally unique).
        // Iterate a copy: a handler may remove a sink (session teardown).
        auto fire = [this](const Event &e) {
            const auto sinks = _sinks;
            for (auto *sink : sinks)
                if (std::find(_sinks.begin(), _sinks.end(), sink) != _sinks.end())
                    sink->fire_copy(e);
        };

        if (auto ev = normalizeSlackEvent(event))
            fire(*ev);
        // Huddle detection is ADDITIVE: a huddle_thread message still flows as a
        // normal message above (its notification/chat line are untouched); this
        // fires an extra EvHuddleChanged so the huddle banner can react.
        if (auto huddle = huddleEventFor(event))
            fire(*huddle);
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
            // previous_message carries the deleted message; toMessage derives
            // threadRoot (set only when it was a reply) so the channel list can
            // drop the root's reply count.
            const auto prev = JsonMappers::toMessage(ev.value("previous_message").toObject());
            return EvMessageDeleted{
                ConversationId{ev.value("channel").toString()},
                ev.value("deleted_ts").toString(),
                prev.threadRoot
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
            ConversationId{ev.value("channel").toString()}, JsonMappers::toMessage(ev)
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
    if (type == "channel_marked" || type == "group_marked" || type == "im_marked" ||
        type == "mpim_marked") {
        return EvConvMarked{
            ConversationId{ev.value("channel").toString()},
            ev.value("ts").toString(),
            ev.value("unread_count_display").toInt(),
            ev.value("mention_count_display").toInt()
        };
    }

    // NOTE: dead branch in practice. user_typing is an RTM-only event — it has
    // no Events API equivalent, so Slack never delivers it in an "events_api"
    // envelope over Socket Mode (the only frame this method ever sees). A Slack
    // maintainer has confirmed there is no Events API typing event and none is
    // planned (slackapi/node-slack-sdk#1130). The only sources of user_typing
    // are legacy RTM (classic apps, EOL 2026-11-16) and Slack's internal desktop
    // websocket (scraped xoxc/xoxd session creds) — neither is a supported path
    // for this OAuth-token app. Kept so the typing UI lights up automatically if
    // such an event ever does arrive; see MainWindow's EvTyping handler.
    if (type == "user_typing") {
        return EvTyping{
            ConversationId{ev.value("channel").toString()}, UserId{ev.value("user").toString()}
        };
    }

    if (type == "presence_change") {
        return EvPresenceChanged{
            UserId{ev.value("user").toString()}, ev.value("presence").toString() == "active"
        };
    }

    if (type == "dnd_updated_user") {
        return EvDndChanged{
            UserId{ev.value("user").toString()},
            ev.value("dnd_status").toObject().value("dnd_enabled").toBool()
        };
    }

    if (type == "channel_created") {
        return EvChannelCreated{JsonMappers::toConversation(ev.value("channel").toObject())};
    }

    if (type == "member_joined_channel") {
        return EvMemberJoined{
            ConversationId{ev.value("channel").toString()}, UserId{ev.value("user").toString()}
        };
    }

    // A member updated their profile (incl. avatar). user_change carries the
    // full user object — same shape as users.list — so toUser parses it
    // directly. (user_profile_changed carries only id+profile and would zero
    // out is_admin/is_bot/etc., so we don't map it.)
    if (type == "user_change") {
        return EvUserChanged{JsonMappers::toUser(ev.value("user").toObject())};
    }

    return std::nullopt;
}

std::optional<Event> SocketModeRealtime::huddleEventFor(const QJsonObject &ev) {
    if (ev.value("type").toString() != "message")
        return std::nullopt;

    const auto subtype = ev.value("subtype").toString();

    // A huddle starting: USLACKBOT posts a "huddle_thread" message carrying the
    // live `room`. The subtype itself proves it's a huddle, so "ongoing" is just
    // "no end timestamp" (participants may not be populated at the announce
    // moment); a roomless announce still counts as a start.
    QJsonObject room;
    QString     channel  = ev.value("channel").toString();
    bool        isHuddle = false;
    if (subtype == "huddle_thread") {
        room     = ev.value("room").toObject();
        isHuddle = true;
    } else if (subtype == "message_changed") {
        // A huddle ending/changing arrives as an edit of the huddle_thread
        // message (its room gains a date_end).
        const auto inner = ev.value("message").toObject();
        if (inner.value("subtype").toString() == "huddle_thread") {
            room     = inner.value("room").toObject();
            isHuddle = true;
        }
    }
    if (!isHuddle)
        return std::nullopt;

    const auto h = JsonMappers::readHuddleRoom(room);
    return EvHuddleChanged{ConversationId{channel}, h.active, h.link, h.participants};
}

} // namespace slack

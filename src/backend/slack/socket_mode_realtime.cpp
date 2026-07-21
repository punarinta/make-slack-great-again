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
        // Ticks every _watchdogMs. It force-reconnects ONLY when its own tick cadence
        // reveals the process was frozen (a suspend gap > _staleMs) — the half-open-
        // after-sleep case; it does not treat mere idle silence as death. See the
        // _lastCheckMs comment and checkLiveness().
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
    // Drop each socket's signals before aborting so its `disconnected` /
    // `errorOccurred` can't re-enter our slots and queue a competing reconnect.
    if (_pendingWs) {
        disconnect(_pendingWs, nullptr, this, nullptr);
        _pendingWs->abort();
        _pendingWs->deleteLater();
        _pendingWs = nullptr;
    }
    if (_ws) {
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
        // NB: do NOT reset the backoff here. A successful apps.connections.open
        // handshake proves nothing about whether the socket will SURVIVE — Slack
        // may evict it seconds after "hello" (the bare-1000 contention signature).
        // Resetting on every handshake pinned the retry gap at 1 s, so an evicted
        // socket reconnected once per second forever: pointless load, a self-
        // inflicted conversations.* 429 storm, and it makes a two-client eviction
        // fight maximally violent. The backoff is instead reset in onDisconnected
        // only after a connection proved durable (see _stableConnectionMs).
        // _connecting stays set until the socket connects (onConnected) or the
        // attempt fails (onDisconnected / errorOccurred), so a stray reconnect
        // trigger in the meantime can't open a second socket.
        connectWs(QUrl(obj.value("url").toString()));
    });
}

void SocketModeRealtime::connectWs(const QUrl &url) {
    // Build the new socket into _pendingWs and leave _ws (if any) untouched and
    // live: onConnected promotes _pendingWs and only then drops the old socket,
    // so a recycle hands over without a gap. A half-built earlier replacement is
    // discarded first.
    if (_pendingWs) {
        disconnect(_pendingWs, nullptr, this, nullptr);
        _pendingWs->abort();
        _pendingWs->deleteLater();
    }
    auto *sock = new QWebSocket(QString{}, QWebSocketProtocol::VersionLatest, this);
    _pendingWs = sock;
    connect(sock, &QWebSocket::connected, this, &SocketModeRealtime::onConnected);
    connect(sock, &QWebSocket::disconnected, this, &SocketModeRealtime::onDisconnected);
    connect(sock, &QWebSocket::textMessageReceived, this, &SocketModeRealtime::onTextMessage);
    // A failed handshake/connect would otherwise leave _connecting stuck true
    // (QWebSocket emits no `disconnected` for a socket that never connected),
    // wedging the single-flight guard. Release it and back off. The old _ws (if
    // this was an overlapping recycle replacement) stays live meanwhile.
    connect(sock, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        auto *s = qobject_cast<QWebSocket *>(sender());
        if (!s || (s != _ws && s != _pendingWs))
            return; // a stale/superseded socket — ignore
        if (s->state() == QAbstractSocket::ConnectedState)
            return; // transient error on a live socket; `disconnected` owns real drops
        if (s == _pendingWs) {
            disconnect(_pendingWs, nullptr, this, nullptr);
            _pendingWs->deleteLater();
            _pendingWs = nullptr;
        }
        _connecting = false;
        if (!_stopped)
            scheduleReconnect();
    });
    sock->open(url);
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
    // Called when the app detected the stream silently missed events — the socket
    // still answers pings but Slack quietly stopped routing events to it (the
    // watchdog can't see this). The socket itself is still CONNECTED, so recover
    // the gapless way Slack's own "warning" recycle does: bring up an OVERLAPPING
    // replacement (openAndConnect builds it into _pendingWs; onConnected promotes
    // it and only then aborts the old socket) so no event is lost during the swap.
    //
    // A teardown-first forceReconnect() here would instead open a window with no
    // live socket, and Slack does NOT replay events missed while disconnected — so
    // it would drop events for EVERY workspace on this shared socket, and the next
    // safety poll would flag THOSE as missed and re-trigger this, spinning a
    // self-sustaining reconnect storm. (openAndConnect is a no-op while a connect
    // cycle is already in flight — the single-flight guard lets it finish.)
    if (_ws && _ws->state() == QAbstractSocket::ConnectedState) {
        qWarning() << "Socket Mode: realtime stalled — bringing up overlapping replacement";
        _reconnectMs = 1000; // fresh start, no inherited backoff
        openAndConnect();
        return;
    }
    // Not connected (or mid-handshake with no live socket to keep reading): there
    // is nothing to overlap, so fall back to the hard path that clears any
    // half-built attempt and starts one fresh connection.
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
    // Measure the gap since the previous tick on every fire (even while
    // disconnected, so a reconnect in progress can't leave a stale baseline that
    // looks like a suspend on the next connected tick).
    const qint64 now     = QDateTime::currentMSecsSinceEpoch();
    const qint64 tickGap = _lastCheckMs ? now - _lastCheckMs : 0;
    _lastCheckMs         = now;

    if (_stopped || !_ws || _ws->state() != QAbstractSocket::ConnectedState)
        return; // not connected: onDisconnected / scheduleReconnect own recovery

    // Suspend/sleep detection — the ONLY reason this watchdog force-reconnects. The
    // timer fires every _watchdogMs; a wall-clock gap far larger than that means the
    // process was frozen (laptop sleep/hibernate), which silently severs TCP and
    // leaves QWebSocket half-open ("connected" but dead, so `disconnected` never
    // fires). That is exactly the case no transport signal catches, so reconnect.
    if (tickGap > _staleMs) {
        qWarning() << "Socket Mode: watchdog gap" << tickGap
                   << "ms — process was suspended, forcing reconnect";
        forceReconnect();
        return;
    }

    // Keepalive only. This ping is NOT a liveness probe: Slack never pongs it (see
    // the _lastCheckMs comment), so we don't reconnect on its absence — it just keeps
    // middlebox NAT mappings from idle-dropping the connection. A quiet-but-healthy
    // socket is deliberately left alone; Session's history poll re-establishes it, on
    // evidence, if it ever silently stops delivering events.
    _ws->ping();
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
    auto *sock = qobject_cast<QWebSocket *>(sender());
    if (!sock || sock != _pendingWs)
        return; // a stale/superseded socket connecting late — ignore
    // Promote the freshly-connected replacement. If an old socket is still live
    // (a recycle overlap), drop it only now — events kept flowing on it right up
    // to this point, so the handover is gapless.
    if (_ws && _ws != sock) {
        disconnect(_ws, nullptr, this, nullptr);
        _ws->abort();
        _ws->deleteLater();
        // We briefly held two of our own sockets; Slack's next num_connections may
        // still count the one we just aborted. Note the moment so the hello handler
        // grants a settle-window allowance instead of flagging false contention.
        _recentOverlapPromoteMs = QDateTime::currentMSecsSinceEpoch();
    }
    _ws               = sock;
    _pendingWs        = nullptr;
    _connecting       = false; // cycle complete — future reconnects may proceed
    _connectedSinceMs = QDateTime::currentMSecsSinceEpoch();
    qDebug() << "Socket Mode: connected";
    sendPresenceSub();
}

void SocketModeRealtime::onDisconnected() {
    auto *sock = qobject_cast<QWebSocket *>(sender());
    if (sock && sock != _ws)
        return; // a stale/superseded socket's late disconnect — ignore
    // Consume the expected-close flag for this disconnect regardless of the path
    // below (a server-requested close is never an eviction).
    const bool expected   = _serverRequestedClose;
    _serverRequestedClose = false;
    bool bareClose        = false;
    // Only the live socket (_ws) ever reaches here: a replacement that never
    // connected emits errorOccurred, not disconnected. Clean it up — connectWs no
    // longer recycles _ws, so nothing else will.
    if (sock) {
        const auto code = sock->closeCode();
        qDebug() << "Socket Mode: disconnected — close code" << code << "reason"
                 << sock->closeReason();
        // Slack cleanly closing our live socket with the normal code and NO
        // preceding "disconnect" envelope is an eviction from the app's socket
        // pool — the contention signature. A close we asked for (refresh_requested)
        // or an abnormal transport drop (sleep/network, code 1006) is not.
        bareClose = !expected && code == QWebSocketProtocol::CloseCodeNormal;
        disconnect(sock, nullptr, this, nullptr);
        sock->deleteLater();
        _ws = nullptr;
    } else {
        qDebug() << "Socket Mode: disconnected";
    }
    // A replacement may already be in flight — an overlapping recycle whose old
    // socket dropped before the new one connected, or any connect cycle the
    // single-flight guard is running. Let it finish instead of racing a second.
    // (Also skips contention counting: an overlap close isn't an eviction.)
    if (_connecting || _pendingWs)
        return;
    // How long did this socket live? A connection Slack evicts seconds after
    // "hello" (short-lived, over and over) is the external-contention signature;
    // a long-lived one that finally drops is a routine recycle/network blip.
    const qint64 nowMs    = QDateTime::currentMSecsSinceEpoch();
    const qint64 lifetime = _connectedSinceMs ? nowMs - _connectedSinceMs : 0;
    _connectedSinceMs     = 0;
    if (bareClose) {
        // A bare 1000 close (no disconnect envelope) has TWO causes we can't tell
        // apart from the close alone: another client evicting us from the app's
        // pool (contention — confirmed only when a hello reports num_connections >
        // ours), OR Slack idle-closing a socket whose keepalives don't traverse the
        // network (a middlebox/proxy dropping WS control frames — the signature is
        // a near-constant ~10 s lifetime with num_connections == ours throughout).
        qDebug() << "Socket Mode: bare 1000 close after" << lifetime << "ms alive";
        noteBareClose();
    }
    // Reset the backoff ONLY when the connection proved durable — otherwise let it
    // grow exponentially so a flapping/evicted socket stops hammering a 1 s retry.
    if (lifetime >= _stableConnectionMs)
        _reconnectMs = 1000;
    if (!_stopped)
        scheduleReconnect();
}

void SocketModeRealtime::onTextMessage(const QString &text) {
    const auto envelope = QJsonDocument::fromJson(text.toUtf8()).object();
    const auto type     = envelope.value("type").toString();

    if (type == "hello") {
        // The hello frame reports num_connections: how many WebSocket connections
        // this app (identified by the xapp token) currently has open across the
        // whole fleet — not just ours. debug_info.host is Slack's edge host, not
        // the client, so it can't identify the competitor; the count is what tells
        // us one exists. Count the sockets WE hold: onConnected has already
        // promoted this one to _ws (hello arrives after `connected`); a recycle may
        // still hold _pendingWs. Anything beyond ours belongs to another client.
        const int    numConnections = envelope.value("num_connections").toInt(1);
        const auto   host = envelope.value("debug_info").toObject().value("host").toString();
        const int    ours = (_ws ? 1 : 0) + (_pendingWs ? 1 : 0);
        const qint64 now  = QDateTime::currentMSecsSinceEpoch();
        const bool   recentOverlap =
            _recentOverlapPromoteMs && now - _recentOverlapPromoteMs < _overlapGraceMs;
        // Right after our own recycle, Slack may momentarily still count the socket
        // we just aborted — allow one extra so a routine recycle isn't mistaken for
        // a competitor.
        const int allowance   = ours + (recentOverlap ? 1 : 0);
        _lastOtherConnections = std::max(0, numConnections - allowance);
        qDebug() << "Socket Mode: hello received — num_connections" << numConnections << "(we hold"
                 << ours << ") host" << host;
        if (_lastOtherConnections > 0) {
            // Another client is in the app's pool. msga is single-instance per user
            // (SingleInstance), so it is NOT a second copy on this computer — it's
            // another device/account running msga on the same compiled-in xapp
            // token, and it's stealing/evicting our share of the events.
            maybeNotifyContention();
        }
        if (_hadHello) {
            // Session re-established after a gap. Slack does not replay missed
            // events, so tell every backend/UI to backfill (refetch history /
            // conversation badges).
            broadcast(Event{EvRealtimeReconnected{}});
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
            // Slack will recycle this socket soon but keeps it alive a little
            // longer, expecting us to bring up a replacement now and keep reading
            // this one until the new socket is live — so the handover drops no
            // events (Slack does not replay events missed while disconnected).
            // openAndConnect builds the replacement into _pendingWs; onConnected
            // promotes it and only then drops this socket. The single-flight guard
            // coalesces duplicate warnings into one replacement.
            qDebug() << "Socket Mode: disconnect warning (recycle pending) — opening overlapping "
                        "replacement";
            openAndConnect();
            return;
        }
        qDebug() << "Socket Mode: server requested disconnect — reason" << reason;
        // Too-many-connections is Slack saying outright that the app's socket pool
        // is full: unambiguous contention (another instance on the shared xapp
        // token). Surface it now; the bare-close counter in onDisconnected covers
        // the case where Slack evicts us with no envelope at all.
        if (reason == "too_many_connections" || reason == "too_many_websockets")
            maybeNotifyContention();
        // This close is one we asked for — mark it so onDisconnected doesn't count
        // it as an eviction (our close() reports the same code 1000 Slack uses).
        _serverRequestedClose = true;
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
        if (auto ev = normalizeSlackEvent(event))
            broadcast(*ev);
        // Huddle detection is ADDITIVE: a huddle_thread message still flows as a
        // normal message above (its notification/chat line are untouched); this
        // fires an extra EvHuddleChanged so the huddle banner can react.
        if (auto huddle = huddleEventFor(event))
            broadcast(*huddle);
    }
}

void SocketModeRealtime::ack(const QString &envelopeId) {
    const QJsonObject obj{{"envelope_id", envelopeId}};
    _ws->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void SocketModeRealtime::broadcast(const Event &e) {
    // Iterate a copy: a handler may remove a sink (session teardown).
    const auto sinks = _sinks;
    for (auto *sink : sinks)
        if (std::find(_sinks.begin(), _sinks.end(), sink) != _sinks.end())
            sink->fire_copy(e);
}

void SocketModeRealtime::noteBareClose() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    _bareCloseTimes.push_back(now);
    while (!_bareCloseTimes.empty() && now - _bareCloseTimes.front() > _contentionWindowMs)
        _bareCloseTimes.pop_front();
    if (static_cast<int>(_bareCloseTimes.size()) >= _contentionThreshold)
        maybeNotifyContention();
}

void SocketModeRealtime::maybeNotifyContention() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (_lastContentionNoticeMs && now - _lastContentionNoticeMs < _contentionNoticeGapMs)
        return; // already warned recently — don't spam a sustained storm
    _lastContentionNoticeMs = now;
    if (_lastOtherConnections > 0) {
        // Real contention: a hello actually counted other connections on this app.
        qWarning() << "Socket Mode: connection-pool contention —" << _lastOtherConnections
                   << "other connection(s) open for this app; another msga instance elsewhere is "
                      "sharing this app's xapp token (nothing local — SingleInstance forbids a "
                      "second local copy)";
        broadcast(Event{EvRealtimeContended{_lastOtherConnections}});
        return;
    }
    // Repeated bare closes but NO other connections were ever counted: this is NOT
    // contention (claiming "another instance" here was a false alarm). The likely
    // cause is Slack idle-closing because our WS keepalives don't traverse the
    // network — a middlebox/proxy/VPN dropping WebSocket control frames. Warn
    // honestly and do NOT raise EvRealtimeContended (it drives a wrong UI banner).
    qWarning() << "Socket Mode: socket repeatedly closed by the server with no other connections "
                  "present — realtime keepalives may be blocked by a proxy/VPN/firewall (WS "
                  "control frames not traversing the network)";
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

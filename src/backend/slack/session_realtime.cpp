// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/slack/session_realtime.h"

#include "backend/slack/slack_events.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QWebSocket>

namespace slack {

namespace {
constexpr int kPingIntervalMs = 30'000;
constexpr int kMaxReconnectMs = 30'000;

// Cookie must ride the jar (QNAM clobbers a manual Cookie header from its jar).
class SeededCookieJar : public QNetworkCookieJar {
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    void seed(const QNetworkCookie &c) { insertCookie(c); }
};
} // namespace

SessionRealtime::SessionRealtime(
    QString token, QString cookie, rpl::event_stream<Event> *events, QObject *parent
)
    : QObject(parent), _token(std::move(token)), _cookie(std::move(cookie)), _events(events),
      _nam(new QNetworkAccessManager(this)) {
    auto          *jar = new SeededCookieJar(_nam);
    QNetworkCookie c("d", _cookie.toUtf8());
    c.setDomain(QStringLiteral(".slack.com"));
    c.setPath(QStringLiteral("/"));
    jar->seed(c);
    _nam->setCookieJar(jar);
}

SessionRealtime::~SessionRealtime() {
    teardownConnection();
}

void SessionRealtime::start() {
    if (_started)
        return;
    _started = true;
    _stopped = false;
    openAndConnect();
}

void SessionRealtime::stop() {
    _stopped = true;
    _started = false;
    teardownConnection();
}

void SessionRealtime::ensureConnected() {
    if (_stopped || _connecting || _reconnectPending)
        return; // a backoff reconnect is already scheduled — don't bypass it
    if (_ws && _ws->state() == QAbstractSocket::ConnectedState)
        return; // healthy — no-op
    openAndConnect();
}

void SessionRealtime::reconnectNow() {
    if (_stopped)
        return;
    teardownConnection();
    _reconnectMs = 1000;
    openAndConnect();
}

void SessionRealtime::teardownConnection() {
    _connecting       = false;
    _reconnectPending = false;
    _connectedSinceMs = 0;
    if (_ping)
        _ping->stop();
    if (_openReply) {
        _openReply->disconnect(this);
        _openReply->abort();
        _openReply->deleteLater();
        _openReply = nullptr;
    }
    if (_ws) {
        _ws->disconnect(this); // detach slots before teardown so they can't re-enter
        _ws->abort();
        _ws->deleteLater();
        _ws = nullptr;
    }
}

void SessionRealtime::openAndConnect() {
    if (_connecting || _stopped)
        return;
    _connecting = true;

    // rtm.connect returns a one-use, pre-authenticated wss URL (the URL itself
    // carries auth, so the WebSocket open needs no cookie/bearer).
    QNetworkRequest req{_rtmConnectUrl};
    req.setRawHeader("Authorization", QByteArray("Bearer ") + _token.toUtf8());
    req.setTransferTimeout(20'000); // `d` cookie supplied by the jar
    _openReply = _nam->get(req);
    connect(_openReply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = _openReply;
        if (!reply)
            return;
        _openReply = nullptr;
        reply->deleteLater();
        if (_stopped)
            return;
        const auto    obj = QJsonDocument::fromJson(reply->readAll()).object();
        const QString url = obj.value(QStringLiteral("url")).toString();
        if (!obj.value(QStringLiteral("ok")).toBool() || url.isEmpty()) {
            qWarning().noquote() << "SessionRealtime: rtm.connect failed —"
                                 << obj.value(QStringLiteral("error")).toString("no_url");
            _connecting = false;
            scheduleReconnect();
            return;
        }
        connectWs(QUrl(url));
    });
}

void SessionRealtime::connectWs(const QUrl &url) {
    if (_ws) { // defensive: never leak/overlap a previous socket
        _ws->disconnect(this);
        _ws->abort();
        _ws->deleteLater();
        _ws = nullptr;
    }
    _ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(_ws, &QWebSocket::connected, this, &SessionRealtime::onConnected);
    connect(_ws, &QWebSocket::disconnected, this, &SessionRealtime::onDisconnected);
    connect(_ws, &QWebSocket::textMessageReceived, this, &SessionRealtime::onTextMessage);
    connect(_ws, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        // Treat a socket error like a disconnect; onDisconnected schedules retry.
        if (_ws && _ws->state() != QAbstractSocket::ConnectedState)
            onDisconnected();
    });
    _ws->open(url);
}

void SessionRealtime::onConnected() {
    _connecting       = false;
    _connectedSinceMs = QDateTime::currentMSecsSinceEpoch();
    qInfo().noquote() << "SessionRealtime: RTM connected";
    if (!_ping) {
        _ping = new QTimer(this);
        connect(_ping, &QTimer::timeout, this, &SessionRealtime::sendPing);
    }
    _ping->start(kPingIntervalMs);
}

void SessionRealtime::onDisconnected() {
    _connecting = false;
    if (_ping)
        _ping->stop();
    // Reset the backoff only if the socket proved durable; a quick drop keeps the
    // exponential backoff so we don't hammer rtm.connect into a ratelimit.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (_ws) {
        qInfo().noquote() << "SessionRealtime: RTM disconnected — code" << _ws->closeCode()
                          << "reason" << _ws->closeReason() << "err" << _ws->errorString() << "(up"
                          << (_connectedSinceMs ? now - _connectedSinceMs : 0) << "ms)";
    }
    const bool durable = _connectedSinceMs && (now - _connectedSinceMs) >= _stableMs;
    if (durable)
        _reconnectMs = 1000;
    _connectedSinceMs = 0;
    if (_ws) {
        _ws->disconnect(this); // no more slot callbacks from this socket
        _ws->deleteLater();
        _ws = nullptr;
    }
    if (!_stopped)
        scheduleReconnect();
}

void SessionRealtime::scheduleReconnect() {
    if (_stopped || _reconnectPending)
        return; // coalesce: only one pending reconnect at a time
    const int delay   = _reconnectMs;
    _reconnectMs      = std::min(_reconnectMs * 2, kMaxReconnectMs);
    _reconnectPending = true;
    QTimer::singleShot(delay, this, [this]() {
        _reconnectPending = false;
        if (!_stopped && (!_ws || _ws->state() != QAbstractSocket::ConnectedState))
            openAndConnect();
    });
}

void SessionRealtime::sendPing() {
    if (_ws && _ws->state() == QAbstractSocket::ConnectedState)
        _ws->sendTextMessage(QStringLiteral("{\"type\":\"ping\",\"id\":%1}").arg(++_pingId));
}

void SessionRealtime::onTextMessage(const QString &text) {
    const auto frame = QJsonDocument::fromJson(text.toUtf8()).object();
    const auto type  = frame.value("type").toString();

    if (type == "hello") {
        if (_hadHello) {
            // Session re-established after a gap — Slack doesn't replay missed
            // events, so have the backend/UI backfill history + badges.
            _events->fire_copy(Event{EvRealtimeReconnected{}});
        }
        _hadHello = true;
        return;
    }
    // pong (reply to our ping) and reply_to (ack of a message WE sent over RTM,
    // which we don't do) carry no event to surface.
    if (type == "pong" || frame.contains("reply_to"))
        return;

    if (auto ev = normalizeSlackEvent(frame))
        _events->fire_copy(*ev);
    if (auto huddle = huddleEventFor(frame))
        _events->fire_copy(*huddle);
}

} // namespace slack

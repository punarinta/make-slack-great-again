// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Covers the Socket Mode liveness watchdog: after a laptop sleeps, the TCP
// connection backing the WebSocket is severed silently, leaving a half-open
// ("connected" but dead) socket whose `disconnected` signal never fires. The
// watchdog pings the socket and, when no frame/pong comes back within the
// deadline, forces a reconnect. These tests exercise both the positive path
// (silent peer → reconnect) and the negative path (healthy peer → no churn).

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocketServer>
#include <QWebSocket>

#include "backend/slack/socket_mode_realtime.h"
#include "rpl/producer.h"

#include "fake_http_server.h"

using slack::SocketModeRealtime;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// Pumps the Qt event loop until pred() returns true or timeoutMs elapses.
static bool waitFor(std::function<bool()> pred, int timeoutMs = 5000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!pred() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

// ── DeadPeerWsServer ──────────────────────────────────────────────────────────
// A minimal WebSocket server that completes the opening handshake and then
// goes completely silent: it never sends frames and never replies to pings.
// This models the half-open socket a sleeping laptop leaves behind — the kind
// QWebSocketServer can't simulate, because a live QWebSocket auto-replies to
// every ping with a pong and would keep the connection looking alive.
class DeadPeerWsServer {
public:
    DeadPeerWsServer() {
        QObject::connect(&_server, &QTcpServer::newConnection, &_server, [this] { onConn(); });
        _server.listen(QHostAddress::LocalHost);
    }

    QString wsUrl() const { return QString("ws://127.0.0.1:%1/").arg(_server.serverPort()); }

    int handshakes = 0; // number of completed opening handshakes (== connect attempts)

private:
    void onConn() {
        auto *sock = _server.nextPendingConnection();
        auto *buf  = new QByteArray;
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
            buf->append(sock->readAll());
            const int headerEnd = buf->indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return; // wait for full handshake request

            // Pull the client's Sec-WebSocket-Key and compute the accept token.
            QByteArray key;
            for (const QByteArray &line : buf->left(headerEnd).split('\n')) {
                const QByteArray l = line.trimmed();
                if (l.toLower().startsWith("sec-websocket-key:"))
                    key = l.mid(QByteArray("sec-websocket-key:").size()).trimmed();
            }
            const QByteArray accept =
                QCryptographicHash::hash(
                    key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1
                )
                    .toBase64();

            QByteArray resp = "HTTP/1.1 101 Switching Protocols\r\n";
            resp += "Upgrade: websocket\r\n";
            resp += "Connection: Upgrade\r\n";
            resp += "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
            sock->write(resp);
            sock->flush();
            ++handshakes;
            // Deliberately ignore everything after this: no frames, no pongs.
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, [sock, buf] {
            delete buf;
            sock->deleteLater();
        });
    }

    QTcpServer _server;
};

// Builds the apps.connections.open JSON Slack returns, pointing at `wsUrl`.
static QByteArray openOk(const QString &wsUrl) {
    return QByteArray(R"({"ok":true,"url":")") + wsUrl.toUtf8() + R"("})";
}

// ── ConnCountingWsServer ────────────────────────────────────────────────────────
// A healthy WebSocket server that tracks how many connections it has accepted in
// total and the high-water mark of simultaneously-open connections. Used to prove
// the client never holds two sockets to the same app at once (Slack would then
// load-balance events across them and the client would only read one).
class ConnCountingWsServer {
public:
    ConnCountingWsServer() : _server("test", QWebSocketServer::NonSecureMode) {
        _server.listen(QHostAddress::LocalHost);
        QObject::connect(&_server, &QWebSocketServer::newConnection, &_server, [this] {
            auto *peer = _server.nextPendingConnection();
            _peers.push_back(peer);
            ++total;
            ++live;
            maxLive = std::max(maxLive, live);
            QObject::connect(peer, &QWebSocket::disconnected, peer, [this] { --live; });
        });
    }
    ~ConnCountingWsServer() {
        for (auto *p : _peers)
            p->deleteLater();
    }

    QString wsUrl() const { return QString("ws://127.0.0.1:%1/").arg(_server.serverPort()); }

    int total   = 0; // connections accepted over the test's lifetime
    int live    = 0; // currently open
    int maxLive = 0; // high-water mark of simultaneously open

private:
    QWebSocketServer          _server;
    std::vector<QWebSocket *> _peers;
};

TEST_CASE("watchdog reconnects when the socket goes silent") {
    DeadPeerWsServer ws;
    FakeHttpServer   http;
    // Each (re)connect performs one apps.connections.open POST; queue several.
    for (int i = 0; i < 6; ++i)
        http.enqueue(openOk(ws.wsUrl()));

    SocketModeRealtime rt("xapp-test-token");
    rt.setConnectionsOpenUrlForTest(QUrl(http.baseUrl() + "apps.connections.open"));
    rt.setWatchdogTimingForTest(/*watchdogMs=*/40, /*staleMs=*/120);
    rt.start();

    // First handshake establishes the connection.
    REQUIRE(waitFor([&] { return ws.handshakes >= 1; }));
    // The peer never pongs, so activity goes stale and the watchdog forces a
    // reconnect — yielding a second (and further) handshakes.
    REQUIRE(waitFor([&] { return ws.handshakes >= 2; }));

    rt.stop();
}

TEST_CASE("a re-established socket fires EvRealtimeReconnected; the first hello does not") {
    // Slack's Socket Mode does not replay events missed while disconnected, so
    // on every reconnect the higher layers must backfill — driven by an
    // EvRealtimeReconnected fired on the second (and later) "hello", never the
    // first. The server sends hello on each connection and recycles the first
    // one (refresh_requested) so the client reconnects and greets again.
    QWebSocketServer ws("test", QWebSocketServer::NonSecureMode);
    REQUIRE(ws.listen(QHostAddress::LocalHost));
    std::vector<QWebSocket *> peers;
    int                       helloCount = 0;
    QObject::connect(&ws, &QWebSocketServer::newConnection, &ws, [&] {
        auto *peer = ws.nextPendingConnection();
        peers.push_back(peer);
        peer->sendTextMessage(R"({"type":"hello"})");
        if (++helloCount == 1)
            peer->sendTextMessage(R"({"type":"disconnect","reason":"refresh_requested"})");
    });

    FakeHttpServer http;
    const QString  wsUrl = QString("ws://127.0.0.1:%1/").arg(ws.serverPort());
    for (int i = 0; i < 4; ++i)
        http.enqueue(openOk(wsUrl));

    SocketModeRealtime rt("xapp-test-token");
    rt.setConnectionsOpenUrlForTest(QUrl(http.baseUrl() + "apps.connections.open"));

    int                      reconnectedEvents = 0;
    rpl::lifetime            lt;
    rpl::event_stream<Event> sink;
    sink.events() | rpl::on_next(
                        [&](Event e) {
                            if (std::get_if<EvRealtimeReconnected>(&e))
                                ++reconnectedEvents;
                        },
                        lt
                    );
    rt.addSink(&sink);
    rt.start();

    // The second hello (after the forced recycle) must produce exactly one event.
    REQUIRE(waitFor([&] { return reconnectedEvents >= 1; }));
    CHECK(helloCount >= 2);
    CHECK(reconnectedEvents == 1);

    rt.stop();
    for (auto *p : peers)
        p->deleteLater();
}

TEST_CASE("overlapping connect triggers never open competing connections") {
    // Reconnect can be triggered from several places at once (a real disconnect,
    // the liveness watchdog, the reachability watcher). Each extra trigger that
    // races an in-flight handshake used to start its own apps.connections.open
    // and thus its own WebSocket — leaving the app with multiple live sockets.
    // Slack delivers each event to only ONE of an app's sockets, so the ones the
    // app isn't actively reading silently swallow a share of messages (the user
    // sees "1, 3, 5" until a REST history refetch heals the gap). The
    // single-flight guard must coalesce the extra triggers into one connection.
    ConnCountingWsServer ws;
    FakeHttpServer       http;
    for (int i = 0; i < 6; ++i)
        http.enqueue(openOk(ws.wsUrl()));

    SocketModeRealtime rt("xapp-test-token");
    rt.setConnectionsOpenUrlForTest(QUrl(http.baseUrl() + "apps.connections.open"));
    rt.start();
    // Two more reconnect triggers fire while the initial handshake is still in
    // flight (the POST reply hasn't been processed yet — no event loop has run).
    rt.connectNowForTest();
    rt.connectNowForTest();

    REQUIRE(waitFor([&] { return ws.total >= 1; }));
    // Give any erroneously-queued extra handshakes time to resolve into sockets.
    waitFor([] { return false; }, 300);

    int opens = 0;
    for (const QByteArray &p : http.requestPaths)
        if (p.endsWith("apps.connections.open"))
            ++opens;

    // Exactly one handshake, one socket ever, and never two at once.
    CHECK(opens == 1);
    CHECK(ws.total == 1);
    CHECK(ws.maxLive == 1);

    rt.stop();
}

TEST_CASE("watchdog leaves a healthy idle connection alone") {
    // A real QWebSocket server auto-replies to pings with pongs, so the
    // connection keeps showing activity even with no app traffic.
    QWebSocketServer ws("test", QWebSocketServer::NonSecureMode);
    REQUIRE(ws.listen(QHostAddress::LocalHost));
    int                       connections = 0;
    // Keep the accepted sockets alive for the duration of the test.
    std::vector<QWebSocket *> peers;
    QObject::connect(&ws, &QWebSocketServer::newConnection, &ws, [&] {
        ++connections;
        peers.push_back(ws.nextPendingConnection());
    });

    FakeHttpServer http;
    const QString  wsUrl = QString("ws://127.0.0.1:%1/").arg(ws.serverPort());
    for (int i = 0; i < 4; ++i)
        http.enqueue(openOk(wsUrl));

    SocketModeRealtime rt("xapp-test-token");
    rt.setConnectionsOpenUrlForTest(QUrl(http.baseUrl() + "apps.connections.open"));
    rt.setWatchdogTimingForTest(/*watchdogMs=*/40, /*staleMs=*/120);
    rt.start();

    REQUIRE(waitFor([&] { return connections >= 1; }));

    // Run well past several watchdog ticks and the stale deadline. A healthy
    // connection must NOT be torn down: still exactly one connection.
    waitFor([] { return false; }, 400);
    CHECK(connections == 1);

    rt.stop();
    for (auto *p : peers)
        p->deleteLater();
}

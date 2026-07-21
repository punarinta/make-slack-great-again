// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Covers the Socket Mode liveness watchdog. Its sole job is the one failure no
// transport signal reports: after the machine suspends (laptop sleep) TCP is severed
// silently, leaving a half-open ("connected" but dead) socket whose `disconnected`
// never fires. The watchdog notices via its OWN tick cadence — a wall-clock gap
// between ticks far larger than its interval means the process was frozen — and
// reconnects. It deliberately does NOT reconnect on mere silence (Slack never pongs
// our client pings, so a quiet-but-healthy socket is indistinguishable from a dead
// one). These tests exercise the positive path (a simulated suspend gap → reconnect)
// and the negative path (a healthy, normally-ticking connection → no churn).

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
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
// A minimal WebSocket server that completes the opening handshake and then goes
// completely silent: it never sends frames and never replies to pings — exactly
// like real Slack, which does not pong client pings. QWebSocketServer can't model
// this because a live QWebSocket auto-replies to every ping with a pong. The
// watchdog leaves such a quiet-but-connected peer alone; only a simulated suspend
// gap (see the test below) makes it reconnect.
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

TEST_CASE("watchdog reconnects after a suspend gap") {
    DeadPeerWsServer ws;
    FakeHttpServer   http;
    // Each (re)connect performs one apps.connections.open POST; queue several.
    for (int i = 0; i < 6; ++i)
        http.enqueue(openOk(ws.wsUrl()));

    SocketModeRealtime rt("xapp-test-token");
    rt.setConnectionsOpenUrlForTest(QUrl(http.baseUrl() + "apps.connections.open"));
    // watchdogMs = tick interval; staleMs = the between-tick gap that counts as a
    // process suspend.
    rt.setWatchdogTimingForTest(/*watchdogMs=*/40, /*staleMs=*/120);
    rt.start();

    // First handshake establishes the connection; let onConnected settle so the
    // watchdog sees a CONNECTED socket.
    REQUIRE(waitFor([&] { return ws.handshakes >= 1; }));
    waitFor([] { return false; }, 100);

    // Simulate the process being frozen across a watchdog interval (laptop sleep):
    // block the event loop past the suspend threshold, so the next tick sees a large
    // wall-clock gap between ticks and force-reconnects — a second handshake. Mere
    // silence would NOT do this (the peer never pongs and we don't reconnect on that).
    QThread::msleep(250);
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

TEST_CASE("a recycle warning opens an overlapping replacement before dropping the old socket") {
    // Slack recycles Socket Mode connections periodically: it sends a `disconnect`
    // with reason "warning" shortly before it actually closes the socket, and
    // expects the client to bring up a replacement and keep reading the warned
    // socket until the new one is live. Doing so leaves no window without a live
    // socket — Slack does not replay events missed while disconnected, so a
    // close-then-reconnect gap silently drops messages. Prove the client opens the
    // replacement WHILE the warned socket is still open (both live at once), then
    // converges back to a single live socket once the new one takes over.
    QWebSocketServer ws("test", QWebSocketServer::NonSecureMode);
    REQUIRE(ws.listen(QHostAddress::LocalHost));
    std::vector<QWebSocket *> peers;
    int                       total = 0, live = 0, maxLive = 0, warned = 0;
    QObject::connect(&ws, &QWebSocketServer::newConnection, &ws, [&] {
        auto *peer = ws.nextPendingConnection();
        peers.push_back(peer);
        ++total;
        ++live;
        maxLive = std::max(maxLive, live);
        QObject::connect(peer, &QWebSocket::disconnected, peer, [&] { --live; });
        peer->sendTextMessage(R"({"type":"hello"})");
        // Warn the very first socket of an imminent recycle, but never close it
        // server-side: the client must drop it itself, and only after its
        // replacement is connected.
        if (++warned == 1)
            peer->sendTextMessage(R"({"type":"disconnect","reason":"warning"})");
    });

    FakeHttpServer http;
    const QString  wsUrl = QString("ws://127.0.0.1:%1/").arg(ws.serverPort());
    for (int i = 0; i < 6; ++i)
        http.enqueue(openOk(wsUrl));

    SocketModeRealtime rt("xapp-test-token");
    rt.setConnectionsOpenUrlForTest(QUrl(http.baseUrl() + "apps.connections.open"));
    rt.start();

    // Both sockets live at once → a genuine overlap, not close-then-reconnect.
    REQUIRE(waitFor([&] { return maxLive >= 2; }));
    // The client then drops the warned socket, converging back to exactly one.
    REQUIRE(waitFor([&] { return live == 1; }));
    CHECK(total >= 2);

    rt.stop();
    for (auto *p : peers)
        p->deleteLater();
}

TEST_CASE("reconnectNow recovers a silent stall with a gapless overlapping replacement") {
    // When the safety poll detects the stream silently missed events, it calls
    // reconnectNow() on a socket that is still CONNECTED (Slack stopped routing
    // events but the socket still answers pings). A teardown-first reconnect here
    // would open a window with no live socket; Slack does not replay events missed
    // while disconnected, so it would drop events for every workspace on this
    // shared socket — and the next poll would flag THOSE as missed, spinning a
    // reconnect storm. Prove reconnectNow instead opens the replacement WHILE the
    // stalled socket is still live (both live at once), then converges to one.
    QWebSocketServer ws("test", QWebSocketServer::NonSecureMode);
    REQUIRE(ws.listen(QHostAddress::LocalHost));
    std::vector<QWebSocket *> peers;
    int                       total = 0, live = 0, maxLive = 0;
    QObject::connect(&ws, &QWebSocketServer::newConnection, &ws, [&] {
        auto *peer = ws.nextPendingConnection();
        peers.push_back(peer);
        ++total;
        ++live;
        maxLive = std::max(maxLive, live);
        QObject::connect(peer, &QWebSocket::disconnected, peer, [&] { --live; });
        // The server never closes a socket itself: the client must drop the
        // stalled one on its own, and only after the replacement is connected.
        peer->sendTextMessage(R"({"type":"hello"})");
    });

    FakeHttpServer http;
    const QString  wsUrl = QString("ws://127.0.0.1:%1/").arg(ws.serverPort());
    for (int i = 0; i < 6; ++i)
        http.enqueue(openOk(wsUrl));

    SocketModeRealtime rt("xapp-test-token");
    rt.setConnectionsOpenUrlForTest(QUrl(http.baseUrl() + "apps.connections.open"));
    rt.start();

    // Wait until the socket is actually connected, so reconnectNow() takes the
    // gapless overlapping path rather than the not-connected fallback.
    REQUIRE(waitFor([&] { return rt.isConnectedForTest(); }));
    REQUIRE(total == 1);

    rt.reconnectNow();

    // Both sockets live at once → a genuine overlap, not close-then-reconnect.
    REQUIRE(waitFor([&] { return maxLive >= 2; }));
    // The client then drops the stalled socket, converging back to exactly one.
    REQUIRE(waitFor([&] { return live == 1; }));
    CHECK(total >= 2);

    rt.stop();
    for (auto *p : peers)
        p->deleteLater();
}

TEST_CASE("repeated bare code-1000 closes WITHOUT other connections do NOT flag contention") {
    // Slack cleanly closing our LIVE socket (WebSocket close code 1000) with no
    // preceding "disconnect" envelope, over and over, is ambiguous: it can be an
    // eviction by another instance sharing the xapp token OR Slack idle-closing a
    // socket whose keepalives don't traverse the network (a proxy/VPN/firewall
    // dropping WS control frames — observed as a near-constant ~10 s lifetime with
    // num_connections == ours). Because the close alone can't distinguish them,
    // EvRealtimeContended (which tells the user "another device stole your
    // connection") must NOT fire on bare closes unless a hello actually counted
    // other connections. Model the network case: a server that accepts each
    // connection and immediately closes it cleanly, envelope-free and hello-free.
    QWebSocketServer ws("test", QWebSocketServer::NonSecureMode);
    REQUIRE(ws.listen(QHostAddress::LocalHost));
    std::vector<QWebSocket *> peers;
    int                       total = 0;
    QObject::connect(&ws, &QWebSocketServer::newConnection, &ws, [&] {
        auto *peer = ws.nextPendingConnection();
        peers.push_back(peer);
        ++total;
        // Evict immediately: a clean normal-closure close, no disconnect envelope.
        peer->close(QWebSocketProtocol::CloseCodeNormal);
    });

    FakeHttpServer http;
    const QString  wsUrl = QString("ws://127.0.0.1:%1/").arg(ws.serverPort());
    for (int i = 0; i < 12; ++i)
        http.enqueue(openOk(wsUrl));

    SocketModeRealtime rt("xapp-test-token");
    rt.setConnectionsOpenUrlForTest(QUrl(http.baseUrl() + "apps.connections.open"));

    int                      contended = 0;
    rpl::lifetime            lt;
    rpl::event_stream<Event> sink;
    sink.events() | rpl::on_next(
                        [&](Event e) {
                            if (std::get_if<EvRealtimeContended>(&e))
                                ++contended;
                        },
                        lt
                    );
    rt.addSink(&sink);
    rt.start();

    // Let several bare closes cluster (enough to trip the old counter-based guard).
    REQUIRE(waitFor([&] { return total >= 3; }, 15000));
    // No hello ever reported other connections, so contention must NOT be flagged.
    CHECK(contended == 0);

    rt.stop();
    for (auto *p : peers)
        p->deleteLater();
}

TEST_CASE("a hello reporting num_connections > ours raises EvRealtimeContended") {
    // The direct detector: Slack's hello frame reports num_connections — how many
    // sockets the app (xapp token) holds fleet-wide. We hold exactly one here, so
    // a hello claiming two means another client is on the same token. msga is
    // single-instance per user, so that client is provably not a second local
    // copy — the event's otherConnections lets the UI say the cause is elsewhere.
    QWebSocketServer ws("test", QWebSocketServer::NonSecureMode);
    REQUIRE(ws.listen(QHostAddress::LocalHost));
    std::vector<QWebSocket *> peers;
    QObject::connect(&ws, &QWebSocketServer::newConnection, &ws, [&] {
        auto *peer = ws.nextPendingConnection();
        peers.push_back(peer);
        peer->sendTextMessage(
            R"({"type":"hello","num_connections":2,"debug_info":{"host":"applink-test"}})"
        );
    });

    FakeHttpServer http;
    const QString  wsUrl = QString("ws://127.0.0.1:%1/").arg(ws.serverPort());
    for (int i = 0; i < 4; ++i)
        http.enqueue(openOk(wsUrl));

    SocketModeRealtime rt("xapp-test-token");
    rt.setConnectionsOpenUrlForTest(QUrl(http.baseUrl() + "apps.connections.open"));

    int                      contended = 0, reportedOthers = -1;
    rpl::lifetime            lt;
    rpl::event_stream<Event> sink;
    sink.events() | rpl::on_next(
                        [&](Event e) {
                            if (auto *c = std::get_if<EvRealtimeContended>(&e)) {
                                ++contended;
                                reportedOthers = c->otherConnections;
                            }
                        },
                        lt
                    );
    rt.addSink(&sink);
    rt.start();

    // A single hello suffices — no reconnect storm needed.
    REQUIRE(waitFor([&] { return contended >= 1; }));
    // num_connections(2) − our one socket = one competitor.
    CHECK(reportedOthers == 1);

    rt.stop();
    for (auto *p : peers)
        p->deleteLater();
}

TEST_CASE("a hello reporting num_connections == ours does NOT flag contention") {
    // The healthy baseline: we hold one socket and Slack reports one. This must
    // never fire — otherwise every normal session would nag about a phantom
    // competitor.
    QWebSocketServer ws("test", QWebSocketServer::NonSecureMode);
    REQUIRE(ws.listen(QHostAddress::LocalHost));
    std::vector<QWebSocket *> peers;
    QObject::connect(&ws, &QWebSocketServer::newConnection, &ws, [&] {
        auto *peer = ws.nextPendingConnection();
        peers.push_back(peer);
        peer->sendTextMessage(R"({"type":"hello","num_connections":1})");
    });

    FakeHttpServer http;
    const QString  wsUrl = QString("ws://127.0.0.1:%1/").arg(ws.serverPort());
    for (int i = 0; i < 4; ++i)
        http.enqueue(openOk(wsUrl));

    SocketModeRealtime rt("xapp-test-token");
    rt.setConnectionsOpenUrlForTest(QUrl(http.baseUrl() + "apps.connections.open"));

    int                      contended = 0;
    rpl::lifetime            lt;
    rpl::event_stream<Event> sink;
    sink.events() | rpl::on_next(
                        [&](Event e) {
                            if (std::get_if<EvRealtimeContended>(&e))
                                ++contended;
                        },
                        lt
                    );
    rt.addSink(&sink);
    rt.start();

    // Let the hello be processed and give any erroneous notice time to fire.
    waitFor([] { return false; }, 300);
    CHECK(contended == 0);

    rt.stop();
    for (auto *p : peers)
        p->deleteLater();
}

TEST_CASE("watchdog leaves a healthy idle connection alone") {
    // The watchdog reconnects only on a suspend-sized gap between its own ticks, not
    // on silence. A normally-ticking event loop never produces that gap, so a healthy
    // idle connection is left completely alone even with no app traffic.
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

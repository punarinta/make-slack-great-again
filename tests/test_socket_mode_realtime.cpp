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

#include "backend/public_backend/socket_mode_realtime.h"

#include "fake_http_server.h"

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

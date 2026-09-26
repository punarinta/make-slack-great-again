// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// RtmPresence: the RTM socket held on a session token so Slack counts msga as a
// connected client (→ the user appears "active" without the official app). The
// contract under test:
//   • the socket handshake carries the `d` cookie (the wss URL has no auth of its
//     own — without the cookie Slack sends an invalid_auth frame and drops it);
//   • Active is declared on `hello`, kept alive with json pings, and tickled;
//   • Native drops the link; a server-side close reconnects with backoff;
//   • a fatal rtm.connect refusal (OAuth token) gives up instead of retrying;
//   • WhileUsing idles the link out and brings it back on real input; activity
//     tickles are throttled;
//   • a socket that stops ponging is replaced.

#include <catch2/catch_test_macros.hpp>

#include "test_main.h"
#include "test_support.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>

#include "backend/slack/rtm_presence.h"
#include "backend/slack/web_api_client.h"

#include "fake_http_server.h"

using slack::RtmPresence;

MSGA_TEST_MAIN(argc, argv) {
    return msga_test::runCoreApp(argc, argv);
}

static bool waitFor(std::function<bool()> pred, int timeoutMs = 5000) {
    return msga_test::waitFor(std::move(pred), timeoutMs);
}

static void pumpFor(int ms) {
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// A stand-in Slack gateway: sends `hello` on connect, pongs every json ping
// (unless told not to), records every frame and the handshake's Cookie header.
class FakeGateway {
public:
    FakeGateway() : _server("test", QWebSocketServer::NonSecureMode) {
        _server.listen(QHostAddress::LocalHost);
        QObject::connect(&_server, &QWebSocketServer::newConnection, &_server, [this] {
            auto *peer = _server.nextPendingConnection();
            peers.push_back(peer);
            cookies.push_back(QString::fromLatin1(peer->request().rawHeader("Cookie")));
            live.push_back(true);
            const int idx = int(peers.size()) - 1;
            QObject::connect(peer, &QWebSocket::disconnected, peer, [this, idx] {
                live[idx] = false;
            });
            QObject::connect(
                peer, &QWebSocket::textMessageReceived, peer, [this, peer](const QString &t) {
                    frames.push_back(t);
                    const auto obj = QJsonDocument::fromJson(t.toUtf8()).object();
                    if (obj.value("type").toString() == "ping") {
                        pings++;
                        if (pong)
                            peer->sendTextMessage(
                                QStringLiteral("{\"type\":\"pong\",\"reply_to\":%1}")
                                    .arg(obj.value("id").toInt())
                            );
                    } else if (obj.value("type").toString() == "tickle") {
                        tickles++;
                    }
                }
            );
            if (sayHello)
                peer->sendTextMessage(QStringLiteral(R"({"type":"hello","start":true})"));
        });
    }
    ~FakeGateway() {
        for (auto *p : peers)
            p->deleteLater();
    }

    QString wsUrl() const { return QString("ws://127.0.0.1:%1/").arg(_server.serverPort()); }

    std::vector<QWebSocket *> peers;
    std::vector<QString>      cookies; // Cookie header of each handshake
    std::vector<bool>         live;
    QStringList               frames;
    int                       pings    = 0;
    int                       tickles  = 0;
    bool                      pong     = true;
    bool                      sayHello = true;

private:
    QWebSocketServer _server;
};

struct Harness {
    FakeHttpServer                 http;
    FakeGateway                    gw;
    slack::WebApiClient            api;
    RtmPresence                    rtm{&api, QStringLiteral("xoxd-cookie")};
    std::vector<PresenceLinkState> states;

    Harness() {
        api.setBaseUrl(http.baseUrl());
        api.setToken(QStringLiteral("xoxc-token"));
        api.setCookie(QStringLiteral("xoxd-cookie"));
        rtm.setTimingForTest(
            RtmPresence::Timing{
                /*pingMs=*/80,
                /*tickleGapMs=*/400,
                /*alwaysTickleMs=*/120,
                /*idleMs=*/300,
                /*reconnectMinMs=*/40,
                /*reconnectMaxMs=*/160,
            }
        );
        QObject::connect(&rtm, &RtmPresence::stateChanged, &rtm, [this](PresenceLinkState s) {
            states.push_back(s);
        });
    }

    // Queue one successful rtm.connect answer pointing at the fake gateway.
    void enqueueConnectOk() {
        http.enqueue(QStringLiteral(R"({"ok":true,"url":"%1"})").arg(gw.wsUrl()).toUtf8());
    }
    bool seen(PresenceLinkState s) const {
        return std::find(states.begin(), states.end(), s) != states.end();
    }
};

TEST_CASE("WhileRunning holds an authenticated RTM socket and keeps it alive", "[rtm]") {
    Harness h;
    h.enqueueConnectOk();

    h.rtm.setMode(PresenceMode::WhileRunning);
    REQUIRE(waitFor([&] { return h.gw.peers.size() == 1; }));
    // The handshake — not the URL — carries the auth.
    CHECK(h.gw.cookies[0] == "d=xoxd-cookie");
    REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Active; }));
    CHECK(h.states.front() == PresenceLinkState::Connecting);

    // Pings flow at the test cadence and are pong'ed; tickles keep coming in
    // WhileRunning mode even with no input at all.
    REQUIRE(waitFor([&] { return h.gw.pings >= 3 && h.gw.tickles >= 3; }, 3000));
    CHECK(h.rtm.isConnectedForTest());
    CHECK(h.rtm.state() == PresenceLinkState::Active);
    // rtm.connect was asked exactly once.
    CHECK(h.http.requestCount == 1);
    CHECK(h.http.requestPaths.size() == 1);
    CHECK(h.http.requestPaths[0].startsWith("/rtm.connect"));

    SECTION("Native drops the link") {
        h.rtm.setMode(PresenceMode::Native);
        REQUIRE(waitFor([&] { return !h.gw.live[0]; }));
        CHECK(h.rtm.state() == PresenceLinkState::Off);
        pumpFor(300);
        CHECK(h.http.requestCount == 1); // nothing reconnects
    }

    SECTION("a server-side close reconnects — with the cookie again") {
        h.enqueueConnectOk();
        h.gw.peers[0]->close();
        REQUIRE(waitFor([&] { return h.gw.peers.size() == 2; }, 3000));
        CHECK(h.gw.cookies[1] == "d=xoxd-cookie");
        REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Active; }));
        CHECK(h.seen(PresenceLinkState::Connecting));
    }

    SECTION("a socket that stops ponging is replaced") {
        h.gw.pong = false;
        h.enqueueConnectOk();
        REQUIRE(waitFor([&] { return h.gw.peers.size() == 2; }, 3000));
        CHECK(!h.gw.live[0]);
        REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Active; }));
    }
}

TEST_CASE("a fatal rtm.connect refusal gives up instead of retrying", "[rtm]") {
    Harness h;
    h.http.enqueue(QByteArrayLiteral(R"({"ok":false,"error":"not_allowed_token_type"})"));

    h.rtm.setMode(PresenceMode::WhileRunning);
    REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Unavailable; }));
    pumpFor(400); // several backoff periods
    CHECK(h.http.requestCount == 1);
    CHECK(h.gw.peers.empty());

    // A deliberate mode change is a fresh start (the user may have re-signed in).
    h.enqueueConnectOk();
    h.rtm.setMode(PresenceMode::WhileUsing);
    REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Active; }));
}

TEST_CASE("a transient rtm.connect failure backs off and retries", "[rtm]") {
    Harness h;
    h.http.enqueue(QByteArrayLiteral(R"({"ok":false,"error":"ratelimited"})"));
    h.enqueueConnectOk();

    h.rtm.setMode(PresenceMode::WhileRunning);
    REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Active; }, 3000));
    CHECK(h.http.requestCount == 2);
}

TEST_CASE("WhileUsing idles the link out and brings it back on input", "[rtm]") {
    Harness h;
    h.enqueueConnectOk();

    h.rtm.setMode(PresenceMode::WhileUsing);
    REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Active; }));
    // No input for idleMs → the link is dropped on purpose and stays down.
    REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Idle; }, 3000));
    REQUIRE(waitFor([&] { return !h.gw.live[0]; }));
    pumpFor(300);
    CHECK(h.gw.peers.size() == 1);
    CHECK(h.http.requestCount == 1);

    // Real input reconnects at once.
    h.enqueueConnectOk();
    h.rtm.noteActivity();
    REQUIRE(waitFor([&] { return h.gw.peers.size() == 2; }));
    REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Active; }));
}

TEST_CASE("activity tickles are throttled", "[rtm]") {
    Harness h;
    h.enqueueConnectOk();

    // No periodic tickle in this mode — and an idle timeout well past the waits
    // below, so the link can't idle out mid-test.
    h.rtm.setTimingForTest(
        RtmPresence::Timing{
            /*pingMs=*/80,
            /*tickleGapMs=*/400,
            /*alwaysTickleMs=*/120,
            /*idleMs=*/5000,
            /*reconnectMinMs=*/40,
            /*reconnectMaxMs=*/160,
        }
    );
    h.rtm.setMode(PresenceMode::WhileUsing);
    REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Active; }));
    // `hello` sends one forced tickle so the server registers us immediately.
    REQUIRE(waitFor([&] { return h.gw.tickles == 1; }));
    const int before = h.rtm.ticklesSentForTest();

    // A burst of input inside tickleGapMs yields no further frame…
    for (int i = 0; i < 5; ++i)
        h.rtm.noteActivity();
    pumpFor(100);
    CHECK(h.rtm.ticklesSentForTest() == before);
    // …but input after the gap does (and keeps the idle clock from firing).
    pumpFor(350);
    h.rtm.noteActivity();
    REQUIRE(waitFor([&] { return h.rtm.ticklesSentForTest() == before + 1; }));
    CHECK(h.rtm.state() == PresenceLinkState::Active);
}

TEST_CASE("an error frame after the handshake forces a reconnect", "[rtm]") {
    Harness h;
    h.gw.sayHello = false;
    h.enqueueConnectOk();

    h.rtm.setMode(PresenceMode::WhileRunning);
    REQUIRE(waitFor([&] { return h.gw.peers.size() == 1; }));
    CHECK(h.rtm.state() == PresenceLinkState::Connecting); // no hello yet → not Active
    h.enqueueConnectOk();
    h.gw.sayHello = true;
    h.gw.peers[0]->sendTextMessage(
        QStringLiteral(R"({"type":"error","error":{"msg":"invalid_auth","code":401}})")
    );
    REQUIRE(waitFor([&] { return h.gw.peers.size() == 2; }, 3000));
    REQUIRE(waitFor([&] { return h.rtm.state() == PresenceLinkState::Active; }));
}

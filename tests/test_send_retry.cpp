// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Transport-failure retry behavior of WebApiClient and the duplicate-free
// send-reconcile loop of PublicBackend::sendMessage.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrlQuery>

#include "auth/token_store.h"
#include "backend/public_backend/public_backend.h"
#include "fake_http_server.h"
#include "network/web_api_client.h"
#include "rpl/producer.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

// Pumps the Qt event loop until pred() returns true or timeoutMs elapses.
static bool waitFor(std::function<bool()> pred, int timeoutMs = 3000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!pred() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

// Pumps the event loop for a fixed duration (to assert nothing else happens).
static void pumpFor(int ms) {
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// =============================================================================
// WebApiClient — transport-failure retries
// =============================================================================

TEST_CASE("idempotent call retries a dropped connection until it succeeds", "[send_retry]") {
    FakeHttpServer server;
    server.dropConnections = 2; // first two attempts die without a response
    server.enqueue(R"({"ok":true,"value":"finally"})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");
    client.setRetryBaseDelayMs(10);

    QString value;
    client.call("conversations.history", QUrlQuery{}, [&](QJsonObject resp) {
        value = resp.value("value").toString();
    });

    REQUIRE(waitFor([&] { return !value.isEmpty(); }));
    CHECK(value == "finally");
    CHECK(server.requestCount == 3);
}

TEST_CASE("non-idempotent call is NOT resent after an ambiguous failure", "[send_retry]") {
    FakeHttpServer server;
    server.dropConnections = 1;
    server.enqueue(R"({"ok":true})"); // must never be consumed

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");
    client.setRetryBaseDelayMs(10);

    QString err;
    client.callNonIdempotent(
        "chat.postMessage", QUrlQuery{}, [](QJsonObject) {}, [&](QString e) { err = e; }
    );

    REQUIRE(waitFor([&] { return !err.isEmpty(); }));
    CHECK(err == WebApiClient::kConnectionLost);
    pumpFor(100); // a blind retry would land here
    CHECK(server.requestCount == 1);
}

TEST_CASE("queued calls survive a retried head call in order", "[send_retry]") {
    FakeHttpServer server;
    server.dropConnections = 1;
    server.enqueue(R"({"ok":true,"first":true})");
    server.enqueue(R"({"ok":true,"second":true})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");
    client.setRetryBaseDelayMs(10);

    bool first = false, second = false;
    client.call("a.method", QUrlQuery{}, [&](QJsonObject r) { first = r.value("first").toBool(); });
    client.call("b.method", QUrlQuery{}, [&](QJsonObject r) {
        second = r.value("second").toBool();
        CHECK(first); // FIFO order preserved across the retry
    });

    REQUIRE(waitFor([&] { return first && second; }));
    CHECK(server.requestCount == 3); // a (dropped) + a (retry) + b
}

// =============================================================================
// PublicBackend::sendMessage — reconcile instead of blind resend
// =============================================================================

namespace {

struct SendFixture {
    FakeHttpServer server;
    PublicBackend  backend{
        TokenStore::Credentials{"xoxp-test", "T1", "Test", "", "", 0},
        TokenStore::AppConfig{"id", "secret", ""}
    };
    std::vector<Event> events;
    rpl::lifetime      lt;

    SendFixture() {
        backend.setApiBaseUrlForTests(server.baseUrl());
        backend.setSendRetryDelayMsForTests(10);
        backend.events() | rpl::on_next([this](Event e) { events.push_back(std::move(e)); }, lt);
    }

    OutgoingMessage out(const QString &text) {
        OutgoingMessage m;
        m.rawText = text;
        m.sinceTs = "100.000000";
        return m;
    }

    const EvMessageNew *newMessageEvent() {
        for (auto &e : events)
            if (auto *ev = std::get_if<EvMessageNew>(&e))
                return ev;
        return nullptr;
    }
    const EvSendFailed *sendFailedEvent() {
        for (auto &e : events)
            if (auto *ev = std::get_if<EvSendFailed>(&e))
                return ev;
        return nullptr;
    }
};

} // namespace

TEST_CASE_METHOD(SendFixture, "send confirms from the chat.postMessage response", "[send_retry]") {
    server.enqueue(
        R"({"ok":true,"ts":"123.456","message":{"ts":"123.456","user":"U1","text":"hello"}})"
    );

    backend.sendMessage(ConversationId{"C1"}, out("hello"));

    REQUIRE(waitFor([&] { return newMessageEvent() != nullptr; }));
    CHECK(newMessageEvent()->msg.ts == "123.456");
    CHECK(server.requestCount == 1);
}

TEST_CASE_METHOD(
    SendFixture, "lost send that WAS delivered is found in history, not resent", "[send_retry]"
) {
    server.dropConnections = 1; // chat.postMessage response is lost
    server.enqueue(             // the reconcile history scan finds the message
        R"({"ok":true,"messages":[{"ts":"124.000","user":"U1","text":"hello"}]})"
    );

    backend.sendMessage(ConversationId{"C1"}, out("hello"));

    REQUIRE(waitFor([&] { return newMessageEvent() != nullptr; }));
    CHECK(newMessageEvent()->msg.ts == "124.000");
    REQUIRE(server.requestPaths.size() == 2);
    CHECK(server.requestPaths[0] == "/chat.postMessage");
    CHECK(server.requestPaths[1] == "/conversations.history"); // no second post
}

TEST_CASE_METHOD(SendFixture, "lost send that was NOT delivered is posted again", "[send_retry]") {
    server.dropConnections = 1;
    server.enqueue(R"({"ok":true,"messages":[]})"); // history: nothing arrived
    server.enqueue(
        R"({"ok":true,"ts":"125.000","message":{"ts":"125.000","user":"U1","text":"hello"}})"
    );

    backend.sendMessage(ConversationId{"C1"}, out("hello"));

    REQUIRE(waitFor([&] { return newMessageEvent() != nullptr; }));
    CHECK(newMessageEvent()->msg.ts == "125.000");
    REQUIRE(server.requestPaths.size() == 3);
    CHECK(server.requestPaths[0] == "/chat.postMessage");
    CHECK(server.requestPaths[1] == "/conversations.history");
    CHECK(server.requestPaths[2] == "/chat.postMessage");
}

TEST_CASE_METHOD(
    SendFixture, "matching tolerates Slack's entity escaping of & < >", "[send_retry]"
) {
    server.dropConnections = 1;
    server.enqueue(R"({"ok":true,"messages":[{"ts":"126.000","user":"U1","text":"a &amp; b"}]})");

    backend.sendMessage(ConversationId{"C1"}, out("a & b"));

    REQUIRE(waitFor([&] { return newMessageEvent() != nullptr; }));
    CHECK(newMessageEvent()->msg.ts == "126.000");
    CHECK(server.requestPaths.size() == 2); // confirmed via history, not resent
}

TEST_CASE_METHOD(SendFixture, "definitive Slack error fires EvSendFailed", "[send_retry]") {
    server.enqueue(R"({"ok":false,"error":"not_in_channel"})");

    backend.sendMessage(ConversationId{"C1"}, out("hello"));

    REQUIRE(waitFor([&] { return sendFailedEvent() != nullptr; }));
    CHECK(sendFailedEvent()->reason == "not_in_channel");
    CHECK(newMessageEvent() == nullptr);
    CHECK(server.requestCount == 1);
}

TEST_CASE_METHOD(
    SendFixture, "thread replies reconcile via conversations.replies", "[send_retry]"
) {
    server.dropConnections = 1;
    // Root message comes first in the replies payload and must be skipped
    // even though its text matches.
    server.enqueue(
        R"({"ok":true,"messages":[{"ts":"100.000","user":"U1","text":"hello"},)"
        R"({"ts":"127.000","user":"U1","text":"hello"}]})"
    );

    OutgoingMessage m = out("hello");
    m.threadRoot      = QString("100.000");
    backend.sendMessage(ConversationId{"C1"}, std::move(m));

    REQUIRE(waitFor([&] { return newMessageEvent() != nullptr; }));
    CHECK(newMessageEvent()->msg.ts == "127.000");
    REQUIRE(server.requestPaths.size() == 2);
    CHECK(server.requestPaths[1] == "/conversations.replies");
}

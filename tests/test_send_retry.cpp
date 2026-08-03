// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Transport-failure retry behavior of WebApiClient and the duplicate-free
// send-reconcile loop of PublicBackend::sendMessage.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUrlQuery>

#include "auth/token_store.h"
#include "backend/slack/public_backend.h"
#include "backend/slack/slack_auth.h"
#include "backend/slack/web_api_client.h"
#include "fake_http_server.h"
#include "rpl/producer.h"

using namespace slack;

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

TEST_CASE("a 429 is honored via Retry-After and the call eventually succeeds", "[send_retry]") {
    FakeHttpServer server;
    server.enqueue429(1);                          // first attempt is rate-limited
    server.enqueue(R"({"ok":true,"value":"ok"})"); // retry after the cooldown succeeds

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");

    QString value;
    client.call("conversations.list", QUrlQuery{}, [&](QJsonObject r) {
        value = r.value("value").toString();
    });

    REQUIRE(waitFor([&] { return !value.isEmpty(); }, 4000));
    CHECK(value == "ok");
    CHECK(server.requestCount == 2);
}

TEST_CASE(
    "a 429 on one method does not block other methods (per-method throttle)", "[send_retry]"
) {
    FakeHttpServer server;
    // Arrival order is deterministic (single-slot FIFO): A is sent first and gets
    // a 429 with a 2 s cooldown; B is sent next and answers immediately; A retries
    // only after its cooldown clears.
    server.enqueue429(2);                         // A: conversations.list — rate-limited 2 s
    server.enqueue(R"({"ok":true,"value":"B"})"); // B: conversations.history — immediate
    server.enqueue(R"({"ok":true,"value":"A"})"); // A: succeeds after the cooldown

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");

    QString aVal, bVal;
    bool    aDoneBeforeB = false;
    client.call("conversations.list", QUrlQuery{}, [&](QJsonObject r) {
        aVal = r.value("value").toString();
    });
    client.call("conversations.history", QUrlQuery{}, [&](QJsonObject r) {
        bVal         = r.value("value").toString();
        aDoneBeforeB = !aVal.isEmpty();
    });

    // B must finish while A is still serving out its Retry-After — proof that A's
    // 429 didn't head-of-line block the whole queue (the old global-throttle bug).
    // The order is captured inside B's callback rather than compared against the
    // wall clock, so a slow machine can't turn a pass into a flake.
    REQUIRE(waitFor([&] { return !bVal.isEmpty(); }, 4000));
    CHECK(bVal == "B");
    CHECK_FALSE(aDoneBeforeB); // A was still cooling down when B got through

    // A recovers on its own once the cooldown elapses.
    REQUIRE(waitFor([&] { return !aVal.isEmpty(); }, 4000));
    CHECK(aVal == "A");
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

TEST_CASE("idempotent call retries a transient Slack error", "[send_retry]") {
    FakeHttpServer server;
    // HTTP 200 + ok:false transient server-side errors, then success.
    server.enqueue(R"({"ok":false,"error":"internal_error"})");
    server.enqueue(R"({"ok":false,"error":"service_unavailable"})");
    server.enqueue(R"({"ok":true,"value":"finally"})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");
    client.setRetryBaseDelayMs(10);

    QString value, err;
    client.call(
        "users.getPresence",
        QUrlQuery{},
        [&](QJsonObject resp) { value = resp.value("value").toString(); },
        [&](QString e) { err = e; }
    );

    REQUIRE(waitFor([&] { return !value.isEmpty(); }));
    CHECK(value == "finally");
    CHECK(err.isEmpty());            // transient errors never surfaced to caller
    CHECK(server.requestCount == 3); // internal_error + service_unavailable + ok
}

TEST_CASE("a persistent transient Slack error is bounded, then surfaces", "[send_retry]") {
    FakeHttpServer server;
    // Slack keeps returning internal_error (e.g. users.getPresence for an
    // ineligible user) — must NOT loop forever; after the retry cap the error
    // is reported to the caller. 1 initial attempt + kMaxTransientSlackRetries.
    for (int i = 0; i < 8; ++i)
        server.enqueue(R"({"ok":false,"error":"internal_error"})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");
    client.setRetryBaseDelayMs(10);

    QString err;
    client.call("users.getPresence", QUrlQuery{}, [](QJsonObject) {}, [&](QString e) { err = e; });

    REQUIRE(waitFor([&] { return !err.isEmpty(); }));
    CHECK(err == "internal_error");  // surfaced instead of retried forever
    pumpFor(100);                    // any further retry would land here
    CHECK(server.requestCount == 7); // 1 initial + 6 bounded retries
}

TEST_CASE("non-idempotent call does NOT retry a transient Slack error", "[send_retry]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"internal_error"})");
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
    CHECK(err == "internal_error"); // surfaced so the caller can reconcile
    pumpFor(100);                   // a blind retry would land here
    CHECK(server.requestCount == 1);
}

// =============================================================================
// WebApiClient — paced background lane (bulk sweeps yield to interactive calls)
// =============================================================================

TEST_CASE("an interactive call preempts already-queued background calls", "[send_retry]") {
    FakeHttpServer server;
    // Responses are consumed in arrival order; the bodies just echo which call.
    server.enqueue(R"({"ok":true,"who":"bg1"})");
    server.enqueue(R"({"ok":true,"who":"fg"})");
    server.enqueue(R"({"ok":true,"who":"bg2"})");
    server.enqueue(R"({"ok":true,"who":"bg3"})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");
    client.setBackgroundPaceMs(20); // small, so the test isn't slow

    // Three background calls enqueue together: bg1 dispatches (single slot), bg2/bg3
    // wait. An interactive call enqueued while bg1 is in flight must jump the queue
    // ahead of bg2/bg3.
    client.callBackground("conversations.info", QUrlQuery{}, [](QJsonObject) {});
    client.callBackground("conversations.info", QUrlQuery{}, [](QJsonObject) {});
    client.callBackground("conversations.info", QUrlQuery{}, [](QJsonObject) {});
    client.call("conversations.history", QUrlQuery{}, [](QJsonObject) {});

    REQUIRE(waitFor([&] { return server.requestCount == 4; }, 4000));
    REQUIRE(server.requestPaths.size() == 4);
    CHECK(server.requestPaths[0] == "/conversations.info");    // bg1 already in flight
    CHECK(server.requestPaths[1] == "/conversations.history"); // interactive preempts
    CHECK(server.requestPaths[2] == "/conversations.info");    // bg lane resumes
    CHECK(server.requestPaths[3] == "/conversations.info");
}

TEST_CASE("background calls are paced by the configured interval", "[send_retry]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":true})");
    server.enqueue(R"({"ok":true})");
    server.enqueue(R"({"ok":true})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");
    client.setBackgroundPaceMs(300);

    const qint64 start = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < 3; ++i)
        client.callBackground("conversations.info", QUrlQuery{}, [](QJsonObject) {});

    REQUIRE(waitFor([&] { return server.requestCount == 3; }, 4000));
    // First goes immediately; the next two are each gated by ~300 ms, so the third
    // can't land before ~2×300 ms. Generous lower bound to avoid flakiness.
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - start;
    CHECK(elapsed >= 500);
}

// =============================================================================
// WebApiClient — form/query encoding of '+'
// =============================================================================
//
// QUrlQuery::toString(QUrl::FullyEncoded) leaves a literal '+' as-is (legal in
// an RFC 3986 query), but form-urlencoded decoding — which Slack applies to
// POST bodies AND query strings — turns '+' into a space. This is the bug that
// mangled a sent "C++" into "C  ".

TEST_CASE("'+' in a form-body value is percent-encoded, not sent literally", "[send_retry]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":true})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");

    QUrlQuery params;
    params.addQueryItem("channel", "C1");
    params.addQueryItem("text", "C++ is 100% fun");
    bool done = false;
    client.callNonIdempotent("chat.postMessage", params, [&](QJsonObject) { done = true; });

    REQUIRE(waitFor([&] { return done; }));
    REQUIRE(server.requestBodies.size() == 1);
    CHECK(server.requestBodies[0] == "channel=C1&text=C%2B%2B%20is%20100%25%20fun");
}

TEST_CASE("'+' in a GET query value is percent-encoded, not sent literally", "[send_retry]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":true})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");

    QUrlQuery params;
    params.addQueryItem("query", "C++");
    bool done = false;
    client.call("search.messages", params, [&](QJsonObject) { done = true; });

    REQUIRE(waitFor([&] { return done; }));
    REQUIRE(server.requestTargets.size() == 1);
    CHECK(server.requestTargets[0] == "/search.messages?query=C%2B%2B");
}

// =============================================================================
// WebApiClient::paginate — the self-referential ctx cycle is always broken
// =============================================================================
//
// paginate keeps its state alive across async pages with `ctx->loadPage =
// [ctx]{...}` — a deliberate self-reference. If that cycle isn't cleared on an
// error exit, the whole ctx (and the partial page accumulator it captures)
// leaks. A sentinel captured in the callbacks lets us assert the ctx really
// died: while it's alive, the sentinel's weak_ptr stays valid.

TEST_CASE("paginate breaks its ctx cycle when a page returns ok:false", "[send_retry]") {
    FakeHttpServer server;
    // First page succeeds and yields a cursor (so a second request is made),
    // then the second page fails at the Slack level — the path that used to
    // skip clearing loadPage and leak the whole pagination. The error must be
    // terminal (not a transient one the client would retry) so it propagates.
    server.enqueue(
        R"({"ok":true,"channels":[{"id":"C1"}],"response_metadata":{"next_cursor":"x"}})"
    );
    server.enqueue(R"({"ok":false,"error":"invalid_cursor"})");

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");

    auto               sentinel = std::make_shared<int>(0);
    std::weak_ptr<int> alive    = sentinel;
    int                pages    = 0;
    QString            err;

    client.paginate(
        "conversations.list",
        "channels",
        QUrlQuery{},
        [sentinel, &pages](QJsonArray a) { pages += a.size(); },
        [sentinel] {},
        [sentinel, &err](QString e) { err = e; }
    );
    sentinel.reset(); // only the captures inside the ctx hold it now

    REQUIRE(waitFor([&] { return !err.isEmpty(); }));
    CHECK(err == "invalid_cursor");
    CHECK(pages == 1);               // the good first page was delivered
    CHECK(server.requestCount == 2); // both pages were fetched
    CHECK(alive.expired());          // ctx destroyed → cycle broken, no leak
}

TEST_CASE("paginate breaks its ctx cycle on a clean finish", "[send_retry]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":true,"channels":[{"id":"C1"},{"id":"C2"}]})"); // no cursor → done

    WebApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setToken("t");

    auto               sentinel = std::make_shared<int>(0);
    std::weak_ptr<int> alive    = sentinel;
    bool               done     = false;

    client.paginate(
        "conversations.list",
        "channels",
        QUrlQuery{},
        [sentinel](QJsonArray) {},
        [sentinel, &done] { done = true; },
        [sentinel](QString) {}
    );
    sentinel.reset();

    REQUIRE(waitFor([&] { return done; }));
    CHECK(alive.expired());
}

// =============================================================================
// PublicBackend::sendMessage — reconcile instead of blind resend
// =============================================================================

namespace {

struct SendFixture {
    FakeHttpServer server;
    PublicBackend  backend{
        slack::Credentials{"xoxp-test", "T1", "Test", "", "", 0},
        slack::AppConfig{"id", "secret", ""}
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
    const EvMessageChanged *messageChangedEvent() {
        for (auto &e : events)
            if (auto *ev = std::get_if<EvMessageChanged>(&e))
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

TEST_CASE_METHOD(SendFixture, "edit confirms from the chat.update response", "[send_retry]") {
    // chat.update returns a sparse message (text/user only, no ts) — the echo
    // must patch ts from the top level and be marked textOnly so the UI merges
    // instead of wiping the row's files/reactions/thread state.
    server.enqueue(
        R"({"ok":true,"channel":"C1","ts":"123.456","text":"fixed",)"
        R"("message":{"text":"fixed","user":"U1"}})"
    );

    backend.editMessage(ConversationId{"C1"}, "123.456", TextWithEntities{"fixed", {}});

    REQUIRE(waitFor([&] { return messageChangedEvent() != nullptr; }));
    const auto *ev = messageChangedEvent();
    CHECK(ev->conv.value == "C1");
    CHECK(ev->msg.ts == "123.456");
    CHECK(ev->msg.rawText == "fixed");
    CHECK(ev->msg.edited);
    CHECK(ev->textOnly);
    CHECK(server.requestPaths[0] == "/chat.update");
}

TEST_CASE_METHOD(
    SendFixture,
    "file upload confirms via history reconcile, not just the realtime echo",
    "[send_retry]"
) {
    // files.completeUploadExternal returns no message ts, so the optimistic
    // ghost can only be replaced by reconciling the shared message from history.
    QTemporaryFile file;
    REQUIRE(file.open());
    file.write("payload");
    file.flush();

    // 1) getUploadURLExternal  2) raw byte POST to upload_url (on this server)
    // 3) completeUploadExternal  4) conversations.history reconcile
    server.enqueue(
        QString(R"({"ok":true,"upload_url":"%1up","file_id":"F1"})").arg(server.baseUrl()).toUtf8()
    );
    server.enqueue(R"(OK)");
    server.enqueue(R"({"ok":true,"files":[{"id":"F1","title":"x"}]})");
    server.enqueue(
        R"({"ok":true,"messages":[{"ts":"200.000","user":"U1","subtype":"file_share",)"
        R"("files":[{"id":"F1","name":"x"}],"text":""}]})"
    );

    backend.uploadFiles(ConversationId{"C1"}, {file.fileName()}, "");

    REQUIRE(waitFor([&] { return newMessageEvent() != nullptr; }));
    CHECK(newMessageEvent()->msg.ts == "200.000");
    CHECK_FALSE(newMessageEvent()->msg.files.empty());
    REQUIRE(server.requestPaths.size() == 4);
    CHECK(server.requestPaths[0] == "/files.getUploadURLExternal");
    CHECK(server.requestPaths[2] == "/files.completeUploadExternal");
    CHECK(server.requestPaths[3] == "/conversations.history");
}

TEST_CASE_METHOD(
    SendFixture, "threaded file upload posts thread_ts and reconciles via replies", "[send_retry]"
) {
    QTemporaryFile file;
    REQUIRE(file.open());
    file.write("payload");
    file.flush();

    server.enqueue(
        QString(R"({"ok":true,"upload_url":"%1up","file_id":"F1"})").arg(server.baseUrl()).toUtf8()
    );
    server.enqueue(R"(OK)");
    server.enqueue(R"({"ok":true,"files":[{"id":"F1","title":"x"}]})");
    server.enqueue(
        R"({"ok":true,"messages":[{"ts":"100.000","user":"U1"},)"
        R"({"ts":"200.000","user":"U1","subtype":"file_share",)"
        R"("files":[{"id":"F1","name":"x"}],"text":"","thread_ts":"100.000"}]})"
    );

    backend.uploadFiles(ConversationId{"C1"}, {file.fileName()}, "", Ts{"100.000"});

    REQUIRE(waitFor([&] { return newMessageEvent() != nullptr; }));
    CHECK(newMessageEvent()->msg.ts == "200.000");
    REQUIRE(server.requestPaths.size() == 4);
    CHECK(server.requestPaths[2] == "/files.completeUploadExternal");
    CHECK(server.requestPaths[3] == "/conversations.replies");
    CHECK(server.requestBodies[2].contains("thread_ts"));
    CHECK(server.requestBodies[2].contains("100.000"));
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

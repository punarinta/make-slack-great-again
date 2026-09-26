// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// PublicBackend::pressBotButton: the internal blocks.actions the official
// client calls on a Block Kit button press. Session (xoxc) tokens only, so an
// OAuth workspace must not claim the capability.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrlQuery>

#include "backend/slack/public_backend.h"
#include "backend/slack/slack_auth.h"
#include "fake_http_server.h"

using namespace slack;

MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return msga_test::runCatch(argc, argv);
}

namespace {

bool waitFor(std::function<bool()> pred, int timeoutMs = 3000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!pred() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

const AppConfig   kTestApp{"id", "secret", ""};
const Credentials kOAuth{"xoxp-test", "T1", "Test", "", "", 0};

Credentials sessionCreds() {
    Credentials c{"xoxc-test", "T1", "Test", "", "", 0};
    c.cookie = "xoxd-test";
    return c;
}

const BotButton kRetry{
    .text     = "Try again",
    .style    = "primary",
    .actionId = "retry_failed_task",
    .blockId  = "aXl4u",
    .value    = R"({"runId":40})",
};

QJsonObject jsonField(const QUrlQuery &q, const QString &key) {
    const auto doc = QJsonDocument::fromJson(q.queryItemValue(key, QUrl::FullyDecoded).toUtf8());
    return doc.isArray() ? doc.array().first().toObject() : doc.object();
}

} // namespace

TEST_CASE("bot-button presses are a session-token capability", "[botbuttons]") {
    CHECK(PublicBackend{sessionCreds(), kTestApp}.capabilities().botButtons);
    CHECK_FALSE(PublicBackend{kOAuth, kTestApp}.capabilities().botButtons);
}

TEST_CASE("pressBotButton posts blocks.actions with the message context", "[botbuttons]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":true})");

    PublicBackend backend{sessionCreds(), kTestApp};
    backend.setApiBaseUrlForTests(server.baseUrl());

    bool    done = false, ok = false;
    QString err;
    backend.pressBotButton(
        ConversationId{"D1"},
        "1785242750.286859",
        Ts{"1785242747.097849"},
        "B0BL4TE9M44",
        kRetry,
        [&](bool o, QString e) {
            done = true;
            ok   = o;
            err  = e;
        }
    );
    REQUIRE(waitFor([&] { return done; }));
    CHECK(ok);
    CHECK(err.isEmpty());

    REQUIRE(server.requestPaths.size() == 1);
    CHECK(server.requestPaths[0] == "/blocks.actions");
    // A write: POST body, never a retransmittable GET query.
    const QUrlQuery body(QString::fromUtf8(server.requestBodies[0]));
    CHECK(body.queryItemValue("service_id") == "B0BL4TE9M44");
    CHECK(!body.queryItemValue("client_token").isEmpty());

    const auto action = jsonField(body, "actions");
    CHECK(action.value("action_id").toString() == "retry_failed_task");
    CHECK(action.value("block_id").toString() == "aXl4u");
    CHECK(action.value("type").toString() == "button");
    CHECK(action.value("value").toString() == R"({"runId":40})");
    CHECK(action.value("text").toObject().value("text").toString() == "Try again");

    const auto container = jsonField(body, "container");
    CHECK(container.value("type").toString() == "message");
    CHECK(container.value("channel_id").toString() == "D1");
    CHECK(container.value("message_ts").toString() == "1785242750.286859");
    CHECK(container.value("thread_ts").toString() == "1785242747.097849");
}

TEST_CASE("a refused press reports Slack's error", "[botbuttons]") {
    FakeHttpServer server;
    server.enqueue(R"({"ok":false,"error":"invalid_service_id"})");

    PublicBackend backend{sessionCreds(), kTestApp};
    backend.setApiBaseUrlForTests(server.baseUrl());

    bool    done = false, ok = true;
    QString err;
    backend.pressBotButton(
        ConversationId{"C1"}, "1.000001", std::nullopt, "B1", kRetry, [&](bool o, QString e) {
            done = true;
            ok   = o;
            err  = e;
        }
    );
    REQUIRE(waitFor([&] { return done; }));
    CHECK_FALSE(ok);
    CHECK(err.contains("invalid_service_id"));
    REQUIRE(server.requestCount == 1); // never retried: a re-press would run the action twice
    const QUrlQuery body(QString::fromUtf8(server.requestBodies[0]));
    CHECK_FALSE(jsonField(body, "container").contains("thread_ts"));
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include "backend/backend.h"
#include "backend/backend_factory.h"
#include "backend/slack/slack_auth.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// ── Slack credential <-> neutral record round-trip ────────────────────────────

TEST_CASE("slack::toRecord packs the token fields into the auth blob", "[factory][slack]") {
    slack::Credentials c;
    c.xoxp         = "xoxp-abc";
    c.teamId       = "T0123";
    c.teamName     = "Acme";
    c.iconUrl      = "https://icon/x.png";
    c.refreshToken = "refresh-xyz";
    c.expiresAt    = 1700000000LL;

    const auto rec = slack::toRecord(c);
    CHECK(rec.key == WorkspaceKey{Service::Slack, "T0123"});
    CHECK(rec.displayName == "Acme");
    CHECK(rec.iconUrl == "https://icon/x.png");

    const auto blob = QJsonDocument::fromJson(rec.auth).object();
    CHECK(blob.value("xoxp").toString() == "xoxp-abc");
    CHECK(blob.value("refreshToken").toString() == "refresh-xyz");
    CHECK(blob.value("expiresAt").toString() == "1700000000");
}

TEST_CASE("slack::fromRecord reverses toRecord exactly", "[factory][slack]") {
    slack::Credentials c;
    c.xoxp         = "xoxp-abc";
    c.teamId       = "T0123";
    c.teamName     = "Acme";
    c.iconUrl      = "https://icon/x.png";
    c.refreshToken = "refresh-xyz";
    c.expiresAt    = 1700000000LL;

    const auto back = slack::fromRecord(slack::toRecord(c));
    CHECK(back.xoxp == c.xoxp);
    CHECK(back.teamId == c.teamId);
    CHECK(back.teamName == c.teamName);
    CHECK(back.iconUrl == c.iconUrl);
    CHECK(back.refreshToken == c.refreshToken);
    CHECK(back.expiresAt == c.expiresAt);
}

// ── makeBackend dispatch ──────────────────────────────────────────────────────

TEST_CASE("makeBackend builds a backend for a Slack record", "[factory]") {
    auto rec     = slack::toRecord(slack::Credentials{"xoxp-abc", "T0123", "Acme", {}, {}, 0});
    auto backend = makeBackend(rec);
    REQUIRE(backend != nullptr);
    // A freshly-built Slack backend reports its team url empty until auth.test
    // runs — enough to confirm we got a live Backend object and not a crash.
    CHECK(backend->teamUrl().isEmpty());
}

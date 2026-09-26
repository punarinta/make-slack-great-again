// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include "backend/backend.h"
#include "backend/backend_factory.h"
#include "backend/backend_registry.h"
#include "backend/slack/slack_auth.h"
#include "backend/slack/slack_module.h"
#include "backend/teams/teams_auth.h"
#include "backend/teams/teams_module.h"

MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    slack::registerBackend();
    teams::registerBackend();
    return msga_test::runCatch(argc, argv);
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
    CHECK(rec.key == WorkspaceKey{slack::kService, "T0123"});
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

TEST_CASE("makeBackend returns nullptr for a service not in this build", "[factory]") {
    TokenStore::WorkspaceRecord rec;
    rec.key  = WorkspaceKey{Service{QStringLiteral("carrier-pigeon")}, "coop-1"};
    rec.auth = "{}";
    CHECK(makeBackend(rec) == nullptr);
}

// ── Teams credential <-> neutral record round-trip ───────────────────────────

TEST_CASE("teams::toRecord packs the token set into the auth blob", "[factory][teams]") {
    teams::Credentials c;
    c.accessToken  = "eyJ.access";
    c.tenantId     = "a071f9a6-tenant";
    c.orgName      = "Contoso";
    c.iconUrl      = "https://icon/org.png";
    c.refreshToken = "refresh-xyz";
    c.expiresAt    = 1700000000LL;
    c.userId       = "aa7b-oid";

    const auto rec = teams::toRecord(c);
    // The Teams workspace handle is keyed by tenant id, NOT a Slack-style team id.
    CHECK(rec.key == WorkspaceKey{teams::kService, "a071f9a6-tenant"});
    CHECK(rec.displayName == "Contoso");
    CHECK(rec.iconUrl == "https://icon/org.png");

    const auto blob = QJsonDocument::fromJson(rec.auth).object();
    CHECK(blob.value("accessToken").toString() == "eyJ.access");
    CHECK(blob.value("refreshToken").toString() == "refresh-xyz");
    CHECK(blob.value("expiresAt").toString() == "1700000000");
    CHECK(blob.value("userId").toString() == "aa7b-oid");
}

TEST_CASE("teams::fromRecord reverses toRecord exactly", "[factory][teams]") {
    teams::Credentials c;
    c.accessToken  = "eyJ.access";
    c.tenantId     = "a071f9a6-tenant";
    c.orgName      = "Contoso";
    c.iconUrl      = "https://icon/org.png";
    c.refreshToken = "refresh-xyz";
    c.expiresAt    = 1700000000LL;
    c.userId       = "aa7b-oid";

    const auto back = teams::fromRecord(teams::toRecord(c));
    CHECK(back.accessToken == c.accessToken);
    CHECK(back.tenantId == c.tenantId);
    CHECK(back.orgName == c.orgName);
    CHECK(back.iconUrl == c.iconUrl);
    CHECK(back.refreshToken == c.refreshToken);
    CHECK(back.expiresAt == c.expiresAt);
    CHECK(back.userId == c.userId);
}

TEST_CASE(
    "makeBackend builds a backend for a Teams record with the Teams capabilities",
    "[factory][teams]"
) {
    auto rec = teams::toRecord(teams::Credentials{"acc", "tenant-1", "Contoso", {}, {}, 0, "oid"});
    auto backend = makeBackend(rec);
    REQUIRE(backend != nullptr);

    // The Teams capability surface: reactions/edit/threads/files yes; the
    // Slack-only affordances (canvases, huddles, slash commands) and live
    // typing/presence are off, so the shared UI hides them with no dead controls.
    const auto caps = backend->capabilities();
    CHECK(caps.reactions);
    CHECK(caps.editMessage);
    CHECK(caps.threads);
    CHECK(caps.fileUpload);
    CHECK_FALSE(caps.canvases);
    CHECK_FALSE(caps.huddles);
    CHECK_FALSE(caps.slashCommands);
    CHECK_FALSE(caps.typing);
    CHECK_FALSE(caps.livePresence);
}

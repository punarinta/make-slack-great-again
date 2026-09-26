// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QCoreApplication>

#include "auth/auth_strategy.h"
#include "auth/auth_strategy_factory.h"
#include "backend/backend_registry.h"
#include "backend/domain.h"
#include "backend/slack/oauth_flow.h"
#include "backend/slack/slack_auth.h"

MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    // Just the auth half of Slack's descriptor — the real slack::registerBackend()
    // would drag the whole Slack backend into this test.
    backends::registerBackend({
        .service          = slack::kService,
        .displayName      = QStringLiteral("Slack"),
        .pickerOrder      = 0,
        .makeBackend      = {},
        .makeAuthStrategy = [](QObject *parent) -> std::unique_ptr<auth::AuthStrategy> {
            return std::make_unique<slack::OAuthFlow>(slack::appConfig(), parent);
        },
    });
    return msga_test::runCatch(argc, argv);
}

namespace {
BackendDescriptor stubDescriptor(const char *token, int order, bool canSignIn = true) {
    BackendDescriptor d;
    d.service     = Service{QString::fromLatin1(token)};
    d.displayName = QString::fromLatin1(token).toUpper();
    d.pickerOrder = order;
    if (canSignIn)
        d.makeAuthStrategy = [](QObject *) { return std::unique_ptr<auth::AuthStrategy>{}; };
    return d;
}
} // namespace

// ── Factory dispatch ──────────────────────────────────────────────────────────

TEST_CASE("registeredAuthServices lists Slack", "[auth]") {
    const auto services = auth::registeredAuthServices();
    REQUIRE_FALSE(services.empty());
    CHECK(std::find(services.begin(), services.end(), slack::kService) != services.end());
}

TEST_CASE("makeAuthStrategy builds a strategy for Slack", "[auth]") {
    auto strategy = auth::makeAuthStrategy(slack::kService);
    REQUIRE(strategy != nullptr);
    // It IS-A neutral AuthStrategy — the UI only ever sees this contract.
    auto *neutral = static_cast<auth::AuthStrategy *>(strategy.get());
    CHECK(neutral != nullptr);
}

TEST_CASE("the picker offers services in pickerOrder, skipping hidden ones", "[auth][registry]") {
    backends::registerBackend(stubDescriptor("zeta", 30));
    backends::registerBackend(stubDescriptor("alpha", 5));
    backends::registerBackend(stubDescriptor("hidden", -1));         // e.g. Demo
    backends::registerBackend(stubDescriptor("nosignin", 7, false)); // no strategy

    std::vector<QString> tokens;
    for (const auto &s : auth::registeredAuthServices())
        tokens.push_back(s.token);
    CHECK(tokens == std::vector<QString>{"slack", "alpha", "zeta"});

    CHECK(backends::displayName(Service{"alpha"}) == "ALPHA");
    // An unknown service reads as its raw token rather than a blank label.
    CHECK(backends::displayName(Service{"unknown"}) == "unknown");
    CHECK(auth::makeAuthStrategy(Service{"unknown"}) == nullptr);
    CHECK(auth::makeAuthStrategy(Service{"nosignin"}) == nullptr);

    // Re-registering replaces by service instead of duplicating the entry.
    backends::registerBackend(stubDescriptor("alpha", 50));
    tokens.clear();
    for (const auto &s : auth::registeredAuthServices())
        tokens.push_back(s.token);
    CHECK(tokens == std::vector<QString>{"slack", "zeta", "alpha"});
}

// ── The interface can express a non-OAuth flow ────────────────────────────────

// A strategy with no browser, no redirect callback — proving the contract isn't
// OAuth-shaped. It hands back a ready WorkspaceRecord straight from start(),
// exactly as a phone+code or device flow eventually would.
namespace {
class FakeStrategy : public auth::AuthStrategy {
    Q_OBJECT
public:
    using auth::AuthStrategy::AuthStrategy;
    void start() override {
        TokenStore::WorkspaceRecord rec;
        rec.key         = WorkspaceKey{slack::kService, "T-FAKE"};
        rec.displayName = "Fake Workspace";
        rec.auth        = QByteArray("{}");
        emit succeeded(rec);
    }
    // Deliberately does NOT override handleCallbackUri — a non-redirect flow
    // relies on the default no-op.
};
} // namespace

TEST_CASE("a non-OAuth strategy yields a neutral record from start()", "[auth]") {
    FakeStrategy s;

    bool                        got = false;
    TokenStore::WorkspaceRecord received;
    QObject::connect(&s, &auth::AuthStrategy::succeeded, [&](TokenStore::WorkspaceRecord r) {
        got      = true;
        received = std::move(r);
    });

    // The default callback hook is a harmless no-op for non-redirect flows.
    s.handleCallbackUri(QUrl("msga://oauth/callback?code=ignored"));
    CHECK_FALSE(got);

    s.start();
    REQUIRE(got);
    CHECK(received.key == WorkspaceKey{slack::kService, "T-FAKE"});
    CHECK(received.displayName == "Fake Workspace");
}

#include "test_auth_strategy.moc"

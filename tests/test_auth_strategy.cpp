// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include "auth/auth_strategy.h"
#include "auth/auth_strategy_factory.h"
#include "backend/domain.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    return Catch::Session().run(argc, argv);
}

// ── Factory dispatch ──────────────────────────────────────────────────────────

TEST_CASE("registeredAuthServices lists Slack", "[auth]") {
    const auto services = auth::registeredAuthServices();
    REQUIRE_FALSE(services.empty());
    CHECK(std::find(services.begin(), services.end(), Service::Slack) != services.end());
}

TEST_CASE("makeAuthStrategy builds a strategy for Slack", "[auth]") {
    auto strategy = auth::makeAuthStrategy(Service::Slack);
    REQUIRE(strategy != nullptr);
    // It IS-A neutral AuthStrategy — the UI only ever sees this contract.
    auto *neutral = static_cast<auth::AuthStrategy *>(strategy.get());
    CHECK(neutral != nullptr);
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
        rec.key         = WorkspaceKey{Service::Slack, "T-FAKE"};
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
    CHECK(received.key == WorkspaceKey{Service::Slack, "T-FAKE"});
    CHECK(received.displayName == "Fake Workspace");
}

#include "test_auth_strategy.moc"

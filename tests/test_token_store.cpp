// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include "auth/token_store.h"

// Redirect QSettings("msga","msga") to a temp dir so tests never touch the
// user's real credentials stored in ~/.config/msga/msga.conf.
static QTemporaryDir *gTempDir = nullptr;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    QTemporaryDir tempDir;
    gTempDir = &tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

// Clear settings before and after each test so tests are fully isolated.
struct TokenStoreFixture {
    static void clearSettings() {
        QSettings s("msga", "msga");
        s.clear();
        s.sync();
    }
    TokenStoreFixture() { clearSettings(); }
    ~TokenStoreFixture() { clearSettings(); }
};

// ── saveWorkspace / loadWorkspace ─────────────────────────────────────────────

TEST_CASE_METHOD(TokenStoreFixture, "saveWorkspace/loadWorkspace round-trip", "[tokenstore]") {
    TokenStore::Credentials c{"xoxp-token", "T001", "My Team", "https://icon.example.com/t.png"};
    TokenStore::saveWorkspace(c);
    auto loaded = TokenStore::loadWorkspace("T001");
    CHECK(loaded.xoxp == "xoxp-token");
    CHECK(loaded.teamId == "T001");
    CHECK(loaded.teamName == "My Team");
    CHECK(loaded.iconUrl == "https://icon.example.com/t.png");
}

TEST_CASE_METHOD(TokenStoreFixture, "saveWorkspace registers id in workspaceIds", "[tokenstore]") {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    CHECK(TokenStore::workspaceIds().contains("T001"));
}

TEST_CASE_METHOD(
    TokenStoreFixture, "saveWorkspace twice for same id does not duplicate", "[tokenstore]"
) {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::saveWorkspace({"xoxp-2", "T001", "Team One v2", ""});
    auto ids = TokenStore::workspaceIds();
    CHECK(ids.count("T001") == 1);
    CHECK(TokenStore::loadWorkspace("T001").xoxp == "xoxp-2");
    CHECK(TokenStore::loadWorkspace("T001").teamName == "Team One v2");
}

TEST_CASE_METHOD(
    TokenStoreFixture, "multiple workspaces all appear in workspaceIds", "[tokenstore]"
) {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::saveWorkspace({"xoxp-2", "T002", "Team Two", ""});
    auto ids = TokenStore::workspaceIds();
    REQUIRE(ids.size() == 2);
    CHECK(ids.contains("T001"));
    CHECK(ids.contains("T002"));
}

// ── removeWorkspace ───────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    TokenStoreFixture, "removeWorkspace removes id from workspaceIds", "[tokenstore]"
) {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::saveWorkspace({"xoxp-2", "T002", "Team Two", ""});
    TokenStore::removeWorkspace("T001");
    auto ids = TokenStore::workspaceIds();
    CHECK(!ids.contains("T001"));
    CHECK(ids.contains("T002"));
}

TEST_CASE_METHOD(TokenStoreFixture, "removeWorkspace clears stored credentials", "[tokenstore]") {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::removeWorkspace("T001");
    auto loaded = TokenStore::loadWorkspace("T001");
    CHECK(loaded.xoxp.isEmpty());
    CHECK(loaded.teamName.isEmpty());
}

TEST_CASE_METHOD(
    TokenStoreFixture, "removeWorkspace active shifts to first remaining", "[tokenstore]"
) {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::saveWorkspace({"xoxp-2", "T002", "Team Two", ""});
    TokenStore::setActiveWorkspace("T001");
    TokenStore::removeWorkspace("T001");
    CHECK(TokenStore::activeWorkspaceId() == "T002");
}

TEST_CASE_METHOD(
    TokenStoreFixture, "removeWorkspace last workspace clears active", "[tokenstore]"
) {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::setActiveWorkspace("T001");
    TokenStore::removeWorkspace("T001");
    CHECK(TokenStore::activeWorkspaceId().isEmpty());
    CHECK(!TokenStore::hasAnyWorkspace());
}

TEST_CASE_METHOD(TokenStoreFixture, "removeWorkspace non-existent id is a no-op", "[tokenstore]") {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::removeWorkspace("T_GHOST");
    REQUIRE(TokenStore::workspaceIds().size() == 1);
    CHECK(TokenStore::workspaceIds()[0] == "T001");
}

// ── active workspace ──────────────────────────────────────────────────────────

TEST_CASE_METHOD(TokenStoreFixture, "activeWorkspaceId empty when nothing set", "[tokenstore]") {
    CHECK(TokenStore::activeWorkspaceId().isEmpty());
}

TEST_CASE_METHOD(
    TokenStoreFixture, "setActiveWorkspace/activeWorkspaceId round-trip", "[tokenstore]"
) {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team", ""});
    TokenStore::setActiveWorkspace("T001");
    CHECK(TokenStore::activeWorkspaceId() == "T001");
}

TEST_CASE_METHOD(
    TokenStoreFixture, "setActiveWorkspace can switch between workspaces", "[tokenstore]"
) {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::saveWorkspace({"xoxp-2", "T002", "Team Two", ""});
    TokenStore::setActiveWorkspace("T001");
    CHECK(TokenStore::activeWorkspaceId() == "T001");
    TokenStore::setActiveWorkspace("T002");
    CHECK(TokenStore::activeWorkspaceId() == "T002");
}

// ── setWorkspaceOrder ─────────────────────────────────────────────────────────

TEST_CASE_METHOD(TokenStoreFixture, "setWorkspaceOrder persists new order", "[tokenstore]") {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::saveWorkspace({"xoxp-2", "T002", "Team Two", ""});
    TokenStore::saveWorkspace({"xoxp-3", "T003", "Team Three", ""});

    TokenStore::setWorkspaceOrder({"T003", "T001", "T002"});
    CHECK(TokenStore::workspaceIds() == QStringList{"T003", "T001", "T002"});
}

TEST_CASE_METHOD(TokenStoreFixture, "setWorkspaceOrder ignores unknown ids", "[tokenstore]") {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::saveWorkspace({"xoxp-2", "T002", "Team Two", ""});

    TokenStore::setWorkspaceOrder({"T002", "T_BOGUS", "T001"});
    CHECK(TokenStore::workspaceIds() == QStringList{"T002", "T001"});
}

TEST_CASE_METHOD(
    TokenStoreFixture, "setWorkspaceOrder appends known ids missing from the list", "[tokenstore]"
) {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::saveWorkspace({"xoxp-2", "T002", "Team Two", ""});
    TokenStore::saveWorkspace({"xoxp-3", "T003", "Team Three", ""});

    // A stale/partial order must never drop a workspace.
    TokenStore::setWorkspaceOrder({"T002"});
    CHECK(TokenStore::workspaceIds() == QStringList{"T002", "T001", "T003"});
}

TEST_CASE_METHOD(TokenStoreFixture, "setWorkspaceOrder keeps credentials intact", "[tokenstore]") {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team One", ""});
    TokenStore::saveWorkspace({"xoxp-2", "T002", "Team Two", ""});
    TokenStore::setActiveWorkspace("T001");

    TokenStore::setWorkspaceOrder({"T002", "T001"});
    CHECK(TokenStore::loadWorkspace("T001").xoxp == "xoxp-1");
    CHECK(TokenStore::loadWorkspace("T002").xoxp == "xoxp-2");
    CHECK(TokenStore::activeWorkspaceId() == "T001");
}

// ── hasAnyWorkspace ───────────────────────────────────────────────────────────

TEST_CASE_METHOD(TokenStoreFixture, "hasAnyWorkspace false when empty", "[tokenstore]") {
    CHECK(!TokenStore::hasAnyWorkspace());
}

TEST_CASE_METHOD(TokenStoreFixture, "hasAnyWorkspace true after save", "[tokenstore]") {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team", ""});
    CHECK(TokenStore::hasAnyWorkspace());
}

TEST_CASE_METHOD(
    TokenStoreFixture, "hasAnyWorkspace false after removing only workspace", "[tokenstore]"
) {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team", ""});
    TokenStore::removeWorkspace("T001");
    CHECK(!TokenStore::hasAnyWorkspace());
}

// ── legacy wrappers ───────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    TokenStoreFixture, "save() stores credentials and sets active", "[tokenstore][legacy]"
) {
    TokenStore::save({"xoxp-1", "T001", "Team One", ""});
    CHECK(TokenStore::activeWorkspaceId() == "T001");
    CHECK(TokenStore::load().xoxp == "xoxp-1");
}

TEST_CASE_METHOD(
    TokenStoreFixture,
    "load() returns credentials for current active workspace",
    "[tokenstore][legacy]"
) {
    TokenStore::save({"xoxp-1", "T001", "Team One", ""});
    TokenStore::save({"xoxp-2", "T002", "Team Two", ""});
    TokenStore::setActiveWorkspace("T001");
    CHECK(TokenStore::load().teamId == "T001");
    TokenStore::setActiveWorkspace("T002");
    CHECK(TokenStore::load().teamId == "T002");
}

TEST_CASE_METHOD(
    TokenStoreFixture, "clear() removes the active workspace", "[tokenstore][legacy]"
) {
    TokenStore::save({"xoxp-1", "T001", "Team One", ""});
    TokenStore::clear();
    CHECK(!TokenStore::hasAnyWorkspace());
}

TEST_CASE_METHOD(TokenStoreFixture, "hasToken mirrors hasAnyWorkspace", "[tokenstore][legacy]") {
    CHECK(!TokenStore::hasToken());
    TokenStore::save({"xoxp-1", "T001", "Team", ""});
    CHECK(TokenStore::hasToken());
    TokenStore::clear();
    CHECK(!TokenStore::hasToken());
}

// ── migration from old auth/* format ─────────────────────────────────────────

TEST_CASE_METHOD(
    TokenStoreFixture, "workspaceIds migrates old auth/* format", "[tokenstore][migrate]"
) {
    {
        QSettings s("msga", "msga");
        s.setValue("auth/xoxp", "xoxp-old");
        s.setValue("auth/team_id", "T_OLD");
        s.setValue("auth/team_name", "Old Team");
        s.sync();
    }
    auto ids = TokenStore::workspaceIds();
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "T_OLD");
    CHECK(TokenStore::loadWorkspace("T_OLD").xoxp == "xoxp-old");
    CHECK(TokenStore::loadWorkspace("T_OLD").teamName == "Old Team");
    CHECK(TokenStore::activeWorkspaceId() == "T_OLD");
}

TEST_CASE_METHOD(
    TokenStoreFixture, "migration uses 'legacy' id when team_id was empty", "[tokenstore][migrate]"
) {
    {
        QSettings s("msga", "msga");
        s.setValue("auth/xoxp", "xoxp-old");
        s.setValue("auth/team_id", "");
        s.setValue("auth/team_name", "Old Team");
        s.sync();
    }
    auto ids = TokenStore::workspaceIds();
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "legacy");
    CHECK(TokenStore::loadWorkspace("legacy").xoxp == "xoxp-old");
}

TEST_CASE_METHOD(TokenStoreFixture, "migration removes old auth/* keys", "[tokenstore][migrate]") {
    {
        QSettings s("msga", "msga");
        s.setValue("auth/xoxp", "xoxp-old");
        s.setValue("auth/team_id", "T_OLD");
        s.setValue("auth/team_name", "Old Team");
        s.sync();
    }
    TokenStore::workspaceIds(); // triggers migration
    QSettings s("msga", "msga");
    CHECK(!s.contains("auth/xoxp"));
    CHECK(!s.contains("auth/team_id"));
    CHECK(!s.contains("auth/team_name"));
}

TEST_CASE_METHOD(
    TokenStoreFixture, "migration skipped when auth/xoxp is empty", "[tokenstore][migrate]"
) {
    {
        QSettings s("msga", "msga");
        s.setValue("auth/xoxp", "");
        s.setValue("auth/team_id", "T_EMPTY");
        s.sync();
    }
    CHECK(TokenStore::workspaceIds().isEmpty());
}

TEST_CASE_METHOD(
    TokenStoreFixture,
    "migration is idempotent when workspaces key already exists",
    "[tokenstore][migrate]"
) {
    TokenStore::saveWorkspace({"xoxp-new", "T_NEW", "New Team", ""});
    TokenStore::workspaceIds(); // should not alter the already-migrated state
    auto ids = TokenStore::workspaceIds();
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "T_NEW");
}

// ── expiresAt ─────────────────────────────────────────────────────────────────

TEST_CASE_METHOD(
    TokenStoreFixture, "saveWorkspace/loadWorkspace round-trips expiresAt", "[tokenstore][expiry]"
) {
    TokenStore::Credentials c{"xoxp-1", "T001", "Team", "", "refresh-tok", 1234567890LL};
    TokenStore::saveWorkspace(c);
    auto loaded = TokenStore::loadWorkspace("T001");
    CHECK(loaded.expiresAt == 1234567890LL);
}

TEST_CASE_METHOD(
    TokenStoreFixture,
    "loadWorkspace returns expiresAt=0 when key is absent",
    "[tokenstore][expiry]"
) {
    // Write credentials without an expiresAt key (simulates an old install).
    {
        QSettings s("msga", "msga");
        s.setValue("workspaces", QStringList{"T_OLD"});
        s.setValue("workspace/T_OLD/xoxp", "xoxp-old");
        s.setValue("workspace/T_OLD/name", "Old Team");
        s.setValue("workspace/T_OLD/iconUrl", "");
        s.setValue("workspace/T_OLD/refreshToken", "refresh-old");
        // intentionally no expiresAt key
        s.sync();
    }
    auto loaded = TokenStore::loadWorkspace("T_OLD");
    CHECK(loaded.expiresAt == 0);
}

TEST_CASE_METHOD(
    TokenStoreFixture, "saveWorkspace overwrites expiresAt on update", "[tokenstore][expiry]"
) {
    TokenStore::saveWorkspace({"xoxp-1", "T001", "Team", "", "refresh-1", 1000LL});
    TokenStore::saveWorkspace({"xoxp-2", "T001", "Team", "", "refresh-2", 9999LL});
    CHECK(TokenStore::loadWorkspace("T001").expiresAt == 9999LL);
}

TEST_CASE_METHOD(
    TokenStoreFixture, "save/load legacy wrappers preserve expiresAt", "[tokenstore][expiry]"
) {
    TokenStore::save({"xoxp-1", "T001", "Team", "", "refresh-tok", 7777LL});
    CHECK(TokenStore::load().expiresAt == 7777LL);
}

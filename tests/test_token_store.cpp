// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include "auth/token_store.h"

// Redirect QSettings("msga","msga") to a temp dir so tests never touch the
// user's real credentials stored in ~/.config/msga/msga.conf.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

using TokenStore::WorkspaceRecord;

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

static WorkspaceKey slackKey(const QString &id) {
    return WorkspaceKey{Service::Slack, id};
}
static WorkspaceRecord
rec(const QString &id, const QString &name, const QString &icon = {}, const QByteArray &auth = {}) {
    return WorkspaceRecord{slackKey(id), name, icon, auth};
}

// ── WorkspaceKey canonical form ───────────────────────────────────────────────

TEST_CASE("WorkspaceKey round-trips through its canonical string", "[tokenstore][key]") {
    const auto k = slackKey("T0123ABCD");
    CHECK(k.toString() == "slack:T0123ABCD");
    const auto parsed = WorkspaceKey::fromString("slack:T0123ABCD");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == k);
}

TEST_CASE("WorkspaceKey::fromString rejects malformed handles", "[tokenstore][key]") {
    CHECK_FALSE(WorkspaceKey::fromString("T0123").has_value());       // no service
    CHECK_FALSE(WorkspaceKey::fromString("bogus:T0123").has_value()); // unknown service
    CHECK_FALSE(WorkspaceKey::fromString(":T0123").has_value());      // empty service
    CHECK_FALSE(WorkspaceKey::fromString("slack:").has_value());      // empty id
}

// ── saveWorkspace / loadWorkspace ─────────────────────────────────────────────

TEST_CASE_METHOD(TokenStoreFixture, "saveWorkspace/loadWorkspace round-trip", "[tokenstore]") {
    TokenStore::saveWorkspace(rec("T001", "My Team", "https://icon.example.com/t.png", "blob"));
    const auto loaded = TokenStore::loadWorkspace(slackKey("T001"));
    REQUIRE(loaded.has_value());
    CHECK(loaded->key == slackKey("T001"));
    CHECK(loaded->displayName == "My Team");
    CHECK(loaded->iconUrl == "https://icon.example.com/t.png");
    CHECK(loaded->auth == QByteArray("blob"));
}

TEST_CASE_METHOD(
    TokenStoreFixture, "loadWorkspace returns nullopt for unknown key", "[tokenstore]"
) {
    CHECK_FALSE(TokenStore::loadWorkspace(slackKey("T_GHOST")).has_value());
}

TEST_CASE_METHOD(
    TokenStoreFixture, "saveWorkspace registers key in workspaceKeys", "[tokenstore]"
) {
    TokenStore::saveWorkspace(rec("T001", "Team One"));
    const auto keys = TokenStore::workspaceKeys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == slackKey("T001"));
}

TEST_CASE_METHOD(
    TokenStoreFixture, "saveWorkspace twice for same key does not duplicate", "[tokenstore]"
) {
    TokenStore::saveWorkspace(rec("T001", "Team One", {}, "a"));
    TokenStore::saveWorkspace(rec("T001", "Team One v2", {}, "b"));
    CHECK(TokenStore::workspaceKeys().size() == 1);
    const auto loaded = TokenStore::loadWorkspace(slackKey("T001"));
    REQUIRE(loaded.has_value());
    CHECK(loaded->displayName == "Team One v2");
    CHECK(loaded->auth == QByteArray("b"));
}

TEST_CASE_METHOD(
    TokenStoreFixture, "multiple workspaces all appear in workspaceKeys", "[tokenstore]"
) {
    TokenStore::saveWorkspace(rec("T001", "Team One"));
    TokenStore::saveWorkspace(rec("T002", "Team Two"));
    const auto keys = TokenStore::workspaceKeys();
    REQUIRE(keys.size() == 2);
    CHECK(keys[0] == slackKey("T001"));
    CHECK(keys[1] == slackKey("T002"));
}

// ── removeWorkspace ───────────────────────────────────────────────────────────

TEST_CASE_METHOD(TokenStoreFixture, "removeWorkspace removes key + clears record", "[tokenstore]") {
    TokenStore::saveWorkspace(rec("T001", "Team One", {}, "a"));
    TokenStore::saveWorkspace(rec("T002", "Team Two"));
    TokenStore::removeWorkspace(slackKey("T001"));
    const auto keys = TokenStore::workspaceKeys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == slackKey("T002"));
    CHECK_FALSE(TokenStore::loadWorkspace(slackKey("T001")).has_value());
}

TEST_CASE_METHOD(
    TokenStoreFixture, "removeWorkspace active shifts to first remaining", "[tokenstore]"
) {
    TokenStore::saveWorkspace(rec("T001", "Team One"));
    TokenStore::saveWorkspace(rec("T002", "Team Two"));
    TokenStore::setActiveWorkspace(slackKey("T001"));
    TokenStore::removeWorkspace(slackKey("T001"));
    const auto active = TokenStore::activeWorkspace();
    REQUIRE(active.has_value());
    CHECK(*active == slackKey("T002"));
}

TEST_CASE_METHOD(
    TokenStoreFixture, "removeWorkspace last workspace clears active", "[tokenstore]"
) {
    TokenStore::saveWorkspace(rec("T001", "Team One"));
    TokenStore::setActiveWorkspace(slackKey("T001"));
    TokenStore::removeWorkspace(slackKey("T001"));
    CHECK_FALSE(TokenStore::activeWorkspace().has_value());
    CHECK_FALSE(TokenStore::hasAnyWorkspace());
}

TEST_CASE_METHOD(TokenStoreFixture, "removeWorkspace non-existent key is a no-op", "[tokenstore]") {
    TokenStore::saveWorkspace(rec("T001", "Team One"));
    TokenStore::removeWorkspace(slackKey("T_GHOST"));
    const auto keys = TokenStore::workspaceKeys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == slackKey("T001"));
}

// ── active workspace ──────────────────────────────────────────────────────────

TEST_CASE_METHOD(TokenStoreFixture, "activeWorkspace empty when nothing set", "[tokenstore]") {
    CHECK_FALSE(TokenStore::activeWorkspace().has_value());
}

TEST_CASE_METHOD(
    TokenStoreFixture, "setActiveWorkspace/activeWorkspace round-trip", "[tokenstore]"
) {
    TokenStore::saveWorkspace(rec("T001", "Team"));
    TokenStore::setActiveWorkspace(slackKey("T001"));
    const auto active = TokenStore::activeWorkspace();
    REQUIRE(active.has_value());
    CHECK(*active == slackKey("T001"));
}

// ── setWorkspaceOrder ─────────────────────────────────────────────────────────

TEST_CASE_METHOD(TokenStoreFixture, "setWorkspaceOrder persists new order", "[tokenstore]") {
    TokenStore::saveWorkspace(rec("T001", "Team One"));
    TokenStore::saveWorkspace(rec("T002", "Team Two"));
    TokenStore::saveWorkspace(rec("T003", "Team Three"));

    TokenStore::setWorkspaceOrder({slackKey("T003"), slackKey("T001"), slackKey("T002")});
    CHECK(
        TokenStore::workspaceKeys() ==
        std::vector<WorkspaceKey>{slackKey("T003"), slackKey("T001"), slackKey("T002")}
    );
}

TEST_CASE_METHOD(TokenStoreFixture, "setWorkspaceOrder ignores unknown keys", "[tokenstore]") {
    TokenStore::saveWorkspace(rec("T001", "Team One"));
    TokenStore::saveWorkspace(rec("T002", "Team Two"));

    TokenStore::setWorkspaceOrder({slackKey("T002"), slackKey("T_BOGUS"), slackKey("T001")});
    CHECK(
        TokenStore::workspaceKeys() == std::vector<WorkspaceKey>{slackKey("T002"), slackKey("T001")}
    );
}

TEST_CASE_METHOD(
    TokenStoreFixture, "setWorkspaceOrder appends known keys missing from the list", "[tokenstore]"
) {
    TokenStore::saveWorkspace(rec("T001", "Team One"));
    TokenStore::saveWorkspace(rec("T002", "Team Two"));
    TokenStore::saveWorkspace(rec("T003", "Team Three"));

    // A stale/partial order must never drop a workspace.
    TokenStore::setWorkspaceOrder({slackKey("T002")});
    CHECK(
        TokenStore::workspaceKeys() ==
        std::vector<WorkspaceKey>{slackKey("T002"), slackKey("T001"), slackKey("T003")}
    );
}

// ── hasAnyWorkspace ───────────────────────────────────────────────────────────

TEST_CASE_METHOD(TokenStoreFixture, "hasAnyWorkspace false when empty", "[tokenstore]") {
    CHECK_FALSE(TokenStore::hasAnyWorkspace());
}

TEST_CASE_METHOD(TokenStoreFixture, "hasAnyWorkspace true after save", "[tokenstore]") {
    TokenStore::saveWorkspace(rec("T001", "Team"));
    CHECK(TokenStore::hasAnyWorkspace());
}

// ── migration: bare-id (v1) → composite handle (v2) ───────────────────────────

TEST_CASE_METHOD(
    TokenStoreFixture,
    "migrates bare-id slack entries to composite handles",
    "[tokenstore][migrate]"
) {
    {
        QSettings s("msga", "msga");
        s.setValue("workspaces", QStringList{"T_OLD"});
        s.setValue("workspace/T_OLD/xoxp", "xoxp-old");
        s.setValue("workspace/T_OLD/name", "Old Team");
        s.setValue("workspace/T_OLD/iconUrl", "https://icon/x.png");
        s.setValue("workspace/T_OLD/refreshToken", "refresh-old");
        s.setValue("workspace/T_OLD/expiresAt", 1234567890LL);
        s.setValue("active", "T_OLD");
        // intentionally no storeVersion → migration runs
        s.sync();
    }

    const auto keys = TokenStore::workspaceKeys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == slackKey("T_OLD"));

    const auto loaded = TokenStore::loadWorkspace(slackKey("T_OLD"));
    REQUIRE(loaded.has_value());
    CHECK(loaded->displayName == "Old Team");
    CHECK(loaded->iconUrl == "https://icon/x.png");

    // The token-shaped fields are packed into the opaque auth blob.
    const auto blob = QJsonDocument::fromJson(loaded->auth).object();
    CHECK(blob.value("xoxp").toString() == "xoxp-old");
    CHECK(blob.value("refreshToken").toString() == "refresh-old");
    CHECK(blob.value("expiresAt").toString() == "1234567890");

    const auto active = TokenStore::activeWorkspace();
    REQUIRE(active.has_value());
    CHECK(*active == slackKey("T_OLD"));

    // Old bare-id subtree is gone.
    QSettings s("msga", "msga");
    CHECK_FALSE(s.contains("workspace/T_OLD/xoxp"));
}

// ── migration: legacy single-account auth/* (v0) → v2 ─────────────────────────

TEST_CASE_METHOD(
    TokenStoreFixture, "migrates old auth/* single-account format", "[tokenstore][migrate]"
) {
    {
        QSettings s("msga", "msga");
        s.setValue("auth/xoxp", "xoxp-old");
        s.setValue("auth/team_id", "T_OLD");
        s.setValue("auth/team_name", "Old Team");
        s.sync();
    }

    const auto keys = TokenStore::workspaceKeys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == slackKey("T_OLD"));
    const auto loaded = TokenStore::loadWorkspace(slackKey("T_OLD"));
    REQUIRE(loaded.has_value());
    CHECK(loaded->displayName == "Old Team");
    CHECK(QJsonDocument::fromJson(loaded->auth).object().value("xoxp").toString() == "xoxp-old");

    QSettings s("msga", "msga");
    CHECK_FALSE(s.contains("auth/xoxp"));
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
    const auto keys = TokenStore::workspaceKeys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == slackKey("legacy"));
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
    CHECK(TokenStore::workspaceKeys().empty());
}

TEST_CASE_METHOD(
    TokenStoreFixture,
    "migration is idempotent once already on the current version",
    "[tokenstore][migrate]"
) {
    TokenStore::saveWorkspace(rec("T_NEW", "New Team", {}, "blob"));
    TokenStore::workspaceKeys(); // should not alter the already-migrated state
    const auto keys = TokenStore::workspaceKeys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == slackKey("T_NEW"));
    CHECK(TokenStore::loadWorkspace(slackKey("T_NEW"))->auth == QByteArray("blob"));
}

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
    return WorkspaceKey{Service{QStringLiteral("slack")}, id};
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
    CHECK_FALSE(WorkspaceKey::fromString("Bogus:T0123").has_value()); // not a token (uppercase)
    CHECK_FALSE(WorkspaceKey::fromString("a b:T0123").has_value());   // not a token (space)
    CHECK_FALSE(WorkspaceKey::fromString(":T0123").has_value());      // empty service
    CHECK_FALSE(WorkspaceKey::fromString("slack:").has_value());      // empty id
}

TEST_CASE(
    "WorkspaceKey::fromString parses a service this build may not have", "[tokenstore][key]"
) {
    // Parsing is registry-agnostic: a workspace of a compiled-out backend must
    // survive as a key, or it would be lost from storage.
    const auto k = WorkspaceKey::fromString("claude-code:local");
    REQUIRE(k.has_value());
    CHECK(k->service.token == "claude-code");
    CHECK(k->id == "local");
}

TEST_CASE_METHOD(
    TokenStoreFixture,
    "the service filter hides a workspace without deleting it",
    "[tokenstore][filter]"
) {
    const WorkspaceKey gone{Service{QStringLiteral("teams")}, "tenant-1"};
    TokenStore::saveWorkspace(rec("T001", "Slack team"));
    TokenStore::saveWorkspace(WorkspaceRecord{gone, "Contoso", {}, "blob"});
    TokenStore::setActiveWorkspace(gone);

    // "This build only has Slack."
    TokenStore::setServiceFilter([](const Service &s) { return s.token == "slack"; });
    const auto visible = TokenStore::workspaceKeys();
    REQUIRE(visible.size() == 1);
    CHECK(visible.front() == slackKey("T001"));
    CHECK_FALSE(TokenStore::activeWorkspace().has_value()); // the hidden one can't be active

    // Reordering the visible list must not drop the hidden record.
    TokenStore::setWorkspaceOrder(visible);
    CHECK(TokenStore::loadWorkspace(gone).has_value());

    // A build that has the backend sees it again, untouched.
    TokenStore::setServiceFilter(nullptr);
    CHECK(TokenStore::workspaceKeys().size() == 2);
    REQUIRE(TokenStore::activeWorkspace().has_value());
    CHECK(*TokenStore::activeWorkspace() == gone);
    CHECK(TokenStore::loadWorkspace(gone)->auth == QByteArray("blob"));
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

// ── Custom workspace icon path ────────────────────────────────────────────────

TEST_CASE_METHOD(
    TokenStoreFixture, "custom icon path round-trips and clears", "[tokenstore][icon]"
) {
    const auto k = slackKey("T1");
    CHECK(TokenStore::customWorkspaceIconPath(k).isEmpty());
    TokenStore::setCustomWorkspaceIconPath(k, "/tmp/x.png");
    CHECK(TokenStore::customWorkspaceIconPath(k) == "/tmp/x.png");
    TokenStore::setCustomWorkspaceIconPath(k, {});
    CHECK(TokenStore::customWorkspaceIconPath(k).isEmpty());
}

TEST_CASE_METHOD(
    TokenStoreFixture, "removeWorkspace drops the custom icon path", "[tokenstore][icon]"
) {
    TokenStore::saveWorkspace(rec("T1", "One", "https://x/1.png"));
    TokenStore::setCustomWorkspaceIconPath(slackKey("T1"), "/tmp/x.png");
    TokenStore::removeWorkspace(slackKey("T1"));
    CHECK(TokenStore::customWorkspaceIconPath(slackKey("T1")).isEmpty());
}

TEST_CASE_METHOD(
    TokenStoreFixture, "displayIconUrl ignores a path whose file is missing", "[tokenstore][icon]"
) {
    const auto r = rec("T1", "One", "https://x/1.png");
    TokenStore::setCustomWorkspaceIconPath(r.key, "/nonexistent/dir/icon.png");
    CHECK(TokenStore::displayIconUrl(r) == "https://x/1.png");
}

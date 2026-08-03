// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// SecretStore, QSettings backend. That is the backend these tests can reach:
// the macOS keychain unit is deliberately not linked into test targets (see
// MSGA_SECRET_STORE_SRCS in tests/CMakeLists.txt) so runs stay hermetic — no
// keychain prompts, nothing left on the developer's login keychain. So what is
// covered here is the Linux/Windows path, where the whole promise is "behaves
// exactly like the plaintext QSettings code it replaced".
//
// The sharp edge worth pinning down: on this backend read/write operate on the
// *same* QSettings key the app has always used, so the write-then-scrub-the-
// plaintext-copy sequence a keychain platform needs would here delete the value
// it just stored. writeScrubbingLegacy() is what keeps that straight, and the
// last two cases below are its regression guard.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include "util/secret_store.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga");
    app.setOrganizationName("msga");
    // SecretStore hardcodes QSettings("msga", "msga"); redirect user-scope
    // storage so tests never touch the real config.
    static QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    return Catch::Session().run(argc, argv);
}

namespace {

// The key shape real callers use, so the tests exercise nested-group paths.
const QString kKey = QStringLiteral("workspace/slack:T0TEST/auth");

QString rawSetting(const QString &key) {
    return QSettings("msga", "msga").value(key).toString();
}

} // namespace

TEST_CASE("this build is on the QSettings fallback, not a keychain", "[secret]") {
    // Guards the assumption every case below rests on. On a macOS build this
    // whole file would be testing a different backend.
    CHECK_FALSE(SecretStore::isKeychainBacked());
}

TEST_CASE("write/read/remove round-trips", "[secret]") {
    SecretStore::remove(kKey);
    CHECK(SecretStore::read(kKey).isEmpty());

    REQUIRE(SecretStore::write(kKey, QStringLiteral("xoxp-secret")));
    CHECK(SecretStore::read(kKey) == QStringLiteral("xoxp-secret"));

    // Overwrite replaces rather than appends.
    REQUIRE(SecretStore::write(kKey, QStringLiteral("xoxp-rotated")));
    CHECK(SecretStore::read(kKey) == QStringLiteral("xoxp-rotated"));

    SecretStore::remove(kKey);
    CHECK(SecretStore::read(kKey).isEmpty());
}

TEST_CASE("an empty value clears the secret", "[secret]") {
    REQUIRE(SecretStore::write(kKey, QStringLiteral("xoxp-secret")));
    REQUIRE(SecretStore::write(kKey, QString()));
    CHECK(SecretStore::read(kKey).isEmpty());
    // Cleared, not stored as an empty string that a contains() check would find.
    CHECK_FALSE(QSettings("msga", "msga").contains(kKey));
}

TEST_CASE("a compact-JSON auth blob survives the UTF-8 round-trip", "[secret]") {
    // TokenStore hands the blob over as QString::fromUtf8(QByteArray) and takes
    // it back with toUtf8(); the blob is always compact JSON.
    const QString blob = QStringLiteral(R"({"xoxp":"xoxp-1-abc","refreshToken":"xoxe-1-def"})");
    REQUIRE(SecretStore::write(kKey, blob));
    CHECK(SecretStore::read(kKey).toUtf8() == blob.toUtf8());
    SecretStore::remove(kKey);
}

TEST_CASE("readMigrating is a plain read on the fallback", "[secret]") {
    // No migration to do when the fallback store *is* the legacy store: the
    // value must come back untouched and stay where it is.
    QSettings("msga", "msga").setValue(kKey, QStringLiteral("legacy-plaintext"));
    CHECK(SecretStore::readMigrating(kKey) == QStringLiteral("legacy-plaintext"));
    // Still present — nothing was "promoted" and scrubbed out from under us.
    CHECK(rawSetting(kKey) == QStringLiteral("legacy-plaintext"));
    SecretStore::remove(kKey);
}

TEST_CASE("readMigrating on an absent key yields empty", "[secret]") {
    SecretStore::remove(kKey);
    CHECK(SecretStore::readMigrating(kKey).isEmpty());
}

TEST_CASE("writeScrubbingLegacy keeps the value on the fallback", "[secret]") {
    // The regression guard: scrubbing "the plaintext copy" here would delete the
    // secret itself, logging every Linux/Windows user out on save.
    REQUIRE(SecretStore::writeScrubbingLegacy(kKey, QStringLiteral("xoxp-secret")));
    CHECK(SecretStore::read(kKey) == QStringLiteral("xoxp-secret"));
    CHECK(rawSetting(kKey) == QStringLiteral("xoxp-secret"));
    SecretStore::remove(kKey);
}

TEST_CASE("writeScrubbingLegacy with an empty value clears the secret", "[secret]") {
    REQUIRE(SecretStore::writeScrubbingLegacy(kKey, QStringLiteral("xoxp-secret")));
    REQUIRE(SecretStore::writeScrubbingLegacy(kKey, QString()));
    CHECK(SecretStore::read(kKey).isEmpty());
}

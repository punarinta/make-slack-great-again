// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Cross-platform part of SecretStore: the read-with-migration helper. The
// read/write/remove/isKeychainBacked primitives are provided per-platform
// (secret_store_mac.mm on macOS, secret_store_qsettings.cpp elsewhere).
#include "util/secret_store.h"

#include <QSettings>

namespace SecretStore {

QString readMigrating(const QString &key) {
    const QString v = read(key);
    // Already in the keychain, or the fallback store *is* QSettings(key) — either
    // way there is nothing to migrate.
    if (!v.isEmpty() || !isKeychainBacked())
        return v;

    // Keychain miss: pull a pre-keychain plaintext value forward, once.
    QSettings     s("msga", "msga");
    const QString legacy = s.value(key).toString();
    if (legacy.isEmpty())
        return QString();
    write(key, legacy); // promote into the keychain
    s.remove(key);      // scrub the plaintext copy
    return legacy;
}

} // namespace SecretStore

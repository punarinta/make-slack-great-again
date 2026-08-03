// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Cross-platform part of SecretStore: the read-with-migration helper. The
// read/write/remove/isKeychainBacked primitives are provided per-platform
// (secret_store_mac.mm on macOS, secret_store_qsettings.cpp elsewhere).
#include "util/secret_store.h"

#include <QDebug>
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
    if (!write(key, legacy)) {
        // The keychain refused (locked, prompt denied). The plaintext copy is
        // the only one there is — keep it and retry the promotion next read.
        qWarning() << "[SecretStore] could not promote" << key
                   << "into the keychain; leaving the plaintext copy in place";
        return legacy;
    }
    s.remove(key); // promoted — now scrub the plaintext copy
    return legacy;
}

bool writeScrubbingLegacy(const QString &key, const QString &value) {
    if (!write(key, value)) {
        // Don't touch the plaintext copy: it may be the last one standing.
        qWarning() << "[SecretStore] keychain write failed for" << key
                   << "— keeping any existing plaintext copy";
        return false;
    }
    if (isKeychainBacked())
        QSettings("msga", "msga").remove(key);
    return true;
}

} // namespace SecretStore

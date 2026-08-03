// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Fallback SecretStore backend for platforms without a wired-up OS keychain
// (Linux, Windows). Secrets stay in the same QSettings store as before — this
// is a documented limitation, not encryption at rest. Reads/writes the key
// verbatim so behaviour is unchanged from the pre-SecretStore code; swap this
// TU for a libsecret / Windows Credential Manager backend to close the gap.
#include "util/secret_store.h"

#include <QSettings>

namespace SecretStore {

bool isKeychainBacked() {
    return false;
}

QString read(const QString &key) {
    return QSettings("msga", "msga").value(key).toString();
}

bool write(const QString &key, const QString &value) {
    QSettings s("msga", "msga");
    if (value.isEmpty())
        s.remove(key);
    else
        s.setValue(key, value);
    return true;
}

void remove(const QString &key) {
    QSettings("msga", "msga").remove(key);
}

} // namespace SecretStore

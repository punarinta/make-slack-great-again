// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QString>

// Process-wide storage for credentials — OAuth tokens, refresh tokens, API
// keys, IMAP passwords, the personal Slack app secret. These are the values
// that must never sit in the plaintext QSettings/plist, where any process
// running as the user (or a backup/cloud-sync of that folder) could lift them.
//
//   • macOS   — Keychain generic-password items (encrypted at rest, gated by
//               the login keychain). Backend: Security.framework.
//   • other   — no OS keychain backend yet, so secrets fall back to the same
//               QSettings store as before (a documented limitation; the seam
//               here is where a libsecret / Credential-Manager backend slots in).
//
// `key` is an opaque, stable identifier the caller owns the namespace for
// (e.g. "workspace/slack:T0/auth", "llm/anthropic/apiKey"). Values are UTF-8.
//
// NOTE on the QSettings fallback: it reads/writes the *same* key the app used
// historically, so on Linux/Windows this is behaviour-preserving and needs no
// migration. Because of that, callers must not independently QSettings::remove()
// a key they just wrote through SecretStore unless isKeychainBacked() is true.
namespace SecretStore {

// Returns the stored secret, or an empty QString if the key is absent.
QString read(const QString &key);

// Inserts or replaces the secret. An empty value removes the key. True on success.
bool write(const QString &key, const QString &value);

// Removes the secret if present.
void remove(const QString &key);

// True when secrets live in the OS keychain (macOS); false on the QSettings
// fallback (Linux/Windows).
bool isKeychainBacked();

// read(), plus a one-time migration: on a keychain platform, if the key is not
// yet in the keychain but a legacy plaintext copy exists in QSettings under the
// same key, it is promoted into the keychain and the plaintext copy is deleted.
// On the QSettings fallback this is exactly read().
//
// A failed promotion keeps the plaintext copy: the keychain can refuse (locked,
// denied prompt), and scrubbing then would destroy the only copy that exists.
QString readMigrating(const QString &key);

// The write counterpart every caller wants: write(), and on a keychain platform
// drop any leftover plaintext copy of the same key once the write has actually
// succeeded. Use this instead of hand-rolling write() + isKeychainBacked() +
// QSettings::remove() — on the QSettings fallback that sequence deletes the
// value it just stored, and on a keychain platform it deletes the plaintext
// copy even when the keychain write failed.
bool writeScrubbingLegacy(const QString &key, const QString &value);

} // namespace SecretStore

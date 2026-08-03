// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// macOS SecretStore backend: Keychain generic-password items via the Security
// framework. Items are device-local (kSecAttrAccessibleAfterFirstUnlock…
// ThisDeviceOnly) so they never sync to iCloud Keychain. The Security API is a
// synchronous C API, so no Objective-C is actually required — this is a .mm only
// to sit cleanly alongside the other platform-native units and keep it out of
// the PCH.
#include "util/secret_store.h"

#include <QByteArray>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace {

// One service name for every msga keychain item; the caller's key is the account.
CFStringRef kService = CFSTR("app.msga.msga");

CFStringRef makeCFString(const QString &s) {
    const QByteArray u = s.toUtf8();
    return CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(u.constData()),
        static_cast<CFIndex>(u.size()), kCFStringEncodingUTF8, false
    );
}

// class + service + account — the fields that identify one item. Caller CFReleases.
CFMutableDictionaryRef identityQuery(const QString &account) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, kService);
    CFStringRef acct = makeCFString(account);
    CFDictionarySetValue(q, kSecAttrAccount, acct);
    CFRelease(acct); // the dictionary retains it
    return q;
}

} // namespace

namespace SecretStore {

bool isKeychainBacked() {
    return true;
}

QString read(const QString &key) {
    CFMutableDictionaryRef q = identityQuery(key);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef out = nullptr;
    const OSStatus st = SecItemCopyMatching(q, &out);
    CFRelease(q);
    if (st != errSecSuccess || out == nullptr)
        return QString();

    CFDataRef     data   = reinterpret_cast<CFDataRef>(out);
    const QString result = QString::fromUtf8(
        reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
        static_cast<qsizetype>(CFDataGetLength(data))
    );
    CFRelease(out);
    return result;
}

bool write(const QString &key, const QString &value) {
    if (value.isEmpty()) {
        remove(key);
        return true;
    }

    const QByteArray bytes = value.toUtf8();
    CFDataRef        data  = CFDataCreate(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.constData()),
        static_cast<CFIndex>(bytes.size())
    );

    // Update the existing item's data if present; otherwise add a fresh item.
    CFMutableDictionaryRef q   = identityQuery(key);
    CFMutableDictionaryRef upd = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(upd, kSecValueData, data);
    OSStatus st = SecItemUpdate(q, upd);
    CFRelease(upd);

    if (st == errSecItemNotFound) {
        CFDictionarySetValue(q, kSecValueData, data);
        CFDictionarySetValue(
            q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        );
        st = SecItemAdd(q, nullptr);
    }
    CFRelease(q);
    CFRelease(data);
    return st == errSecSuccess;
}

void remove(const QString &key) {
    CFMutableDictionaryRef q = identityQuery(key);
    SecItemDelete(q);
    CFRelease(q);
}

} // namespace SecretStore

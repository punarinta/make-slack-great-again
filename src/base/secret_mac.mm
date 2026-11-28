// secret.h on the macOS Keychain: generic-password items in the user's
// default (login) keychain, account = key, under two services:
//
//   "com.nisdos.msga"  msga's own items, the ones it reads
//   "app.msga.msga"    earlier versions' items, kept up to date for a
//                      rollback to an earlier version
//
// The legacy keychain ties an item's access list to the code requirement of
// the binary that created it. Earlier releases were signed ad hoc, so their
// items admit only that one build (its cdhash): the first read of each from
// this app asks the user once. What it reads is then copied into msga's own
// item, created by this app; the release signature's designated requirement
// is the bundle identifier alone (scripts/release.sh), so later builds read
// their own items without asking. Writing an old item needs no permission (an
// item's "encrypt" entry admits any app); deleting one an earlier version created
// does, so a removed key leaves an empty own item behind (a tombstone) rather
// than reading the old one back. Plain CoreFoundation + Security C API —
// nothing here needs Foundation.
//
// Not the data-protection keychain (kSecUseDataProtectionKeychain): that one
// needs a keychain-access-groups entitlement, i.e. a Developer ID signed app.
#include "base/secret.h"

#include "base/cfstr.h"
#include "base/old_settings.h"
#include "base/process.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <cstdio>
#include <cstdlib>

namespace secret {

namespace detail {
// Tests only (see tests/test_secret.cpp): a throwaway SecKeychainRef that
// every call uses instead of the user's default keychain.
const void *testKeychain = nullptr;
} // namespace detail

namespace {

// Set the first time the keychain refuses a write this process: a locked
// keychain must not turn into a prompt per call.
bool g_refused = false;

using base::cfString;

// Tests (base::testProcess(): base::test marks them) never use a keychain: a
// keychain may prompt, over ssh it blocks for good, and the user's is no
// place for test items. Their credentials go where Linux and Windows keep
// them, the settings store of earlier versions, which in tests is a temporary INI file
// (old_settings.h). Only test_secret.cpp's throwaway keychain file
// (detail::testKeychain) is a real one. A demo run (base::isolatedStorage())
// keeps to that INI file too, under its throwaway XDG_CONFIG_HOME.
bool testStore() {
    return !detail::testKeychain && base::isolatedStorage();
}

// msga's own items, and earlier versions' (the top of this file). Tests' items in
// their throwaway keychain have services of their own.
enum class Service { Own, Old };

// The attributes naming the item for `key`; the caller releases it. Every
// keychain call starts here, so this is where a test process that got past
// testStore() stops, loudly, before the keychain can prompt.
CFMutableDictionaryRef itemQuery(Service which, std::string_view key) {
    if (testStore()) {
        std::fprintf(stderr, "secret: an isolated process reached the user's keychain; aborting\n");
        std::abort();
    }
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    const bool  own     = which == Service::Own;
    CFStringRef service = cfString(
        base::testProcess() ? (own ? "com.nisdos.msga.tests" : "app.msga.msga.tests")
                            : (own ? "com.nisdos.msga" : "app.msga.msga")
    );
    CFStringRef account = cfString(key);
    if (service)
        CFDictionarySetValue(q, kSecAttrService, service);
    if (account)
        CFDictionarySetValue(q, kSecAttrAccount, account);
    if (service)
        CFRelease(service);
    if (account)
        CFRelease(account);
    if (detail::testKeychain) {
        CFArrayRef list = CFArrayCreate(nullptr, &detail::testKeychain, 1, &kCFTypeArrayCallBacks);
        CFDictionarySetValue(q, kSecMatchSearchList, list);
        CFRelease(list);
    }
    return q;
}

bool keychainWrite(Service which, std::string_view key, std::string_view value) {
    CFDataRef data =
        CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(value.data()), CFIndex(value.size()));
    CFMutableDictionaryRef q      = itemQuery(which, key);
    CFMutableDictionaryRef change = CFDictionaryCreateMutable(
        nullptr, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(change, kSecValueData, data);
    // An update can't empty an item (the keychain ignores it): a tombstone
    // is a new one.
    OSStatus st = value.empty() ? errSecItemNotFound : SecItemUpdate(q, change);
    if (st == errSecItemNotFound) {
        CFDictionarySetValue(q, kSecValueData, data);
        // Device-local, never synced to iCloud — as earlier versions added them.
        CFDictionarySetValue(
            q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        );
        // An earlier-version item this app adds is marked: it may delete it again.
        if (which == Service::Old)
            CFDictionarySetValue(q, kSecAttrComment, CFSTR("msga"));
        if (detail::testKeychain) { // an add names its keychain differently
            CFDictionaryRemoveValue(q, kSecMatchSearchList);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            CFDictionarySetValue(q, kSecUseKeychain, detail::testKeychain);
#pragma clang diagnostic pop
        }
        st = SecItemAdd(q, nullptr);
    }
    CFRelease(change);
    CFRelease(q);
    CFRelease(data);
    return st == errSecSuccess;
}

std::string keychainRead(Service which, std::string_view key, bool *found) {
    CFMutableDictionaryRef q = itemQuery(which, key);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef      out = nullptr;
    const OSStatus st  = SecItemCopyMatching(q, &out);
    CFRelease(q);
    std::string value;
    const bool  ok = st == errSecSuccess && out && CFGetTypeID(out) == CFDataGetTypeID();
    if (ok) {
        CFDataRef d = static_cast<CFDataRef>(out);
        value.assign(
            reinterpret_cast<const char *>(CFDataGetBytePtr(d)), size_t(CFDataGetLength(d))
        );
    }
    if (out)
        CFRelease(out);
    if (found)
        *found = ok;
    return value;
}

// Whose the earlier-version item for `key` is: the comment keychainWrite marks
// ours with. Its attributes only: no permission needed.
enum class OldItem { None, Ours, Theirs };

OldItem oldItem(std::string_view key) {
    CFMutableDictionaryRef q = itemQuery(Service::Old, key);
    CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef      out = nullptr;
    const OSStatus st  = SecItemCopyMatching(q, &out);
    CFRelease(q);
    OldItem whose = OldItem::None;
    if (st == errSecSuccess && out && CFGetTypeID(out) == CFDictionaryGetTypeID()) {
        const void *c = CFDictionaryGetValue(static_cast<CFDictionaryRef>(out), kSecAttrComment);
        whose         = c && CFGetTypeID(c) == CFStringGetTypeID() &&
                                CFStringCompare(static_cast<CFStringRef>(c), CFSTR("msga"), 0) ==
                                    kCFCompareEqualTo
                            ? OldItem::Ours
                            : OldItem::Theirs;
    }
    if (out)
        CFRelease(out);
    return whose;
}

void keychainRemove(Service which, std::string_view key) {
    CFMutableDictionaryRef q = itemQuery(which, key);
    SecItemDelete(q);
    CFRelease(q);
}

// `value` into msga's own item and, for a rollback, the earlier versions' one.
bool keychainStore(std::string_view key, std::string_view value) {
    if (!keychainWrite(Service::Own, key, value))
        return false;
    keychainWrite(Service::Old, key, value);
    return true;
}

} // namespace

bool available() {
    return true;
}

bool write(std::string_view key, std::string_view value) {
    if (testStore())
        return value.empty() ? oldsettings::remove(key) : oldsettings::write(key, value);
    if (value.empty()) {
        remove(key);
        return true;
    }
    if (!g_refused && keychainStore(key, value)) {
        if (oldsettings::get(key).kind != oldsettings::Value::Kind::None)
            oldsettings::remove(key); // in the keychain now: scrub the plaintext copy
        return true;
    }
    // Refused: the plaintext copy where earlier versions kept it, rather than a
    // workspace with no credentials. A later read moves it in.
    g_refused = true;
    return oldsettings::write(key, value);
}

std::string read(std::string_view key, bool *found) {
    if (testStore()) {
        const oldsettings::Value v = oldsettings::get(key);
        if (found)
            *found = v.kind != oldsettings::Value::Kind::None;
        return v.text();
    }
    bool        have  = false;
    std::string value = keychainRead(Service::Own, key, &have);
    if (have) {
        if (found)
            *found = !value.empty(); // empty: a tombstone
        return value;
    }
    // The earlier versions' item: the first read may ask the user (the top of this
    // file); a copy in our own item makes it the only time.
    value = keychainRead(Service::Old, key, &have);
    if (have) {
        if (!g_refused && !keychainWrite(Service::Own, key, value))
            g_refused = true;
        if (found)
            *found = true;
        return value;
    }
    if (detail::testKeychain) {
        if (found)
            *found = false;
        return {};
    }
    // In no keychain item: a plaintext copy from before the keychain (or
    // from a refused write) is moved in, once.
    const oldsettings::Value legacy = oldsettings::get(key);
    if (found)
        *found = legacy.kind != oldsettings::Value::Kind::None;
    const std::string text = legacy.text();
    if (!text.empty() && !g_refused) {
        if (keychainStore(key, text))
            oldsettings::remove(key);
        else
            g_refused = true;
    }
    return text;
}

void remove(std::string_view key) {
    if (testStore()) {
        oldsettings::remove(key);
        return;
    }
    keychainRemove(Service::Own, key);
    // An item an earlier version created can't be deleted without asking: a
    // tombstone keeps it from being read back.
    switch (oldItem(key)) {
    case OldItem::Ours:
        keychainRemove(Service::Old, key);
        break;
    case OldItem::Theirs:
        keychainWrite(Service::Own, key, {});
        break;
    case OldItem::None:
        break;
    }
    if (!detail::testKeychain && oldsettings::get(key).kind != oldsettings::Value::Kind::None)
        oldsettings::remove(key);
}

} // namespace secret

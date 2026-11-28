// The keychain backend (secret.h), and what tests get instead.
//
// The keychain case (macOS only) runs against a throwaway keychain file
// created (unlocked, never added to the search list) and deleted here, never
// the user's keychains: those may be locked or prompt (always so over ssh),
// and tests must not touch them.
#include "base/file.h"
#include "base/old_settings.h"
#include "base/process.h"
#include "base/secret.h"
#include "support/test.h"

#include <cstring>
#include <string>

TEST("secret: tests keep credentials in their temporary settings file") {
    // runAll's isolation: the settings store of earlier versions as an INI file under
    // the test's own XDG_CONFIG_HOME, on every OS — never a keychain.
    const std::string config = base::env("XDG_CONFIG_HOME");
    REQUIRE(!config.empty());
    CHECK(oldsettings::iniPath().compare(0, config.size(), config) == 0);
    const std::string key   = "test/msga_base_tests_store";
    bool              found = true;
    CHECK(secret::read(key, &found).empty());
    CHECK_FALSE(found);
    REQUIRE(secret::write(key, "token \xE2\x9C\x93"));
    CHECK_STR(secret::read(key, &found), "token \xE2\x9C\x93");
    CHECK(found);
    CHECK_STR(oldsettings::get(key).text(), "token \xE2\x9C\x93");
    std::string ini;
    CHECK(file::readAll(oldsettings::iniPath(), &ini));
    CHECK(ini.find("msga_base_tests_store") != std::string::npos);
    secret::remove(key);
    secret::read(key, &found);
    CHECK_FALSE(found);
}

#ifdef __APPLE__
#include <Security/Security.h>

namespace secret::detail {
extern const void *testKeychain;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" // the SecKeychain* file API

TEST("secret: keychain write, replace, read, remove") {
    const std::string dir = base::test::makeTempDir("msga_secret_test_");
    REQUIRE(!dir.empty());
    const std::string path = dir + "/test.keychain";
    SecKeychainRef    kc   = nullptr;
    REQUIRE(SecKeychainCreate(path.c_str(), 4, "test", false, nullptr, &kc) == errSecSuccess);
    secret::detail::testKeychain = kc;

    const std::string key = "test/msga_base_tests";
    CHECK(secret::available());
    bool found = true;
    CHECK(secret::read(key, &found).empty());
    CHECK_FALSE(found);
    CHECK(secret::write(key, "first \xE2\x9C\x93"));
    CHECK_STR(secret::read(key, &found), "first \xE2\x9C\x93");
    CHECK(found);
    CHECK(secret::write(key, "second")); // replaces
    CHECK_STR(secret::read(key), "second");
    CHECK(secret::write(key, "")); // empty removes
    CHECK(secret::read(key, &found).empty());
    CHECK_FALSE(found);
    CHECK(secret::write(key, "again"));
    secret::remove(key);
    secret::read(key, &found);
    CHECK_FALSE(found);

    secret::detail::testKeychain = nullptr;
    SecKeychainDelete(kc); // removes the file too
    CFRelease(kc);
    base::test::removeTree(dir);
}

namespace {

// The test keychain's services (secret_mac.mm): earlier versions' and msga's own.
CFStringRef cf(const char *s) {
    return CFStringCreateWithCString(nullptr, s, kCFStringEncodingUTF8);
}

CFMutableDictionaryRef query(SecKeychainRef kc, const char *service, const char *key) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFStringRef svc = cf(service), acct = cf(key);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, svc);
    CFDictionarySetValue(q, kSecAttrAccount, acct);
    CFRelease(svc);
    CFRelease(acct);
    const void *list[] = {kc};
    CFArrayRef  l      = CFArrayCreate(nullptr, list, 1, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(q, kSecMatchSearchList, l);
    CFRelease(l);
    return q;
}

// An item as an earlier version added it.
bool addItem(SecKeychainRef kc, const char *service, const char *key, const char *value) {
    CFMutableDictionaryRef q = query(kc, service, key);
    CFDictionaryRemoveValue(q, kSecMatchSearchList);
    CFDictionarySetValue(q, kSecUseKeychain, kc);
    CFDataRef d =
        CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(value), CFIndex(strlen(value)));
    CFDictionarySetValue(q, kSecValueData, d);
    const OSStatus st = SecItemAdd(q, nullptr);
    CFRelease(d);
    CFRelease(q);
    return st == errSecSuccess;
}

// The item's data; "-" when there is no item.
std::string itemData(SecKeychainRef kc, const char *service, const char *key) {
    CFMutableDictionaryRef q = query(kc, service, key);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef   out = nullptr;
    std::string v   = "-";
    if (SecItemCopyMatching(q, &out) == errSecSuccess && out) {
        CFDataRef d = static_cast<CFDataRef>(out);
        v.assign(reinterpret_cast<const char *>(CFDataGetBytePtr(d)), size_t(CFDataGetLength(d)));
    }
    if (out)
        CFRelease(out);
    CFRelease(q);
    return v;
}

constexpr const char *kOld = "app.msga.msga.tests", *kOwn = "com.nisdos.msga.tests";

} // namespace

TEST("secret: an earlier version's item is copied once, and kept up to date") {
    const std::string dir = base::test::makeTempDir("msga_secret_test_");
    REQUIRE(!dir.empty());
    const std::string path = dir + "/test.keychain";
    SecKeychainRef    kc   = nullptr;
    REQUIRE(SecKeychainCreate(path.c_str(), 4, "test", false, nullptr, &kc) == errSecSuccess);
    secret::detail::testKeychain = kc;

    // An upgrade: only the earlier version's item exists. The first read takes it and
    // copies it into msga's own; later reads use that one.
    const char *key = "workspace/slack:T0/auth";
    REQUIRE(addItem(kc, kOld, key, "{\"xoxp\":\"old\"}"));
    bool found = false;
    CHECK_STR(secret::read(key, &found), "{\"xoxp\":\"old\"}");
    CHECK(found);
    CHECK_STR(itemData(kc, kOwn, key), "{\"xoxp\":\"old\"}");
    // A write (a refreshed token) goes to both: an earlier version reads it after a
    // rollback.
    CHECK(secret::write(key, "{\"xoxp\":\"new\"}"));
    CHECK_STR(itemData(kc, kOwn, key), "{\"xoxp\":\"new\"}");
    CHECK_STR(itemData(kc, kOld, key), "{\"xoxp\":\"new\"}");
    CHECK_STR(secret::read(key), "{\"xoxp\":\"new\"}");
    // A removal leaves the old item (deleting it would ask the user) and an
    // empty own one, so the old value does not come back.
    secret::remove(key);
    CHECK_STR(secret::read(key, &found), "");
    CHECK_FALSE(found);
    CHECK_STR(itemData(kc, kOwn, key), "");
    CHECK_STR(itemData(kc, kOld, key), "{\"xoxp\":\"new\"}");
    // Stored again after that: both again.
    CHECK(secret::write(key, "{\"xoxp\":\"again\"}"));
    CHECK_STR(secret::read(key, &found), "{\"xoxp\":\"again\"}");
    CHECK(found);
    CHECK_STR(itemData(kc, kOld, key), "{\"xoxp\":\"again\"}");
    // A key earlier versions never had: its earlier-version item is ours, so a removal
    // deletes both, no tombstone.
    CHECK(secret::write("llm/x/apiKey", "k"));
    CHECK_STR(itemData(kc, kOld, "llm/x/apiKey"), "k");
    secret::remove("llm/x/apiKey");
    CHECK_STR(itemData(kc, kOwn, "llm/x/apiKey"), "-");
    CHECK_STR(itemData(kc, kOld, "llm/x/apiKey"), "-");

    secret::detail::testKeychain = nullptr;
    SecKeychainDelete(kc);
    CFRelease(kc);
    base::test::removeTree(dir);
}

#pragma clang diagnostic pop
#endif

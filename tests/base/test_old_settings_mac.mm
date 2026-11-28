// old_settings_mac.mm against values laid out as earlier versions wrote them
// into CFPreferences ('/' → '.', CF types), in the tests' own domain
// (a test process's: com.msga-tests.msga), removed again afterwards.
#include "base/old_settings.h"
#include "base/process.h"
#include "support/test.h"

#include <CoreFoundation/CoreFoundation.h>

namespace {

const CFStringRef kDomain = CFSTR("com.msga-tests.msga");

void put(CFStringRef key, CFPropertyListRef v) {
    CFPreferencesSetValue(key, v, kDomain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
}

// oldsettings on the native store for one case (tests otherwise get the INI
// file), and the test domain emptied again however the case ends.
struct NativeStore {
    NativeStore() { oldsettings::detail::nativeInTests = true; }
    ~NativeStore() {
        oldsettings::detail::nativeInTests = false;
        CFArrayRef keys =
            CFPreferencesCopyKeyList(kDomain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        if (keys) {
            CFPreferencesSetMultiple(
                nullptr, keys, kDomain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost
            );
            CFRelease(keys);
        }
        CFPreferencesSynchronize(kDomain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    }
};

} // namespace

TEST("old settings (macOS): the CFPreferences layout") {
    REQUIRE(base::testProcess());
    NativeStore native;
    // A string, a "@"-escaped one, an int, a bool, a string list, bytes.
    put(CFSTR("workspace.slack:T1.auth"), CFSTR("{\"xoxp\":\"a\"}"));
    put(CFSTR("gif.giphy.apiKey"), CFSTR("@@g"));
    int         n   = 7;
    CFNumberRef num = CFNumberCreate(nullptr, kCFNumberIntType, &n);
    put(CFSTR("emoji.skinTone"), num);
    put(CFSTR("updates.autoCheck"), kCFBooleanFalse);
    const void *items[] = {CFSTR("slack:T1"), CFSTR("claude-code:local")};
    CFArrayRef  list    = CFArrayCreate(nullptr, items, 2, &kCFTypeArrayCallBacks);
    put(CFSTR("workspaces"), list);
    const UInt8 bytes[] = {0x01, 0xd9, 0x00, 0xcb};
    CFDataRef   data    = CFDataCreate(nullptr, bytes, 4);
    put(CFSTR("window.geometry"), data);
    // A '.' inside a store key is a middle dot in the CF key.
    put(CFSTR("llm.providers.custom·x.model"), CFSTR("m"));
    CFPreferencesSynchronize(kDomain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);

    const oldsettings::Map m = oldsettings::load();
    CHECK_STR(m.at("workspace/slack:T1/auth").text(), "{\"xoxp\":\"a\"}");
    CHECK_STR(m.at("gif/giphy/apiKey").text(), "@g");
    CHECK(m.at("emoji/skinTone").toInt(0) == 7);
    CHECK_FALSE(m.at("updates/autoCheck").toBool(true));
    CHECK(
        (m.at("workspaces").toList() == std::vector<std::string>{"slack:T1", "claude-code:local"})
    );
    CHECK((m.at("window/geometry").s == std::string("\x01\xd9\0\xcb", 4)));
    CHECK_STR(m.at("llm/providers/custom.x/model").text(), "m");
    // Writes land where earlier versions read them.
    REQUIRE(oldsettings::write("llm/anthropic/apiKey", "@k"));
    CFPropertyListRef v = CFPreferencesCopyValue(
        CFSTR("llm.anthropic.apiKey"), kDomain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost
    );
    CHECK(
        (v && CFStringCompare(static_cast<CFStringRef>(v), CFSTR("@@k"), 0) == kCFCompareEqualTo)
    );
    if (v)
        CFRelease(v);
    CHECK_STR(oldsettings::get("llm/anthropic/apiKey").text(), "@k");
    REQUIRE(oldsettings::remove("llm/anthropic/apiKey"));
    CHECK(oldsettings::get("llm/anthropic/apiKey").kind == oldsettings::Value::Kind::None);
    CFRelease(num);
    CFRelease(list);
    CFRelease(data);
}

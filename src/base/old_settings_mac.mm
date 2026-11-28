// old_settings.h on macOS: the store's CFPreferences domain, "com.msga.<app>"
// (the organization "msga" becomes "com.msga", then the application
// name). Keys rotate '/' → '.', '.' → U+00B7, U+00B7 → '/';
// values are CF property-list types (a string a CFString, an int a CFNumber,
// bytes CFData, a string list a CFArray). Read through CFPreferences,
// never the .plist file, which cfprefsd may not have flushed.
#include "base/old_settings.h"

#include "base/cfstr.h"
#include "base/process.h"
#include "base/str.h"
#include "base/utf8.h"

#include <CoreFoundation/CoreFoundation.h>

namespace oldsettings {

namespace {

CFStringRef domain(std::string_view app) {
    const bool tests = base::testProcess();
    return base::cfString(str::concat({tests ? "com.msga-tests." : "com.msga.", app}));
}

// rotateSlashesDotsAndMiddots, one way or the other.
std::string rotate(std::string_view key, bool toMac) {
    std::string out;
    for (size_t i = 0; i < key.size();) {
        const uint32_t cp = utf8::decode(key, i);
        uint32_t       r  = cp;
        if (toMac)
            r = cp == '/' ? '.' : cp == '.' ? 0xB7 : cp == 0xB7 ? '/' : cp;
        else
            r = cp == '.' ? '/' : cp == 0xB7 ? '.' : cp == '/' ? 0xB7 : cp;
        utf8::append(out, r);
    }
    return out;
}

Value valueOf(CFPropertyListRef v) {
    Value out;
    if (!v)
        return out;
    const CFTypeID t = CFGetTypeID(v);
    if (t == CFStringGetTypeID())
        return decodeString(base::fromCFString(static_cast<CFStringRef>(v)));
    if (t == CFBooleanGetTypeID()) {
        out.kind = Value::Kind::Bool;
        out.n    = CFBooleanGetValue(static_cast<CFBooleanRef>(v)) ? 1 : 0;
    } else if (t == CFNumberGetTypeID()) {
        long long n = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberLongLongType, &n);
        out.kind = Value::Kind::Int;
        out.n    = n;
    } else if (t == CFDataGetTypeID()) {
        CFDataRef   d = static_cast<CFDataRef>(v);
        std::string bytes(
            reinterpret_cast<const char *>(CFDataGetBytePtr(d)), size_t(CFDataGetLength(d))
        );
        if (!bytes.empty() && bytes[0] == '@') // an encoded variant (qtValue)
            return decodeString(std::move(bytes));
        out.kind = Value::Kind::Bytes;
        out.s    = std::move(bytes);
    } else if (t == CFArrayGetTypeID()) {
        CFArrayRef a = static_cast<CFArrayRef>(v);
        out.kind     = Value::Kind::List;
        for (CFIndex i = 0, n = CFArrayGetCount(a); i < n; ++i)
            out.list.push_back(valueOf(CFArrayGetValueAtIndex(a, i)).text());
    }
    return out;
}

bool setValue(std::string_view key, CFPropertyListRef value, std::string_view app) {
    CFStringRef d = domain(app);
    CFStringRef k = base::cfString(rotate(key, true));
    CFPreferencesSetValue(k, value, d, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    const bool ok = CFPreferencesSynchronize(d, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    CFRelease(k);
    CFRelease(d);
    return ok;
}

} // namespace

// The native store (old_settings.cpp picks it, or the tests' INI file).
namespace native {

Map load(std::string_view app) {
    Map         out;
    CFStringRef d   = domain(app);
    CFArrayRef keys = CFPreferencesCopyKeyList(d, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    if (keys) {
        for (CFIndex i = 0, n = CFArrayGetCount(keys); i < n; ++i) {
            CFStringRef       k = static_cast<CFStringRef>(CFArrayGetValueAtIndex(keys, i));
            CFPropertyListRef v =
                CFPreferencesCopyValue(k, d, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
            out.insert_or_assign(rotate(base::fromCFString(k), false), valueOf(v));
            if (v)
                CFRelease(v);
        }
        CFRelease(keys);
    }
    CFRelease(d);
    return out;
}

Value get(std::string_view key, std::string_view app) {
    CFStringRef       d = domain(app);
    CFStringRef       k = base::cfString(rotate(key, true));
    CFPropertyListRef v =
        CFPreferencesCopyValue(k, d, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    Value out = valueOf(v);
    if (v)
        CFRelease(v);
    CFRelease(k);
    CFRelease(d);
    return out;
}

bool write(std::string_view key, std::string_view value, std::string_view app) {
    CFStringRef v  = base::cfString(encodeString(value));
    const bool  ok = v && setValue(key, v, app);
    if (v)
        CFRelease(v);
    return ok;
}

bool remove(std::string_view key, std::string_view app) {
    return setValue(key, nullptr, app);
}

} // namespace native

} // namespace oldsettings

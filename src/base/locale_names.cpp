// The OS's regional format (locale_names.h): Windows GetLocaleInfoEx, macOS
// CFDateFormatter, Linux compiled-in CLDR data (a static binary has no
// usable libc locale data) keyed by LC_ALL / LC_TIME / LANG.
#include "base/locale_names.h"

#include "base/str.h"
#include "base/time.h"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include "base/winstr.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include "base/cfstr.h"
#endif

namespace base {

namespace {

#if defined(_WIN32) || defined(__APPLE__)
// An OS date pattern in formatCivil's dialect: ICU's lone "y" (and "yyy")
// is the full year there.
std::string qtPattern(std::string_view p) {
    std::string out;
    for (size_t i = 0; i < p.size();) {
        if (p[i] == '\'') { // quoted text as is
            const size_t j = p.find('\'', i + 1);
            const size_t e = j == std::string_view::npos ? p.size() : j + 1;
            out.append(p.substr(i, e - i));
            i = e;
            continue;
        }
        size_t n = 1;
        while (i + n < p.size() && p[i + n] == p[i])
            ++n;
        if (p[i] == 'y' && n != 2)
            out += "yyyy";
        else
            out.append(p.substr(i, n));
        i += n;
    }
    return out;
}
#endif

#if defined(_WIN32)

// The current user's value, their overrides included.
std::string info(LCTYPE type) {
    wchar_t buf[128];
    return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, buf, 128) > 0 ? narrow(buf)
                                                                         : std::string();
}

#elif defined(__APPLE__)

// `count` strings of a formatter's array property into `out`.
bool symbols(CFDateFormatterRef f, CFStringRef key, std::string *out, CFIndex count) {
    auto *arr = static_cast<CFArrayRef>(CFDateFormatterCopyProperty(f, key));
    if (!arr)
        return false;
    const bool ok = CFArrayGetCount(arr) == count;
    for (CFIndex i = 0; ok && i < count; ++i)
        out[i] = fromCFString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(arr, i)));
    CFRelease(arr);
    return ok;
}

std::string symbol(CFDateFormatterRef f, CFStringRef key) {
    auto       *s   = static_cast<CFStringRef>(CFDateFormatterCopyProperty(f, key));
    std::string out = fromCFString(s);
    if (s)
        CFRelease(s);
    return out;
}

#else

#include "base/date_locales_generated.inc"

// The next NUL-terminated string of kDateLocales.
const char *next(const char *p) {
    return p + std::strlen(p) + 1;
}

#endif

} // namespace

std::string osLocale() {
    std::string tag;
#if defined(_WIN32)
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
        tag = narrow(name);
#elif defined(__APPLE__)
    if (CFLocaleRef loc = CFLocaleCopyCurrent()) {
        tag = fromCFString(CFLocaleGetIdentifier(loc)); // "sv_SE", "en_US@rg=sezzzz"
        CFRelease(loc);
    }
#else
    for (const char *var : {"LC_ALL", "LC_TIME", "LANG"}) {
        const char *v = std::getenv(var);
        if (v && *v) {
            tag = v; // "sv_SE.UTF-8", "de_DE@euro"
            break;
        }
    }
    tag = tag.substr(0, tag.find('.'));
#endif
    tag = tag.substr(0, tag.find('@'));
    for (char &c : tag)
        if (c == '_')
            c = '-';
    if (tag.empty() || tag == "C" || tag == "POSIX")
        return "en-US";
    return tag;
}

namespace detail {

bool osDateNames(const std::string &tag, OsDateNames &out) {
#if defined(_WIN32)
    (void)tag;
    for (int i = 0; i < 12; ++i) {
        out.month[i]      = info(LOCALE_SMONTHNAME1 + i);
        out.monthShort[i] = info(LOCALE_SABBREVMONTHNAME1 + i);
    }
    for (int i = 0; i < 7; ++i) { // Windows counts from Monday
        out.weekday[(i + 1) % 7]      = info(LOCALE_SDAYNAME1 + i);
        out.weekdayShort[(i + 1) % 7] = info(LOCALE_SABBREVDAYNAME1 + i);
    }
    out.am        = info(LOCALE_S1159);
    out.pm        = info(LOCALE_S2359);
    out.shortDate = qtPattern(info(LOCALE_SSHORTDATE));
    // "0" = Monday … "6" = Sunday.
    if (const std::string d = info(LOCALE_IFIRSTDAYOFWEEK);
        d.size() == 1 && d[0] >= '0' && d[0] <= '6')
        out.firstDay = (d[0] - '0' + 1) % 7;
    return !out.month[0].empty() && !out.weekday[0].empty();
#elif defined(__APPLE__)
    (void)tag;
    CFLocaleRef loc = CFLocaleCopyCurrent();
    if (!loc)
        return false;
    CFDateFormatterRef f =
        CFDateFormatterCreate(nullptr, loc, kCFDateFormatterShortStyle, kCFDateFormatterNoStyle);
    CFRelease(loc);
    if (!f)
        return false;
    bool ok = symbols(f, kCFDateFormatterMonthSymbols, out.month, 12);
    ok      = symbols(f, kCFDateFormatterShortMonthSymbols, out.monthShort, 12) && ok;
    ok      = symbols(f, kCFDateFormatterWeekdaySymbols, out.weekday, 7) && ok;
    ok      = symbols(f, kCFDateFormatterShortWeekdaySymbols, out.weekdayShort, 7) && ok;
    out.am  = symbol(f, kCFDateFormatterAMSymbol);
    out.pm  = symbol(f, kCFDateFormatterPMSymbol);
    if (CFStringRef fmt = CFDateFormatterGetFormat(f))
        out.shortDate = qtPattern(fromCFString(fmt));
    CFRelease(f);
    if (CFCalendarRef cal = CFCalendarCopyCurrent()) { // 1 = Sunday
        out.firstDay = int(CFCalendarGetFirstWeekday(cal) - 1) % 7;
        CFRelease(cal);
    }
    return ok;
#else
    const std::string lang = str::asciiLower(std::string_view(tag).substr(0, tag.find('-')));
    bool              ok   = false;
    const char       *p    = kDateLocales;
    while (*p) {
        if (lang == p) {
            const char *f = next(p);
            for (int i = 0; i < 12; ++i, f = next(f))
                out.month[i] = f;
            for (int i = 0; i < 12; ++i, f = next(f))
                out.monthShort[i] = f;
            for (int i = 0; i < 7; ++i, f = next(f))
                out.weekday[i] = f;
            for (int i = 0; i < 7; ++i, f = next(f))
                out.weekdayShort[i] = f;
            out.am        = f;
            f             = next(f);
            out.pm        = f;
            f             = next(f);
            out.shortDate = f;
            out.firstDay  = next(f)[0] - '0';
            ok            = true;
        }
        for (int i = 0; i <= kDateLocaleFields; ++i)
            p = next(p);
    }
    if (!ok && lang == "en")
        out.shortDate = "M/d/yy", out.firstDay = 0; // en-US; a region below may differ
    // The region's own short date and first weekday, where they differ.
    for (p = next(p); *p; p = next(next(next(p))))
        if (str::asciiLower(tag) == str::asciiLower(p)) {
            out.shortDate = next(p);
            out.firstDay  = next(next(p))[0] - '0';
        }
    return ok;
#endif
}

} // namespace detail

} // namespace base

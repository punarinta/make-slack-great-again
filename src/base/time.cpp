#include "base/time.h"

#include "base/i18n.h"
#include "base/locale_names.h"
#include "base/str.h"

#include <cstdio>
#include <ctime>

#if defined(MSGA_PORT)
#include "port/port.h"
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace base {

namespace {
bool g_use24h = false;

#ifndef MSGA_PORT // a port's localTime needs no tm
CivilTime fromTm(const std::tm &tm, int offset) {
    CivilTime c;
    c.year      = tm.tm_year + 1900;
    c.month     = tm.tm_mon + 1;
    c.day       = tm.tm_mday;
    c.hour      = tm.tm_hour;
    c.minute    = tm.tm_min;
    c.second    = tm.tm_sec;
    c.weekday   = tm.tm_wday;
    c.utcOffset = offset;
    return c;
}
#endif
} // namespace

int64_t nowMicros() {
#if defined(MSGA_PORT)
    return port_time_wall_us();
#elif defined(_WIN32)
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t t100ns = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t100ns / 10 - 11644473600000000LL; // 1601 → 1970
#else
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
#endif
}

int64_t nowSecs() {
    return nowMicros() / 1000000;
}

int64_t monotonicMs() {
#if defined(MSGA_PORT)
    return port_time_monotonic_ns() / 1000000;
#elif defined(_WIN32)
    return int64_t(GetTickCount64());
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
#endif
}

int64_t monotonicUs() {
#if defined(MSGA_PORT)
    return port_time_monotonic_ns() / 1000;
#elif defined(_WIN32)
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return int64_t(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return int64_t(c.QuadPart / freq * 1000000 + c.QuadPart % freq * 1000000 / freq);
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
#endif
}

#ifdef MSGA_PORT
// The port knows the zone's offset at an instant; the rest is the proleptic
// Gregorian arithmetic below, so no C library time zone code (and no tm).
CivilTime localTime(int64_t secs) {
    const int offset = port_time_local_offset(secs);
    CivilTime c      = utcTime(secs + offset);
    c.utcOffset      = offset;
    return c;
}
#else
CivilTime localTime(int64_t secs) {
    const std::time_t t = std::time_t(secs);
    std::tm           tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
    std::tm       copy  = tm;
    const int64_t asUtc = int64_t(_mkgmtime(&copy));
    return fromTm(tm, int(asUtc - secs));
#else
    localtime_r(&t, &tm);
    return fromTm(tm, int(tm.tm_gmtoff));
#endif
}
#endif

CivilTime utcTime(int64_t secs) {
    CivilTime c;
    int64_t   days = secs / 86400, rem = secs % 86400;
    if (rem < 0)
        rem += 86400, --days;
    civilFromDays(days, &c.year, &c.month, &c.day);
    c.hour    = int(rem / 3600);
    c.minute  = int(rem / 60 % 60);
    c.second  = int(rem % 60);
    c.weekday = int(((days % 7) + 11) % 7); // 1970-01-01 was a Thursday
    return c;
}

int64_t fromLocal(int year, int month, int day, int hour, int minute, int second) {
#ifdef MSGA_PORT
    // Out-of-range fields normalise as mktime's do: months into years here,
    // days, hours, minutes and seconds by plain addition.
    int64_t       m = int64_t(month) - 1;
    const int64_t y = year + (m >= 0 ? m / 12 : (m - 11) / 12);
    m -= (y - year) * 12;
    const int64_t local = (daysFromCivil(int(y), int(m + 1), 1) + day - 1) * 86400 +
                          int64_t(hour) * 3600 + int64_t(minute) * 60 + second;
    // The offset in effect at the answer, not at the wall-clock time read as
    // UTC: a second round settles it (inside a DST gap it lands on the later
    // offset, as mktime with tm_isdst = -1 does).
    const int64_t guess = local - port_time_local_offset(local);
    return local - port_time_local_offset(guess);
#else
    std::tm tm{};
    tm.tm_year  = year - 1900;
    tm.tm_mon   = month - 1;
    tm.tm_mday  = day;
    tm.tm_hour  = hour;
    tm.tm_min   = minute;
    tm.tm_sec   = second;
    tm.tm_isdst = -1; // let the OS decide whether DST applies on that date
    return int64_t(std::mktime(&tm));
#endif
}

// Howard Hinnant's days_from_civil / civil_from_days.
int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = unsigned((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

void civilFromDays(int64_t z, int *year, int *month, int *day) {
    z += 719468;
    const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp  = (5 * doy + 2) / 153;
    const unsigned d   = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m   = mp < 10 ? mp + 3 : mp - 9;
    *year              = int(int64_t(yoe) + era * 400 + (m <= 2));
    *month             = int(m);
    *day               = int(d);
}

int64_t localDay(int64_t secs) {
    const CivilTime c = localTime(secs);
    return daysFromCivil(c.year, c.month, c.day);
}

void setUse24h(bool on) {
    g_use24h = on;
}

bool use24h() {
    return g_use24h;
}

// ── Date language ───────────────────────────────────────────────────────────

namespace {

const char *const kEnMonths[] = {
    "January",
    "February",
    "March",
    "April",
    "May",
    "June",
    "July",
    "August",
    "September",
    "October",
    "November",
    "December",
};
const char *const kEnMonthsShort[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
const char *const kEnWeekdays[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
const char *const kEnWeekdaysShort[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
// Japanese (CLDR): months are numbers with 月.
const char *const kJaWeekdays[]      = {
    "\xE6\x97\xA5\xE6\x9B\x9C\xE6\x97\xA5",
    "\xE6\x9C\x88\xE6\x9B\x9C\xE6\x97\xA5",
    "\xE7\x81\xAB\xE6\x9B\x9C\xE6\x97\xA5",
    "\xE6\xB0\xB4\xE6\x9B\x9C\xE6\x97\xA5",
    "\xE6\x9C\xA8\xE6\x9B\x9C\xE6\x97\xA5",
    "\xE9\x87\x91\xE6\x9B\x9C\xE6\x97\xA5",
    "\xE5\x9C\x9F\xE6\x9B\x9C\xE6\x97\xA5",
};

// What dates are written with now: the English tables until set.
struct DateNames {
    std::string month[12], monthShort[12], weekday[7], weekdayShort[7], am, pm, shortDate;
    int         firstDay = 0;    // 0 = Sunday
    std::string language = "en"; // "en", "ja" or the OS's
    bool        ja       = false;
};

void setEnglish(DateNames &n) {
    for (int i = 0; i < 12; ++i)
        n.month[i] = kEnMonths[i], n.monthShort[i] = kEnMonthsShort[i];
    for (int i = 0; i < 7; ++i)
        n.weekday[i] = kEnWeekdays[i], n.weekdayShort[i] = kEnWeekdaysShort[i];
    n.am = "AM", n.pm = "PM", n.shortDate = "M/d/yy", n.language = "en", n.ja = false;
    n.firstDay = 0;
}

DateNames &names() {
    static DateNames n = [] {
        DateNames e;
        setEnglish(e);
        return e;
    }();
    return n;
}

void setJapanese(DateNames &n) {
    for (int i = 0; i < 12; ++i)
        n.month[i] = n.monthShort[i] = str::number(i + 1) + "\xE6\x9C\x88";
    for (int i = 0; i < 7; ++i) {
        n.weekday[i]      = kJaWeekdays[i];
        n.weekdayShort[i] = n.weekday[i].substr(0, 3); // the first character
    }
    n.am        = "\xE5\x8D\x88\xE5\x89\x8D"; // 午前
    n.pm        = "\xE5\x8D\x88\xE5\xBE\x8C"; // 午後
    n.shortDate = "yyyy/MM/dd";
    n.language  = "ja";
    n.ja        = true;
}

} // namespace

void setDateLanguage(std::string_view setting) {
    DateNames &n = names();
    setEnglish(n);
    if (setting == "ja")
        setJapanese(n);
    if (setting != "system")
        return;
    const std::string   tag  = osLocale();
    const std::string   lang = osLanguage();
    detail::OsDateNames os;
    const bool          have = detail::osDateNames(tag, os);
    if (lang == "ja") {
        setJapanese(n);
    } else if (have) { // else English
        for (int i = 0; i < 12; ++i)
            n.month[i] = std::move(os.month[i]), n.monthShort[i] = std::move(os.monthShort[i]);
        for (int i = 0; i < 7; ++i)
            n.weekday[i]      = std::move(os.weekday[i]),
            n.weekdayShort[i] = std::move(os.weekdayShort[i]);
        if (!os.am.empty() && !os.pm.empty()) // some locales have none: keep AM/PM
            n.am = std::move(os.am), n.pm = std::move(os.pm);
    }
    n.language = lang;
    // The region's own short date ("en-GB": dd/MM/yyyy).
    if (!os.shortDate.empty())
        n.shortDate = std::move(os.shortDate);
    if (os.firstDay >= 0 && os.firstDay <= 6)
        n.firstDay = os.firstDay;
}

const char *dateLanguage() {
    return names().language.c_str();
}

std::string osLanguage() {
    const std::string tag = osLocale();
    return str::asciiLower(std::string_view(tag).substr(0, tag.find('-')));
}

const char *monthName(int m) {
    return (m >= 1 && m <= 12) ? names().month[m - 1].c_str() : "";
}

const char *monthShortName(int m) {
    return (m >= 1 && m <= 12) ? names().monthShort[m - 1].c_str() : "";
}

const char *weekdayName(int wd) {
    return (wd >= 0 && wd <= 6) ? names().weekday[wd].c_str() : "";
}

const char *weekdayShortName(int wd) {
    return (wd >= 0 && wd <= 6) ? names().weekdayShort[wd].c_str() : "";
}

const char *dayPeriodName(int hour) {
    return hour < 12 ? names().am.c_str() : names().pm.c_str();
}

const char *shortDatePattern() {
    return names().shortDate.c_str();
}

int firstDayOfWeek() {
    return names().firstDay;
}

std::string formatMonthYear(int year, int month) {
    CivilTime c;
    c.year  = year;
    c.month = month;
    return formatCivil(c, names().ja ? "yyyy\xE5\xB9\xB4M\xE6\x9C\x88" : "MMMM yyyy");
}

std::string formatCivil(const CivilTime &c, std::string_view pattern) {
    std::string out;
    char        buf[16];
    auto        num = [&](int v, size_t width) {
        std::snprintf(buf, sizeof buf, width == 2 ? "%02d" : "%d", v);
        out += buf;
    };
    for (size_t i = 0; i < pattern.size();) {
        const char ch = pattern[i];
        if (ch == '\'') { // quoted literal text; '' is a quote
            size_t j = i + 1;
            if (j < pattern.size() && pattern[j] == '\'')
                out += '\'';
            else
                for (; j < pattern.size(); ++j) {
                    if (pattern[j] == '\'' && (j + 1 >= pattern.size() || pattern[j + 1] != '\''))
                        break;
                    out += pattern[j];
                    j += pattern[j] == '\'';
                }
            i = j + 1;
            continue;
        }
        if (ch == 'A' && i + 1 < pattern.size() && pattern[i + 1] == 'P') {
            out += dayPeriodName(c.hour);
            i += 2;
            continue;
        }
        size_t n = 1;
        while (i + n < pattern.size() && pattern[i + n] == ch)
            ++n;
        if (ch == 'y' && n == 4)
            num(c.year, 1);
        else if (ch == 'M' && n >= 4)
            out += monthName(c.month);
        else if (ch == 'M' && n == 3)
            out += monthShortName(c.month);
        else if (ch == 'M')
            num(c.month, n);
        else if (ch == 'd' && n == 4)
            out += weekdayName(c.weekday);
        else if (ch == 'd' && n <= 2)
            num(c.day, n);
        else if (ch == 'H' && n <= 2)
            num(c.hour, n);
        else if (ch == 'h' && n <= 2)
            num(c.hour % 12 == 0 ? 12 : c.hour % 12, n);
        else if (ch == 'm' && n <= 2)
            num(c.minute, n);
        else if (ch == 's' && n <= 2)
            num(c.second, n);
        else
            out.append(pattern.substr(i, n));
        i += n;
    }
    return out;
}

static const char *timePattern(bool withSecs) {
    // 24-hour: a two-digit hour ("09:05"); 12-hour: none ("9:05 AM"), with
    // the day period where the language puts it (Japanese: "午後2:34";
    // with seconds, it still comes after the time).
    if (g_use24h)
        return withSecs ? "HH:mm:ss" : "HH:mm";
    if (withSecs)
        return "h:mm:ss AP";
    return names().ja ? "APh:mm" : "h:mm AP";
}

std::string formatTime(int64_t secs) {
    return formatCivil(localTime(secs), timePattern(false));
}

std::string formatTimeSecs(int64_t secs) {
    return formatCivil(localTime(secs), timePattern(true));
}

// formatDate of c, with `now` already converted: the labels below convert
// each time once.
static std::string formatDateOf(const CivilTime &c, const CivilTime &now) {
    const bool year = c.year != now.year;
    if (names().ja) // 2026年3月15日 / 3月15日
        return formatCivil(
            c,
            year ? "yyyy\xE5\xB9\xB4M\xE6\x9C\x88"
                   "d\xE6\x97\xA5"
                 : "M\xE6\x9C\x88"
                   "d\xE6\x97\xA5"
        );
    return formatCivil(c, year ? "MMMM d, yyyy" : "MMMM d");
}

std::string formatDate(int64_t secs, int64_t now) {
    return formatDateOf(localTime(secs), localTime(now));
}

std::string formatDateTime(int64_t secs) {
    // "Mar 15, 2:34 PM" / "3月15日 午後2:34".
    const CivilTime c = localTime(secs);
    return formatCivil(
               c,
               names().ja ? "M\xE6\x9C\x88"
                            "d\xE6\x97\xA5 "
                          : "MMM d, "
           ) +
           formatCivil(c, timePattern(false));
}

std::string isoDate(int64_t secs) {
    const CivilTime c = localTime(secs);
    char            buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", c.year, c.month, c.day);
    return buf;
}

namespace {
// `n` digits at s[*i] as a number, advancing past them; -1 when they aren't
// all digits.
int isoDigits(std::string_view s, size_t *i, int n) {
    if (*i + size_t(n) > s.size())
        return -1;
    int v = 0;
    for (int k = 0; k < n; ++k) {
        const char c = s[*i + size_t(k)];
        if (c < '0' || c > '9')
            return -1;
        v = v * 10 + (c - '0');
    }
    *i += size_t(n);
    return v;
}

bool isoExpect(std::string_view s, size_t *i, char c) {
    if (*i >= s.size() || s[*i] != c)
        return false;
    ++*i;
    return true;
}
} // namespace

int64_t parseIsoMicros(std::string_view s) {
    // YYYY-MM-DDTHH:MM[:SS[.f…]][Z|±HH:MM|±HHMM]; no zone = local time.
    size_t    i  = 0;
    const int y  = isoDigits(s, &i, 4);
    const int mo = isoExpect(s, &i, '-') ? isoDigits(s, &i, 2) : -1;
    const int d  = isoExpect(s, &i, '-') ? isoDigits(s, &i, 2) : -1;
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31)
        return 0;
    if (i >= s.size() || (s[i] != 'T' && s[i] != ' '))
        return 0;
    ++i;
    const int h   = isoDigits(s, &i, 2);
    const int mi  = isoExpect(s, &i, ':') ? isoDigits(s, &i, 2) : -1;
    int       sec = 0;
    if (isoExpect(s, &i, ':'))
        sec = isoDigits(s, &i, 2);
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60)
        return 0;
    int64_t frac = 0;
    if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
        ++i;
        int digits = 0;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, ++digits)
            if (digits < 6) // finer than micros is dropped
                frac = frac * 10 + (s[i] - '0');
        if (digits == 0)
            return 0;
        for (; digits < 6; ++digits)
            frac *= 10;
    }
    int64_t secs = 0;
    if (i == s.size()) {
        secs = fromLocal(y, mo, d, h, mi, sec);
    } else {
        int offset = 0;
        if (s[i] == 'Z' || s[i] == 'z') {
            ++i;
        } else if (s[i] == '+' || s[i] == '-') {
            const int sign = s[i] == '-' ? -1 : 1;
            ++i;
            const int oh = isoDigits(s, &i, 2);
            isoExpect(s, &i, ':');
            const int om = isoDigits(s, &i, 2);
            if (oh < 0 || oh > 23 || om < 0 || om > 59)
                return 0;
            offset = sign * (oh * 3600 + om * 60);
        } else {
            return 0;
        }
        if (i != s.size())
            return 0;
        secs = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - offset;
    }
    return secs * 1000000 + frac;
}

static int64_t dayOf(const CivilTime &c) {
    return daysFromCivil(c.year, c.month, c.day);
}

std::string dayLabel(int64_t secs, int64_t now) {
    const CivilTime c = localTime(secs), n = localTime(now);
    const int64_t   d = dayOf(n) - dayOf(c);
    if (d == 0)
        return i18n::tr("Today");
    if (d == 1)
        return i18n::tr("Yesterday");
    return formatDateOf(c, n);
}

std::string lastReplyLabel(int64_t secs, int64_t now) {
    const CivilTime   c = localTime(secs), n = localTime(now);
    const std::string time = formatCivil(c, timePattern(false));
    const int64_t     d    = dayOf(n) - dayOf(c);
    if (d == 0)
        return i18n::arg(i18n::tr("today at %1"), time);
    if (d == 1)
        return i18n::arg(i18n::tr("yesterday at %1"), time);
    return i18n::arg(i18n::tr("%1 at %2"), formatDateOf(c, n), time);
}

std::string dateTimeLabel(int64_t secs, int64_t now) {
    const CivilTime   c = localTime(secs), n = localTime(now);
    const std::string time = formatCivil(c, timePattern(false));
    const int64_t     d    = dayOf(n) - dayOf(c);
    if (d <= 0)
        return time;
    if (d == 1)
        return i18n::arg(i18n::tr("yesterday at %1"), time);
    if (d < 7)
        return i18n::arg(i18n::tr("%1 at %2"), weekdayName(c.weekday), time);
    return i18n::arg(i18n::tr("%1 at %2"), formatDateOf(c, n), time);
}

std::string relativeTime(int64_t secs, int64_t now) {
    const int64_t age = now - secs;
    if (age < 60)
        return i18n::tr("just now");
    if (age < 3600)
        return i18n::trn("%n minute ago", "%n minutes ago", age / 60);
    if (age < 86400)
        return i18n::trn("%n hour ago", "%n hours ago", age / 3600);
    if (age < 86400 * 30)
        return i18n::trn("%n day ago", "%n days ago", age / 86400);
    if (age < 86400 * 365)
        return i18n::trn("%n month ago", "%n months ago", age / (86400 * 30));
    return i18n::trn("%n year ago", "%n years ago", age / (86400 * 365));
}

} // namespace base

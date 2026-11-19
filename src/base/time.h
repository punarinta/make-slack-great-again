// Time: UTC clock, local civil time through the OS time-zone database
// (localtime_r / mktime; the Windows CRT equivalents), and the user-facing
// formats the UI shows. Month, weekday and AM/PM names and the date patterns
// follow the date language (setDateLanguage), not the translation table, so
// a language change reformats dates at once; the words
// around them ("Today", "%1 at %2") are UI text and go through i18n::tr.
//
// Seconds are Unix epoch seconds; micros are epoch microseconds (the unit of
// Slack message timestamps, see model).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace base {

int64_t nowMicros(); // UTC
int64_t nowSecs();
// Monotonic milliseconds (timeouts, animation): never jumps with the wall clock.
int64_t monotonicMs();
// The same clock in microseconds (frame timing, animation).
int64_t monotonicUs();

struct CivilTime {
    int year = 1970, month = 1, day = 1; // month 1-12
    int hour = 0, minute = 0, second = 0;
    int weekday   = 4; // 0 = Sunday … 6 = Saturday
    int utcOffset = 0; // seconds east of UTC in effect at that instant
};

CivilTime localTime(int64_t secs);
CivilTime utcTime(int64_t secs);
// Epoch seconds of a local wall-clock time. Out-of-range fields normalise
// (day 0 = last day of the previous month, hour 24 = next day), so callers
// can do date arithmetic by adding to fields. DST gaps resolve as the OS does.
int64_t   fromLocal(int year, int month, int day, int hour = 0, int minute = 0, int second = 0);

// Days since 1970-01-01 for a civil date (proleptic Gregorian), and back.
int64_t daysFromCivil(int year, int month, int day);
void    civilFromDays(int64_t days, int *year, int *month, int *day);
// The local calendar day of an instant, as daysFromCivil of its local date:
// equal numbers mean "same day here", difference 1 means "yesterday".
int64_t localDay(int64_t secs);

// ── Preferences ─────────────────────────────────────────────────────────────
// 24-hour clock ("14:34") vs 12-hour ("2:34 PM"). Default 12-hour, the
// English default; the settings screen persists and re-applies it.
void setUse24h(bool on);
bool use24h();

// ── Date language ───────────────────────────────────────────────────────────
// The Appearance → Language setting: "en" (US English names and patterns),
// "ja" (Japanese), or "system": the OS's regional format — its month and
// weekday names, AM/PM and short date, with the English patterns (the
// Japanese ones when the OS language is Japanese). Until set: "en". Not thread-safe (UI thread).
void        setDateLanguage(std::string_view setting);
// The language dates are in: "en", "ja", or the OS's ("sv", "de" …).
const char *dateLanguage();
// The OS's regional-format locale as a BCP 47 tag ("sv-SE"; "en-US" when the
// OS says nothing usable: the C locale), and its language subtag ("sv"):
// Windows GetUserDefaultLocaleName, macOS CFLocale, elsewhere LC_ALL /
// LC_TIME / LANG.
std::string osLocale();
std::string osLanguage();

// ── Names (the date language) ───────────────────────────────────────────────
const char *monthName(int month);          // 1-12: "March"
const char *monthShortName(int month);     // "Mar"
const char *weekdayName(int weekday);      // 0 = Sunday: "Sunday"
const char *weekdayShortName(int weekday); // "Sun"
const char *dayPeriodName(int hour);       // 0-23: "AM" / "PM"
// The numeric short date as a formatCivil pattern ("M/d/yy", "dd.MM.yyyy",
// "yyyy/MM/dd"): what a date input field shows.
const char *shortDatePattern();
// The week's first day, 0 = Sunday … 6 = Saturday (the calendar popup):
// Sunday for "en" and "ja".
int         firstDayOfWeek();
// A month heading: "March 2026" / "2026年3月" (the calendar popup).
std::string formatMonthYear(int year, int month);

// ── UI formats (all local time) ─────────────────────────────────────────────
// A date/time by pattern, so a translation can reorder and dress
// it ("MMMM d, yyyy" -> "yyyy年M月d日"): yyyy, M/MM (number), MMM (short
// name), MMMM (name), d/dd, dddd (weekday name), H/HH (24-hour), h/hh
// (12-hour), mm, ss, AP (AM/PM, translated); '...' is literal text, '' a quote.
std::string formatCivil(const CivilTime &c, std::string_view pattern);
std::string formatTime(int64_t secs);     // "2:34 PM" / "14:34"
std::string formatTimeSecs(int64_t secs); // "2:34:05 PM" / "14:34:05"
// "March 15"; "March 15, 2025" when not in the current year of `now`.
std::string formatDate(int64_t secs, int64_t now);
std::string formatDateTime(int64_t secs); // "Mar 15, 2:34 PM" (no year)
std::string isoDate(int64_t secs);        // "2026-03-15"
// "2026-09-25T10:26:03.123Z" / "…+02:00" / no fraction → epoch micros; 0 when
// malformed (Claude Code's record timestamps).
int64_t     parseIsoMicros(std::string_view iso);
// Date separators in the message list: "Today", "Yesterday", else formatDate.
std::string dayLabel(int64_t secs, int64_t now);
// Thread footers: "today at 2:34 PM", "yesterday at …", "March 15 at …".
std::string lastReplyLabel(int64_t secs, int64_t now);
// Hover/tooltips: time only today, "yesterday at …", "Friday at …" within the
// week, else "March 15 at …" — the official client's rule.
std::string dateTimeLabel(int64_t secs, int64_t now);
// "just now", "5 minutes ago", "3 days ago", "2 months ago", "1 year ago".
std::string relativeTime(int64_t secs, int64_t now);

} // namespace base

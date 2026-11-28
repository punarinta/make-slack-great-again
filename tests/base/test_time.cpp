#include "support/test.h"
#include "base/time.h"

// ctest sets TZ=Europe/Stockholm; running the binary by hand uses the host
// zone. Every expectation is built from local wall-clock times, so it passes
// in any zone.
namespace {
const int64_t kAfternoon = base::fromLocal(2026, 3, 15, 14, 34);
const int64_t kMorning   = base::fromLocal(2026, 3, 15, 9, 5);
} // namespace

TEST("time: formatTime 12-hour and 24-hour") {
    base::setUse24h(false);
    CHECK_STR(base::formatTime(kAfternoon), "2:34 PM");
    CHECK_STR(base::formatTime(kMorning), "9:05 AM");
    CHECK_STR(base::formatTime(base::fromLocal(2026, 3, 15, 0, 7)), "12:07 AM");
    CHECK_STR(base::formatTime(base::fromLocal(2026, 3, 15, 12, 0)), "12:00 PM");
    CHECK_STR(base::formatTimeSecs(kAfternoon + 5), "2:34:05 PM");
    base::setUse24h(true);
    CHECK_STR(base::formatTime(kAfternoon), "14:34");
    CHECK_STR(base::formatTime(kMorning), "09:05");
    CHECK_STR(base::formatTimeSecs(kMorning + 9), "09:05:09");
    base::setUse24h(false);
}

TEST("time: formatDate includes the year only outside the current one") {
    const int64_t now = base::fromLocal(2026, 9, 30, 12, 0);
    CHECK_STR(base::formatDate(kAfternoon, now), "March 15");
    CHECK_STR(base::formatDate(base::fromLocal(2020, 3, 15, 12, 0), now), "March 15, 2020");
    CHECK_STR(base::formatDateTime(kAfternoon), "Mar 15, 2:34 PM");
    base::setUse24h(true);
    CHECK_STR(base::formatDateTime(kAfternoon), "Mar 15, 14:34");
    base::setUse24h(false);
    CHECK_STR(base::isoDate(kAfternoon), "2026-03-15");
}

TEST("time: Today / Yesterday separators and reply labels") {
    const int64_t now = base::fromLocal(2026, 3, 15, 16, 0);
    CHECK_STR(base::dayLabel(kMorning, now), "Today");
    CHECK_STR(base::dayLabel(base::fromLocal(2026, 3, 14, 23, 59), now), "Yesterday");
    CHECK_STR(base::dayLabel(base::fromLocal(2026, 3, 13, 10, 0), now), "March 13");
    // Midnight boundary: 23:59 yesterday seen from 00:01 today.
    const int64_t justAfterMidnight = base::fromLocal(2026, 3, 16, 0, 1);
    CHECK_STR(base::dayLabel(base::fromLocal(2026, 3, 15, 23, 59), justAfterMidnight), "Yesterday");
    CHECK_STR(base::lastReplyLabel(kAfternoon, now), "today at 2:34 PM");
    CHECK_STR(
        base::lastReplyLabel(base::fromLocal(2026, 3, 14, 14, 34), now), "yesterday at 2:34 PM"
    );
    CHECK_STR(base::lastReplyLabel(base::fromLocal(2026, 3, 1, 8, 0), now), "March 1 at 8:00 AM");
    CHECK_STR(base::dateTimeLabel(kAfternoon, now), "2:34 PM");
    // 2026-03-12 is a Thursday.
    CHECK_STR(base::dateTimeLabel(base::fromLocal(2026, 3, 12, 9, 5), now), "Thursday at 9:05 AM");
    CHECK_STR(base::dateTimeLabel(base::fromLocal(2026, 3, 1, 9, 5), now), "March 1 at 9:05 AM");
}

TEST("time: relative time") {
    const int64_t now = 1'800'000'000;
    CHECK_STR(base::relativeTime(now - 5, now), "just now");
    CHECK_STR(base::relativeTime(now - 60, now), "1 minute ago");
    CHECK_STR(base::relativeTime(now - 5 * 60, now), "5 minutes ago");
    CHECK_STR(base::relativeTime(now - 3 * 3600, now), "3 hours ago");
    CHECK_STR(base::relativeTime(now - 86400, now), "1 day ago");
    CHECK_STR(base::relativeTime(now - 65 * 86400, now), "2 months ago");
    CHECK_STR(base::relativeTime(now - 400 * 86400, now), "1 year ago");
}

TEST("time: civil date arithmetic") {
    CHECK(base::daysFromCivil(1970, 1, 1) == 0);
    CHECK(base::daysFromCivil(2000, 3, 1) == 11017);
    int y, m, d;
    base::civilFromDays(base::daysFromCivil(2024, 2, 29), &y, &m, &d);
    CHECK((y == 2024 && m == 2 && d == 29));
    const auto u = base::utcTime(1758445200); // 2025-09-21 09:00:00 UTC, a Sunday
    CHECK((u.year == 2025 && u.month == 9 && u.day == 21 && u.hour == 9 && u.weekday == 0));
    // fromLocal normalises out-of-range fields: day 0 is the last of the previous month.
    const auto l = base::localTime(base::fromLocal(2026, 3, 0, 10, 0));
    CHECK((l.month == 2 && l.day == 28 && l.hour == 10));
    CHECK(base::localTime(kAfternoon).weekday == 0); // 2026-03-15 is a Sunday
    CHECK(base::nowMicros() > 1'700'000'000'000'000LL);
    const int64_t a = base::monotonicMs();
    CHECK(base::monotonicMs() >= a);
}

TEST("time: formatCivil patterns") {
    const base::CivilTime c = base::localTime(base::fromLocal(2026, 3, 5, 14, 4, 9));
    CHECK_STR(base::formatCivil(c, "yyyy-MM-dd HH:mm:ss"), "2026-03-05 14:04:09");
    CHECK_STR(base::formatCivil(c, "dddd, MMMM d"), "Thursday, March 5");
    CHECK_STR(base::formatCivil(c, "MMM d, h:mm AP"), "Mar 5, 2:04 PM");
    CHECK_STR(base::formatCivil(c, "yyyy年M月d日 APh:mm"), "2026年3月5日 PM2:04");
    CHECK_STR(base::formatCivil(c, "'day' d 'o''clock' ''"), "day 5 o'clock '");
}

TEST("time: the date language — en, ja, and the OS's regional format") {
    const int64_t now = base::fromLocal(2026, 9, 30, 12, 0);
    base::setUse24h(false);
    base::setDateLanguage("ja");
    CHECK_STR(base::dateLanguage(), "ja");
    CHECK_STR(base::formatDate(kAfternoon, now), "3月15日");
    CHECK_STR(base::formatTime(kAfternoon), "午後2:34");
    CHECK_STR(base::weekdayName(0), "日曜日");
    CHECK_STR(base::weekdayShortName(1), "月");
    CHECK_STR(base::shortDatePattern(), "yyyy/MM/dd");
    CHECK_STR(base::formatMonthYear(2026, 3), "2026年3月");
    CHECK(base::firstDayOfWeek() == 0);
    base::setDateLanguage("en");
    CHECK_STR(base::formatDate(kAfternoon, now), "March 15");
    CHECK_STR(base::shortDatePattern(), "M/d/yy");
    CHECK_STR(base::formatMonthYear(2026, 3), "March 2026");
#if !defined(_WIN32) && !defined(__APPLE__)
    // Linux reads the regional format from the environment.
    base::test::setEnv("LC_ALL", "sv_SE.UTF-8");
    CHECK_STR(base::osLocale(), "sv-SE");
    CHECK_STR(base::osLanguage(), "sv");
    base::setDateLanguage("system");
    CHECK_STR(base::dateLanguage(), "sv");
    // English patterns, Swedish names (the OS's regional format).
    CHECK_STR(base::formatDate(kAfternoon, now), "mars 15");
    CHECK_STR(base::formatTime(kAfternoon), "2:34 em");
    CHECK_STR(base::weekdayName(0), "söndag");
    CHECK_STR(base::shortDatePattern(), "yyyy-MM-dd");
    CHECK(base::firstDayOfWeek() == 1); // Monday
    base::test::setEnv("LC_ALL", "en_GB.UTF-8");
    base::setDateLanguage("system");
    CHECK_STR(base::dateLanguage(), "en");
    CHECK_STR(base::formatDate(kAfternoon, now), "March 15");
    CHECK_STR(base::shortDatePattern(), "dd/MM/yyyy");
    CHECK(base::firstDayOfWeek() == 1);
    base::test::setEnv("LC_ALL", "en_US.UTF-8");
    base::setDateLanguage("system");
    CHECK(base::firstDayOfWeek() == 0);
    base::test::setEnv("LC_ALL", "de_DE.UTF-8");
    base::setDateLanguage("system");
    CHECK(base::firstDayOfWeek() == 1);
    base::test::setEnv("LC_ALL", "pt_BR.UTF-8");
    base::setDateLanguage("system");
    CHECK(base::firstDayOfWeek() == 0);
    base::test::setEnv("LC_ALL", "de_CH");
    base::setDateLanguage("system");
    CHECK_STR(base::monthName(3), "März");
    CHECK_STR(base::shortDatePattern(), "dd.MM.yy");
    base::test::setEnv("LC_ALL", "ja_JP.UTF-8");
    base::setDateLanguage("system");
    CHECK_STR(base::formatDate(kAfternoon, now), "3月15日");
    base::test::setEnv("LC_ALL", "C");
    CHECK_STR(base::osLocale(), "en-US");
    base::test::setEnv("LC_ALL", "xx_YY");
    base::setDateLanguage("system"); // unknown: English names
    CHECK_STR(base::monthName(1), "January");
    base::test::unsetEnv("LC_ALL");
#endif
    base::setDateLanguage("en");
}

TEST("time: parseIsoMicros") {
    // 2026-09-25T10:26:03Z = 1790331963.
    CHECK(base::parseIsoMicros("2026-09-25T10:26:03.123Z") == 1790331963123000);
    CHECK(base::parseIsoMicros("2026-09-25T10:26:03Z") == 1790331963000000);
    CHECK(base::parseIsoMicros("2026-09-25T12:26:03.5+02:00") == 1790331963500000);
    CHECK(base::parseIsoMicros("2026-09-25T05:26:03-0500") == 1790331963000000);
    CHECK(base::parseIsoMicros("2026-09-25T10:26:03.1234567Z") == 1790331963123456);
    CHECK(base::parseIsoMicros("1970-01-01T00:00:00Z") == 0);
    // No zone: local wall-clock time.
    CHECK(base::parseIsoMicros("2026-03-15T14:34:00") == kAfternoon * 1000000);
    CHECK(base::parseIsoMicros("") == 0);
    CHECK(base::parseIsoMicros("2026-09-25") == 0);
    CHECK(base::parseIsoMicros("2026-13-25T10:26:03Z") == 0);
    CHECK(base::parseIsoMicros("2026-09-25T10:26:03.Z") == 0);
    CHECK(base::parseIsoMicros("2026-09-25T10:26:03Zjunk") == 0);
    CHECK(base::parseIsoMicros("garbage") == 0);
}

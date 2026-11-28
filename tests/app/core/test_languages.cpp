#include "app/i18n/languages.h"
#include "base/i18n.h"
#include "support/test.h"
#include "base/time.h"

TEST("languages: the compiled-in Japanese table unpacks and translates") {
    app_i18n::registerLanguages();
    REQUIRE(i18n::setLanguage("ja"));
    CHECK_STR(i18n::currentCode(), "ja");
    CHECK_STR(i18n::tr("Today"), "今日");
    CHECK_STR(i18n::tr("Not a UI string"), "Not a UI string");
    // Japanese dates (the date language, set apart from the table: a
    // language change shows in dates at once): year-month-day with counters,
    // the day period first.
    base::setDateLanguage("ja");
    const int64_t t   = base::fromLocal(2026, 3, 15, 14, 34);
    const int64_t now = base::fromLocal(2026, 9, 30, 12, 0);
    CHECK_STR(base::formatDate(t, now), "3月15日");
    CHECK_STR(base::formatDate(base::fromLocal(2020, 3, 15, 12, 0), now), "2020年3月15日");
    base::setUse24h(false);
    CHECK_STR(base::formatTime(t), "午後2:34");
    CHECK_STR(base::formatDateTime(t), "3月15日 午後2:34");
    i18n::setPreferredLanguage({"ja_JP.UTF-8"});
    CHECK_STR(i18n::currentCode(), "ja");
    i18n::setLanguage("en");
    CHECK_STR(i18n::tr("Today"), "Today");
    base::setDateLanguage("en");
}

TEST("languages: a corrupt table is refused") {
    CHECK(
        app_i18n::inflate(reinterpret_cast<const unsigned char *>("\x01\x02\x03"), 3, 10).empty()
    );
}

#include "base/i18n.h"
#include "support/test.h"
#include "base/time.h"

#include <algorithm>
#include <cstring>

namespace {
// A three-form language (the East Slavic rule) to prove the plural machinery.
int slavicPlural(int64_t n) {
    if (n % 10 == 1 && n % 100 != 11)
        return 0;
    if (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14))
        return 1;
    return 2;
}

struct Row {
    const char *msgid;
    const char *forms[3];
};
const Row kRows[] = {
    {"%1 at %2", {"%2, %1"}},
    {"%n minute ago", {"%n минуту назад", "%n минуты назад", "%n минут назад"}},
    {"Today", {"Сегодня"}},
    {"Yesterday", {nullptr}}, // untranslated: falls back to English
};

void put32(std::string &out, uint32_t v) {
    char b[4];
    std::memcpy(b, &v, 4);
    out.append(b, 4);
}

// What tools/i18n.py writes (before compressing), built from kRows.
std::string testTable() {
    std::vector<const Row *> rows;
    for (const Row &r : kRows)
        rows.push_back(&r);
    std::sort(rows.begin(), rows.end(), [](const Row *a, const Row *b) {
        return i18n::hash(a->msgid) < i18n::hash(b->msgid);
    });
    std::string out;
    put32(out, uint32_t(rows.size()));
    for (const Row *r : rows)
        put32(out, i18n::hash(r->msgid));
    for (const Row *r : rows)
        for (const char *f : r->forms) {
            if (f)
                out += f;
            out += '\0';
        }
    return out;
}

const i18n::Language kTest{"xx", slavicPlural, 3, testTable};
const i18n::Language kBroken{"yy", slavicPlural, 3, [] { return std::string("\x05", 1); }};
} // namespace

TEST("i18n: FNV-1a matches tools/i18n.py") {
    CHECK(i18n::hash("") == 2166136261u);
    CHECK(i18n::hash("Today") == 0xe7c0775eu);
}

TEST("i18n: English is the identity with an n == 1 plural rule") {
    i18n::setLanguage("en");
    CHECK_STR(i18n::tr("Add channels"), "Add channels");
    CHECK_STR(i18n::trn("%n reply", "%n replies", 1), "1 reply");
    CHECK_STR(i18n::trn("%n reply", "%n replies", 0), "0 replies");
    CHECK_STR(i18n::trn("%n reply", "%n replies", 21), "21 replies");
    CHECK_STR(i18n::arg("%1 at %2", "Friday", "9:05 AM"), "Friday at 9:05 AM");
    CHECK_STR(i18n::arg("%3 · %1 · %2", "a", "b", "c"), "c · a · b");
    CHECK_FALSE(i18n::setLanguage("zz"));
    CHECK_STR(i18n::currentCode(), "en");
}

TEST("i18n: a registered table with three plural forms") {
    i18n::registerLanguage(&kTest);
    REQUIRE(i18n::setLanguage("xx"));
    CHECK_STR(i18n::currentCode(), "xx");
    CHECK_STR(i18n::tr("Today"), "Сегодня");
    CHECK_STR(i18n::tr("Yesterday"), "Yesterday");
    CHECK_STR(i18n::tr("Not in table"), "Not in table");
    CHECK_STR(i18n::trn("%n minute ago", "%n minutes ago", 1), "1 минуту назад");
    CHECK_STR(i18n::trn("%n minute ago", "%n minutes ago", 3), "3 минуты назад");
    CHECK_STR(i18n::trn("%n minute ago", "%n minutes ago", 11), "11 минут назад");
    CHECK_STR(i18n::trn("%n minute ago", "%n minutes ago", 21), "21 минуту назад");
    // Untranslated plurals use the English rule.
    CHECK_STR(i18n::trn("%n hour ago", "%n hours ago", 2), "2 hours ago");
    // Translations can reorder arguments.
    CHECK_STR(i18n::arg(i18n::tr("%1 at %2"), "March 15", "14:34"), "14:34, March 15");
    // The time formatters read their words through the table.
    const int64_t now = base::fromLocal(2026, 3, 15, 16, 0);
    CHECK_STR(base::dayLabel(now - 3600, now), "Сегодня");
    CHECK_STR(base::relativeTime(now - 120, now), "2 минуты назад");
    i18n::setLanguage("en");
    CHECK_STR(base::dayLabel(now - 3600, now), "Today");
}

TEST("i18n: a table that fails to load keeps the current language") {
    i18n::registerLanguage(&kBroken);
    REQUIRE(i18n::setLanguage("xx"));
    CHECK_FALSE(i18n::setLanguage("yy"));
    CHECK_STR(i18n::currentCode(), "xx");
    CHECK_STR(i18n::tr("Today"), "Сегодня");
    i18n::setLanguage("en");
}

TEST("i18n: the OS's preferred languages pick the first one msga has") {
    i18n::registerLanguage(&kTest);
    i18n::setPreferredLanguage({"de-DE", "XX_YY.UTF-8", "en-US"});
    CHECK_STR(i18n::currentCode(), "xx");
    i18n::setPreferredLanguage({"de-DE", "en-US", "xx"});
    CHECK_STR(i18n::currentCode(), "en");
    i18n::setPreferredLanguage({"xx"});
    i18n::setPreferredLanguage({"C"});
    CHECK_STR(i18n::currentCode(), "en");
    i18n::setPreferredLanguage({"xx@euro"});
    CHECK_STR(i18n::currentCode(), "xx");
    i18n::setPreferredLanguage({});
    CHECK_STR(i18n::currentCode(), "en");
}

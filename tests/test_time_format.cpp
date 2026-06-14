// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include "util/time_format.h"

// Redirect QSettings("msga","msga") to a temp dir so tests never touch the
// user's real preferences in ~/.config/msga/msga.conf.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

namespace {

void setPrefs(const QString &lang, bool use24h) {
    QSettings s("msga", "msga");
    s.clear();
    TimeFmt::setLanguage(lang);
    TimeFmt::setUse24h(use24h);
}

// Build timestamps from local wall-clock time so expectations are
// timezone-independent on any test machine.
const QDateTime kAfternoon(QDate(2026, 3, 15), QTime(14, 34));
const QDateTime kMorning(QDate(2026, 3, 15), QTime(9, 5));

} // namespace

// ── formatTime ────────────────────────────────────────────────────────────────

TEST_CASE("formatTime English 12-hour", "[timefmt]") {
    setPrefs("en", false);
    CHECK(TimeFmt::formatTime(kAfternoon) == "2:34 PM");
    CHECK(TimeFmt::formatTime(kMorning) == "9:05 AM");
}

TEST_CASE("formatTime English 24-hour", "[timefmt]") {
    setPrefs("en", true);
    CHECK(TimeFmt::formatTime(kAfternoon) == "14:34");
    CHECK(TimeFmt::formatTime(kMorning) == "09:05");
}

TEST_CASE("formatTime Japanese 12-hour puts day period first", "[timefmt]") {
    setPrefs("ja", false);
    CHECK(TimeFmt::formatTime(kAfternoon) == "午後2:34");
    CHECK(TimeFmt::formatTime(kMorning) == "午前9:05");
}

TEST_CASE("formatTime Japanese 24-hour", "[timefmt]") {
    setPrefs("ja", true);
    CHECK(TimeFmt::formatTime(kAfternoon) == "14:34");
}

TEST_CASE("formatTime unix-seconds overload round-trips local time", "[timefmt]") {
    setPrefs("en", true);
    CHECK(TimeFmt::formatTime(kAfternoon.toSecsSinceEpoch()) == "14:34");
}

// ── formatDate ────────────────────────────────────────────────────────────────

TEST_CASE("formatDate English includes year only when not current", "[timefmt]") {
    setPrefs("en", false);
    const int year = QDate::currentDate().year();
    CHECK(TimeFmt::formatDate(QDate(year, 3, 15)) == "March 15");
    CHECK(TimeFmt::formatDate(QDate(2020, 3, 15)) == "March 15, 2020");
}

TEST_CASE("formatDate Japanese uses year-month-day counters", "[timefmt]") {
    setPrefs("ja", false);
    const int year = QDate::currentDate().year();
    CHECK(TimeFmt::formatDate(QDate(year, 3, 15)) == "3月15日");
    CHECK(TimeFmt::formatDate(QDate(2020, 3, 15)) == "2020年3月15日");
}

// ── formatDateTime ────────────────────────────────────────────────────────────

TEST_CASE("formatDateTime English short form", "[timefmt]") {
    setPrefs("en", false);
    CHECK(TimeFmt::formatDateTime(kAfternoon.toSecsSinceEpoch()) == "Mar 15, 2:34 PM");
    setPrefs("en", true);
    CHECK(TimeFmt::formatDateTime(kAfternoon.toSecsSinceEpoch()) == "Mar 15, 14:34");
}

TEST_CASE("formatDateTime Japanese short form", "[timefmt]") {
    setPrefs("ja", true);
    CHECK(TimeFmt::formatDateTime(kAfternoon.toSecsSinceEpoch()) == "3月15日 14:34");
    setPrefs("ja", false);
    CHECK(TimeFmt::formatDateTime(kAfternoon.toSecsSinceEpoch()) == "3月15日 午後2:34");
}

// ── editFormat ────────────────────────────────────────────────────────────────

TEST_CASE("editFormat patterns per locale and clock", "[timefmt]") {
    setPrefs("en", false);
    CHECK(TimeFmt::editFormat() == "MMM d, yyyy h:mm AP");
    setPrefs("en", true);
    CHECK(TimeFmt::editFormat() == "MMM d, yyyy HH:mm");
    setPrefs("ja", true);
    CHECK(TimeFmt::editFormat() == "yyyy年M月d日 HH:mm");
    setPrefs("ja", false);
    CHECK(TimeFmt::editFormat() == "yyyy年M月d日 APh:mm");
}

// ── Preferences ───────────────────────────────────────────────────────────────

TEST_CASE("preferences persist through reload", "[timefmt]") {
    setPrefs("ja", true);
    TimeFmt::reload();
    CHECK(TimeFmt::language() == "ja");
    CHECK(TimeFmt::use24h());
    CHECK(TimeFmt::locale().language() == QLocale::Japanese);
}

TEST_CASE("clock defaults to 24h for Japanese, 12h for English", "[timefmt]") {
    {
        QSettings s("msga", "msga");
        s.clear();
        s.setValue("appearance/language", "ja");
    }
    TimeFmt::reload();
    CHECK(TimeFmt::use24h());

    {
        QSettings s("msga", "msga");
        s.clear();
        s.setValue("appearance/language", "en");
    }
    TimeFmt::reload();
    CHECK_FALSE(TimeFmt::use24h());
}

TEST_CASE("unknown language falls back to system locale", "[timefmt]") {
    setPrefs("system", false);
    CHECK(TimeFmt::locale().name() == QLocale::system().name());
}

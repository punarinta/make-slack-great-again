// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Central user-facing time/date formatting. All display timestamps go through
// here so the 12h/24h preference and the UI language apply everywhere at once.
#pragma once
#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QString>

namespace TimeFmt {

// ── Preferences (persisted in QSettings under appearance/*) ──────────────
// Cached after first read; setters update both the cache and QSettings.
bool use24h();
void setUse24h(bool on);

// "system" | "en" | "ja". Applying a language change requires an app restart
// (translator is installed once in main()), but date/time patterns follow the
// resolved locale immediately.
QString language();
void    setLanguage(const QString &lang);

// Locale resolved from language() ("system" → QLocale::system()).
QLocale locale();

// Re-read preferences from QSettings (e.g. after an external change).
void reload();

// ── Formatting ────────────────────────────────────────────────────────────
// Qt6's QDateTime::toString(format) renders month/day names and AM/PM in the
// C locale (English) regardless of the default locale, so everything below
// formats through QLocale::toString().

// "2:34 PM" / "14:34" / "午後2:34"
QString formatTime(const QDateTime &dt);
QString formatTime(qint64 unixSecs);

// "March 15" / "March 15, 2025" / "3月15日" / "2025年3月15日"
// The year is included only when the date is outside the current year.
QString formatDate(const QDate &date);

// Short date + time, no year: "Mar 15, 2:34 PM" / "3月15日 14:34"
QString formatDateTime(qint64 unixSecs);

// Display-format pattern for QDateTimeEdit (includes the year):
// "MMM d, yyyy h:mm AP" / "yyyy年M月d日 H:mm". Set locale() on the widget too.
QString editFormat();

} // namespace TimeFmt

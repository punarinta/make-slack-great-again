// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "relative_time.h"
#include <QCoreApplication>
#include <QDateTime>

QString relativeTime(qint64 unixSecs) {
    const qint64 age = QDateTime::currentSecsSinceEpoch() - unixSecs;
    if (age < 60)
        return QCoreApplication::translate("relativeTime", "just now");
    if (age < 3600) {
        const int n = age / 60;
        return QCoreApplication::translate("relativeTime", "%n minute ago", "", n);
    }
    if (age < 86400) {
        const int n = age / 3600;
        return QCoreApplication::translate("relativeTime", "%n hour ago", "", n);
    }
    if (age < 86400 * 30) {
        const int n = age / 86400;
        return QCoreApplication::translate("relativeTime", "%n day ago", "", n);
    }
    if (age < 86400 * 365) {
        const int n = age / (86400 * 30);
        return QCoreApplication::translate("relativeTime", "%n month ago", "", n);
    }
    const int n = age / (86400 * 365);
    return QCoreApplication::translate("relativeTime", "%n year ago", "", n);
}

QString relativeTime(const QString &slackTs) {
    bool ok = false;
    const qint64 secs = static_cast<qint64>(slackTs.toDouble(&ok));
    return ok ? relativeTime(secs) : QString{};
}

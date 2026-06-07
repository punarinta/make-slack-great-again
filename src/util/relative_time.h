// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once
#include <QString>
#include <QtGlobal>

QString relativeTime(qint64 unixSecs);
QString relativeTime(const QString &slackTs); // parses "1234567890.123456"

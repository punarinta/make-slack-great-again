// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Precompiled header of msga_tests (force-included into every test TU).
// Third-party headers only — Catch2, QtCore and the standard library, the ones
// the test files include most. No project headers: those change often (a PCH
// rebuild recompiles every test), and lib_base's `namespace base` would clash
// with test-local names such as test_imap_compose's base().
//
// Measured (2026-09): this set cut the test TUs' compile CPU by about a third.
// Adding the common QtGui/QtWidgets/QtNetwork/QtTest headers was no better and
// sometimes worse — most test TUs don't use them, and each TU pays for loading
// a bigger PCH — so re-measure before growing this list.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

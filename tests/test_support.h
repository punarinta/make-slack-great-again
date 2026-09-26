// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// Setup and helpers several test suites share. Header-only (everything inline),
// so each lives once in msga_tests however many suites include it.
#pragma once

#include "test_main.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QSettings>
#include <QTemporaryDir>

#include <functional>

namespace msga_test {

// A suite's process setup (the body of its MSGA_TEST_MAIN): a QCoreApplication
// named "msga-test" (organization "msga-test"), then the suite's test cases.
inline int runCoreApp(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    return runCatch(argc, argv);
}

// runCoreApp with QSettings (NativeFormat, user scope) redirected to a
// throwaway directory for the run, so the suite never reads or writes the
// user's real ~/.config/msga/msga.conf.
inline int runCoreAppWithTempSettings(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return runCatch(argc, argv);
}

// Pumps the Qt event loop until pred() returns true or timeoutMs elapses;
// returns pred()'s final answer.
inline bool waitFor(std::function<bool()> pred, int timeoutMs = 3000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!pred() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}

} // namespace msga_test

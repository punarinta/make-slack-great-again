// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Every test .cpp is linked into one executable, msga_tests. Each file is a
// "suite" named after it (test_session.cpp -> test_session), and a run always
// selects exactly one:
//
//     msga_tests test_session                 # every test case in the file
//     msga_tests test_theme "[migration]"     # plus any Catch2 test spec
//     msga_tests test_theme --list-tests      # any Catch2 option
//
// ctest runs one process per suite, so process-global state a suite sets up
// (QCoreApplication/QApplication, QSettings::setPath, QStandardPaths test
// mode, environment variables, backend registration) is still per suite.
//
// A suite that needs its own process setup declares it with MSGA_TEST_MAIN in
// place of main(); the body is the old main(), calling msga_test::runCatch()
// in place of Catch::Session().run():
//
//     MSGA_TEST_MAIN(argc, argv) {
//         QCoreApplication app(argc, argv);
//         return msga_test::runCatch(argc, argv);
//     }
//
// A suite without one runs Catch2 directly, like Catch2WithMain.
//
// Everything a test file defines at namespace scope besides its test cases
// (fixtures, stub backends, helpers) belongs in an anonymous namespace or is
// static: all files share one program now, and two files' global
// `struct Fixture` would silently merge their inline members (an ODR
// violation the linker does not report).
#pragma once

namespace msga_test {

using SuiteMain = int (*)(int argc, char **argv);

// Registers `fn` as the process setup of the suite whose file is `file`
// (__FILE__). Used by MSGA_TEST_MAIN at static-initialisation time.
struct SuiteMainRegistration {
    SuiteMainRegistration(const char *file, SuiteMain fn);
};

// Runs Catch2 over the selected suite's test cases only, honouring any test
// spec / option in argv.
int runCatch(int argc, char **argv);

} // namespace msga_test

#define MSGA_TEST_MAIN(argc, argv)                                                                 \
    static int                                      msgaTestSuiteMain(int argc, char **argv);      \
    static const ::msga_test::SuiteMainRegistration msgaTestSuiteMainRegistration{                 \
        __FILE__, &msgaTestSuiteMain                                                               \
    };                                                                                             \
    static int msgaTestSuiteMain(int argc, char **argv)

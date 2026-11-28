// A tiny test harness for msga's ctest executables. Catch2 would dwarf the
// code under test and pull <iostream>/exceptions into every test binary; this
// is all the tests need: named cases, CHECK (continue) and REQUIRE (abort the
// case), and a non-zero exit when anything failed.
//
//   TEST("json: round trip") { CHECK(a == b); REQUIRE(p != nullptr); }
//   int main() { return base::test::runAll(); }   // or BASE_TEST_MAIN()
//
// Each test file is a suite named after it (test_shell.cpp -> test_shell), so
// many suites can share one executable (msga_app_tests: all of tests/app) and
// ctest still runs each in its own process. The command line selects by exact
// name, never by substring:
//
//   msga_app_tests test_shell           one suite, every case in the file
//   msga_app_tests test_messages image  one case group of that suite: the
//                                       cases named "image: ..."
//   ui_tests hover                      one case group, from every suite
//   ui_tests "hover: tooltips appear"   one case, by its whole name
//   ui_tests                            every case, in one process
//   ui_tests --list                     every suite and case
//
// A suite that needs its own process setup (signal handlers, a log file)
// declares it with BASE_TEST_SUITE_MAIN, which runs in place of main() when
// that suite is selected; the body ends with runSuite():
//
//   BASE_TEST_SUITE_MAIN(argc, argv) {
//       crash::install(log);
//       return base::test::runSuite(argc, argv);
//   }
//
// Everything else a test file defines at namespace scope (fixtures, fake
// backends, helpers) belongs in an anonymous namespace or is static: all the
// suites of an executable share one program, and two files' global `struct
// Fixture` would silently merge their inline members (an ODR violation the
// linker does not report).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace base::test {

using Fn = void (*)();
struct Case {
    const char *name;
    const char *file; // __FILE__: the suite it belongs to
    Fn          fn;
    Case       *next;
};

// A suite's own process setup (BASE_TEST_SUITE_MAIN). It gets the command
// line without the suite's name and returns runSuite()'s result.
using SuiteMain = int (*)(int argc, char **argv);
struct Suite {
    const char *file;
    SuiteMain   main;
    Suite      *next;
};

// Registers a case at static-init time (called by the TEST macro).
int  add(Case *c);
// Registers a suite's main at static-init time (BASE_TEST_SUITE_MAIN).
int  addSuite(Suite *s);
// Records a failed check; returns false so REQUIRE can bail out.
bool fail(const char *file, int line, const char *expr);
// Prints both sides of a failed string comparison.
bool failEq(const char *file, int line, const char *expr, std::string_view a, std::string_view b);
// main(): isolates the process from the user's session, then runs what the
// command line selects (see the top of this file): `[suite] [group | case]`,
// or `--list`. An unknown suite, group or case is an error (exit 2), so a
// misspelt ctest entry cannot pass by running nothing.
int  runAll(int argc = 0, char **argv = nullptr);
// Runs the selected suite's cases, or its case group (argv[1]); the end of a
// BASE_TEST_SUITE_MAIN, which gets the arguments after the suite's name.
int  runSuite(int argc, char **argv);

inline bool eq(std::string_view a, std::string_view b) {
    return a == b;
}

// ── Portable helpers for what tests need from POSIX (mkdtemp, setenv, …) ───
// A new empty directory "<parent>/<prefix>XXXXXX", the parent made first;
// parent "" = /tmp, or %TEMP% on Windows. '/' separators; "" when it could
// not be made. Deleted with all it holds when the test process exits.
std::string makeTempDir(const char *prefix, const std::string &parent = {});
// Deletes a file or a directory tree; symlinks are removed, never followed.
void        removeTree(const std::string &path);
// setenv / unsetenv (Windows: _wputenv_s, so child processes see it too).
void        setEnv(const char *name, const std::string &value);
void        unsetEnv(const char *name);
// Sets an environment variable back to `old` ("" = unset), as a test found it.
void        restoreEnv(const char *name, const std::string &old);
// Sets a file's modification time (utimes).
bool        setModifiedTime(const std::string &path, int64_t unixSeconds);
// realpath: absolute, symlinks resolved ('/' separators); "" when missing.
std::string realPath(const std::string &path);
// Kills a process outright (SIGKILL, TerminateProcess).
void        killProcess(int64_t pid);
// "file://" + an absolute path, as a file manager writes it ("file:///C:/x"
// on Windows); the path is taken as already percent-encoded.
std::string fileUri(const std::string &path);

#ifndef _WIN32
// While it lives, the process acts as one in a Flatpak sandbox (base/host.h):
// a stand-in flatpak-spawn takes the real one's options and runs the command
// here — so the commands, the host-side scripts and their plumbing are what
// a sandbox gets, only on this machine.
class FakeSandbox {
public:
    FakeSandbox();
    ~FakeSandbox();
    FakeSandbox(const FakeSandbox &)            = delete;
    FakeSandbox &operator=(const FakeSandbox &) = delete;
    // How many commands went through it so far, and the last one's options
    // (each on a line, up to the command).
    int          spawns() const;
    std::string  lastOptions() const;

private:
    std::string _log;
};
#endif

} // namespace base::test

#define BASE_TEST_CAT2(a, b) a##b
#define BASE_TEST_CAT(a, b) BASE_TEST_CAT2(a, b)
#define TEST(name)                                                                                 \
    static void             BASE_TEST_CAT(testFn_, __LINE__)();                                    \
    static base::test::Case BASE_TEST_CAT(testCase_, __LINE__){                                    \
        name, __FILE__, &BASE_TEST_CAT(testFn_, __LINE__), nullptr                                 \
    };                                                                                             \
    static int BASE_TEST_CAT(testReg_, __LINE__) =                                                 \
        base::test::add(&BASE_TEST_CAT(testCase_, __LINE__));                                      \
    static void BASE_TEST_CAT(testFn_, __LINE__)()

#define CHECK(expr) ((expr) ? true : base::test::fail(__FILE__, __LINE__, #expr))
#define CHECK_FALSE(expr) CHECK(!(expr))
#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            base::test::fail(__FILE__, __LINE__, #expr);                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)
// String equality that prints both sides on failure.
#define CHECK_STR(a, b)                                                                            \
    (base::test::eq((a), (b)) ? true                                                               \
                              : base::test::failEq(__FILE__, __LINE__, #a " == " #b, (a), (b)))

#define BASE_TEST_MAIN()                                                                           \
    int main(int argc, char **argv) {                                                              \
        return base::test::runAll(argc, argv);                                                     \
    }

// The file's own process setup, run in place of main() when its suite is
// selected (see the top of this file). One per file.
#define BASE_TEST_SUITE_MAIN(argc, argv)                                                           \
    static int               baseTestSuiteMain(int argc, char **argv);                             \
    static base::test::Suite baseTestSuite{__FILE__, &baseTestSuiteMain, nullptr};                 \
    static int               baseTestSuiteReg = base::test::addSuite(&baseTestSuite);              \
    static int               baseTestSuiteMain(int argc, char **argv)

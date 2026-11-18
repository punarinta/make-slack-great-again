#include "base/file.h"
#include "base/process.h"
#include "support/test.h"
#include "base/time.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
base::RunResult cmd(const std::string &line, const base::RunOptions &o = {}) {
    std::string exe = base::env("COMSPEC");
    if (exe.empty())
        exe = "C:/Windows/System32/cmd.exe";
    return base::run(exe, {"/d", "/c", line}, o);
}
#else
std::string tempDir() {
    const std::string d = base::test::makeTempDir("next_process_test_");
    return d.empty() ? "/tmp" : d;
}

base::RunResult sh(const std::string &script, const base::RunOptions &o = {}) {
    return base::run("/bin/sh", {"-c", script}, o);
}
#endif

} // namespace

TEST("process: a program that is not there did not start") {
    const auto r = base::run("/nonexistent/program", {}, {});
    CHECK_FALSE(r.started);
    CHECK(r.code == -1);
    CHECK_FALSE(r.output.empty()); // why
}

#ifdef _WIN32
TEST("process (Windows): command-line quoting and the environment block") {
    std::wstring c;
    base::appendQuoted(c, L"C:\\Program Files\\x.exe");
    base::appendQuoted(c, L"plain");
    base::appendQuoted(c, L"");
    base::appendQuoted(c, L"a\nb");
    base::appendQuoted(c, L"say \"hi\"\\");
    CHECK(c == L"\"C:\\Program Files\\x.exe\" plain \"\" \"a\nb\" \"say \\\"hi\\\"\\\\\"");
    const std::wstring b = base::envBlock({"MSGA_ENV_TEST=1"});
    CHECK(b.size() >= 2 && b[b.size() - 1] == L'\0' && b[b.size() - 2] == L'\0');
    CHECK(b.find(L"MSGA_ENV_TEST=1") != std::wstring::npos);
}

TEST("process (Windows): output, exit code and environment through cmd.exe") {
    const auto r = cmd("echo one& exit /b 3");
    CHECK(r.started);
    CHECK_FALSE(r.timedOut);
    CHECK(r.code == 3);
    CHECK_STR(r.output, "one\r\n");
    base::RunOptions o;
    o.env = {"MSGA_RUN_TEST_A=1", "MSGA_RUN_TEST_A=2"};
    CHECK_STR(cmd("echo %MSGA_RUN_TEST_A%", o).output, "2\r\n");
    o.mergeStderr = false;
    CHECK_STR(cmd("echo out& echo err 1>&2", o).output, "out\r\n");
}
#else
// The cases below drive /bin/sh: POSIX only.

TEST("process: run captures stdout and the exit code") {
    const auto r = sh("printf 'one\\ntwo'; exit 3");
    CHECK(r.started);
    CHECK_FALSE(r.timedOut);
    CHECK(r.code == 3);
    CHECK_STR(r.output, "one\ntwo");
    CHECK(sh("true").code == 0);
    // 127 is a program's own exit too, not a sign that it never started.
    const auto exited127 = sh("exit 127");
    CHECK(exited127.started);
    CHECK(exited127.code == 127);
}

TEST("process: what can't be started is said, for a Process too") {
    base::Process p;
    CHECK_FALSE(p.start("/nonexistent/program", {}));
    // There, but not a program it may run.
    const std::string dir = tempDir();
    REQUIRE(file::writeAtomic(dir + "/data", "#!/bin/sh\nexit 0\n", 0644));
    const auto r = base::run(dir + "/data", {}, {});
    CHECK_FALSE(r.started);
    CHECK_FALSE(r.output.empty());
    CHECK_FALSE(p.start(dir + "/data", {}));
    file::remove(dir + "/data");
    file::remove(dir);
}

TEST("process: a bad working directory is a launch failure") {
    base::RunOptions o;
    o.cwd        = "/nonexistent/msga-working-directory";
    const auto r = sh("echo should-not-run", o);
    CHECK_FALSE(r.started);
    CHECK(r.code == -1);
    CHECK_FALSE(r.output.empty()); // why, not what the program said
}

TEST("process: executable paths are relative to the child's working directory") {
    const std::string dir = tempDir();
    REQUIRE(file::writeAtomic(dir + "/child", "#!/bin/sh\nprintf relative", 0755));
    base::RunOptions o;
    o.cwd        = dir;
    const auto r = base::run("./child", {}, o);
    CHECK(r.started);
    CHECK(r.code == 0);
    CHECK_STR(r.output, "relative");
    file::remove(dir + "/child");
    file::remove(dir);
}

TEST("process: the executable path stays absolute after a change of directory") {
    const std::string exe = base::executablePath();
    REQUIRE(!exe.empty());
    CHECK(exe.front() == '/');
    CHECK(file::exists(exe));
    // A copy of this binary started with a relative argv[0], and one found
    // on $PATH, each changing directory (and $PATH) before it asks: the
    // path was taken at its start (OpenBSD keeps none for a running image).
    const char *const expect = "MSGA_TEST_EXE_EXPECT";
    if (const std::string want = base::env(expect); !want.empty()) {
        REQUIRE(::chdir("/") == 0);
        base::test::setEnv("PATH", "/nonexistent/msga-test-bin");
        CHECK_STR(base::executablePath(), want);
        return;
    }
    char cwd[4096];
    REQUIRE(::getcwd(cwd, sizeof cwd) != nullptr);
    REQUIRE(::chdir("/") == 0);
    CHECK_STR(base::executablePath(), exe);
    REQUIRE(::chdir(cwd) == 0);
    const std::string              name(file::baseName(exe));
    const std::vector<std::string> args = {
        "test_process", "process: the executable path stays absolute after a change of directory"
    };
    base::RunOptions o;
    o.cwd               = std::string(file::dirName(exe));
    o.env               = {std::string(expect) + "=" + exe};
    const auto relative = base::run("./" + name, args, o);
    CHECK(relative.code == 0);
    if (relative.code != 0)
        std::fprintf(stderr, "%s\n", relative.output.c_str());
    const std::string path = base::env("PATH");
    base::test::setEnv("PATH", o.cwd + ":" + path);
    o.cwd             = "/";
    const auto onPath = base::run(name, args, o);
    base::test::setEnv("PATH", path);
    CHECK(onPath.code == 0);
    if (onPath.code != 0)
        std::fprintf(stderr, "%s\n", onPath.output.c_str());
}

TEST("process: stderr is merged, or discarded") {
    CHECK_STR(sh("echo out; echo err >&2").output, "out\nerr\n");
    base::RunOptions o;
    o.mergeStderr = false;
    CHECK_STR(sh("echo out; echo err >&2", o).output, "out\n");
}

TEST("process: stdin is the input, then closed, or the null device") {
    base::RunOptions o;
    o.input = "hello\nworld\n";
    CHECK_STR(sh("cat", o).output, "hello\nworld\n");
    // No input: reading sees the end at once (never waits on a prompt).
    CHECK_STR(sh("cat; echo done").output, "done\n");
    // More input than a pipe holds, read only after a while.
    o.input = std::string(300000, 'x');
    CHECK_STR(sh("sleep 0.1; wc -c | tr -d ' '", o).output, "300000\n");
    // A child that never reads its input neither blocks us nor SIGPIPEs us.
    const auto r = sh("exit 0", o);
    CHECK(r.started);
    CHECK(r.code == 0);
}

TEST("process: the merged environment") {
    base::test::setEnv("MSGA_MERGE_TEST_GONE", "x");
    base::test::setEnv("MSGA_MERGE_TEST_SET", "old");
    const std::vector<std::string> e =
        base::mergedEnv({"MSGA_MERGE_TEST_SET=new", "MSGA_MERGE_TEST_GONE="});
    int set = 0, gone = 0, path = 0;
    for (const std::string &kv : e) {
        set += kv.rfind("MSGA_MERGE_TEST_SET=", 0) == 0;
        gone += kv.rfind("MSGA_MERGE_TEST_GONE=", 0) == 0;
        path += kv.rfind("PATH=", 0) == 0;
    }
    CHECK(set == 1 && gone == 0 && path == 1);
    CHECK_STR(e.back(), "MSGA_MERGE_TEST_SET=new");
    base::test::unsetEnv("MSGA_MERGE_TEST_GONE");
    base::test::unsetEnv("MSGA_MERGE_TEST_SET");
}

TEST("process: cwd and environment overrides") {
    const std::string dir = tempDir();
    base::RunOptions  o;
    o.cwd                  = dir;
    // /tmp may be a symlink (macOS): compare what pwd -P says for both.
    const std::string real = sh("cd " + dir + " && pwd -P").output;
    CHECK_STR(sh("pwd -P", o).output, real);
    base::test::setEnv("MSGA_RUN_TEST_UNSET", "was set");
    o.env = {"MSGA_RUN_TEST_A=1", "MSGA_RUN_TEST_A=2", "MSGA_RUN_TEST_UNSET=", "HOME=/nowhere"};
    CHECK_STR(
        sh("echo \"$MSGA_RUN_TEST_A|${MSGA_RUN_TEST_UNSET-unset}|$HOME\"", o).output,
        "2|unset|/nowhere\n"
    );
    base::test::unsetEnv("MSGA_RUN_TEST_UNSET");
    // PATH is still ours: commands are found.
    CHECK(sh("command -v sh >/dev/null", o).code == 0);
    file::remove(dir);
}

TEST("process: a timeout kills the whole group") {
    const std::string dir     = tempDir();
    const std::string pidFile = dir + "/child.pid";
    base::RunOptions  o;
    o.timeoutMs     = 300;
    const int64_t t = base::monotonicMs();
    // The child of the shell holds the output pipe and is in its group.
    const auto    r = sh("sleep 30 & echo $! > " + pidFile + "; echo started; wait", o);
    CHECK(base::monotonicMs() - t < 3000);
    CHECK(r.started);
    CHECK(r.timedOut);
    CHECK(r.code == 128 + 9);
    CHECK_STR(r.output, "started\n");
    std::string pid;
    REQUIRE(file::readAll(pidFile, &pid));
    // Reaped by init a moment later; gone or a zombie, never running on.
    bool gone = false;
    for (int i = 0; i < 100 && !gone; ++i) {
        gone = sh("ps -o stat= -p " + pid + " | grep -qv Z").code != 0;
        if (!gone)
            usleep(20000);
    }
    CHECK(gone);
    file::remove(pidFile);
    file::remove(dir);
}

TEST("process: a background process holding the output does not keep run() waiting") {
    const int64_t t = base::monotonicMs();
    const auto    r = sh("(sleep 5; echo late) & echo now");
    CHECK(base::monotonicMs() - t < 2000);
    CHECK(r.code == 0);
    CHECK_STR(r.output, "now\n");
    // Its own session, so its own process group: the one a timeout ends.
    CHECK(sh("[ \"$(ps -o pgid= -p $$ | tr -d ' ')\" = $$ ]").code == 0);
}

TEST("process: an exit by signal is 128 + the signal") {
    CHECK(sh("kill -TERM $$").code == 128 + 15);
}
#endif

TEST("process: homeDir") {
#ifdef _WIN32
    std::string want = base::env("USERPROFILE");
    for (char &c : want)
        if (c == '\\')
            c = '/';
    CHECK_STR(base::homeDir(), want);
#else
    CHECK_STR(base::homeDir(), base::env("HOME"));
#endif
}

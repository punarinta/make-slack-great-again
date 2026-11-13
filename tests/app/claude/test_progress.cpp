// Progress reports: `msga --claude-status`, its report file, and what a
// background command's output says of how far along it is (progress.h).
#include "app/claude/progress.h"

#include "base/file.h"
#include "base/process.h"
#include "base/time.h"
#include "support/test.h"

#include <cmath>
#include <string>
#include <vector>

using namespace claude;

namespace {

bool near(float a, float b) {
    return std::fabs(a - b) < 1e-4f;
}

// runStatusCommand on `args` (after "msga --claude-status").
int statusCommand(std::vector<std::string> args) {
    args.insert(args.begin(), {"msga", std::string(kStatusFlag)});
    std::vector<char *> argv;
    for (std::string &a : args)
        argv.push_back(a.data());
    return runStatusCommand(int(argv.size()), argv.data());
}

} // namespace

TEST("progress: --progress takes done/total, a percentage or a fraction") {
    CHECK(near(parseProgress("9846/10000"), 0.9846f));
    CHECK(near(parseProgress(" 9,846 / 10,000 "), 0.9846f));
    CHECK(near(parseProgress("98.5%"), 0.985f));
    CHECK(near(parseProgress("0.985"), 0.985f));
    CHECK(near(parseProgress("1"), 1.f));
    CHECK(near(parseProgress("0/7"), 0.f));
    CHECK(parseProgress("") < 0);
    CHECK(parseProgress("abc") < 0);
    CHECK(parseProgress("7/5") < 0); // more done than there is
    CHECK(parseProgress("3/0") < 0);
    CHECK(parseProgress("150%") < 0);
    CHECK(parseProgress("42") < 0);             // a bare count isn't a fraction
    CHECK(near(parseProgress("1.5/2"), 0.75f)); // 1.5 of 2 GB
}

TEST("progress: a line of output says how far along it is") {
    CHECK(near(progressInLine("9846/10000 programs, 0 failures"), 0.9846f));
    CHECK(near(progressInLine("[ 42/120] Building CXX object a.o"), 0.35f));
    CHECK(near(progressInLine("Tested 9,846 / 10,000"), 0.9846f));
    CHECK(near(progressInLine("downloading… 37%"), 0.37f));
    CHECK(near(progressInLine("step 3/4 (75.0%)"), 0.75f));
    // Paths, dates, versions, times, ratios out of range: no progress.
    CHECK(progressInLine("wrote src/2/3/out.txt") < 0);
    CHECK(progressInLine("2026/11/13 16:23 started") < 0);
    CHECK(progressInLine("v1.2/3") < 0);
    CHECK(progressInLine("12:30/45") < 0);
    CHECK(progressInLine("3/2 of them") < 0);
    CHECK(progressInLine("x86_64/linux") < 0);
    CHECK(progressInLine("1/2.5") < 0);
    CHECK(progressInLine("all done") < 0);
}

TEST("progress: the last line of a command's output") {
    CHECK_STR(lastOutputLine("one\ntwo\nthree\n"), "three");
    CHECK_STR(lastOutputLine("one\ntwo\n\n  \n"), "two");
    // A bar redrawn with \r: its last drawing.
    CHECK_STR(lastOutputLine("start\n 10%\r 20%\r 30%"), "30%");
    // Colours and other escapes out.
    CHECK_STR(lastOutputLine("\x1b[32mPASS\x1b[0m 12/40\n"), "PASS 12/40");
    CHECK_STR(lastOutputLine("\x1b]0;title\x07ok\n"), "ok");
    // A tail cut mid-character starts at the next whole one.
    CHECK_STR(lastOutputLine(std::string("\xA6 caf\xC3\xA9")), "caf\xC3\xA9");
    CHECK_STR(lastOutputLine(""), "");
    CHECK_STR(lastOutputLine("\n\r\n"), "");
    // Long lines are cut.
    const std::string longLine = lastOutputLine(std::string(500, 'x'), 20);
    CHECK(longLine.size() < 30 && longLine.substr(0, 19) == std::string(19, 'x'));
}

TEST("progress: a report is written to the job's folder and read back") {
    const std::string old = base::env("CLAUDE_JOB_DIR");
    const std::string job = base::test::makeTempDir("job-", base::env("HOME"));
    base::test::setEnv("CLAUDE_JOB_DIR", job);
    Report r;
    CHECK_FALSE(readReport(job, &r)); // none yet

    const int64_t before = base::nowMicros() / 1000;
    CHECK(statusCommand({"9846/10000 programs,", "0 failures", "--progress", "9846/10000"}) == 0);
    REQUIRE(readReport(job, &r));
    CHECK_STR(r.text, "9846/10000 programs, 0 failures");
    CHECK(near(r.progress, 0.9846f));
    CHECK(r.atMs >= before && r.atMs <= base::nowMicros() / 1000);

    // The next one replaces it; progress alone is a report too.
    CHECK(statusCommand({"--progress=50%"}) == 0);
    REQUIRE(readReport(job, &r));
    CHECK_STR(r.text, "");
    CHECK(near(r.progress, 0.5f));
    CHECK(statusCommand({"linking"}) == 0);
    REQUIRE(readReport(job, &r));
    CHECK(r.progress < 0);

    // Mistakes change nothing.
    CHECK(statusCommand({"x", "--progress", "lots"}) == 2);
    CHECK(statusCommand({}) == 2);
    CHECK(statusCommand({"--progress"}) == 2);
    REQUIRE(readReport(job, &r));
    CHECK_STR(r.text, "linking");
    CHECK(statusCommand({"--help"}) == 0);

    // Outside a background session: nowhere to report to.
    base::test::unsetEnv("CLAUDE_JOB_DIR");
    CHECK(statusCommand({"hello"}) == 1);
    base::test::restoreEnv("CLAUDE_JOB_DIR", old);

    // A broken file is no report.
    file::writeAtomic(reportPath(job), "{not json", 0644, false);
    CHECK_FALSE(readReport(job, &r));
}

TEST("progress: the prompt note names msga's own path") {
    const std::string note = progressNote("/opt/msga/msga");
    CHECK(note.find("\"/opt/msga/msga\" --claude-status") != std::string::npos);
    CHECK(note.find("--progress") != std::string::npos);
}

TEST("progress: a job is named from its command line") {
    CHECK_STR(jobName("./scripts/fuzz-checkout.sh --programs 10000"), "fuzz-checkout");
    CHECK_STR(jobName("npm test"), "npm test");
    CHECK_STR(jobName("npm run build -- --watch"), "npm run");
    CHECK_STR(jobName("cmake --build build-release -j 16"), "cmake --build");
    CHECK_STR(jobName("cargo build --release"), "cargo build");
    CHECK_STR(jobName("make -j8 all"), "make all");
    CHECK_STR(jobName("python3 tools/gen_tables.py --all"), "gen_tables");
    CHECK_STR(jobName("bash ./run-e2e.sh"), "run-e2e");
    CHECK_STR(jobName("CI=1 ctest --test-dir build -j 8"), "ctest");
    CHECK_STR(jobName("cd build && ninja -j8 2>&1 | tail"), "ninja");
    CHECK_STR(jobName("/usr/bin/rsync -a src/ dst/"), "rsync");
    CHECK_STR(jobName(""), "");
}

TEST("progress: named reports each keep a file of their own") {
    const std::string old = base::env("CLAUDE_JOB_DIR");
    const std::string job = base::test::makeTempDir("jobs-", base::env("HOME"));
    base::test::setEnv("CLAUDE_JOB_DIR", job);
    CHECK(
        statusCommand({"6200/10000 orders", "--name", "migration", "--progress", "6200/10000"}) == 0
    );
    CHECK(statusCommand({"--name=docs build", "half way", "--progress", "50%"}) == 0);
    CHECK(statusCommand({"unnamed"}) == 0);
    CHECK(statusCommand({"x", "--name", ""}) == 2);
    base::test::restoreEnv("CLAUDE_JOB_DIR", old);
    const std::vector<Report> all = readReports(job);
    REQUIRE(all.size() == 3);
    CHECK_STR(all[0].name, "");
    CHECK_STR(all[0].text, "unnamed");
    CHECK_STR(all[1].name, "docs build");
    CHECK_STR(all[1].ratio, "");
    CHECK(near(all[1].progress, 0.5f));
    CHECK_STR(all[2].name, "migration");
    CHECK_STR(all[2].ratio, "6200/10000");
    // The ratio a line of output says.
    std::string ratio;
    CHECK(near(progressInLine("[ 42/120] Building", &ratio), 0.35f));
    CHECK_STR(ratio, "42/120");
}

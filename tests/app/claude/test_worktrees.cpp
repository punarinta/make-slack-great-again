// The git worktrees a session used: found in Claude Code's records, and
// deleted from a real throwaway repository (git runs; `claude` never does).
#include "app/claude/outputs.h"
#include "app/claude/roster.h"
#include "app/claude/worktrees.h"

#include "base/file.h"
#include "base/process.h"
#include "base/str.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/plat.h"
#include "app/model/jobs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace claude;

namespace {

std::string tempDir() {
    return base::test::makeTempDir("claude_worktrees_test_");
}

bool waitFor(plat::App &app, const std::function<bool()> &pred, int ms) {
    const int64_t until = base::monotonicMs() + ms;
    while (!pred() && base::monotonicMs() < until)
        app.pump(10);
    return pred();
}

bool contains(const std::vector<std::string> &v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST("worktrees: the worktrees a session used are found in its records") {
    const auto job = worktreesOfJob(
        R"({"sessionId":"S","worktreePath":"/r/.claude/worktrees/a","worktreeBranch":"wt-a",)"
        R"("originCwd":"/r"})"
    );
    REQUIRE(job.size() == 1);
    CHECK((job[0] == WorktreeRef{"/r/.claude/worktrees/a", "wt-a", "/r"}));
    CHECK(worktreesOfJob(R"({"sessionId":"S","worktreePath":null})").empty());
    CHECK(worktreesOfJob("not json").empty());

    const std::string dir = tempDir();
    const std::string t   = dir + "/S.jsonl";
    file::writeAtomic(
        t,
        R"({"type":"user","message":{"role":"user","content":"worktree-state"}})"
        "\n"
        R"({"type":"worktree-state","worktreeSession":{"worktreePath":"/r/w/b","worktreeBranch":"wt-b","originalCwd":"/r"}})"
        "\n"
        R"({"type":"worktree-state","worktreeSession":null})"
        "\n"
        R"({"type":"worktree-state","worktreeSession":{"worktreePath":"/r/w/b/","originalCwd":"/r"}})"
        "\n"
        R"({"type":"worktree-state","worktreeSession":{"worktreePath":"/r/w/c","worktreeBranch":"wt-c"}})"
    );
    file::writeAtomic(
        dir + "/S/subagents/agent-x.meta.json",
        R"({"agentType":"x","worktreePath":"/r/w/d","worktreeBranch":"wt-d"})"
    );
    file::writeAtomic(dir + "/S/subagents/agent-y.meta.json", R"({"agentType":"y"})");
    const auto seen = worktreesOfTranscript(t);
    REQUIRE(seen.size() == 3);
    CHECK((seen[0] == WorktreeRef{"/r/w/b", "wt-b", "/r"})); // named twice: once
    CHECK((seen[1] == WorktreeRef{"/r/w/c", "wt-c", ""}));
    CHECK((seen[2] == WorktreeRef{"/r/w/d", "wt-d", ""}));
    CHECK(worktreesOfTranscript(dir + "/none.jsonl").empty());
    file::removeTree(dir);

    CHECK(pathWithin("/r/w/b/src", "/r/w/b"));
    CHECK(pathWithin("/r/w/b", "/r/w/b/"));
    CHECK_FALSE(pathWithin("/r/w/bc", "/r/w/b"));
    CHECK_FALSE(pathWithin("/r", "/r/w/b"));
    CHECK_FALSE(pathWithin("", "/r"));

    const auto list = parseWorktreeList(
        "worktree /r\nHEAD 1\nbranch refs/heads/master\n\n"
        "worktree /r/w/b\nHEAD 2\nbranch refs/heads/wt-b\n\n"
        "worktree /r/w/e\nHEAD 3\ndetached\nprunable gitdir file points to non-existent location\n"
    );
    REQUIRE(list.size() == 3);
    CHECK_STR(list[0].path, "/r");
    CHECK_STR(list[0].branch, "master");
    CHECK_STR(list[1].path, "/r/w/b");
    CHECK_STR(list[1].branch, "wt-b");
    CHECK(list[2].branch.empty());
}

TEST("worktrees: removing a session deletes every worktree it used, and only those") {
    const std::string git = base::findExecutable("git");
    if (git.empty()) {
        std::fprintf(stderr, "  no git: skipped\n");
        return;
    }
    const std::string work = tempDir();
    const std::string real = base::test::realPath(work);
    REQUIRE(!real.empty());
    const std::string repo = real + "/repo";
    file::makeDirs(repo);
    bool       ok  = true;
    const auto run = [&](const std::string &cwd, std::vector<std::string> args) {
        std::vector<std::string> all = {
            "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"
        };
        all.insert(all.end(), args.begin(), args.end());
        base::RunOptions o;
        o.cwd                   = cwd;
        o.mergeStderr           = false;
        o.timeoutMs             = 20'000;
        const base::RunResult r = base::run(git, all, o);
        if (r.code != 0)
            ok = false;
        return r.output;
    };
    run(repo, {"init", "-q", "-b", "master"});
    file::writeAtomic(repo + "/a.txt", "a\n");
    run(repo, {"add", "a.txt"});
    run(repo, {"commit", "-q", "-m", "a"});
    const std::string trees = repo + "/.claude/worktrees";
    const auto        add   = [&](const std::string &name) {
        run(repo, {"worktree", "add", "-q", "-b", "wt-" + name, trees + "/" + name});
        return trees + "/" + name;
    };
    const std::string inJob     = add("job");     // the job's own, dirty
    const std::string inRecords = add("records"); // only in the transcript, unpushed
    const std::string inSub     = add("sub");     // a subagent's
    const std::string inUse     = add("busy");    // a live session works in it
    const std::string foreign   = add("foreign"); // another session's, not named
    file::writeAtomic(inJob + "/scratch.txt", "uncommitted\n");
    file::writeAtomic(inRecords + "/b.txt", "b\n");
    run(inRecords, {"add", "b.txt"});
    run(inRecords, {"commit", "-q", "-m", "x"});
    file::writeAtomic(inRecords + "/a.txt", "changed\n");
    // A folder that is no worktree at all, named as if one.
    const std::string plain = work + "/plain";
    file::writeAtomic(plain + "/keep.txt", "mine\n");
    REQUIRE(ok);

    // A terminal session (this very process: alive) works in `inUse`.
    const Paths paths{work + "/claude-home"};
    file::writeAtomic(
        paths.sessionsDir() + "/t.json",
        str::concat(
            {R"({"pid":)",
             str::number(int64_t(getpid())),
             R"(,"sessionId":"eeee5555","cwd":")",
             inUse,
             R"(/src","kind":"interactive","entrypoint":"cli","status":"idle"})"}
        )
    );
    CHECK(worktreeInUse(paths, inUse));
    CHECK_FALSE(worktreeInUse(paths, inJob));

    std::vector<WorktreeRef> refs;
    addWorktree(refs, {inJob, "wt-job", repo});
    addWorktree(refs, {inRecords, "wt-records", repo});
    addWorktree(refs, {inUse, "wt-busy", repo});
    addWorktree(refs, {repo, "master", repo}); // the main checkout named as if one
    addWorktree(refs, {inSub, "wt-sub", ""});  // found from itself
    addWorktree(refs, {plain, "", ""});
    addWorktree(refs, {work + "/gone", "wt-gone", repo});
    REQUIRE(refs.size() == 7);

    base::test::setEnv("PLAT_BACKEND", "headless");
    auto app = plat::App::create();
    REQUIRE(app);
    struct Drain { // the git runs post to the App: done before it goes
        ~Drain() { model::waitBackground(); }
    } drain;
    bool                     finished = false;
    std::vector<std::string> notDeleted;
    reapWorktrees(
        *app,
        refs,
        [&](const std::string &path) { return worktreeInUse(paths, path); },
        [&](std::vector<std::string> left) {
            notDeleted = std::move(left);
            finished   = true;
        }
    );
    CHECK_FALSE(finished); // never from inside the call
    REQUIRE(waitFor(*app, [&] { return finished; }, 60'000));
    CHECK(notDeleted.empty());

    CHECK_FALSE(file::exists(inJob));
    CHECK_FALSE(file::exists(inRecords));
    CHECK_FALSE(file::exists(inSub));
    const std::string        branchText = run(repo, {"branch", "--format=%(refname:short)"});
    std::vector<std::string> left;
    for (std::string_view rest = branchText; !rest.empty();) {
        const size_t nl = rest.find('\n');
        if (nl > 0)
            left.emplace_back(rest.substr(0, nl));
        rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
    }
    CHECK_FALSE(contains(left, "wt-job"));
    CHECK_FALSE(contains(left, "wt-records"));
    CHECK_FALSE(contains(left, "wt-sub"));
    // Kept: the one in use, the main checkout and its branch, the other one,
    // and the folder that was never a worktree.
    CHECK(contains(left, "wt-busy"));
    CHECK(contains(left, "master"));
    CHECK(contains(left, "wt-foreign"));
    CHECK(file::exists(inUse));
    CHECK(file::exists(repo + "/a.txt"));
    CHECK(file::exists(foreign));
    CHECK(file::exists(plain + "/keep.txt"));
    const std::string list = run(repo, {"worktree", "list", "--porcelain"});
    CHECK(list.find(inJob) == std::string::npos);
    CHECK(list.find(inRecords) == std::string::npos);
    CHECK(list.find(inSub) == std::string::npos);
    CHECK(list.find(inUse) != std::string::npos);

    // Nothing to do: done all the same, on the loop.
    bool empty = false;
    reapWorktrees(*app, {}, nullptr, [&](std::vector<std::string>) { empty = true; });
    CHECK_FALSE(empty);
    CHECK(waitFor(*app, [&] { return empty; }, 5'000));
    file::removeTree(work);
}

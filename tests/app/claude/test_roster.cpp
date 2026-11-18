// The roster: Claude Code's sessions/<pid>.json and jobs/<id>/state.json,
// against a throwaway Claude Code home (never the user's ~/.claude).
#include "app/claude/roster.h"
#include "app/claude/common.h"

#include "base/file.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "base/time.h"
#include "support/test.h"

#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace claude;

namespace {

// Under the test HOME (a throwaway one: base::test's, or ctest's test-home),
// so nothing is left in /tmp.
std::string tempDir() {
    return base::test::makeTempDir("roster-", base::env("HOME"));
}

} // namespace

TEST("roster: roster files parse into sessions") {
    const auto live = parseInteractiveSession(
        R"({"pid":42,"sessionId":"S1","cwd":"/src/app","name":"app-1","status":"waiting",
            "statusUpdatedAt":1790000000000,"entrypoint":"cli","kind":"interactive"})"
    );
    REQUIRE(live);
    CHECK_STR(live->sessionId, "S1");
    CHECK(live->kind == SessionInfo::Kind::Interactive);
    CHECK_STR(live->entrypoint, "cli");
    CHECK(statusNeedsUser(live->status));
    CHECK(live->pid == 42);
    CHECK(live->statusSinceMs == 1790000000000);

    const auto job = parseBackgroundJob(
        R"({"state":"done","sessionId":"S2","name":"Refactor","cwd":"/src/x","needs":null,
            "linkScanPath":"/p/S2.jsonl","updatedAt":"2026-09-02T06:56:01.982Z"})"
    );
    REQUIRE(job);
    CHECK(job->kind == SessionInfo::Kind::Background);
    CHECK_FALSE(job->running); // done: msga may continue it
    CHECK_STR(job->transcriptPath, "/p/S2.jsonl");
    CHECK(job->statusSinceMs == 1788332161982);
    CHECK(job->worktreePath.empty());
    const auto inWorktree = parseBackgroundJob(
        R"({"state":"done","sessionId":"S4","cwd":"/src/x/.claude/worktrees/w",)"
        R"("worktreePath":"/src/x/.claude/worktrees/w","worktreeBranch":"worktree-w"})"
    );
    REQUIRE(inWorktree);
    CHECK_STR(inWorktree->worktreePath, "/src/x/.claude/worktrees/w");

    // Waiting for an approval: "working" plus a needs line (verified live).
    const auto approval = parseBackgroundJob(
        R"({"state":"working","sessionId":"S3","needs":"approve Bash: touch x"})"
    );
    REQUIRE(approval);
    CHECK(statusNeedsUser(approval->status));
    CHECK_FALSE(statusIsBusy(approval->status));
    CHECK(approval->awaitsApproval);
    // A question that happens to begin with "approve" is still a question
    // (seen in 2.1.284): answered by message, not in the terminal.
    const auto question = parseBackgroundJob(
        R"({"state":"blocked","tempo":"blocked","sessionId":"S5",)"
        R"("needs":"approve the data-origin wording before filing the issue"})"
    );
    REQUIRE(question);
    CHECK(statusNeedsUser(question->status));
    CHECK_FALSE(question->awaitsApproval);
    const auto mcp = parseBackgroundJob(
        R"({"state":"blocked","sessionId":"S6","needs":)"
        R"("approve 1 new project MCP server (x) — attach to respond"})"
    );
    REQUIRE(mcp);
    CHECK(mcp->awaitsApproval);

    // A done job whose worker is still alive (idle) counts as running — resuming
    // it would only start a copy — and a busy worker makes it busy.
    auto done   = *job;
    auto worker = parseInteractiveSession(
        R"({"pid":7,"sessionId":"S2","kind":"bg","status":"busy","entrypoint":"cli",)"
        R"("statusUpdatedAt":1790411125875})"
    );
    REQUIRE(worker);
    CHECK(worker->kind == SessionInfo::Kind::Background);
    applyWorker(done, *worker);
    CHECK(done.running);
    CHECK(statusIsBusy(done.status));
    CHECK(done.statusSinceMs == 1790411125875); // the worker's status, the worker's time
    CHECK(done.pid == 7);

    // A job stuck on "working" after its turn ended: the idle worker wins.
    auto stale = *parseBackgroundJob(R"({"state":"working","sessionId":"S2"})");
    auto idle  = parseInteractiveSession(
        R"({"pid":7,"sessionId":"S2","kind":"bg","status":"idle","entrypoint":"cli"})"
    );
    REQUIRE(idle);
    applyWorker(stale, *idle);
    CHECK(stale.running);
    CHECK_FALSE(statusIsBusy(stale.status));
    // ...but a question waiting for the user stays visible.
    auto asking = *approval;
    applyWorker(asking, *idle);
    CHECK(statusNeedsUser(asking.status));

    CHECK_FALSE(parseInteractiveSession("{}"));
    CHECK_FALSE(parseBackgroundJob("garbage"));
    CHECK_FALSE(parseBackgroundJob("[]"));
    CHECK(statusIsBusy("busy"));
    CHECK(statusIsBusy("working"));
    CHECK_FALSE(statusIsBusy("idle"));
    // Idle with a background command running: nobody is typing.
    CHECK_FALSE(statusIsBusy("shell"));
    CHECK(statusHasShell("shell"));
}

TEST("roster: a background worker is paired with its job by job id") {
    // A `/clear` sent to a job starts a new session in the same worker: its
    // sessions/<pid>.json names another session, and only its jobId the job
    // (seen with 2.1.282).
    Paths paths;
    paths.home            = tempDir();
    const std::string sid = "ed076643-40f1-4f2f-aeb1-a0c39466257b";
    const int64_t     me  = int64_t(getpid()); // alive
    REQUIRE(
        file::writeAtomic(
            paths.jobsDir() + "/ed076643/state.json",
            str::concat({R"({"state":"done","sessionId":")", sid, R"(","cwd":"/src/app"})"})
        )
    );
    REQUIRE(
        file::writeAtomic(
            paths.sessionsDir() + "/2.json",
            str::concat(
                {R"({"pid":)",
                 str::number(me),
                 R"(,"sessionId":"b06f80b6-a8d7-45cc-a904-52553cf4e38e","jobId":"ed076643",)",
                 R"("kind":"bg","status":"idle"})"}
            )
        )
    );
    // An interactive session whose process is gone is left out.
    REQUIRE(
        file::writeAtomic(
            paths.sessionsDir() + "/3.json",
            R"({"pid":999999999,"sessionId":"dead","kind":"interactive","status":"idle"})"
        )
    );
    const auto         all = scanSessions(paths);
    const SessionInfo *job = nullptr;
    for (const auto &s : all) {
        CHECK(s.sessionId != "dead");
        if (s.sessionId == sid)
            job = &s;
    }
    REQUIRE(job != nullptr);
    CHECK(job->running);
    CHECK_STR(job->status, "idle");
    CHECK_STR(job->jobId, "ed076643");
    CHECK(job->pid == me);
    const auto pids = liveWorkerPids(paths, sid);
    REQUIRE(pids.size() == 1);
    CHECK(pids[0] == me);
    CHECK(hasLiveWorker(paths, sid));
    CHECK_FALSE(hasLiveWorker(paths, "ffffffff-0000"));
    CHECK(readJobState(paths, "ed076643").find(sid) != std::string::npos);
    CHECK(readJobState(paths, "nope").empty());
    CHECK(readJobState(paths, "").empty());
    // Ours, alive; the pty host check finds none (our parent is no --bg-pty-host).
    const auto stranded = strandedWorker(paths, sid);
#ifdef __linux__
    REQUIRE(stranded.size() == 1);
    CHECK(stranded[0] == me);
#else
    CHECK(stranded.empty());
#endif
}

TEST("roster: a background job's suggested reply is offered only while it asks") {
    // Claude Code writes it on a turn that ends on a question, and its own
    // list offers it only while the job reads "blocked" (verified in 2.1.283).
    const auto asking = parseBackgroundJob(
        R"({"state":"blocked","tempo":"blocked","sessionId":"S1","needs":"confirm the copy",
            "suggestedReply":"copy changes to 'master'"})"
    );
    REQUIRE(asking);
    CHECK_STR(asking->suggestedReply, "copy changes to 'master'");

    // A stopped job keeps a stale one (seen live).
    const auto stopped = parseBackgroundJob(
        R"({"state":"stopped","tempo":"idle","sessionId":"S2","suggestedReply":"finish the rest"})"
    );
    REQUIRE(stopped);
    CHECK(stopped->suggestedReply.empty());

    // Multiple-choice questions are answered by picking, not typing.
    const auto choosing = parseBackgroundJob(
        R"({"state":"blocked","tempo":"blocked","sessionId":"S3","suggestedReply":"yes",
            "block":{"questions":[{"question":"Which?","options":[]}]}})"
    );
    REQUIRE(choosing);
    CHECK(choosing->suggestedReply.empty());
}

TEST("roster: a scan reads only the job states that changed since the last") {
    Paths paths;
    paths.home              = tempDir();
    const std::string state = paths.jobsDir() + "/aaaa1111/state.json";
    REQUIRE(file::writeAtomic(state, R"({"state":"working","sessionId":"aaaa1111-x","cwd":"/a"})"));
    REQUIRE(
        file::writeAtomic(
            paths.jobsDir() + "/bbbb2222/state.json", R"({"state":"done","sessionId":"bbbb2222-y"})"
        )
    );
    const int64_t me = int64_t(getpid()); // alive
    REQUIRE(
        file::writeAtomic(
            paths.sessionsDir() + "/7.json",
            str::concat(
                {R"({"pid":)",
                 str::number(me),
                 R"(,"sessionId":"live-1","cwd":"/src/live","kind":"interactive","status":"busy"})"}
            )
        )
    );
    const auto statusOf = [](const std::vector<SessionInfo> &all, std::string_view id) {
        for (const auto &s : all)
            if (s.sessionId == id)
                return s.status;
        return std::string("(missing)");
    };

    JobStateCache            cache;
    std::vector<SessionInfo> live;
    const auto               first = scanSessions(paths, &live, &cache);
    CHECK(first == scanSessions(paths)); // the same as an uncached scan
    CHECK(cache.byJob.size() == 2);
    REQUIRE(live.size() == 1); // every live process's own entry
    CHECK_STR(live[0].cwd, "/src/live");
    CHECK_STR(statusOf(first, "aaaa1111-x"), "working");

    // Unchanged: the parse kept is taken, the file isn't read. (Shown here by
    // a cached entry edited behind the scan's back: it's what comes out.)
    cache.byJob["aaaa1111"].job->status = "kept";
    CHECK_STR(statusOf(scanSessions(paths, nullptr, &cache), "aaaa1111-x"), "kept");

    // Rewritten: read again. Gone: dropped from the cache too.
    REQUIRE(file::writeAtomic(state, R"({"state":"blocked","sessionId":"aaaa1111-x","cwd":"/a"})"));
#if !defined(_WIN32) && !defined(MSGA_PORT) // file::Stat has no identity there
    // As a coarse clock would have it: the new file, of the same size, at
    // the same mtime. It is another file all the same.
    file::Stat replaced;
    REQUIRE(file::stat(state, &replaced));
    cache.byJob["aaaa1111"].mtimeMicros = replaced.mtimeMicros;
#endif
    file::remove(paths.jobsDir() + "/bbbb2222/state.json");
    file::remove(paths.jobsDir() + "/bbbb2222");
    const auto again = scanSessions(paths, nullptr, &cache);
    CHECK_STR(statusOf(again, "aaaa1111-x"), "blocked");
    CHECK_STR(statusOf(again, "bbbb2222-y"), "(missing)");
    CHECK(cache.byJob.size() == 1);
}

TEST("roster: transcripts are found by session id") {
    Paths paths;
    paths.home = tempDir();
    REQUIRE(file::writeAtomic(paths.projectsDir() + "/-src-app/S1.jsonl", "{}\n"));
    CHECK_STR(paths.findTranscript("S1"), paths.projectsDir() + "/-src-app/S1.jsonl");
    CHECK(paths.findTranscript("S2").empty());
    CHECK(paths.findTranscript("").empty());
    CHECK_STR(
        Paths::subagentTranscript("/p/-src-app/S1.jsonl", "a42"),
        "/p/-src-app/S1/subagents/agent-a42.jsonl"
    );
    CHECK_STR(Paths::subagentsDir("/p/-src-app/S1.jsonl"), "/p/-src-app/S1/subagents");
    CHECK_STR(std::string(Paths::transcriptSessionId("/p/-src-app/S1.jsonl")), "S1");
    CHECK_STR(std::string(Paths::transcriptSessionId("/p/x/.hidden")), ".hidden");
}

TEST("roster: process liveness and leftovers") {
    CHECK(isProcessAlive(int64_t(getpid())));
    CHECK_FALSE(isProcessAlive(0));
    CHECK_FALSE(isProcessAlive(-5));
    CHECK_FALSE(isProcessAlive(999999999));
    // Nothing carries a made-up session's environment.
    CHECK(leftoverProcesses("00000000-dead-beef", "00000000").empty());
    CHECK(leftoverProcesses("", "x").empty());
}

TEST("roster: a folder is trusted when it or a parent was accepted") {
    const std::string dir = tempDir();
    const std::string old = base::env("CLAUDE_CONFIG_DIR");
    base::test::setEnv("CLAUDE_CONFIG_DIR", dir);
    // Absolute folders outside any repository: a bare /src may be one (the
    // static release build mounts the checkout there).
    const std::string r = file::absolute(base::test::makeTempDir("roster-parent-"));
    CHECK_FALSE(isFolderTrusted(r + "/src/app")); // no config at all
    REQUIRE(
        file::writeAtomic(
            dir + "/.claude.json",
            R"({"projects":{")" + r + R"(/src/app":{"hasTrustDialogAccepted":true},")" + r +
                R"(/src/other":{"hasTrustDialogAccepted":false}}})"
        )
    );
    CHECK(isFolderTrusted(r + "/src/app"));
    CHECK(isFolderTrusted(r + "/src/app/"));
    CHECK(isFolderTrusted(r + "/src/app/sub/dir"));
    CHECK_FALSE(isFolderTrusted(r + "/src/other"));
    CHECK_FALSE(isFolderTrusted(r + "/src/application"));
    CHECK_FALSE(isFolderTrusted(r + "/src"));
    if (old.empty())
        base::test::unsetEnv("CLAUDE_CONFIG_DIR");
    else
        base::test::setEnv("CLAUDE_CONFIG_DIR", old);
}

TEST("roster: in a git repository only its root's trust counts; msga records it") {
    const std::string dir = tempDir();
    const std::string old = base::env("CLAUDE_CONFIG_DIR");
    base::test::setEnv("CLAUDE_CONFIG_DIR", dir);
    const std::string cfg  = dir + "/.claude.json";
    // The folders outside any repository (the test HOME may be in one).
    const std::string out  = base::test::makeTempDir("roster-trust-");
    const std::string top  = file::absolute(out) + "/src";
    const std::string repo = top + "/robot";
    REQUIRE(file::makeDirs(repo + "/.git"));
    REQUIRE(file::makeDirs(repo + "/sub"));
    REQUIRE(file::makeDirs(top + "/plain/deeper"));
    REQUIRE(
        file::writeAtomic(
            cfg,
            str::concat(
                {R"({"oauthAccount":{"emailAddress":"a@b"},"numStartups":7,"ratio":0.25,"projects":{)",
                 "\"",
                 top,
                 R"(":{"hasTrustDialogAccepted":true},")",
                 repo,
                 R"(":{"allowedTools":["Bash"],"hasTrustDialogAccepted":false}},"tail":[1,"x"]})"}
            )
        )
    );
    // A trusted parent no longer reaches into a repository (Claude Code 2.1.289).
    CHECK(isFolderTrusted(top + "/plain/deeper"));
    CHECK_FALSE(isFolderTrusted(repo));
    CHECK_FALSE(isFolderTrusted(repo + "/sub"));
    CHECK(canTrustFolder(repo + "/sub"));
    CHECK_FALSE(canTrustFolder(base::homeDir()));

    // Trusting a subfolder trusts the repository's root, everything else kept.
    REQUIRE(trustFolder(repo + "/sub"));
    CHECK(isFolderTrusted(repo));
    CHECK(isFolderTrusted(repo + "/sub"));
    json::Document doc;
    REQUIRE(doc.parseFile(cfg));
    const json::Value root = doc.root();
    CHECK_STR(std::string(root["oauthAccount"]["emailAddress"].str()), "a@b");
    CHECK(root["numStartups"].integer() == 7);
    CHECK(root["ratio"].number() == 0.25);
    CHECK(root["tail"][1].str() == "x");
    CHECK(root["projects"][top]["hasTrustDialogAccepted"].boolean());
    CHECK_STR(std::string(root["projects"][repo]["allowedTools"][0].str()), "Bash");
    CHECK(root["projects"].size() == 2);
    CHECK(!file::exists(cfg + ".lock")); // Claude Code's lock, let go

    // A new entry gets the defaults Claude Code's own prompt writes.
    const std::string other = top + "/other";
    REQUIRE(file::makeDirs(other + "/.git"));
    REQUIRE(trustFolder(other));
    REQUIRE(doc.parseFile(cfg));
    CHECK(doc.root()["projects"][other]["hasTrustDialogAccepted"].boolean());
    CHECK(doc.root()["projects"][other]["mcpServers"].isObject());

    // Claude Code holding its lock: waited for, never broken while fresh.
    REQUIRE(file::makeDirs(cfg + ".lock"));
    const std::string third = top + "/third";
    REQUIRE(file::makeDirs(third + "/.git"));
    const int64_t t0 = base::nowMicros();
    CHECK_FALSE(trustFolder(third)); // a live holder: gives up after a while
    CHECK(base::nowMicros() - t0 >= 3'000'000);
    CHECK(file::exists(cfg + ".lock"));
    file::remove(cfg + ".lock");
    // An unreadable config is never overwritten.
    REQUIRE(file::writeAtomic(cfg, "{broken"));
    CHECK_FALSE(trustFolder(third));
    std::string text;
    CHECK(file::readAll(cfg, &text));
    CHECK_STR(text, "{broken");
    // None at all: one is made.
    file::remove(cfg);
    REQUIRE(trustFolder(third));
    CHECK(isFolderTrusted(third));
    base::test::removeTree(out);

    if (old.empty())
        base::test::unsetEnv("CLAUDE_CONFIG_DIR");
    else
        base::test::setEnv("CLAUDE_CONFIG_DIR", old);
}

TEST("roster: one place derives a transcript's subagents folder") {
    using claude::Paths;
    CHECK_STR(Paths::subagentsDir("/p/q/abc-123.jsonl"), "/p/q/abc-123/subagents");
    CHECK_STR(Paths::subagentsDir("/p/q/name.v2.jsonl"), "/p/q/name.v2/subagents");
    CHECK_STR(Paths::subagentsDir("/p/q/.hidden"), "/p/q/.hidden/subagents");
    // Paths::subagentTranscript files its agents in exactly that folder.
    CHECK_STR(
        Paths::subagentTranscript("/p/q/abc.jsonl", "7"),
        Paths::subagentsDir("/p/q/abc.jsonl") + "/agent-7.jsonl"
    );
}

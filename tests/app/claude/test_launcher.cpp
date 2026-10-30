// The CLI side: Launcher (start, resume, fork, stop, remove, login, commands,
// typing into a live worker) against fake `claude` shell scripts in a
// throwaway CLAUDE_CONFIG_DIR, what it parses from the CLI's answers, and the
// workspace add flow (cli.h). The real `claude` is never run.
#include "app/claude/cli.h"
#include "app/claude/launcher.h"
#include "app/claude/roster.h"
#include "base/file.h"
#include "base/process.h"
#include "base/str.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/plat.h"
#include "app/model/jobs.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h> // mkdtemp (macOS)
#include <vector>

using namespace claude;

std::string fakeAttachProgram(); // test_terminal.cpp: this binary as `claude attach`

namespace {

std::string tempDir() {
    return base::test::makeTempDir("claude_launcher_test_");
}

void removeTree(const std::string &dir) {
    base::test::removeTree(dir);
}

#ifndef _WIN32 // for the script-driven cases below
// A case's own App: the workers still under way (they post to it) are done
// before it goes.
struct AppDeleter {
    void operator()(plat::App *a) const {
        model::waitBackground();
        delete a;
    }
};
using TestApp = std::unique_ptr<plat::App, AppDeleter>;

TestApp headlessApp() {
    base::test::setEnv("PLAT_BACKEND", "headless");
    std::string err;
    TestApp     app(plat::App::create(&err).release());
    if (!app)
        std::fprintf(stderr, "    plat::App::create: %s\n", err.c_str());
    return app;
}

bool waitFor(plat::App &app, const std::function<bool()> &pred, int ms = 10'000) {
    const int64_t until = base::monotonicMs() + ms;
    while (!pred() && base::monotonicMs() < until)
        app.pump(10);
    return pred();
}

#endif

std::string readText(const std::string &path) {
    std::string s;
    file::readAll(path, &s);
    return s;
}

std::vector<std::string> lines(const std::string &text) {
    std::vector<std::string> out;
    size_t                   at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos)
            nl = text.size();
        if (nl > at)
            out.push_back(text.substr(at, nl - at));
        at = nl + 1;
    }
    return out;
}

bool writeScript(const std::string &path, const std::string &text) {
    return file::writeAtomic(path, text, 0755);
}

// Stand-in for the CLI, following what Claude Code 2.1.282 was seen doing:
// `--bg … -- <prompt>` creates a job + worker (a real process: a stop waits
// for it to exit) and answers, printing "backgrounded · <short>"; `stop
// <short>` ends the worker; `rm <short>` too, and drops the job — or keeps it
// (rm-refuse present); `--bg --resume <id> -- <prompt>` continues — or, with
// a copy-next file present, starts a copy of the session and continues there;
// `--fork-session` branches into a new one; with fail-next present a call
// fails. `-p` answers the initialize request on stdin; `attach` runs the fake
// terminal UI ($FAKE_ATTACH). Every other call is logged to calls.log.
constexpr const char *kFakeCli = R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
if [ "$1" = attach ]; then # typing into a live worker: see test_terminal
  printf '%s\n' "$*" >> "$H/attach.log"
  MSGA_FAKE_ATTACH_RUN="$2" exec "$FAKE_ATTACH"
fi
if [ "$1" = -p ]; then
  printf '%s\n' "$*" > "$H/print-args.log"
  read -r req
  printf '%s\n' "$req" > "$H/init-request.log"
  echo "a warning on stderr" >&2
  case "$req" in *'"initialize"'*)
    echo '{"type":"system","subtype":"init"}'
    echo '{"type":"control_response","response":{"subtype":"success","request_id":"msga","response":{"commands":[{"name":"compact","description":"Free up context","argumentHint":"<instructions>","builtin":true},{"name":"verify","description":"Drive the app (project)"}],"account":{"email":"me@example.com","organization":"Org","subscriptionType":"max","apiProvider":"firstParty"}}}}'
  ;; esac
  exit 0
fi
printf '%s\n' "$*" >> "$H/calls.log" # echo would expand \n
if [ "$1" = stop ] || [ "$1" = rm ]; then
  rm -f "$H/sessions/w$2.json"
  kill $(cat "$H/wpid-$2" 2>/dev/null) 2>/dev/null
  if [ "$1" = rm ]; then
    if [ -f "$H/rm-refuse" ]; then
      printf 'kept %s — its worktree is still at /w\n  it has uncommitted changes\n  claude rm %s --discard\n' "$2" "$2"
      exit 1
    fi
    rm -rf "$H/jobs/$2"; echo "removed $2"; exit 0
  fi
  echo "stopped $2"; exit 0
fi
if [ -f "$H/fail-next" ]; then rm -f "$H/fail-next"; echo "error: it broke"; exit 1; fi
sid=""; prompt=""; fork=""; copied=""
while [ $# -gt 0 ]; do
  case "$1" in
    --resume) sid="$2"; shift ;;
    --fork-session) fork=1 ;;
    --) prompt="$2"; shift ;;
  esac; shift
done
[ -n "$fork" ] && sid=""
if [ -n "$sid" ] && [ -f "$H/copy-next" ]; then rm -f "$H/copy-next"; copied=1; sid=""; fi
if [ -z "$sid" ]; then
  n=$(cat "$H/counter" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$H/counter"
  sid="abcdef1$n-0000-4000-8000-00000000000$n"
fi
short=$(echo "$sid" | cut -c1-8)
mkdir -p "$H/jobs/$short" "$H/sessions"
echo "{\"state\":\"done\",\"sessionId\":\"$sid\",\"cwd\":\"$PWD\",\"name\":\"fake-$short\"}" > "$H/jobs/$short/state.json"
kill $(cat "$H/wpid-$short" 2>/dev/null) 2>/dev/null
sleep 60 </dev/null >/dev/null 2>&1 &
echo $! > "$H/wpid-$short"
echo "{\"pid\":$!,\"sessionId\":\"$sid\",\"kind\":\"bg\",\"status\":\"idle\"}" > "$H/sessions/w$short.json"
printf '%s\n' "$prompt" >> "$H/prompts-$short.log"
[ -n "$copied" ] && echo "Worker still running; started a copy of the session."
echo "backgrounded · $short · fake task"
)SH";

// A throwaway Claude Code home with the fake CLI in it.
struct FakeCli {
    std::string home = tempDir();
    std::string cli  = home + "/bin/claude";
    std::string work = home + "/work";
    std::string oldConfig;
    Paths       paths;

    FakeCli() {
        oldConfig = base::env("CLAUDE_CONFIG_DIR");
        base::test::setEnv("CLAUDE_CONFIG_DIR", home);
        base::test::setEnv("FAKE_ATTACH", fakeAttachProgram());
        file::makeDirs(work);
        file::makeDirs(home + "/sessions");
        file::makeDirs(home + "/jobs");
        writeScript(cli, kFakeCli);
        paths.home = home;
    }
    ~FakeCli() {
        // The workers still idling.
        std::vector<file::DirEntry> entries;
        file::listDir(home, &entries);
        for (const auto &e : entries)
            if (str::startsWith(e.name, "wpid-")) {
                const int64_t pid = std::atoll(text(e.name.c_str()).c_str());
                if (pid > 0)
                    base::test::killProcess(pid);
            }
        base::test::unsetEnv("FAKE_ATTACH");
        if (oldConfig.empty())
            base::test::unsetEnv("CLAUDE_CONFIG_DIR");
        else
            base::test::setEnv("CLAUDE_CONFIG_DIR", oldConfig);
        removeTree(home);
    }
    std::string              text(const char *name) const { return readText(home + "/" + name); }
    std::vector<std::string> calls() const { return lines(text("calls.log")); }
    void    touch(const char *name) const { file::writeAtomic(home + "/" + name, ""); }
    int64_t workerPid(const char *shortId) const {
        return std::atoll(text((std::string("wpid-") + shortId).c_str()).c_str());
    }
};

#ifndef _WIN32
struct DoneResult {
    bool        called = false;
    std::string id, error;
};

Launcher::Done collect(DoneResult &r) {
    return [&r](std::string id, std::string error) {
        r.called = true;
        r.id     = std::move(id);
        r.error  = std::move(error);
    };
}

// A `claude` that only answers `auth status`, as logged in or not; anything
// else is logged to calls.log (nothing may get that far when logged out).
std::string fakeLoginCli(const std::string &dir, bool loggedIn) {
    const std::string cli = dir + "/claude";
    const bool        ok  = writeScript(
        cli,
        std::string("#!/bin/sh\n") +
            (loggedIn ? "[ \"$1\" = auth ] && { echo '{\"loggedIn\":true}'; exit 0; }\n"
                      : "[ \"$1\" = auth ] && { echo '{\"loggedIn\":false}'; exit 1; }\n") +
            "printf '%s\\n' \"$*\" >> \"$CLAUDE_CONFIG_DIR/calls.log\"\nexit 1\n"
    );
    return ok ? cli : std::string();
}

#endif

const char *const kSid1 = "abcdef11-0000-4000-8000-000000000001";
const char *const kSid2 = "abcdef12-0000-4000-8000-000000000002";
const char *const kSid3 = "abcdef13-0000-4000-8000-000000000003";

} // namespace

// ── What the CLI says ───────────────────────────────────────────────────────

TEST("launcher: Claude Code's command list") {
    const std::string out =
        R"j({"type":"control_response","response":{"subtype":"success","request_id":"msga",)j"
        R"j("response":{"commands":[)j"
        R"j({"name":"compact","description":"Free up context","argumentHint":"<instructions>","builtin":true},)j"
        R"j({"name":"verify","description":"Drive the app (project)","argumentHint":""},)j"
        R"j({"name":"gui-sudo","description":"Run a root command (user)","argumentHint":""},)j"
        R"j({"name":"__remote-workflow","description":"internal","builtin":true},)j"
        R"j({"name":"agents","description":"(removed) Ask Claude","builtin":true},)j"
        R"j({"name":"extra-usage","description":"Renamed to /usage-credits","builtin":true}],)j"
        R"j("account":{"email":"me@example.com","organization":"Org","subscriptionType":"max"}}}})j"
        "\n";
    const auto cmds = parseCommandList(out);
    REQUIRE(cmds.size() == 3);
    CHECK_STR(cmds[0].name, "compact");
    CHECK_STR(cmds[0].usage, "<instructions>");
    CHECK_STR(cmds[0].source, "Claude Code");
    CHECK_STR(cmds[1].desc, "Drive the app");
    CHECK_STR(cmds[1].source, "Project skill");
    CHECK_STR(cmds[2].desc, "Run a root command");
    CHECK_STR(cmds[2].source, "Skill");
    CHECK(parseCommandList("not json\n").empty());
    const Account a = parseAccount("{\"type\":\"system\"}\n" + out);
    CHECK_STR(a.email, "me@example.com");
    CHECK_STR(a.organization, "Org");
    CHECK_STR(a.subscriptionType, "max");
    CHECK(a.apiProvider.empty());
    CHECK(parseAccount("nothing\n").empty());
}

TEST("launcher: claude rm says why it kept a session") {
    CHECK(
        parseRemoveRefusal("removed abcdef11\n  worktree: /src/x/.claude/worktrees/w\n", 0).empty()
    );
    CHECK_STR(
        parseRemoveRefusal(
            "kept abcdef11 — its worktree is still at /src/x/.claude/worktrees/w\n"
            "  it has uncommitted changes\n"
            "  claude rm abcdef11 --discard-unpushed 1a2b@3c4d\n",
            1
        ),
        "it has uncommitted changes"
    );
    CHECK_STR(
        parseRemoveRefusal("kept abcdef11 — its worktree is still at /w\n", 1),
        "its worktree is still at /w"
    );
    CHECK_STR(
        parseRemoveRefusal("couldn't remove abcdef11 — its worker didn't stop\n", 1),
        "its worker didn't stop"
    );
    CHECK_STR(
        parseRemoveRefusal("error: unknown command 'rm'\n", 1), "error: unknown command 'rm'"
    );
    CHECK_FALSE(parseRemoveRefusal("", 1).empty());
}

TEST("launcher: the CLI's login status") {
    // `claude auth status` (verified 2.1.283): JSON, exit code 1 when logged out.
    CHECK(parseLoginStatus(R"({"loggedIn": true, "authMethod": "claude.ai"})", 0) == Login::In);
    CHECK(parseLoginStatus(R"({"loggedIn": true, "authMethod": "api_key"})", 0) == Login::In);
    CHECK(parseLoginStatus(R"({"loggedIn": false, "authMethod": "none"})", 1) == Login::Out);
    CHECK(parseLoginStatus("a warning first\n{\"loggedIn\": false}\n", 1) == Login::Out);
    CHECK(
        parseLoginStatus("Not logged in. Run claude auth login to authenticate.\n", 1) == Login::Out
    );
    // A CLI without `auth status` can't tell: that never blocks anything.
    CHECK(parseLoginStatus("error: unknown command 'auth'\n", 1) == Login::Unknown);
    CHECK(parseLoginStatus("", 0) == Login::Unknown);
}

TEST("launcher: the session the CLI backgrounded, and a copy it started") {
    CHECK_STR(parseBackgroundedShortId("backgrounded · 1a2b3c4d · fix the build\n"), "1a2b3c4d");
    CHECK_STR(parseBackgroundedShortId("woke session\nbackgrounded·abcdef11\n"), "abcdef11");
    CHECK_STR(
        parseBackgroundedShortId("backgrounded · xyz\nbackgrounded · 0123456789"), "0123456789"
    );
    CHECK(parseBackgroundedShortId("backgrounded · abc12\n").empty()); // too short
    CHECK(parseBackgroundedShortId("error: not trusted\n").empty());
    CHECK(startedACopy("Worker still running; started a copy of the session.\n"));
    CHECK_FALSE(startedACopy("backgrounded · abcdef11"));
}

// ── Against a fake CLI ──────────────────────────────────────────────────────

// POSIX only from here: these cases drive a fake `claude` written as a
// /bin/sh script (and real `sleep` workers); Windows runs no such script.
#ifndef _WIN32

TEST("launcher: start, then stop + resume per turn, a copy, a fork") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeCli  f;
    Launcher launcher(*app, f.cli, f.paths);

    DoneResult started;
    launcher.start(f.work, "first", false, "Be brief.", "{}", collect(started));
    CHECK_FALSE(started.called); // never re-entrantly
    REQUIRE(waitFor(*app, [&] { return started.called; }));
    CHECK_STR(started.error, "");
    CHECK_STR(started.id, kSid1);
    CHECK_STR(launcher.sessionIdForShort("abcdef11"), kSid1);
    const int64_t worker1 = f.workerPid("abcdef11");
    REQUIRE(worker1 > 0);
    CHECK(isProcessAlive(worker1));
    CHECK(hasLiveWorker(f.paths, kSid1));

    // The worker idles on after a turn: stopped (and waited for) first, then
    // resumed without flags, so the session keeps its options.
    DoneResult resumed;
    launcher.resume(kSid1, f.work, "second", true, false, collect(resumed));
    REQUIRE(waitFor(*app, [&] { return resumed.called; }));
    CHECK_STR(resumed.error, "");
    CHECK_STR(resumed.id, kSid1);
    CHECK_FALSE(isProcessAlive(worker1));

    // Claude Code started a copy after all (a resume racing the old worker's
    // exit, seen 2026-09-25): the conversation goes on in the copy.
    f.touch("copy-next");
    DoneResult copied;
    launcher.resume(kSid1, f.work, "third", true, false, collect(copied));
    REQUIRE(waitFor(*app, [&] { return copied.called; }));
    CHECK_STR(copied.error, "");
    CHECK_STR(copied.id, kSid2);

    // A fork branches off: a new session; the original's worker goes on.
    const int64_t worker2 = f.workerPid("abcdef12");
    DoneResult    forked;
    launcher.fork(kSid2, f.work, "fourth", collect(forked));
    REQUIRE(waitFor(*app, [&] { return forked.called; }));
    CHECK_STR(forked.id, kSid3);
    CHECK(isProcessAlive(worker2));

    // A session that was never a background one takes flags, and nothing is
    // stopped for it.
    DoneResult first;
    launcher.resume(
        "fedcba98-0000-4000-8000-000000000009", f.work, "fifth", false, false, collect(first)
    );
    REQUIRE(waitFor(*app, [&] { return first.called; }));
    CHECK_STR(first.id, "fedcba98-0000-4000-8000-000000000009");

    // A failure says what the CLI said.
    f.touch("fail-next");
    DoneResult failed;
    launcher.resume(kSid3, f.work, "sixth", true, false, collect(failed));
    REQUIRE(waitFor(*app, [&] { return failed.called; }));
    CHECK(failed.id.empty());
    CHECK_STR(failed.error, "error: it broke");

    const auto calls = f.calls();
    REQUIRE(calls.size() == 9);
    CHECK_STR(
        calls[0],
        "--bg --disallowedTools AskUserQuestion --append-system-prompt Be brief. --agents {} -- "
        "first"
    );
    CHECK_STR(calls[1], "stop abcdef11");
    CHECK_STR(calls[2], std::string("--bg --resume ") + kSid1 + " -- second");
    CHECK_STR(calls[3], "stop abcdef11");
    CHECK_STR(calls[4], std::string("--bg --resume ") + kSid1 + " -- third");
    CHECK_STR(calls[5], std::string("--bg --resume ") + kSid2 + " --fork-session -- fourth");
    CHECK_STR(
        calls[6],
        "--bg --resume fedcba98-0000-4000-8000-000000000009 --disallowedTools AskUserQuestion -- "
        "fifth"
    );
    CHECK_STR(calls[7], "stop abcdef13"); // the fork's worker was alive
    CHECK_STR(calls[8], std::string("--bg --resume ") + kSid3 + " -- sixth");
}

TEST("launcher: a fork can go without tools, the question tool too") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeCli    f;
    Launcher   launcher(*app, f.cli, f.paths);
    DoneResult forked;
    launcher.fork(
        kSid1, f.work, "@/c/a.png what?", collect(forked), {"Bash", "Edit", "AskUserQuestion"}
    );
    REQUIRE(waitFor(*app, [&] { return forked.called; }));
    CHECK_STR(forked.error, "");
    CHECK_STR(forked.id, kSid1); // the fake's first new session
    const auto calls = f.calls();
    REQUIRE(calls.size() == 1);
    // A list option: the prompt comes after "--", its mention first.
    CHECK_STR(
        calls[0],
        std::string("--bg --resume ") + kSid1 +
            " --fork-session --disallowedTools AskUserQuestion Bash Edit -- @/c/a.png what?"
    );
}

TEST("launcher: the start settings go with every start that saves options") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeCli  f;
    Launcher launcher(*app, f.cli, f.paths);
    launcher.setStartSettings("{}");
    DoneResult started, resumed, forked, first;
    launcher.start(f.work, "first", false, {}, {}, collect(started));
    REQUIRE(waitFor(*app, [&] { return started.called; }));
    launcher.resume(kSid1, f.work, "second", true, false, collect(resumed));
    REQUIRE(waitFor(*app, [&] { return resumed.called; }));
    launcher.fork(kSid1, f.work, "third", collect(forked));
    REQUIRE(waitFor(*app, [&] { return forked.called; }));
    launcher.resume(
        "fedcba98-0000-4000-8000-000000000009", f.work, "fourth", false, false, collect(first)
    );
    REQUIRE(waitFor(*app, [&] { return first.called; }));
    const auto calls = f.calls();
    REQUIRE(calls.size() == 5);
    CHECK_STR(calls[0], "--bg --disallowedTools AskUserQuestion --settings {} -- first");
    CHECK_STR(calls[1], "stop abcdef11");
    // A background session keeps what it was started with: none again.
    CHECK_STR(calls[2], std::string("--bg --resume ") + kSid1 + " -- second");
    CHECK_STR(
        calls[3], std::string("--bg --resume ") + kSid1 + " --fork-session --settings {} -- third"
    );
    CHECK_STR(
        calls[4],
        "--bg --resume fedcba98-0000-4000-8000-000000000009 --disallowedTools AskUserQuestion "
        "--settings {} -- fourth"
    );
}

TEST("launcher: stop waits for the worker; remove says why it kept a job") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeCli    f;
    Launcher   launcher(*app, f.cli, f.paths);
    DoneResult started;
    launcher.start(f.work, "first", true, {}, {}, collect(started));
    REQUIRE(waitFor(*app, [&] { return started.called; }));
    REQUIRE(started.id == kSid1);
    CHECK_STR(
        f.calls()[0],
        "--bg --disallowedTools AskUserQuestion --dangerously-skip-permissions -- first"
    );
    const int64_t worker = f.workerPid("abcdef11");

    bool stopped = false;
    launcher.stop(kSid1, f.work, [&] { stopped = true; });
    REQUIRE(waitFor(*app, [&] { return stopped; }));
    CHECK_FALSE(isProcessAlive(worker));
    CHECK_FALSE(hasLiveWorker(f.paths, kSid1));
    CHECK_STR(f.calls().back(), "stop abcdef11");

    // Kept: the job stays, and the first reason given is the one passed on.
    f.touch("rm-refuse");
    std::optional<std::string> refusal;
    launcher.remove(kSid1, {}, f.work, [&](std::string why) { refusal = std::move(why); });
    REQUIRE(waitFor(*app, [&] { return refusal.has_value(); }));
    CHECK_STR(*refusal, "it has uncommitted changes");
    CHECK(file::exists(f.home + "/jobs/abcdef11/state.json"));

    file::remove(f.home + "/rm-refuse");
    refusal.reset();
    launcher.remove(kSid1, "abcdef11", f.work, [&](std::string why) { refusal = std::move(why); });
    REQUIRE(waitFor(*app, [&] { return refusal.has_value(); }));
    CHECK_STR(*refusal, "");
    CHECK_FALSE(file::exists(f.home + "/jobs/abcdef11"));
    CHECK_STR(f.calls().back(), "rm abcdef11");
}

TEST("launcher: the login and the command list are asked of the CLI") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeCli f;
    {
        Launcher             launcher(*app, f.cli, f.paths);
        std::optional<Login> login;
        launcher.checkLogin([&](Login l) { login = l; });
        REQUIRE(waitFor(*app, [&] { return login.has_value(); }));
        CHECK(*login == Login::In);

        bool                      listed = false;
        std::vector<SlashCommand> cmds;
        Account                   account;
        launcher.listCommands(f.work, [&](std::vector<SlashCommand> c, Account a) {
            listed  = true;
            cmds    = std::move(c);
            account = std::move(a);
        });
        REQUIRE(waitFor(*app, [&] { return listed; }));
        REQUIRE(cmds.size() == 2);
        CHECK_STR(cmds[0].name, "compact");
        CHECK_STR(cmds[1].source, "Project skill");
        CHECK_STR(account.email, "me@example.com");
        CHECK_STR(account.apiProvider, "firstParty");
        // Print mode, asked only to initialize: no session saved, no MCP servers.
        CHECK_STR(
            f.text("print-args.log"),
            "-p --input-format stream-json --output-format stream-json --verbose "
            "--no-session-persistence --strict-mcp-config\n"
        );
        CHECK_STR(
            f.text("init-request.log"),
            "{\"type\":\"control_request\",\"request_id\":\"msga\",\"request\":{\"subtype\":"
            "\"initialize\"}}\n"
        );
    }
    {
        // Logged out; and a CLI that can't tell, or isn't there: unknown.
        const std::string bin = f.home + "/bin2";
        file::makeDirs(bin);
        std::optional<Login> login;
        Launcher             out(*app, fakeLoginCli(bin, false), f.paths);
        out.checkLogin([&](Login l) { login = l; });
        REQUIRE(waitFor(*app, [&] { return login.has_value(); }));
        CHECK(*login == Login::Out);
        login.reset();
        writeScript(
            bin + "/old", "#!/bin/sh\necho \"error: unknown command 'auth'\" >&2\nexit 1\n"
        );
        Launcher old(*app, bin + "/old", f.paths);
        old.checkLogin([&](Login l) { login = l; });
        REQUIRE(waitFor(*app, [&] { return login.has_value(); }));
        CHECK(*login == Login::Unknown);
        login.reset();
        Launcher missing(*app, bin + "/missing", f.paths);
        missing.checkLogin([&](Login l) { login = l; });
        REQUIRE(waitFor(*app, [&] { return login.has_value(); }));
        CHECK(*login == Login::Unknown);

        // A CLI that can't start says so; the command list is empty then.
        DoneResult started;
        missing.start(f.work, "x", false, {}, {}, collect(started));
        REQUIRE(waitFor(*app, [&] { return started.called; }));
        CHECK(started.id.empty());
        CHECK(str::startsWith(started.error, "Couldn't start missing: "));
        bool listed = false;
        missing.listCommands(f.work, [&](std::vector<SlashCommand> c, Account a) {
            listed = c.empty() && a.empty();
        });
        CHECK(waitFor(*app, [&] { return listed; }));
    }
    {
        // A Launcher gone before the CLI answers: no callback.
        bool called   = false;
        auto launcher = std::make_unique<Launcher>(*app, f.cli, f.paths);
        launcher->checkLogin([&](Login) { called = true; });
        launcher.reset();
        const int64_t until = base::monotonicMs() + 1000;
        while (base::monotonicMs() < until)
            app->pump(10);
        CHECK_FALSE(called);
    }
}

TEST("launcher: a message is typed into a live worker through claude attach") {
    auto app = headlessApp();
    REQUIRE(app);
    FakeCli    f;
    Launcher   launcher(*app, f.cli, f.paths);
    DoneResult started;
    launcher.start(f.work, "first", false, {}, {}, collect(started));
    REQUIRE(waitFor(*app, [&] { return started.called && !started.id.empty(); }));
    const int64_t worker = f.workerPid("abcdef11");

    std::string              program;
    std::vector<std::string> argv;
    launcher.attachCommand(kSid1, program, argv);
    CHECK_STR(program, f.cli);
    CHECK((argv == std::vector<std::string>{"attach", "abcdef11"}));

    std::optional<AttachInput::Outcome> outcome;
    std::string                         detail;
    std::weak_ptr<AttachInput>          typing = launcher.sendLive(
        kSid1, f.work, "second, typed", [&](AttachInput::Outcome o, std::string d) {
            outcome = o;
            detail  = std::move(d);
        }
    );
    CHECK_FALSE(typing.expired());
    REQUIRE(waitFor(*app, [&] { return outcome.has_value(); }, 15'000));
    CHECK(*outcome == AttachInput::Outcome::Sent);
    CHECK_STR(detail, "");
    CHECK_STR(f.text("typed.log"), "second, typed\n");
    CHECK_STR(f.text("attach.log"), "attach abcdef11\n");
    CHECK(f.calls().size() == 1);  // no stop, no resume
    CHECK(isProcessAlive(worker)); // the worker (and what it runs) lives on
    CHECK(waitFor(*app, [&] { return typing.expired(); }, 8000));
}

// ── Adding the workspace ────────────────────────────────────────────────────

TEST("launcher: adding the workspace needs the CLI and its login") {
    auto app = headlessApp();
    REQUIRE(app);
    const std::string dir = tempDir();
    const std::string bin = dir + "/bin", fakeHome = dir + "/home", config = dir + "/config";
    file::makeDirs(bin);
    file::makeDirs(fakeHome);
    file::makeDirs(config);
    // Only `bin` is searched: PATH, and the installers' folders under HOME.
    const std::string path = base::env("PATH"), realHome = base::env("HOME"),
                      oldConfig = base::env("CLAUDE_CONFIG_DIR");
    base::test::setEnv("PATH", bin);
    base::test::setEnv("HOME", fakeHome);
    base::test::setEnv("CLAUDE_CONFIG_DIR", config);
    struct Restore {
        std::string path, home, config, dir;
        ~Restore() {
            base::test::setEnv("PATH", path);
            base::test::setEnv("HOME", home);
            if (config.empty())
                base::test::unsetEnv("CLAUDE_CONFIG_DIR");
            else
                base::test::setEnv("CLAUDE_CONFIG_DIR", config);
            removeTree(dir);
        }
    } restore{path, realHome, oldConfig, dir};
    if (!findClaudeExecutable().empty()) {
        std::fprintf(
            stderr,
            "    skipped: a claude outside PATH and HOME (/usr/local/bin, /opt/homebrew/bin)\n"
        );
        return;
    }

    struct Outcome {
        bool        done = false;
        Credentials creds;
        std::string error;
    };
    auto add = [&] {
        Outcome o;
        checkSetup(*app, [&o](Credentials c, std::string e) {
            o.done  = true;
            o.creds = std::move(c);
            o.error = std::move(e);
        });
        CHECK_FALSE(o.done); // never re-entrantly
        waitFor(*app, [&] { return o.done; }, 5000);
        return o;
    };

    Outcome o = add();
    CHECK(o.done);
    CHECK_STR(o.error, notInstalledMessage());

    REQUIRE(!fakeLoginCli(bin, false).empty());
    o = add();
    CHECK_STR(o.error, notLoggedInMessage());
    CHECK_FALSE(file::exists(config + "/calls.log")); // nothing else was asked

    // Claude Code never ran here: its folder is missing.
    base::test::setEnv("CLAUDE_CONFIG_DIR", dir + "/nowhere");
    o = add();
    CHECK(o.error.find(dir + "/nowhere") != std::string::npos);
    base::test::setEnv("CLAUDE_CONFIG_DIR", config);

    const std::string cli = fakeLoginCli(bin, true);
    REQUIRE(!cli.empty());
    o = add();
    CHECK(o.done);
    CHECK_STR(o.error, "");
    CHECK_STR(o.creds.claudePath, cli);

    // The workspace record keeps where the CLI is; one that moved is looked for again.
    const auth::WorkspaceRecord rec = toRecord(o.creds);
    CHECK_STR(rec.service, "claude-code");
    CHECK_STR(rec.id, "local");
    CHECK_STR(rec.displayName, "Claude Code");
    CHECK(fromRecord(rec) == o.creds);
    auth::WorkspaceRecord moved = rec;
    moved.auth                  = R"({"claudePath":"/gone/claude"})";
    CHECK_STR(fromRecord(moved).claudePath, cli);
    // An installer's folder under HOME is searched when PATH has none.
    file::remove(cli);
    REQUIRE(!fakeLoginCli(fakeHome + "/.local/bin", true).empty());
    CHECK_STR(findClaudeExecutable(), fakeHome + "/.local/bin/claude");
}

#endif // !_WIN32

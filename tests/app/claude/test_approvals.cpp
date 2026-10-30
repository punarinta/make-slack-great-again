// Permission questions through Claude Code's PermissionRequest hook: what the
// hook is given (as Claude Code 2.1.314 gave it), the options and answers made
// of it, and the files the hook and msga pass them through.
#include "app/claude/approvals.h"

#include "base/file.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "support/test.h"

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>

using namespace claude;

namespace {

std::string tempDir() {
    return base::test::makeTempDir("approvals-", base::env("HOME"));
}

// As 2.1.314 handed it to the hook in auto mode (a symlink resolving outside
// the project), and with a Bash rule to offer (default mode).
const char *kDirectories = R"({"session_id":"2b6ef376-0000-4000-8000-000000000001",
  "cwd":"/w/probe","permission_mode":"auto","hook_event_name":"PermissionRequest",
  "tool_name":"Bash","tool_input":{"command":"rm -f nm","description":"Remove nm"},
  "permission_suggestions":[{"type":"addDirectories","directories":["/w/outside"],
  "destination":"session"}]})";
const char *kRules       = R"({"session_id":"0255664e-0000-4000-8000-000000000002",
  "cwd":"/w/probe","permission_mode":"default","hook_event_name":"PermissionRequest",
  "tool_name":"Bash","tool_input":{"command":"mkdir -p /w/o/sub && date > /w/o/sub/s"},
  "permission_suggestions":[{"type":"addRules","rules":[{"toolName":"Bash",
  "ruleContent":"mkdir -p /w/o/sub"},{"toolName":"Bash","ruleContent":"date"}],
  "behavior":"allow","destination":"localSettings"},{"type":"replaceRules","rules":[],
  "behavior":"deny","destination":"session"}]})";

json::Value decisionOf(json::Document &doc, const std::string &answer) {
    CHECK(doc.parse(answer));
    CHECK_STR(doc.root()["hookSpecificOutput"]["hookEventName"].str(), "PermissionRequest");
    return doc.root()["hookSpecificOutput"]["decision"];
}

} // namespace

TEST("approvals: a hook's question becomes the dialog's options") {
    HookQuestion q;
    REQUIRE(parseHookQuestion(kDirectories, &q));
    CHECK_STR(q.sessionId, "2b6ef376-0000-4000-8000-000000000001");
    REQUIRE(q.options.size() == 3);
    CHECK_STR(q.options[0].label, "Yes");
    CHECK_STR(q.options[1].label, "Yes, and always allow access to /w/outside from this project");
    CHECK_STR(q.options[2].label, "No");
    {
        json::Document d;
        const auto     v = decisionOf(d, q.options[0].answer);
        CHECK_STR(v["behavior"].str(), "allow");
        CHECK_FALSE(v.has("updatedPermissions"));
    }
    {
        // The suggestion goes back as Claude Code gave it.
        json::Document d;
        const auto     v = decisionOf(d, q.options[1].answer);
        CHECK_STR(v["behavior"].str(), "allow");
        REQUIRE(v["updatedPermissions"].size() == 1);
        CHECK_STR(v["updatedPermissions"][0]["type"].str(), "addDirectories");
        CHECK_STR(v["updatedPermissions"][0]["directories"][0].str(), "/w/outside");
        CHECK_STR(v["updatedPermissions"][0]["destination"].str(), "session");
    }
    {
        // "No" ends the turn, as the dialog's does.
        json::Document d;
        const auto     v = decisionOf(d, q.options[2].answer);
        CHECK_STR(v["behavior"].str(), "deny");
        CHECK(v["interrupt"].boolean());
    }

    // Bash rules read as the dialog words them; a kind it doesn't offer isn't.
    REQUIRE(parseHookQuestion(kRules, &q));
    REQUIRE(q.options.size() == 3);
    CHECK_STR(
        q.options[1].label,
        "Yes, and don't ask again for mkdir -p /w/o/sub and date commands in /w/probe"
    );
    {
        json::Document d;
        const auto     v = decisionOf(d, q.options[1].answer);
        CHECK_STR(v["updatedPermissions"][0]["rules"][1]["ruleContent"].str(), "date");
    }

    // Not a permission question.
    CHECK_FALSE(parseHookQuestion(R"({"hook_event_name":"Stop","session_id":"x"})", &q));
    CHECK_FALSE(parseHookQuestion("not json", &q));
}

TEST("approvals: the settings run msga's hook without a shell") {
    const std::string settings = permissionHookSettings("/opt/my msga/msga", "/d/approvals");
    CHECK(hasPermissionHook(settings));
    CHECK_FALSE(hasPermissionHook(R"({"hooks":{}})"));
    json::Document doc;
    REQUIRE(doc.parse(settings));
    const auto h = doc.root()["hooks"]["PermissionRequest"][0]["hooks"][0];
    CHECK_STR(h["type"].str(), "command");
    CHECK_STR(h["command"].str(), "/opt/my msga/msga");
    REQUIRE(h["args"].size() == 2);
    CHECK_STR(h["args"][0].str(), "--claude-permission-hook");
    CHECK_STR(h["args"][1].str(), "/d/approvals");
    CHECK(h["timeout"].integer() >= 24 * 3600);
}

TEST("approvals: the hook waits for msga's answer and prints it") {
    const std::string dir = tempDir() + "/approvals";
    const std::string in = tempDir() + "/in.json", out = tempDir() + "/out.json";
    REQUIRE(file::writeAtomic(in, kRules));
    // The hook's stdin and stdout, as Claude Code would wire them.
    REQUIRE(std::freopen(in.c_str(), "r", stdin));
    std::fflush(stdout);
    const int saved = dup(1);
    const int fd    = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    REQUIRE(fd >= 0);
    dup2(fd, 1);
    ::close(fd);
    std::thread hook([&] { runPermissionHook(dir); });

    std::vector<HookQuestion> pending;
    for (int i = 0; i < 100 && pending.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        pending = pendingHookQuestions(dir);
    }
    bool answered = false;
    if (pending.size() == 1) {
        takeHookQuestion(dir, pending[0].id);
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // still waiting
        answered = answerHookQuestion(dir, pending[0].id, pending[0].options[0].answer);
        // Answered: no longer pending, whatever msga looks at meanwhile.
        CHECK(pendingHookQuestions(dir).empty());
    }
    hook.join();
    std::fflush(stdout);
    dup2(saved, 1);
    ::close(saved);

    REQUIRE(pending.size() == 1);
    CHECK_STR(pending[0].sessionId, "0255664e-0000-4000-8000-000000000002");
    REQUIRE(answered);
    std::string printed;
    REQUIRE(file::readAll(out, &printed));
    CHECK_STR(printed, pending[0].options[0].answer);
    // It cleaned up after itself; an answer for a question gone goes nowhere.
    std::vector<file::DirEntry> left;
    file::listDir(dir, &left);
    CHECK(left.empty());
    CHECK_FALSE(answerHookQuestion(dir, pending[0].id, "x"));
}

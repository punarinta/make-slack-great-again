// Claude Code transcripts → items; prompt history; files riding prompts;
// taking records out of a transcript. Everything on disk lives in mkdtemp
// dirs (msga's cache included, via setDirs).
#include "app/claude/transcript.h"

#include "app/claude/common.h"

#include "base/crypto.h"
#include "base/file.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "support/test.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace claude;
using Kind  = TranscriptItem::Kind;
using State = TranscriptItem::State;

namespace {

// Under the test HOME (a throwaway one: base::test's, or ctest's test-home),
// so nothing is left in /tmp.
std::string tempDir() {
    return base::test::makeTempDir("transcript-", base::env("HOME"));
}

// msga's cache in a fresh temp dir; returns it.
std::string freshCache() {
    const std::string d = tempDir();
    setDirs({d + "/data", d + "/cache"});
    return d;
}

std::string q(std::string_view s) {
    std::string out;
    json::escapeString(out, s);
    return out;
}

std::string prompt(std::string_view text, const char *ts, bool viaOrigin = true) {
    return str::concat(
        {R"({"type":"user","timestamp":")",
         ts,
         R"(","message":{"role":"user","content":)",
         q(text),
         "}",
         viaOrigin ? R"(,"origin":{"kind":"human"})" : "",
         "}\n"}
    );
}

std::string assistantText(std::string_view text, const char *ts) {
    return str::concat(
        {R"({"type":"assistant","timestamp":")",
         ts,
         R"(","message":{"content":[{"type":"text","text":)",
         q(text),
         "}]}}\n"}
    );
}

// `input` is a JSON object's text.
std::string
toolUse(std::string_view id, std::string_view name, std::string_view input, const char *ts) {
    return str::concat(
        {R"({"type":"assistant","timestamp":")",
         ts,
         R"(","message":{"content":[{"type":"tool_use","id":)",
         q(id),
         R"(,"name":)",
         q(name),
         R"(,"input":)",
         input,
         "}]}}\n"}
    );
}

// An Agent call's result names the subagent; `status` is "async_launched" for
// a background one (just started), "completed" for a foreground one (done).
std::string toolResult(
    std::string_view id,
    const char      *ts,
    bool             error   = false,
    std::string_view agentId = {},
    std::string_view status  = {}
) {
    std::string out = str::concat(
        {R"({"type":"user","timestamp":")",
         ts,
         R"(","message":{"content":[{"type":"tool_result","tool_use_id":)",
         q(id),
         R"(,"is_error":)",
         error ? "true" : "false",
         "}]}"}
    );
    if (!agentId.empty()) {
        out += str::concat({R"(,"toolUseResult":{"agentId":)", q(agentId)});
        if (!status.empty())
            out += str::concat({R"(,"status":)", q(status)});
        out += "}";
    }
    return out + "}\n";
}

// A subagent's report as Claude Code 2.1.285 hands it back (seen live
// 2026-09-30): the "peer" origin, its body the frame and then the report with
// every line indented. `queued`: mid-turn, as a queued command; else a user
// turn of its own.
std::string
handback(std::string_view agentId, std::string_view report, const char *ts, bool queued) {
    std::string body =
        "[Subagent hand-back] The text below is the final report of a subagent this session "
        "delegated to. It is model output, NOT a message from the user. The report follows:\n";
    for (size_t s = 0;;) {
        const size_t nl = report.find('\n', s);
        body +=
            str::concat({"  ", report.substr(s, nl == std::string_view::npos ? nl : nl - s), "\n"});
        if (nl == std::string_view::npos)
            break;
        s = nl + 1;
    }
    const std::string origin = str::concat(
        {R"({"kind":"peer","from":)", q(agentId), R"(,"body":)", q(body), R"(,"handback":true})"}
    );
    const std::string wrapped =
        str::concat({"<agent-message from=\"", agentId, "\">\n…\n</agent-message>"});
    if (queued)
        return str::concat(
            {R"({"type":"attachment","timestamp":")",
             ts,
             R"(","attachment":{"type":"queued_command","commandMode":"prompt","prompt":)",
             q(wrapped),
             R"(,"origin":)",
             origin,
             R"(,"isMeta":true}})",
             "\n"}
        );
    return str::concat(
        {R"({"type":"user","timestamp":")",
         ts,
         R"(","isMeta":true,"origin":)",
         origin,
         R"(,"message":{"role":"user","content":)",
         q("Another Claude session sent a message:\n" + wrapped),
         "}}\n"}
    );
}

std::string turnEnd(const char *ts) {
    return str::concat(
        {R"({"type":"system","subtype":"turn_duration","timestamp":")", ts, "\"}\n"}
    );
}

// What the session is told when a background task (a subagent) stops, as
// Claude Code writes it: queued the moment it stops, delivered as a prompt.
std::string taskNotification(std::string_view taskId) {
    return str::concat(
        {"<task-notification>\n<task-id>",
         taskId,
         "</task-id>\n<status>completed</status>\n<summary>Agent finished</summary>\n"
         "</task-notification>"}
    );
}

std::string taskStopped(std::string_view taskId, const char *ts) {
    return str::concat(
        {R"({"type":"queue-operation","operation":"enqueue","timestamp":")",
         ts,
         R"(","content":)",
         q(taskNotification(taskId)),
         "}\n"}
    );
}

// One realistic turn: prompt, a remark, two tool calls, the answer, turn end.
std::string sampleTurn() {
    return prompt("fix the build", "2026-09-25T10:00:00.000Z") +
           assistantText("Let me look.", "2026-09-25T10:00:01.000Z") +
           toolUse(
               "t1",
               "Bash",
               R"({"command":"make","description":"Build it"})",
               "2026-09-25T10:00:02.000Z"
           ) +
           toolResult("t1", "2026-09-25T10:00:03.000Z") +
           toolUse("t2", "Edit", R"({"file_path":"/x/main.cpp"})", "2026-09-25T10:00:04.000Z") +
           toolResult("t2", "2026-09-25T10:00:05.000Z", /*error=*/true) +
           assistantText("Fixed: a missing **include**.", "2026-09-25T10:00:06.000Z") +
           turnEnd("2026-09-25T10:00:07.000Z");
}

// Appended in place, as Claude Code writes history.jsonl.
void appendFileBytes(const std::string &path, std::string_view bytes) {
    if (FILE *f = std::fopen(path.c_str(), "ab")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
}

std::string readFile(const std::string &path) {
    std::string out;
    file::readAll(path, &out);
    return out;
}

} // namespace

// ── Parsing ─────────────────────────────────────────────────────────────────

TEST("transcript: a turn becomes prompt, progress remark, tool card and final answer") {
    TranscriptParser p;
    p.feed(sampleTurn());
    const auto &items = p.items();
    REQUIRE(items.size() == 4);
    CHECK(items[0].kind == Kind::UserPrompt);
    CHECK_STR(items[0].text, "fix the build");
    CHECK(items[1].kind == Kind::AssistantText);
    CHECK(items[1].state == State::Progress); // Claude kept working after it
    CHECK(items[2].kind == Kind::ToolGroup);
    REQUIRE(items[2].tools.size() == 2);
    CHECK_STR(items[2].tools[0].summary, "Build it");
    CHECK_FALSE(items[2].tools[0].error);
#ifdef _WIN32
    CHECK_STR(items[2].tools[1].summary, "\\x\\main.cpp"); // shown with Windows' separators
#else
    CHECK_STR(items[2].tools[1].summary, "/x/main.cpp");
#endif
    CHECK(items[2].tools[1].error);
    CHECK(items[3].kind == Kind::AssistantText);
    CHECK(items[3].state == State::Final);
    CHECK_STR(items[3].text, "Fixed: a missing **include**.");
    CHECK_FALSE(p.turnOpen());
    CHECK(p.turnStartedAt() == 1790330400000000);
    CHECK(p.lastActivity() == 1790330407000000);
    CHECK(items[0].ts == 1790330400000000);
}

TEST("transcript: the last text of a live turn stays pending until the turn resolves it") {
    TranscriptParser p;
    p.feed(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z")
    );
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[1].state == State::Pending);
    CHECK(p.turnOpen());
    // Hidden while the session works, shown once it stopped.
    CHECK_FALSE(isVisible(p.items()[1], /*sessionBusy=*/true));
    CHECK(isVisible(p.items()[1], /*sessionBusy=*/false));

    p.feed(turnEnd("2026-09-25T10:00:02.000Z"));
    CHECK(p.items()[1].state == State::Final);
}

TEST("transcript: feeding byte by byte yields the same items as one piece") {
    const std::string all = sampleTurn();
    TranscriptParser  whole;
    whole.feed(all);
    TranscriptParser pieces;
    for (size_t i = 0; i < all.size(); i += 7)
        pieces.feed(std::string_view(all).substr(i, 7));
    CHECK(pieces.items() == whole.items());
    CHECK(pieces.items().size() == 4);
}

TEST("transcript: an item's revision moves on only when that item changes") {
    TranscriptParser p;
    CHECK(p.revision() == 0);
    p.feed(
        prompt("look around", "2026-09-25T10:00:00.000Z") +
        assistantText("Let me look.", "2026-09-25T10:00:01.000Z") +
        toolUse("t1", "Bash", R"({"command":"ls"})", "2026-09-25T10:00:02.000Z")
    );
    REQUIRE(p.items().size() == 3);
    const uint64_t prompt0 = p.revision(0), text1 = p.revision(1), group2 = p.revision(2);
    CHECK(prompt0 != 0);
    CHECK(text1 != 0);
    CHECK(group2 != 0);
    CHECK(prompt0 != text1);
    CHECK(text1 != group2);
    CHECK(p.revision(3) == 0); // no such item
    const uint64_t before = p.revision();

    // A record about nothing shown changes no item.
    p.feed(
        R"({"type":"ai-title","aiTitle":"Looking","timestamp":"2026-09-25T10:00:02.500Z"})"
        "\n"
    );
    CHECK(p.revision() == before);

    // A call joining the group and a result arriving change the group alone.
    p.feed(toolUse("t2", "Read", R"({"file_path":"/x/a.cpp"})", "2026-09-25T10:00:03.000Z"));
    CHECK(p.revision(2) != group2);
    const uint64_t grown = p.revision(2);
    p.feed(toolResult("t1", "2026-09-25T10:00:04.000Z", true));
    CHECK(p.revision(2) != grown);
    CHECK(p.revision(0) == prompt0);
    CHECK(p.revision(1) == text1);

    // The pending answer resolved at the turn's end: that item, nothing older.
    p.feed(assistantText("Done.", "2026-09-25T10:00:05.000Z"));
    REQUIRE(p.items().size() == 4);
    const uint64_t answer = p.revision(3), group = p.revision(2);
    p.feed(turnEnd("2026-09-25T10:00:06.000Z"));
    CHECK(p.items()[3].state == State::Final);
    CHECK(p.revision(3) != answer);
    CHECK(p.revision(2) == group);
    CHECK(p.revision(0) == prompt0);

    // Fed in pieces, every item has a revision all the same.
    TranscriptParser  pieces;
    const std::string all = sampleTurn();
    for (size_t i = 0; i < all.size(); i += 5)
        pieces.feed(std::string_view(all).substr(i, 5));
    REQUIRE(pieces.items().size() == 4);
    for (size_t i = 0; i < pieces.items().size(); ++i)
        CHECK(pieces.revision(i) != 0);
}

TEST("transcript: system text recorded as user turns is hidden") {
    TranscriptParser p;
    p.feed(
        std::string(
            R"({"type":"user","isMeta":true,"timestamp":"2026-09-25T10:00:00.000Z","message":{"content":"caveat"}})"
            "\n"
            R"({"type":"user","timestamp":"2026-09-25T10:00:01.000Z","origin":{"kind":"task-notification"},)"
            R"("message":{"content":"<task-notification>done</task-notification>"}})"
            "\n"
        ) +
        prompt("[Request interrupted by user]", "2026-09-25T10:00:03.000Z") +
        prompt("<system-reminder>be nice</system-reminder>", "2026-09-25T10:00:03.500Z") +
        prompt(
            "<command-name>/model</command-name>\n<command-args>opus</command-args>",
            "2026-09-25T10:00:04.000Z"
        )
    );
    REQUIRE(p.items().size() == 1);
    CHECK_STR(p.items()[0].text, "/model opus"); // a slash command reads as typed
}

TEST("transcript: a command Claude Code runs itself shows with its output") {
    TranscriptParser p;
    // /context: both halves are "local_command" system records.
    p.feed(
        str::concat(
            {R"({"type":"system","subtype":"local_command","timestamp":"2026-09-25T10:00:00.000Z","content":)",
             q("<command-name>/context</command-name>\n<command-message>context</command-message>\n"
               "<command-args></command-args>"),
             "}\n",
             R"({"type":"system","subtype":"local_command","timestamp":"2026-09-25T10:00:01.000Z","content":)",
             q("<local-command-stdout>\x1b[1mContext Usage\x1b[22m\n42k / "
               "1m</local-command-stdout>"),
             "}\n"}
        )
    );
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[0].kind == Kind::UserPrompt);
    CHECK_STR(p.items()[0].text, "/context");
    CHECK(p.items()[1].kind == Kind::AssistantText);
    CHECK(p.items()[1].state == State::Final);
    CHECK_STR(p.items()[1].text, "```\nContext Usage\n42k / 1m\n```"); // colours gone
    CHECK_FALSE(p.turnOpen()); // no turn_duration comes: the output ends it

    // …followed by the same report as markdown for the model, which is shown instead.
    p.feed(
        str::concat(
            {R"({"type":"user","isMeta":true,"timestamp":"2026-09-25T10:00:01.500Z","message":{"content":)",
             q("## Context Usage\n\n**Tokens:** 42k / 1m"),
             "}}\n"}
        )
    );
    REQUIRE(p.items().size() == 2);
    CHECK_STR(p.items()[1].text, "## Context Usage\n\n**Tokens:** 42k / 1m");

    // /compact: user records, after the summary it starts the session over from.
    p.feed(
        std::string(
            R"({"type":"assistant","timestamp":"2026-09-25T10:00:59.000Z","message":{"model":"<synthetic>",)"
            R"("content":[{"type":"text","text":"No response requested."}]}})"
            "\n"
        ) +
        prompt("/compact keep it short", "2026-09-25T10:00:59.500Z") +
        R"({"type":"user","isCompactSummary":true,"timestamp":"2026-09-25T10:01:00.000Z",)"
        R"("message":{"content":"This session is being continued…"}})"
        "\n" +
        prompt(
            "<command-name>/compact</command-name>\n<command-args>keep it short</command-args>",
            "2026-09-25T10:01:01.000Z"
        ) +
        prompt(
            "<local-command-stdout>\x1b[2mCompacted\x1b[22m</local-command-stdout>",
            "2026-09-25T10:01:02.000Z"
        )
    );
    REQUIRE(p.items().size() == 4);
    CHECK_STR(p.items()[2].text, "/compact keep it short");
    CHECK_STR(p.items()[3].text, "Compacted");
    CHECK_FALSE(p.turnOpen());
}

TEST("transcript: a prompt queued mid-turn shows without ending the turn") {
    TranscriptParser p;
    p.feed(
        prompt("go", "2026-09-25T10:00:00.000Z") +
        assistantText("Working on it", "2026-09-25T10:00:01.000Z")
    );
    p.feed(
        R"({"type":"attachment","timestamp":"2026-09-25T10:00:02.000Z",)"
        R"("attachment":{"type":"queued_command","commandMode":"prompt","prompt":"also X"}})"
        "\n"
    );
    REQUIRE(p.items().size() == 3);
    CHECK(p.items()[2].kind == Kind::UserPrompt);
    CHECK_STR(p.items()[2].text, "also X");
    CHECK(p.items()[1].state == State::Pending); // the turn goes on
}

TEST("transcript: a subagent's hand-back queued mid-turn is nobody's prompt") {
    // Seen live 2026-09-30: it showed as the user's prompt, <agent-message> and all.
    TranscriptParser p;
    p.feed(
        prompt("go", "2026-09-25T10:00:00.000Z") +
        assistantText("Working on it", "2026-09-25T10:00:01.000Z") +
        handback(
            "a946", "BLOCKED: nothing done.\n\nWhy: the guard.", "2026-09-25T10:00:02.000Z", true
        )
    );
    REQUIRE(p.items().size() == 3);
    CHECK(p.items()[2].kind == Kind::PeerMessage);
    CHECK_STR(p.items()[2].agentId, "a946");
    CHECK_STR(p.items()[2].text, "BLOCKED: nothing done.\n\nWhy: the guard.");
    CHECK(p.items()[1].state == State::Pending); // the turn goes on
    CHECK(p.turnOpen());
}

TEST("transcript: a hand-back arriving on its own starts a turn") {
    TranscriptParser p;
    p.feed(
        prompt("go", "2026-09-25T10:00:00.000Z") +
        assistantText("Started it.", "2026-09-25T10:00:01.000Z") +
        turnEnd("2026-09-25T10:00:02.000Z") +
        handback("a3dc", "Done.", "2026-09-25T10:00:05.000Z", false)
    );
    REQUIRE(p.items().size() == 3);
    CHECK(p.items()[1].state == State::Final);
    CHECK(p.items()[2].kind == Kind::PeerMessage);
    CHECK_STR(p.items()[2].text, "Done.");
    CHECK(p.turnOpen()); // the session answers it
}

TEST("transcript: a message from another session names that session") {
    TranscriptParser p;
    p.feed(
        R"({"type":"attachment","timestamp":"2026-09-25T10:00:02.000Z","attachment":)"
        R"({"type":"queued_command","commandMode":"prompt",)"
        R"("prompt":"<cross-session-message from=\"uds:/x.sock\" from-name=\"audit\">\nHi\n",)"
        R"("origin":{"kind":"peer","from":"uds:/x.sock","name":"audit",)"
        R"("body":"Heads-up: I'm standing down."}}})"
        "\n"
    );
    REQUIRE(p.items().size() == 1);
    CHECK(p.items()[0].kind == Kind::PeerMessage);
    CHECK(p.items()[0].agentId.empty());
    CHECK_STR(p.items()[0].peerName, "audit");
    CHECK_STR(p.items()[0].text, "Heads-up: I'm standing down.");
}

TEST("transcript: an Agent call becomes a subagent item linked by its agent id") {
    TranscriptParser p;
    p.feed(
        prompt("research", "2026-09-25T10:00:00.000Z") +
        toolUse(
            "a1",
            "Agent",
            R"({"description":"Search the docs","prompt":"…"})",
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "abc123")
    );
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[1].kind == Kind::Subagent);
    CHECK_STR(p.items()[1].text, "Search the docs");
    CHECK_STR(p.items()[1].agentId, "abc123");
    REQUIRE(p.items()[1].tools.size() == 1);
    CHECK_STR(p.items()[1].tools[0].name, "Agent");
}

TEST("transcript: a subagent item keeps the type it was started as") {
    TranscriptParser p;
    p.feed(toolUse(
        "a1",
        "Agent",
        R"({"description":"Draw it","subagent_type":"designer"})",
        "2026-09-25T10:00:01.000Z"
    ));
    REQUIRE(p.items().size() == 1);
    CHECK_STR(p.items()[0].agentType, "designer");
}

TEST("transcript: the subagent types a session offers are read") {
    const auto listing = [](const char *added, const char *removed, bool initial) {
        return str::concat(
            {R"({"type":"attachment","timestamp":"2026-09-25T10:00:02.000Z","attachment":{"type":"agent_listing_delta","addedTypes":)",
             added,
             R"(,"removedTypes":)",
             removed,
             R"(,"isInitial":)",
             initial ? "true" : "false",
             "}}\n"}
        );
    };
    using Set = std::unordered_set<std::string>;
    TranscriptParser p;
    CHECK(p.agentTypes().empty());
    p.feed(listing(R"(["claude","Explore","marketer"])", "[]", true));
    CHECK((p.agentTypes() == Set{"claude", "Explore", "marketer"}));
    p.feed(listing(R"(["engineer"])", R"(["marketer"])", false));
    CHECK((p.agentTypes() == Set{"claude", "Explore", "engineer"}));
    p.feed(listing(R"(["claude"])", "[]", true));
    CHECK((p.agentTypes() == Set{"claude"}));
    CHECK(p.items().empty());
}

TEST("transcript: a background task's stop notifications are noted") {
    TranscriptParser p;
    p.keepActivity(); // as a subagent's parser does
    // Queued, delivered mid-turn, delivered as a turn of its own: the latest counts.
    p.feed(taskStopped("agent42", "2026-09-25T10:00:05.000Z"));
    CHECK(p.taskStoppedAt("agent42") == 1790330405000000);
    p.feed(
        str::concat(
            {R"({"type":"attachment","timestamp":"2026-09-25T10:01:00.000Z","attachment":{"type":"queued_command",)",
             R"("commandMode":"task-notification","prompt":)",
             q(taskNotification("agent42")),
             "}}\n"}
        )
    );
    CHECK(p.taskStoppedAt("agent42") == 1790330460000000);
    p.feed(
        str::concat(
            {R"({"type":"user","timestamp":"2026-09-25T10:02:00.000Z","origin":{"kind":"task-notification"},)",
             R"("message":{"role":"user","content":)",
             q(taskNotification("agent7")),
             "}}\n"}
        )
    );
    CHECK(p.taskStoppedAt("agent7") == 1790330520000000);
    CHECK(p.items().empty()); // none of it is anything anyone said
    CHECK(p.activity().size() == 3);

    // A notification quoted by a tool's output isn't one.
    TranscriptParser q2;
    q2.feed(
        toolUse("t1", "Bash", R"({"command":"grep"})", "2026-09-25T10:00:00.000Z") +
        str::concat(
            {R"({"type":"user","timestamp":"2026-09-25T10:00:01.000Z","message":{"content":[{"type":"tool_result",)",
             R"("tool_use_id":"t1","content":)",
             q(taskNotification("agent42")),
             "}]}}\n"}
        )
    );
    CHECK(q2.taskStoppedAt("agent42") == 0);
    CHECK(p.taskStoppedAt("nobody") == 0);
}

TEST("transcript: a foreground subagent's result is its stop") {
    // Only a background subagent is notified when it stops; a foreground one
    // just returns, its result "completed" (Claude Code 2.1.283's Agent tool).
    TranscriptParser p;
    p.feed(
        toolUse("a1", "Agent", R"({"description":"Fix it"})", "2026-09-25T10:00:01.000Z") +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "fg1", "completed") +
        toolUse("a2", "Agent", R"({"description":"Look"})", "2026-09-25T10:00:10.000Z") +
        toolResult("a2", "2026-09-25T10:00:10.500Z", false, "bg1", "async_launched")
    );
    CHECK(p.taskStoppedAt("fg1") == 1790330409000000);
    CHECK(p.taskStoppedAt("bg1") == 0); // started, not stopped
}

TEST("transcript: a reply relayed to a subagent reads back as the reply alone") {
    TranscriptParser  p;
    const std::string relay =
        subagentReplyPrompt("ab0ae6eeb1648c1f4", "What codeword?\n\nJust it.");
    p.feed(prompt(relay, "2026-09-25T10:00:00.000Z") + prompt("plain", "2026-09-25T10:00:01.000Z"));
    REQUIRE(p.items().size() == 2);
    CHECK_STR(p.items()[0].relayTo, "ab0ae6eeb1648c1f4");
    CHECK_STR(p.items()[0].text, "What codeword?\n\nJust it.");
    CHECK(p.items()[1].relayTo.empty());
    // Not a relay: the wrong id quoted, or nothing after it.
    std::string to;
    CHECK_STR(
        typedPrompt(
            "The user replied in the thread of subagent a1. Pass their message on to it "
            "verbatim with SendMessage (to: \"b2\"):\n\nhi",
            &to
        )
            .substr(0, 8),
        "The user"
    );
    CHECK(to.empty());
    CHECK_STR(typedPrompt(subagentReplyPrompt("a1", ""), &to), subagentReplyPrompt("a1", ""));
}

TEST("transcript: text pasted into Claude Code's prompt box shows as a quote") {
    TranscriptParser p;
    p.feed(prompt(
        "I went to sini.id and I see \n\n<pasted_content id=\"e482\">\n429\nTerlalu "
        "cepat\n\nCoba lagi.\n</pasted_content id=\"e482\">\n\n WTF?",
        "2026-10-04T12:20:53.822Z"
    ));
    REQUIRE(p.items().size() == 1);
    CHECK_STR(
        p.items()[0].text,
        "I went to sini.id and I see \n\n> 429\n> Terlalu cepat\n>\n> Coba lagi.\n\n WTF?"
    );
    // Two pastes, one mid-line, one without an id; text right after a close.
    CHECK_STR(
        quotePastes(
            "a <pasted_content id=\"1\">x</pasted_content id=\"1\">b\n"
            "<pasted_content>y\r\nz</pasted_content>"
        ),
        "a \n> x\nb\n> y\n> z"
    );
    // Not a matched pair: left as written.
    for (const std::string_view s :
         {"see <pasted_content id=\"1\">x</pasted_content id=\"2\">",
          "<pasted_content id=\"1\">never closed",
          "<pasted_contents>x</pasted_contents>",
          "no paste at all"})
        CHECK_STR(quotePastes(s), s);
}

TEST("transcript: records in the same millisecond still get distinct, ordered ids") {
    TranscriptParser p;
    p.feed(
        prompt("a", "2026-09-25T10:00:00.000Z") + prompt("b", "2026-09-25T10:00:00.000Z") +
        prompt("c", "2026-09-25T09:00:00.000Z")
    ); // even a clock step back
    REQUIRE(p.items().size() == 3);
    CHECK(p.items()[0].ts < p.items()[1].ts);
    CHECK(p.items()[1].ts < p.items()[2].ts);
    CHECK(p.items()[1].ts == p.items()[0].ts + 1);
    // A reserved ts (a message of msga's own) is never reused.
    p.reserveTs(1790330500000000);
    p.feed(prompt("d", "2026-09-25T10:00:00.000Z"));
    CHECK(p.items()[3].ts == 1790330500000001);
}

TEST("transcript: torn and foreign lines are skipped, not fatal") {
    TranscriptParser p;
    p.feed(
        std::string("{not json\n") +
        R"({"type":"file-history-snapshot"})"
        "\n[1,2]\n\n" +
        prompt("still here", "2026-09-25T10:00:00.000Z")
    );
    REQUIRE(p.items().size() == 1);
    CHECK_STR(p.items()[0].text, "still here");
}

TEST("transcript: a copy's repeated records are read once") {
    const std::string rec = R"({"type":"user","uuid":"u1","timestamp":"2026-09-25T10:00:00.000Z",)"
                            R"("origin":{"kind":"human"},"message":{"content":"once"}})"
                            "\n";
    TranscriptParser  p;
    p.feed(rec + rec);
    REQUIRE(p.items().size() == 1);
    CHECK_STR(p.items()[0].uuid, "u1");
}

TEST("transcript: version, model, permission mode and title are read") {
    TranscriptParser p;
    p.feed(
        R"({"type":"user","version":"2.1.283","permissionMode":"plan","timestamp":"2026-09-25T10:00:00.000Z",)"
        R"("origin":{"kind":"human"},"message":{"content":"hi"}})"
        "\n"
        R"({"type":"assistant","timestamp":"2026-09-25T10:00:01.000Z","message":{"model":"claude-opus-5",)"
        R"("content":[{"type":"thinking","thinking":"hm"},{"type":"text","text":"Hi."}]}})"
        "\n"
        R"({"type":"ai-title","aiTitle":"  Greeting  "})"
        "\n"
    );
    CHECK_STR(p.version(), "2.1.283");
    CHECK_STR(p.permissionMode(), "plan");
    CHECK_STR(p.model(), "claude-opus-5");
    CHECK_STR(p.aiTitle(), "Greeting");
    CHECK(p.items().size() == 2); // the thinking isn't shown
}

TEST("transcript: a subagent's handback is its answer") {
    TranscriptParser p;
    p.feed(
        prompt("Draw it", "2026-09-25T10:00:00.000Z", false) +
        toolUse(
            "h1",
            "SubagentHandback",
            R"({"message":"Drew it: /tmp/x.png"})",
            "2026-09-25T10:00:05.000Z"
        ) +
        toolResult("h1", "2026-09-25T10:00:06.000Z", false)
    );
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[1].kind == Kind::AssistantText);
    CHECK_STR(p.items()[1].text, "Drew it: /tmp/x.png");
    CHECK(p.items()[1].uuid.empty());
}

TEST("transcript: a turn that failed for want of a login is marked") {
    // As Claude Code 2.1.283 records it.
    TranscriptParser p;
    p.feed(prompt("say hi", "2026-09-26T21:21:17.200Z"));
    p.feed(
        R"({"type":"assistant","timestamp":"2026-09-26T21:21:17.500Z","isApiErrorMessage":true,)"
        R"("error":"authentication_failed","message":{"model":"<synthetic>","content":[{"type":"text",)"
        R"("text":"Not logged in · Please run /login"}]}})"
        "\n"
    );
    p.feed(turnEnd("2026-09-26T21:21:17.600Z"));
    REQUIRE(p.items().size() == 2);
    const TranscriptItem &answer = p.items()[1];
    CHECK(answer.kind == Kind::AssistantText);
    CHECK(answer.loginError);
    CHECK(p.loginFailedAt() == answer.ts);
    CHECK_STR(answer.text, "Not logged in · Please run /login");

    // Any other answer is Claude's own.
    TranscriptParser ok;
    ok.feed(assistantText("hi", "2026-09-26T21:21:17.500Z"));
    REQUIRE(ok.items().size() == 1);
    CHECK_FALSE(ok.items()[0].loginError);
    CHECK(ok.loginFailedAt() == 0);
}

TEST("transcript: tool inputs summarize to one line") {
    json::Document d;
    auto           sum = [&d](const char *tool, const char *input) {
        d.parse(std::string(input), nullptr);
        return summarizeToolInput(tool, d.root());
    };
    CHECK_STR(sum("Bash", R"({"command":"git status"})"), "git status");
    CHECK_STR(sum("Bash", R"({"command":"make","description":"Build it"})"), "Build it");
    CHECK_STR(sum("Grep", R"({"pattern":"foo.*"})"), "foo.*");
    CHECK_STR(sum("WebFetch", R"({"url":"https://x.example"})"), "https://x.example");
    CHECK_STR(sum("Skill", R"({"skill":"verify"})"), "verify");
    CHECK_STR(sum("mcp__x__y", R"({"n":3,"q":"  ","what":"the thing"})"), "the thing");
    CHECK_STR(sum("Bash", R"({"command":"echo a\necho b"})"), "echo a …");
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    if (!home.empty())
        CHECK_STR(
            sum("Read", str::concat({R"({"file_path":")", home, R"(/src/x.cpp"})"}).c_str()),
            "~/src/x.cpp"
        );
    // Long lines are cut at 100 code points, "…" included.
    std::string long_(150, 'x');
    for (int i = 0; i < 10; ++i)
        long_.insert(0, "ä");
    const std::string cut = sum("Bash", str::concat({R"({"command":")", long_, "\"}"}).c_str());
    CHECK(cut.size() == 10 * 2 + 89 + 3);
    CHECK(str::endsWith(cut, "x…"));
}

// ── Records only skimmed (tool output) ──────────────────────────────────────

TEST("transcript: a record skims as the whole document reads it") {
    const std::string lines[] = {
        // As Claude Code writes tool output: blocks inside the result, escapes.
        R"({"parentUuid":"p1","isSidechain":false,"type":"user","message":{"role":"user",)"
        R"("content":[{"tool_use_id":"t1","type":"tool_result","content":[{"type":"text",)"
        R"("text":"a \"type\":\"tool_result\" \u00e9 \ud83d\ude00 \\ \/ \b\f\n\r\t"},)"
        R"({"type":"image","source":{"type":"base64","data":"AAAA"}}],"is_error":true}]},)"
        R"("uuid":"u1","timestamp":"2026-09-25T10:00:03.000Z","toolUseResult":{"status":)"
        R"("completed","agentId":"a42","content":[1,-0.5e+10,0,2E-3,true,false,null]},)"
        R"("cwd":"/src/app","version":"2.1.290","permissionMode":"auto","isMeta":false})",
        // Several results; members repeated (the first counts); odd values.
        R"( { "type" : "user" , "type":"assistant", "message" : { "content" : [ )"
        R"({"type":"tool_result","tool_use_id":"x","is_error":"yes"}, 7, )"
        R"({"type":"tool_result","tool_use_id":"y","tool_use_id":"z","is_error":false} ] }, )"
        R"("uuid":null,"timestamp":12,"toolUseResult":"text","origin":null,"isMeta":true } )",
        R"({"type":"user","origin":{"kind":"human"},"isCompactSummary":true,"isSidechain":true,)"
        R"("message":{"content":[{"type":"image"},{"type":"tool_result"}]},"toolUseResult":[]})",
        R"({"type":"user","message":"plain","toolUseResult":{"agentId":7}})",
        R"({"message":{"content":{"type":"tool_result"}}})",
        "{}",
        // What the document refuses, the skim refuses too.
        R"({"type":"user","message":{"content":[{"type":"tool_res)", // torn
        R"({"type":"user","x":"\q"})",                               // bad escape
        R"({"type":"user","x":"\u12g4"})",
        "{\"type\":\"user\",\"x\":\"a\tb\"}", // a raw control character
        R"({"type":"user","x":01})",
        R"({"type":"user","x":1.})",
        R"({"type":"user","x":-})",
        R"({"type":"user","x":.5})",
        R"({"type":"user","x":1e})",
        R"({"type":"user","x":tru})",
        R"({"type":"user","x":[1,]})",
        R"({"type":"user",})",
        R"({"type":"user"} x)",
        R"({"type" "user"})",
        R"(["type","user"])",
        R"("type")",
        "",
        std::string(300, '[') + std::string(300, ']'),
        "{\"x\":" + std::string(300, '[') + std::string(300, ']') + "}",
        "{\"x\":" + std::string(200, '[') + std::string(200, ']') + "}",
    };
    for (const std::string &line : lines) {
        json::Document doc;
        const bool     parsed = doc.parse(line, nullptr) && doc.root().isObject();
        RecordSkim     s;
        const bool     skimmed = skimRecord(line, &s);
        CHECK(skimmed == parsed);
        if (!skimmed || !parsed)
            continue;
        const json::Value o = doc.root();
        CHECK_STR(s.type, o["type"].str());
        CHECK_STR(s.uuid, o["uuid"].str());
        CHECK_STR(s.timestamp, o["timestamp"].str());
        CHECK_STR(s.version, o["version"].str());
        CHECK_STR(s.permissionMode, o["permissionMode"].str());
        CHECK_STR(s.cwd, o["cwd"].str());
        CHECK(s.members == o.size());
        CHECK(s.origin == o["origin"].isObject());
        CHECK(s.isMeta == o["isMeta"].boolean());
        CHECK(s.isCompactSummary == o["isCompactSummary"].boolean());
        CHECK(s.isSidechain == o["isSidechain"].boolean());
        const json::Value content = o["message"]["content"];
        CHECK(s.contentArray == content.isArray());
        std::vector<RecordSkim::ToolResult> results;
        bool                                image = false;
        for (const json::Value b : content) {
            if (b["type"].str() == "tool_result")
                results.push_back({b["tool_use_id"].str(), b["is_error"].boolean()});
            image = image || b["type"].str() == "image";
        }
        CHECK(s.image == image);
        REQUIRE(s.toolResults.size() == results.size());
        for (size_t i = 0; i < results.size(); ++i) {
            CHECK_STR(s.toolResults[i].id, results[i].id);
            CHECK(s.toolResults[i].error == results[i].error);
        }
        CHECK_STR(s.agentId, o["toolUseResult"]["agentId"].str());
        CHECK_STR(s.status, o["toolUseResult"]["status"].str());
    }
    // A field it hands out must be as written: escaped, the document reads it.
    RecordSkim s;
    CHECK_FALSE(skimRecord(R"({"type":"us\u0065r"})", &s));
    CHECK_FALSE(skimRecord(R"({"typ\u0065":"user"})", &s));
    CHECK(skimRecord(R"({"other":{"k\u0065y":"v\u0065"}})", &s)); // nobody's asking
}

TEST("transcript: tool results read off their skim make what the whole record makes") {
    // The same records twice: as Claude Code writes them (skimmed), and with a
    // space that no skim looks for ("type": "tool_result") — read whole.
    const std::string records =
        prompt("look around", "2026-09-25T10:00:00.000Z") +
        R"({"type":"assistant","timestamp":"2026-09-25T10:00:01.000Z","version":"2.1.1",)"
        R"("message":{"model":"m1","content":[{"type":"tool_use","id":"t1","name":"Bash",)"
        R"("input":{"command":"ls"}},{"type":"tool_use","id":"a1","name":"Agent",)"
        R"("input":{"description":"Dig","subagent_type":"designer"}},{"type":"tool_use",)"
        R"("id":"a2","name":"Task","input":{"description":"Fetch"}}]}})"
        "\n"
        // Results of the shell call and both subagents, one record; a version.
        R"({"type":"user","uuid":"r1","timestamp":"2026-09-25T10:00:02.000Z",)"
        R"("version":"2.1.2","permissionMode":"plan","message":{"content":[)"
        R"({"type":"tool_result","tool_use_id":"t1","is_error":true,"content":"no \"type\""},)"
        R"({"type":"tool_result","tool_use_id":"a1","content":[{"type":"text","text":"ok"}]}]},)"
        R"("toolUseResult":{"agentId":"ag1","status":"async_launched"}})"
        "\n"
        // A copy's repeat of it: read once.
        R"({"type":"user","uuid":"r1","timestamp":"2026-09-25T10:00:09.000Z","message":)"
        R"({"content":[{"type":"tool_result","tool_use_id":"a1","is_error":true}]}})"
        "\n"
        // The second subagent, done in the foreground: its result is its stop.
        R"({"type":"user","uuid":"r2","timestamp":"2026-09-25T10:00:04.000Z","message":)"
        R"({"content":[{"type":"tool_result","tool_use_id":"a2"}]},)"
        R"("toolUseResult":{"status":"completed","agentId":"ag2","totalTokens":5}})"
        "\n"
        // Not tool output alone: these the whole record reads.
        R"({"type":"user","isMeta":true,"timestamp":"2026-09-25T10:00:05.000Z","message":)"
        R"({"content":[{"type":"tool_result","tool_use_id":"t1","is_error":false}]}})"
        "\n"
        R"({"type":"user","origin":{"kind":"human"},"timestamp":"2026-09-25T10:00:06.000Z",)"
        R"("message":{"content":[{"type":"tool_result","tool_use_id":"t1"}]}})"
        "\n"
        R"({"type":"user","timestamp":"2026-09-25T10:00:07.000Z","message":{"content":[)"
        R"({"type":"tool_result","tool_use_id":"t1","is_error":true},{"type":"text","text":"x"}]}})"
        "\n"
        // Only a tool's own output says "tool_result": no result of a call here.
        R"({"type":"user","timestamp":"2026-09-25T10:00:08.000Z","message":{"content":[)"
        R"({"type":"text","text":"hi"}]},"toolUseResult":{"blocks":[{"type":"tool_result"}]}})"
        "\n"
        // Torn: skipped either way.
        R"({"type":"user","timestamp":"2026-09-25T10:00:09.500Z","message":{"content":[{"type":"tool_result")"
        "\n" +
        assistantText("Found it.", "2026-09-25T10:00:10.000Z") +
        taskStopped("ag1", "2026-09-25T10:00:11.000Z") + turnEnd("2026-09-25T10:00:12.000Z");
    std::string whole = records;
    for (size_t at = whole.find(R"("type":"tool_result")"); at != std::string::npos;
         at        = whole.find(R"("type":"tool_result")", at))
        whole.replace(at, 20, R"("type": "tool_result")");

    TranscriptParser skimmed, read;
    skimmed.keepActivity();
    read.keepActivity();
    skimmed.feed(records);
    read.feed(whole);
    CHECK(skimmed.items() == read.items());
    for (size_t i = 0; i < read.items().size(); ++i)
        CHECK(skimmed.revision(i) == read.revision(i));
    CHECK(skimmed.revision() == read.revision());
    CHECK(skimmed.activity() == read.activity());
    CHECK(skimmed.lastActivity() == read.lastActivity());
    CHECK_STR(skimmed.version(), read.version());
    CHECK_STR(skimmed.permissionMode(), read.permissionMode());
    for (const char *agent : {"ag1", "ag2", "nobody"})
        CHECK(skimmed.taskStoppedAt(agent) == read.taskStoppedAt(agent));
    // And what they made is right: the calls' errors, the subagents' ids.
    const auto &items = skimmed.items();
    REQUIRE(items.size() == 6); // the prompt, the calls (3), "hi", the answer
    CHECK_STR(items[4].text, "hi");
    CHECK(items[1].kind == Kind::ToolGroup);
    CHECK(items[1].tools[0].error); // the last word on t1: the record with text
    CHECK_STR(items[2].agentId, "ag1");
    CHECK_STR(items[3].agentId, "ag2");
    CHECK(skimmed.taskStoppedAt("ag2") == 1790330404000000);
    CHECK(skimmed.taskStoppedAt("ag1") == 1790330411000000);
    CHECK_STR(skimmed.permissionMode(), "plan");
}

// ── Prompt history (history.jsonl, what ↑ steps through) ────────────────────

TEST("transcript: the prompt history is the folder's, this session's first") {
    const std::string dir    = tempDir();
    const std::string pastes = dir + "/paste-cache";
    REQUIRE(file::writeAtomic(pastes + "/abc123.txt", "pasted from the cache"));

    const auto entry = [](std::string_view text,
                          const char      *project,
                          const char      *session,
                          const char      *pasted = "{}") {
        return str::concat(
            {R"({"display":)",
             q(text),
             R"(,"pastedContents":)",
             pasted,
             R"(,"timestamp":1790419488582,"project":")",
             project,
             R"(","sessionId":")",
             session,
             "\"}\n"}
        );
    };
    const std::string path = dir + "/history.jsonl";
    REQUIRE(
        file::writeAtomic(
            path,
            entry("other session, older", "/src/app", "S2") +
                entry("mine, older", "/src/app", "S1") +
                entry("another folder", "/src/other", "S1") +
                entry(
                    "[Pasted text #1 +3 lines] and inline",
                    "/src/app",
                    "S2",
                    R"({"1":{"id":1,"type":"text","content":"PASTE"}})"
                ) +
                entry(
                    "see [Pasted text #7 +9 lines]",
                    "/src/app",
                    "S2",
                    R"({"7":{"id":7,"type":"text","contentHash":"abc123"}})"
                ) +
                entry("[Image #1] what's wrong here?", "/src/app/", "S1") +
                entry(subagentReplyPrompt("agent42", "Which page?"), "/src/app", "S1") +
                entry("[Image #2]", "/src/app", "S1") + // nothing left to recall
                entry("again", "/src/app", "S1") + entry("again", "/src/app", "S1") + "not json\n"
        )
    );

    using List = std::vector<std::string>;
    CHECK(
        (promptHistory(path, pastes, "/src/app", "S1") ==
         List{
             "again",
             "Which page?",        // the relayed reply as typed, not msga's wrapper
             "what's wrong here?", // the image placeholder dropped
             "mine, older",
             "see pasted from the cache",
             "PASTE and inline",
             "other session, older",
         })
    );
    CHECK(promptHistory(path, pastes, "/src/app", "S1", 2).size() == 2);
    // No session yet (a teammate's page): the folder's prompts, newest first.
    CHECK(
        (promptHistory(path, pastes, "/src/app", "") == List{
                                                            "again",
                                                            "Which page?",
                                                            "what's wrong here?",
                                                            "see pasted from the cache",
                                                            "PASTE and inline",
                                                            "mine, older",
                                                            "other session, older",
                                                        })
    );
    CHECK(promptHistory(dir + "/missing.jsonl", pastes, "/src/app", "S1").empty());

    // A prompt later: what was added is read (a line still being written
    // too, once whole only once).
    appendFileBytes(path, entry("newest", "/src/app", "S1") + R"({"display":"half)");
    CHECK(promptHistory(path, pastes, "/src/app", "S1", 2) == (List{"newest", "again"}));
    appendFileBytes(
        path,
        R"(","project":"/src/app","sessionId":"S2"})"
        "\n"
    );
    CHECK(promptHistory(path, pastes, "/src/app", "", 3) == (List{"half", "newest", "again"}));
    CHECK(promptHistory(path, pastes, "/src/app", "", 3) == (List{"half", "newest", "again"}));
    CHECK(promptHistory(path, pastes, "/src/other", "S1") == (List{"another folder"}));
    // Rewritten (shorter, or other text where the last look ended): read anew.
    REQUIRE(file::writeAtomic(path, entry("all new", "/src/app", "S1")));
    CHECK(promptHistory(path, pastes, "/src/app", "S1") == (List{"all new"}));
    REQUIRE(
        file::writeAtomic(
            path, entry("first", "/src/app", "S1") + entry("x", "/src/app", "S1") + "\n\n\n\n\n\n"
        )
    );
    CHECK(promptHistory(path, pastes, "/src/app", "S1") == (List{"x", "first"}));

    // The UI thread's look: nothing until a folder was read once; then
    // what was appended since, as promptHistory answers.
    List got{"stale"};
    CHECK_FALSE(loadedPromptHistory(path, pastes, "/src/never", "", &got));
    CHECK(got.empty());
    CHECK(loadedPromptHistory(path, pastes, "/src/app", "S1", &got));
    CHECK(got == (List{"x", "first"}));
    appendFileBytes(path, entry("later", "/src/app", "S1"));
    CHECK(loadedPromptHistory(path, pastes, "/src/app", "S1", &got));
    CHECK(got == (List{"later", "x", "first"}));
    // Rewritten: that is a whole read again, a worker's.
    REQUIRE(file::writeAtomic(path, entry("redone", "/src/app", "S1")));
    CHECK_FALSE(loadedPromptHistory(path, pastes, "/src/app", "S1", &got));
    CHECK(promptHistory(path, pastes, "/src/app", "S1") == (List{"redone"}));
    CHECK(loadedPromptHistory(path, pastes, "/src/app", "S1", &got));
    CHECK(got == (List{"redone"}));
}

// ── Images and files ────────────────────────────────────────────────────────

TEST("transcript: a pasted image is cached and attached to the prompt") {
    freshCache();
    const std::string bytes("\x89PNG\r\n\x1a\n-not-really-a-png", 25);
    const std::string b64 = crypto::base64(bytes);
    TranscriptParser  p;
    p.feed(
        str::concat(
            {R"({"type":"user","timestamp":"2026-09-25T10:00:00.000Z","origin":{"kind":"human"},"imagePasteIds":[14],)",
             R"("message":{"content":[{"type":"text","text":"look at this: [Image #14] please"},)",
             R"({"type":"image","source":{"type":"base64","media_type":"image/png","data":")",
             b64,
             "\"}}]}}\n"}
        )
    );
    REQUIRE(p.items().size() == 1);
    const auto &item = p.items()[0];
    REQUIRE(item.images.size() == 1);
    CHECK(str::startsWith(item.images[0], dirs().cache + "/images/"));
    CHECK(str::endsWith(item.images[0], ".png"));
    CHECK(readFile(item.images[0]) == bytes);
    CHECK_STR(item.text, "look at this: please"); // the placeholder goes
    REQUIRE(item.imageNames.size() == 1);
    CHECK_STR(item.imageNames[0], "Image 14.png");
    // The same image again is the same cached file.
    CHECK_STR(cachePastedImage("image/png", b64), item.images[0]);
    CHECK(str::endsWith(cachePastedImage("image/jpeg", b64), ".jpg"));
    CHECK(cachePastedImage("text/plain", b64).empty());
    CHECK(cachePastedImage("image/png", "").empty());
}

TEST("transcript: files sent to a session ride the prompt as mentions") {
    const std::string dir = freshCache();
    REQUIRE(file::writeAtomic(dir + "/in/shot.png", "png bytes"));
    REQUIRE(file::writeAtomic(dir + "/in/my notes.txt", "notes"));
    // Copied into msga's cache, where a temp folder's cleanup can't take them;
    // the same content twice is the same copy.
    const std::string shot  = cacheUpload(dir + "/in/shot.png");
    const std::string notes = cacheUpload(dir + "/in/my notes.txt");
    REQUIRE(str::startsWith(shot, uploadsDir() + "/"));
    CHECK(str::endsWith(shot, "/shot.png"));
    CHECK(file::exists(notes));
    CHECK_STR(cacheUpload(dir + "/in/shot.png"), shot);
    CHECK(cacheUpload(dir + "/in/missing.png").empty());

    // Mentions first — typed live, one ending the prompt would lose the Enter to
    // the completion list — a path with spaces quoted.
    const std::string sent = withAttachments("what's **this**?", {shot, notes});
    CHECK_STR(sent, "@" + shot + " @\"" + notes + "\" what's **this**?");
    CHECK_STR(withAttachments("", {shot}), "@" + shot + " (attached)");
    CHECK_STR(withAttachments("/btw why", {shot}), "/btw why @" + shot);
    CHECK_STR(withAttachments("plain", {}), "plain");

    // Read back, the mentions are the prompt's files again.
    TranscriptParser p;
    p.feed(prompt(sent, "2026-09-26T10:00:00.000Z"));
    p.feed(prompt(withAttachments("", {shot}), "2026-09-26T10:00:01.000Z"));
    p.feed(prompt("see @/etc/hosts", "2026-09-26T10:00:02.000Z"));
    REQUIRE(p.items().size() == 3);
    const auto &item = p.items()[0];
    CHECK_STR(item.text, "what's **this**?");
    CHECK((item.images == std::vector<std::string>{shot, notes}));
    CHECK((item.imageNames == std::vector<std::string>{"shot.png", "my notes.txt"}));
    // Files alone: no stand-in text shown.
    CHECK(p.items()[1].text.empty());
    CHECK((p.items()[1].images == std::vector<std::string>{shot}));
    // A file the user mentioned themselves stays what they typed.
    CHECK_STR(p.items()[2].text, "see @/etc/hosts");
    CHECK(p.items()[2].images.empty());

    std::vector<std::string> paths;
    CHECK_STR(takeAttachments("a @" + shot + " b", &paths), "a b");
    CHECK(paths.size() == 1);
}

// ── Taking records out ──────────────────────────────────────────────────────

namespace {

// A record as Claude Code links them: uuid, and the record before it.
std::string linked(const std::string &rec, const char *uuid, const char *parent) {
    return str::concat(
        {"{\"uuid\":",
         q(uuid),
         ",\"parentUuid\":",
         parent ? q(parent) : std::string("null"),
         ",",
         std::string_view(rec).substr(1)}
    );
}

// One block of an assistant message (Claude Code writes each as a record).
std::string assistantBlock(std::string_view block, const char *msgId, const char *ts) {
    return str::concat(
        {R"({"type":"assistant","timestamp":")",
         ts,
         R"(","message":{"id":")",
         msgId,
         R"(","content":[)",
         block,
         "]}}\n"}
    );
}

std::string thinking(const char *msgId, const char *ts) {
    return assistantBlock(R"({"type":"thinking","thinking":"","signature":"x"})", msgId, ts);
}

std::string answer(std::string_view text, const char *msgId, const char *ts) {
    return assistantBlock(str::concat({R"({"type":"text","text":)", q(text), "}"}), msgId, ts);
}

// Two turns: the second prompt thinks, calls a tool, thinks again, answers.
std::string linkedTurns() {
    return linked(prompt("keep this", "2026-09-25T10:00:00.000Z"), "p1", nullptr) +
           linked(thinking("m1", "2026-09-25T10:00:01.000Z"), "t1", "p1") +
           linked(answer("Kept.", "m1", "2026-09-25T10:00:02.000Z"), "a1", "t1") +
           linked(turnEnd("2026-09-25T10:00:03.000Z"), "e1", "a1") +
           linked(prompt("the secret is banana", "2026-09-25T10:01:00.000Z"), "p2", "e1") +
           linked(thinking("m2", "2026-09-25T10:01:01.000Z"), "t2", "p2") +
           linked(
               toolUse("tu", "Bash", R"({"command":"ls"})", "2026-09-25T10:01:02.000Z"), "u2", "t2"
           ) +
           linked(toolResult("tu", "2026-09-25T10:01:03.000Z"), "r2", "u2") +
           linked(thinking("m3", "2026-09-25T10:01:04.000Z"), "t3", "r2") +
           linked(answer("Noted.", "m3", "2026-09-25T10:01:05.000Z"), "a3", "t3") +
           linked(turnEnd("2026-09-25T10:01:06.000Z"), "e2", "a3") +
           R"({"type":"last-prompt","leafUuid":"e2"})"
           "\n"
           // Claude Code's bookkeeping copies of the second prompt.
           R"({"type":"queue-operation","operation":"enqueue","content":"the secret is banana"})"
           "\n"
           R"({"type":"last-prompt","lastPrompt":"the secret is banana","leafUuid":"e2"})"
           "\n";
}

using Chain = std::vector<std::pair<std::string, std::string>>;

// uuid → parentUuid of every record in the file ("-" for none), in file order.
Chain chain(const std::string &path) {
    Chain             out;
    const std::string data  = readFile(path);
    size_t            start = 0;
    while (start < data.size()) {
        size_t nl = data.find('\n', start);
        if (nl == std::string::npos)
            nl = data.size();
        json::Document d;
        if (d.parse(data.substr(start, nl - start), nullptr) && d.root().has("uuid")) {
            const json::Value parent = d.root()["parentUuid"];
            out.emplace_back(d.root()["uuid"].str(), parent.isString() ? parent.str() : "-");
        }
        start = nl + 1;
    }
    return out;
}

} // namespace

TEST("transcript: a prompt leaves the transcript with its turn's thinking") {
    const std::string path = tempDir() + "/s.jsonl";
    REQUIRE(file::writeAtomic(path, linkedTurns()));
    TranscriptParser before;
    before.feed(readFile(path));
    REQUIRE(before.items().size() == 5); // prompt, answer, prompt, tool calls, answer
    CHECK_STR(before.items()[2].uuid, "p2");
    CHECK(before.items()[3].uuid.empty()); // tool calls can't go on their own
    CHECK_STR(before.items()[4].uuid, "a3");

    std::string error;
    REQUIRE(removeFromTranscript(path, "p2", &error));
    // The prompt and both thoughts of its turn are gone; the tool call and its
    // result stay, and what followed a removed record now follows its parent.
    CHECK(
        (chain(path) == Chain{
                            {"p1", "-"},
                            {"t1", "p1"},
                            {"a1", "t1"},
                            {"e1", "a1"},
                            {"u2", "e1"},
                            {"r2", "u2"},
                            {"a3", "r2"},
                            {"e2", "a3"},
                        })
    );
    const std::string after = readFile(path);
    CHECK(after.find("banana") == std::string::npos);
    // Records that needed no new link are kept byte for byte.
    CHECK(
        str::startsWith(
            after, linked(prompt("keep this", "2026-09-25T10:00:00.000Z"), "p1", nullptr)
        )
    );
    CHECK(
        str::endsWith(after, "{\"type\":\"last-prompt\",\"leafUuid\":\"e2\"}\n")
    ); // its copies are gone

    CHECK_FALSE(removeFromTranscript(path, "p2", &error)); // no longer there
    CHECK(error.find("no record p2") != std::string::npos);
    CHECK_FALSE(removeFromTranscript(path + ".missing", "p1", &error));
}

TEST("transcript: an answer leaves the transcript with its own thinking") {
    const std::string path  = tempDir() + "/s.jsonl";
    std::string       turns = linkedTurns();
    const std::string from  = R"("leafUuid":"e2"})";
    turns.replace(turns.find(from), from.size(), R"("leafUuid":"a1"})");
    REQUIRE(file::writeAtomic(path, turns));
    REQUIRE(removeFromTranscript(path, "a1"));
    const Chain c = chain(path);
    REQUIRE(c.size() == 9);
    CHECK((c[0] == std::pair<std::string, std::string>{"p1", "-"}));
    CHECK((c[1] == std::pair<std::string, std::string>{"e1", "p1"})); // t1 and a1 are gone
    CHECK(
        (c[3] == std::pair<std::string, std::string>{"t2", "p2"})
    ); // the next turn's thinking stays
    CHECK(
        readFile(path).find(R"("leafUuid":"p1")") != std::string::npos
    ); // pointers along the chain move too
}

TEST("transcript: a turn since a byte offset") {
    const std::string path = tempDir() + "/s.jsonl";
    const std::string head = prompt("hi", "2026-09-25T10:00:00.000Z");
    REQUIRE(
        file::writeAtomic(
            path,
            head + R"({"type":"last-prompt","lastPrompt":"hi"})"
                   "\n"
                   R"({"type":"ai-title","aiTitle":"x"})"
                   "\n"
        )
    );
    CHECK(hasTurnSince(path, 0));
    CHECK_FALSE(hasTurnSince(path, int64_t(head.size()))); // bookkeeping only
    REQUIRE(
        file::writeAtomic(path, readFile(path) + assistantText("Hello", "2026-09-25T10:00:05.000Z"))
    );
    CHECK(hasTurnSince(path, int64_t(head.size())));
    CHECK(hasTurnSince(path, int64_t(head.size()), 1790330404000));
    CHECK_FALSE(hasTurnSince(path, int64_t(head.size()), 1790330405000));
    CHECK_FALSE(hasTurnSince(path + ".missing", 0));
    CHECK_FALSE(hasTurnSince(path, 1 << 30));
}

namespace {
// A record with a uuid, as Claude Code writes every one: "type" first, the
// uuid after the message (so a match must be the record's own key).
std::string record(const char *type, const char *uuid, std::string_view body = {}) {
    return str::concat(
        {R"({"type":")",
         type,
         R"(","message":{"content":)",
         q(body),
         R"(},"uuid":")",
         uuid,
         "\"}\n"}
    );
}
} // namespace

TEST("transcript: a session moved on since a record, read from its end") {
    const std::string path = tempDir() + "/s.jsonl";
    // A big line before it all: the file is read 64 KB at a time from its end.
    const std::string big  = record("user", "u0", std::string(200'000, 'x'));
    const std::string head = big + record("user", "u1", "hi") + record("assistant", "a1", "hello") +
                             record("system", "e1");
    REQUIRE(file::writeAtomic(path, head));
    CHECK_FALSE(hasTurnAfter(path, "e1"));
    CHECK(hasTurnAfter(path, "u1"));
    CHECK(hasTurnAfter(path, "u0")); // across the chunks
    // Bookkeeping after it isn't a turn; a record named only inside another
    // (a parent link) isn't the record.
    REQUIRE(
        file::writeAtomic(
            path,
            head + R"({"type":"last-prompt","lastPrompt":"hi","leafUuid":"e1"})"
                   "\n"
                   R"({"type":"attachment","parentUuid":"e1","uuid":"x1"})"
                   "\n"
        )
    );
    CHECK_FALSE(hasTurnAfter(path, "e1"));
    // Unknown, gone, or none at all: nothing says it didn't move on.
    CHECK(hasTurnAfter(path, "nope"));
    CHECK(hasTurnAfter(path, ""));
    CHECK(hasTurnAfter(path + ".missing", "e1"));
    // A line still being written (no newline yet) counts.
    REQUIRE(file::writeAtomic(path, head + R"({"type":"user","uuid":"u2"})"));
    CHECK(hasTurnAfter(path, "e1"));
    CHECK_FALSE(hasTurnAfter(path, "u2"));

    // The records before one, newest first, as many as asked.
    using V = std::vector<std::string>;
    CHECK(recordsBefore(path, "u2", 2) == (V{"e1", "a1"}));
    CHECK(recordsBefore(path, "u2", 10) == (V{"e1", "a1", "u1", "u0"}));
    CHECK(recordsBefore(path, "u0", 10).empty());
    CHECK(recordsBefore(path, "nope", 10).empty());
}

TEST("transcript: the parser knows the records it read") {
    TranscriptParser p;
    p.feed(record("user", "u1", "hi") + record("system", "e1"));
    CHECK(p.hasRecord("u1"));
    CHECK(p.hasRecord("e1"));
    CHECK_FALSE(p.hasRecord("a1"));
    CHECK_FALSE(p.hasRecord(""));
}

namespace {

// A Bash call's result as Claude Code 2.1.323 writes it for a command sent to
// the background: the task id in toolUseResult, the output file in the text.
std::string backgroundStarted(std::string_view toolUseId, std::string_view taskId, const char *ts) {
    const std::string text = str::concat(
        {"Command running in background with ID: ",
         taskId,
         ". Output is being written to: /tmp/claude-1000/-src-app/S1/tasks/",
         taskId,
         ".output. You will be notified when it completes."}
    );
    return str::concat(
        {R"({"type":"user","uuid":"u-)",
         taskId,
         R"(","timestamp":")",
         ts,
         R"(","message":{"role":"user","content":[{"tool_use_id":)",
         q(toolUseId),
         R"(,"type":"tool_result","content":)",
         q(text),
         R"(,"is_error":false}]},"toolUseResult":{"stdout":"","stderr":"","interrupted":false,)",
         R"("backgroundTaskId":)",
         q(taskId),
         "}}\n"}
    );
}

} // namespace

TEST("transcript: a background command runs until its notification") {
    TranscriptParser p;
    p.feed(
        // The command, not its description, names the job.
        toolUse(
            "t1",
            "Bash",
            R"({"command":"ctest -j8","description":"Run the tests"})",
            "2026-09-25T10:00:00.000Z"
        ) +
        backgroundStarted("t1", "bq1", "2026-09-25T10:00:01.000Z") +
        toolUse("t2", "Bash", R"({"command":"make docs"})", "2026-09-25T10:00:02.000Z") +
        backgroundStarted("t2", "bq2", "2026-09-25T10:00:03.000Z")
    );
    auto running = p.runningCommands();
    REQUIRE(running.size() == 2);
    CHECK_STR(running[0].taskId, "bq1");
    CHECK_STR(running[0].outputPath, "/tmp/claude-1000/-src-app/S1/tasks/bq1.output");
    CHECK_STR(running[0].command, "ctest -j8");
    CHECK(running[0].startedAt == 1790330401000000);
    CHECK_STR(running[1].taskId, "bq2");
    // The tool calls show as ever.
    REQUIRE(!p.items().empty());

    // One stops: the other runs on.
    p.feed(taskStopped("bq1", "2026-09-25T10:05:00.000Z"));
    running = p.runningCommands();
    REQUIRE(running.size() == 1);
    CHECK_STR(running[0].taskId, "bq2");
    p.feed(taskStopped("bq2", "2026-09-25T10:06:00.000Z"));
    CHECK(p.runningCommands().empty());

    // A plain tool result starts nothing.
    TranscriptParser q2;
    q2.feed(
        toolUse("t1", "Bash", R"({"command":"ls"})", "2026-09-25T10:00:00.000Z") +
        str::concat(
            {R"({"type":"user","timestamp":"2026-09-25T10:00:01.000Z","message":{"content":[{"type":"tool_result",)",
             R"("tool_use_id":"t1","content":"bq9.output"}]},"toolUseResult":{"stdout":"x"}})",
             "\n"}
        )
    );
    CHECK(q2.runningCommands().empty());
}

TEST("transcript: how a background command ended") {
    const auto notice =
        [](std::string_view id, std::string_view status, std::string_view summary, const char *ts) {
            return str::concat(
                {R"({"type":"queue-operation","operation":"enqueue","timestamp":")",
                 ts,
                 R"(","content":)",
                 q(str::concat(
                     {"<task-notification>\n<task-id>",
                      id,
                      "</task-id>\n<status>",
                      status,
                      "</status>\n<summary>",
                      summary,
                      "</summary>\n</task-notification>"}
                 )),
                 "}\n"}
            );
        };
    using Kind = TranscriptParser::TaskEnd::Kind;
    TranscriptParser p;
    p.feed(
        toolUse("t1", "Bash", R"({"command":"ok"})", "2026-09-25T10:00:00.000Z") +
        backgroundStarted("t1", "bq1", "2026-09-25T10:00:01.000Z") +
        toolUse("t2", "Bash", R"({"command":"bad"})", "2026-09-25T10:00:02.000Z") +
        backgroundStarted("t2", "bq2", "2026-09-25T10:00:03.000Z") +
        toolUse("t3", "Bash", R"({"command":"sleep 300"})", "2026-09-25T10:00:04.000Z") +
        backgroundStarted("t3", "bq3", "2026-09-25T10:00:05.000Z")
    );
    CHECK(p.taskEnd("bq1") == nullptr);
    p.feed(
        notice(
            "bq1",
            "completed",
            "Background command \"ok\" completed (exit code 0)",
            "2026-09-25T10:01:00.000Z"
        ) +
        notice(
            "bq2",
            "failed",
            "Background command \"bad\" failed with exit code 3",
            "2026-09-25T10:02:00.000Z"
        ) +
        // Claude stops one itself: no notification follows (2.1.323).
        toolUse("t4", "TaskStop", R"({"task_id":"bq3"})", "2026-09-25T10:03:00.000Z")
    );
    REQUIRE(p.taskEnd("bq1") != nullptr);
    CHECK(p.taskEnd("bq1")->kind == Kind::Done);
    CHECK(p.taskEnd("bq1")->at == 1790330460000000);
    REQUIRE(p.taskEnd("bq2") != nullptr);
    CHECK(p.taskEnd("bq2")->kind == Kind::Failed);
    CHECK(p.taskEnd("bq2")->exitCode == 3);
    REQUIRE(p.taskEnd("bq3") != nullptr);
    CHECK(p.taskEnd("bq3")->kind == Kind::Stopped);
    CHECK(p.runningCommands().empty());
}

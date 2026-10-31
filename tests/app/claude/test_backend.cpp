// The Claude Code backend end to end, on a fake ~/.claude in a temp dir
// (CLAUDE_CONFIG_DIR) and msga's own dirs in another: roster scans and
// transcripts become Store conversations, users and messages; sending goes
// through fake `claude` shell scripts (the real CLI never runs). Also the
// transcript item → message rendering (render.h).
//
// The assertions are on the Store: a new message appears in the loaded
// conversation, typing shows in Store::typing, a read-only session sets
// Conversation::readOnly, presence is User::active, a removed conversation
// has member == false, and failed sends and notices reach Backend::onError.
#include "app/claude/backend.h"

#include "app/claude/attach.h"
#include "app/claude/catalog.h"
#include "app/claude/common.h"
#include "app/claude/outputs.h"
#include "app/claude/render.h"
#include "app/claude/roles.h"
#include "app/claude/roster.h"
#include "app/claude/transcript.h"
#include "app/model/jobs.h"
#include "app/model/store.h"
#include "app/mrkdwn/mrkdwn.h"
#include "base/file.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "support/test.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <utime.h>
#include <vector>

using namespace claude;
using model::ConvRef;
using model::kNoConv;
using model::Ts;

std::string fakeAttachProgram(); // test_terminal.cpp: this binary as `claude attach`

namespace {

std::string tempDir(const char *what) {
    return base::test::makeTempDir((std::string(what) + "-").c_str(), base::env("HOME"));
}

std::string q(std::string_view s) {
    std::string out;
    json::escapeString(out, s);
    return out;
}

void writeFile(const std::string &path, std::string_view bytes, int mode = 0644) {
    file::makeDirs(file::dirName(path));
    file::writeAtomic(path, bytes, mode);
}

void appendFile(const std::string &path, std::string_view bytes) {
    file::makeDirs(file::dirName(path));
    if (FILE *f = std::fopen(path.c_str(), "ab")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
}

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

int64_t mtimeMicros(const std::string &path) {
    file::Stat st;
    return file::stat(path, &st) ? st.mtimeMicros : -1;
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

// "2026-09-25T10:00:00.000Z" for epoch ms.
std::string isoAt(int64_t ms) {
    const base::CivilTime t = base::utcTime(ms / 1000);
    char                  buf[96];
    std::snprintf(
        buf,
        sizeof buf,
        "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        t.year,
        t.month,
        t.day,
        t.hour,
        t.minute,
        t.second,
        int(ms % 1000)
    );
    return buf;
}

// ── Transcript records, as Claude Code writes them ──────────────────────────

std::string prompt(std::string_view text, std::string_view ts, bool viaOrigin = true) {
    return str::concat(
        {R"({"type":"user","timestamp":")",
         ts,
         R"(","uuid":")",
         ts,
         R"(-u","message":{"role":"user","content":)",
         q(text),
         "}",
         viaOrigin ? R"(,"origin":{"kind":"human"})" : "",
         "}\n"}
    );
}

std::string assistantText(std::string_view text, std::string_view ts) {
    return str::concat(
        {R"({"type":"assistant","timestamp":")",
         ts,
         R"(","uuid":")",
         ts,
         R"(-a","message":{"content":[{"type":"text","text":)",
         q(text),
         "}]}}\n"}
    );
}

// `input` is a JSON object's text.
std::string
toolUse(std::string_view id, std::string_view name, std::string_view input, std::string_view ts) {
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
    std::string_view ts,
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
handback(std::string_view agentId, std::string_view report, std::string_view ts, bool queued) {
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

std::string turnEnd(std::string_view ts) {
    return str::concat(
        {R"({"type":"system","subtype":"turn_duration","timestamp":")", ts, "\"}\n"}
    );
}

// What the session is told when a background task (a subagent) stops, as
// Claude Code writes it: queued the moment it stops, delivered as a prompt.
std::string taskStopped(std::string_view taskId, std::string_view ts) {
    const std::string note = str::concat(
        {"<task-notification>\n<task-id>",
         taskId,
         "</task-id>\n<status>completed</status>\n<summary>Agent finished</summary>\n"
         "</task-notification>"}
    );
    return str::concat(
        {R"({"type":"queue-operation","operation":"enqueue","timestamp":")",
         ts,
         R"(","content":)",
         q(note),
         "}\n"}
    );
}

// The coordinator's message to a subagent (a reply relayed to it): hidden
// in its transcript, but a record of its run.
std::string coordinatorNote(std::string_view text, std::string_view ts) {
    return str::concat(
        {R"({"type":"user","isMeta":true,"timestamp":")",
         ts,
         R"(","origin":{"kind":"coordinator"},"message":{"role":"user","content":)",
         q(text),
         "}}\n"}
    );
}

std::string promptSnapshot(std::string_view systemPrompt, std::string_view ts) {
    return str::concat(
        {R"({"type":"attachment","timestamp":")",
         ts,
         R"(","attachment":{"type":"prompt_snapshot","systemPrompt":[)",
         q(systemPrompt),
         "]}}\n"}
    );
}

// A record as Claude Code links them: uuid, and the record before it.
std::string linked(std::string rec, const char *uuid, const char *parent) {
    // Records built here start with {"type":…; the link goes in front.
    while (!rec.empty() && rec.back() == '\n')
        rec.pop_back();
    // Drop a "uuid" the builder put in (prompt/assistantText add one).
    if (const size_t u = rec.find(R"(,"uuid":")"); u != std::string::npos) {
        const size_t end = rec.find('"', u + 9);
        rec.erase(u, end + 1 - u);
    }
    return str::concat(
        {R"({"uuid":")",
         uuid,
         R"(","parentUuid":)",
         parent ? q(parent) : std::string("null"),
         ",",
         std::string_view(rec).substr(1),
         "\n"}
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
           R"({"type":"queue-operation","operation":"enqueue","content":"the secret is banana"})"
           "\n"
           R"({"type":"last-prompt","lastPrompt":"the secret is banana","leafUuid":"e2"})"
           "\n";
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

void be32(std::string &out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        out += char((v >> s) & 0xFF);
}

// A PNG's signature and header: all model::imageSize reads.
std::string pngBytes(uint32_t w, uint32_t h) {
    std::string out = "\x89PNG\r\n\x1a\n";
    be32(out, 13);
    out += "IHDR";
    be32(out, w);
    be32(out, h);
    out += std::string("\x08\x06\x00\x00\x00", 5);
    be32(out, 0); // crc, unchecked
    return out;
}

// ── A fake Claude Code home ─────────────────────────────────────────────────

// A fake Claude Code home with one interactive session "S1" in /src/app,
// driven by a terminal (its pid: ours, so alive), and msga's dirs of its own.
// Processes a test starts (workers: real `sleep`s, their pids in wpid-*
// files or noted with own()) are killed when it goes.
struct FakeClaudeHome {
    std::string          dir, msga, transcript;
    std::vector<int64_t> pids;

    FakeClaudeHome() {
        dir  = tempDir("claude-home");
        msga = tempDir("msga-dirs");
        base::test::setEnv("CLAUDE_CONFIG_DIR", dir);
        file::makeDirs(dir + "/sessions");
        file::makeDirs(dir + "/projects/-src-app");
        transcript = dir + "/projects/-src-app/S1.jsonl";
        setDirs({msga + "/data", msga + "/cache"});
    }
    ~FakeClaudeHome() {
        std::vector<file::DirEntry> entries;
        file::listDir(dir, &entries);
        for (const auto &e : entries)
            if (str::startsWith(e.name, "wpid-"))
                pids.push_back(std::atoll(readText(dir + "/" + e.name).c_str()));
        for (const int64_t pid : pids)
            if (pid > 1 && pid != getpid())
                base::test::killProcess(pid);
        base::test::unsetEnv("CLAUDE_CONFIG_DIR");
        base::test::unsetEnv("FAKE_ATTACH");
    }

    void writeSession(std::string_view status, std::string_view name = "app-1") {
        json::Writer w;
        w.beginObject();
        w.key("pid").value(int64_t(getpid()));
        w.key("sessionId").value("S1");
        w.key("cwd").value("/src/app");
        w.key("name").value(name);
        w.key("status").value(status);
        w.key("entrypoint").value("cli");
        w.endObject();
        file::writeAtomic(dir + "/sessions/1.json", w.str());
    }
    // A background worker's sessions/<file>.json.
    void writeWorker(
        const std::string &fileName,
        int64_t            pid,
        std::string_view   sessionId,
        std::string_view   status,
        std::string_view   jobId = {},
        std::string_view   cwd   = {}
    ) {
        json::Writer w;
        w.beginObject();
        w.key("pid").value(pid);
        w.key("sessionId").value(sessionId);
        if (!jobId.empty())
            w.key("jobId").value(jobId);
        if (!cwd.empty())
            w.key("cwd").value(cwd);
        w.key("kind").value("bg");
        w.key("status").value(status);
        w.key("entrypoint").value("cli");
        w.endObject();
        file::writeAtomic(dir + "/sessions/" + fileName, w.str());
    }
    void writeJob(const std::string &shortId, std::string_view json) {
        writeFile(dir + "/jobs/" + shortId + "/state.json", json);
    }
    // Appended in place, as Claude Code writes: the file keeps its birth time
    // (a rewrite would make it younger than its forks, see detectForks).
    void append(std::string_view bytes) { appendFile(transcript, bytes); }
    // Claude Code trusts `folder` (background sessions refuse other folders).
    void trust(const std::string &folder) {
        writeFile(
            dir + "/.claude.json",
            str::concat(
                {R"({"projects":{)",
                 q(file::absolute(folder)),
                 R"(:{"hasTrustDialogAccepted":true}}})"}
            )
        );
    }
    std::string text(const std::string &name) const { return readText(dir + "/" + name); }
    std::vector<std::string> calls() const { return lines(text("calls.log")); }
    // A real process standing in for a worker: a `sleep` no child of ours (so
    // nothing of ours reaps or spares it), killed when the home goes.
    int64_t                  spawnWorker(const std::vector<std::string> &env = {}) {
        base::RunOptions o;
        o.env       = env;
        o.timeoutMs = 10'000;
        const base::RunResult r =
            base::run("/bin/sh", {"-c", "sleep 60 </dev/null >/dev/null 2>&1 & echo $!"}, o);
        const int64_t pid = std::atoll(r.output.c_str());
        if (pid > 0)
            pids.push_back(pid);
        return pid;
    }
};

#ifndef _WIN32 // /bin/sh scripts
std::string writeCli(const std::string &dir, std::string_view script) {
    const std::string cli = dir + "/claude";
    writeFile(cli, script, 0755);
    return cli;
}

// A `claude` that only answers `auth status`, as logged in or not; anything
// else is logged to calls.log (nothing may get that far when logged out).
std::string fakeLoginCli(const std::string &dir, bool loggedIn) {
    return writeCli(
        dir,
        std::string("#!/bin/sh\n") +
            (loggedIn ? "[ \"$1\" = auth ] && { echo '{\"loggedIn\":true}'; exit 0; }\n"
                      : "[ \"$1\" = auth ] && { echo '{\"loggedIn\":false}'; exit 1; }\n") +
            "printf '%s\\n' \"$*\" >> \"$CLAUDE_CONFIG_DIR/calls.log\"\nexit 1\n"
    );
}

#endif

bool waitFor(plat::App &app, const std::function<bool()> &pred, int ms = 5000) {
    const int64_t end = base::monotonicMs() + ms;
    while (!pred()) {
        if (base::monotonicMs() > end)
            return false;
        app.pump(10);
    }
    return true;
}

void pumpFor(plat::App &app, int ms) {
    const int64_t end = base::monotonicMs() + ms;
    while (base::monotonicMs() < end)
        app.pump(10);
}

std::string plain(const model::Message &m) {
    return mrkdwn::parse(m.text).text;
}

const model::Conversation *onlyConversation(model::Store &store) {
    const model::Conversation *found = nullptr;
    for (ConvRef c = 0; c < store.conversationCount(); ++c)
        if (store.conversation(c).member) {
            if (found)
                return nullptr;
            found = &store.conversation(c);
        }
    return found;
}

// A backend on a Store, connected, with what it reported kept.
struct Rig {
    std::unique_ptr<plat::App> app = plat::App::create();
    model::Store               store;
    std::unique_ptr<Backend>   backend;
    std::vector<model::Change> changes;
    std::vector<std::string>   errors;
    std::vector<Unsent>        unsent; // given back (onUnsent), in order

    explicit Rig(Credentials creds = {}, bool connect = true) {
        backend          = std::make_unique<Backend>(store, *app, std::move(creds));
        backend->onError = [this](const std::string &m) { errors.push_back(m); };
        backend->onUnsent =
            [this](
                ConvRef c, Ts thread, const std::string &text, const std::vector<std::string> &f
            ) { unsent.push_back({c, thread, text, f}); };
        store.observe(model::Store::kAnyConv, [this](const model::Change &c) {
            changes.push_back(c);
        });
        if (connect) {
            bool connected = false;
            backend->connect([&](bool, const std::string &) { connected = true; });
            waitFor(*app, [&] { return connected; });
        }
    }
    ~Rig() {
        backend.reset();
        // The CLI runs and scans still under way post to this App: they're
        // done before it goes (the next case makes another).
        model::waitBackground();
    }

    bool wait(const std::function<bool()> &pred, int ms = 8000) { return waitFor(*app, pred, ms); }
    void pump(int ms) { pumpFor(*app, ms); }

    ConvRef ref(std::string_view id) const { return store.findConversation(id); }
    bool    listed(std::string_view id) const {
        const ConvRef r = ref(id);
        return r != kNoConv && store.conversation(r).member;
    }
    int listedCount() const {
        int n = 0;
        for (ConvRef c = 0; c < store.conversationCount(); ++c)
            n += store.conversation(c).member;
        return n;
    }
    const model::User *user(std::string_view id) const {
        const model::UserRef u = store.findUser(id);
        return u == model::kNoUser ? nullptr : &store.user(u);
    }
    bool active(std::string_view id) const {
        const model::User *u = user(id);
        return u && u->active;
    }
    // The conversation's history, (re)loaded: its top-level messages.
    const std::vector<model::Message> &load(ConvRef c) {
        bool done = false;
        backend->loadHistory(c, 0, [&](bool, const std::string &) { done = true; });
        wait([&] { return done; }, 5000);
        return store.conversation(c).messages;
    }
    const std::vector<model::Message> &messages(ConvRef c) const {
        return store.conversation(c).messages;
    }
    // A thread's replies, (re)loaded (the root stays in the conversation).
    std::vector<const model::Message *> loadThread(ConvRef c, Ts root) {
        bool done = false;
        backend->loadThread(c, root, [&](bool, const std::string &) { done = true; });
        wait([&] { return done; }, 5000);
        return replies(c, root);
    }
    std::vector<const model::Message *> replies(ConvRef c, Ts root) const {
        std::vector<const model::Message *> out;
        if (const auto *r = store.replies(c, root))
            for (const auto &m : *r)
                out.push_back(&m);
        return out;
    }
    const model::Message *byText(ConvRef c, std::string_view text) const {
        for (const auto &m : messages(c))
            if (plain(m) == text)
                return &m;
        return nullptr;
    }
    // Whether the conversation shows `text` from someone other than me.
    bool answered(ConvRef c, std::string_view text) const {
        for (const auto &m : messages(c))
            if (m.user != store.me && plain(m) == text)
                return true;
        return false;
    }
    std::vector<std::string> ownTexts(ConvRef c) {
        std::vector<std::string> out;
        for (const auto &m : load(c))
            if (m.user == store.me)
                out.push_back(plain(m));
        return out;
    }
    // The typing indicator in conversation c (thread 0 = the chat itself).
    std::optional<model::Typing> typing(ConvRef c, Ts thread = 0) const {
        for (const auto &t : store.typing(c))
            if (t.thread == thread)
                return t;
        return std::nullopt;
    }
    bool anyThreadTyping(ConvRef c) const {
        for (const auto &t : store.typing(c))
            if (t.thread)
                return true;
        return false;
    }
    ConvRef start(const std::string &dir, bool skipChecks = false, const std::string &role = {}) {
        ConvRef     conv = kNoConv;
        std::string error;
        bool        done = false;
        backend->startAgentSession(dir, skipChecks, role, [&](ConvRef c, const std::string &e) {
            conv  = c;
            error = e;
            done  = true;
        });
        wait([&] { return done; }, 5000);
        lastStartError = error;
        return conv;
    }
    std::string lastStartError;
    void        send(ConvRef c, const std::string &text, bool *ok = nullptr, Ts thread = 0) {
        backend->send(c, text, thread, [ok](bool success, const std::string &) {
            if (ok)
                *ok = success;
        });
    }
};

} // namespace

// ── Rendering: transcript items → messages ──────────────────────────────────

TEST("render: bare URLs are linked outside code only, in the one line pass") {
    const std::string out = renderMarkdown(
        "see https://a.b/c and `https://x.y/z`\n```\nhttps://in.fence/x\n```\n"
        "| a |\n| https://t.b/l |\n## On https://h.d/e\n~4 MB & <tag>"
    );
    CHECK(contains(out, "see <https://a.b/c> and `https://x.y/z`\n"));
    CHECK(contains(out, "```\nhttps://in.fence/x\n```\n"));
    CHECK(contains(out, "```\n| a |\n| https://t.b/l |\n```\n"));
    CHECK(contains(out, "<https://h.d/e>"));
    CHECK(contains(out, "&#126;4 MB &amp; &lt;tag&gt;"));
}

TEST("render: a lone tilde is literal, a double one strikes") {
    const mrkdwn::Rich approx =
        mrkdwn::parse(renderMarkdown("It covers ~4 MB on Linux, ~3 MB on Windows and ~2 MB."));
    CHECK_STR(approx.text, "It covers ~4 MB on Linux, ~3 MB on Windows and ~2 MB.");
    for (const auto &e : approx.entities)
        CHECK(e.kind != mrkdwn::Kind::Strike);

    const mrkdwn::Rich struck = mrkdwn::parse(renderMarkdown("was ~~4 MB~~ now ~3 MB"));
    CHECK_STR(struck.text, "was 4 MB now ~3 MB");
    bool strike = false;
    for (const auto &e : struck.entities)
        strike = strike || (e.kind == mrkdwn::Kind::Strike && e.start == 4 && e.length == 4);
    CHECK(strike);
}

TEST("render: headings bold, angle brackets kept, tables fenced") {
    const mrkdwn::Rich heading = mrkdwn::parse(renderMarkdown("## Result\nuse a < b && c > d"));
    CHECK(str::startsWith(heading.text, "Result\n"));
    CHECK(contains(heading.text, "use a < b && c > d"));
    bool bold = false;
    for (const auto &e : heading.entities)
        bold = bold || (e.kind == mrkdwn::Kind::Bold && e.start == 0 && e.length == 6);
    CHECK(bold);

    const mrkdwn::Rich table = mrkdwn::parse(renderMarkdown("| a | b |\n|---|---|\n| 1 | 2 |"));
    bool               pre   = false;
    for (const auto &e : table.entities)
        pre = pre || e.kind == mrkdwn::Kind::Pre;
    CHECK(pre); // monospace keeps the columns aligned
}

TEST("render: a table becomes a table block between the text around it") {
    const auto blocks =
        markdownBlocks("Here:\n\n| Name | `a|b` |\n|:---|---:|\n| *x* | 2 \\| 3 |\n\nDone.");
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[0].kind == model::Block::Kind::Text);
    CHECK_STR(mrkdwn::parse(blocks[0].text).text, "Here:");
    REQUIRE(blocks[1].kind == model::Block::Kind::Table);
    REQUIRE(blocks[1].rows.size() == 2);
    REQUIRE(blocks[1].rows[0].size() == 2);
    CHECK_STR(blocks[1].rows[0][0], "Name");
    CHECK_STR(mrkdwn::parse(blocks[1].rows[0][1]).text, "a|b"); // a pipe in code stays
    CHECK_STR(mrkdwn::parse(blocks[1].rows[1][0]).text, "x");   // markdown in a cell
    CHECK_STR(mrkdwn::parse(blocks[1].rows[1][1]).text, "2 | 3");
    CHECK_STR(mrkdwn::parse(blocks[2].text).text, "Done.");
    // No separator line: no table; and nothing at all without one.
    CHECK(markdownBlocks("| a | b |\n| 1 | 2 |").empty());
    CHECK(markdownBlocks("just text").empty());
    CHECK(markdownBlocks("```\n| a |\n|---|\n```").empty()); // fenced: code
}

TEST("render: tool cards and subagent lines are plain text, the name bold") {
    TranscriptItem g;
    g.kind = TranscriptItem::Kind::ToolGroup;
    g.ts   = 1;
    g.tools.push_back({"t1", "Bash", "cp ~/a ~/b && ls src/**/*.cpp `x` :bug:", false});
    g.tools.push_back({"t2", "mcp__srv__do_it", "", true});
    const model::Message m = toMessage(g, 1, 2);
    REQUIRE(m.attachments().size() == 1);
    const mrkdwn::Rich card = mrkdwn::parse(m.attachments()[0].text);
    CHECK_STR(
        card.text, "Bash  cp ~/a ~/b && ls src/**/*.cpp `x` :bug:\nmcp__srv__do_it  \xE2\x9C\x97"
    );
    int bold = 0;
    for (const auto &e : card.entities) {
        CHECK(e.kind == mrkdwn::Kind::Bold);
        bold += e.kind == mrkdwn::Kind::Bold;
    }
    CHECK(bold == 2);

    TranscriptItem a;
    a.kind                  = TranscriptItem::Kind::Subagent;
    a.ts                    = 2;
    a.text                  = "find snake_case_names in *.md";
    const mrkdwn::Rich line = mrkdwn::parse(toMessage(a, 1, 2).text);
    CHECK_STR(line.text, "Subagent: find snake_case_names in *.md");
    REQUIRE(line.entities.size() == 1);
    CHECK(line.entities[0].kind == mrkdwn::Kind::Italic);
    CHECK(line.entities[0].length == line.text.size());
}

TEST("render: bare URLs in Claude's text are links") {
    const auto links = [](std::string_view md) {
        std::vector<std::string> out;
        for (const auto &e : mrkdwn::parse(renderMarkdown(md)).entities)
            if (e.kind == mrkdwn::Kind::Link)
                out.push_back(e.data);
        return out;
    };
    using V = std::vector<std::string>;
    CHECK(
        links("Here: https://ex.example.com/iA09a_HA?x=1&y=2.") ==
        (V{"https://ex.example.com/iA09a_HA?x=1&y=2"})
    );
    CHECK(links("(see https://a.example/b)") == V{"https://a.example/b"});
    CHECK(
        links("https://en.wikipedia.org/wiki/Foo_(bar)") ==
        (V{"https://en.wikipedia.org/wiki/Foo_(bar)"})
    );
    CHECK(links("<https://auto.example/x>") == V{"https://auto.example/x"});
    CHECK(links("[the docs](https://docs.example/p)") == V{"https://docs.example/p"});
    // Code stays as written: no <url> token (mrkdwn::parse may still link a
    // URL inside `code`, but the text holds none).
    CHECK_STR(
        renderMarkdown("run `curl https://code.example/`"), "run `curl https://code.example/`"
    );
    CHECK_STR(
        renderMarkdown("```\nhttps://fenced.example/\n```"), "```\nhttps://fenced.example/\n```"
    );
    CHECK_STR(
        mrkdwn::parse(renderMarkdown("go to https://x.example/a now")).text,
        "go to https://x.example/a now"
    );
}

TEST("render: a teammate mention renders as a mention") {
    const auto mentions = [](std::string_view md) {
        std::vector<std::string> out;
        for (const auto &e : mrkdwn::parse(renderMarkdown(md)).entities)
            if (e.kind == mrkdwn::Kind::User)
                out.push_back(e.data);
        return out;
    };
    using V = std::vector<std::string>;
    CHECK(mentions("Do you know who @claude:role:engineer is?") == V{"claude:role:engineer"});
    CHECK(
        mentions("ask @claude:agent and @claude:role:data-analyst.") ==
        (V{"claude:agent", "claude:role:data-analyst"})
    );
    CHECK(mentions("run `echo @claude:role:engineer`").empty());
    // Quoted on its own, it's still the teammate (Claude's habit).
    const mrkdwn::Rich quoted =
        mrkdwn::parse(renderMarkdown("Yes. `@claude:role:researcher` is the id."));
    CHECK(mentions("Yes. `@claude:role:researcher` is the id.") == V{"claude:role:researcher"});
    CHECK(std::none_of(quoted.entities.begin(), quoted.entities.end(), [](const auto &e) {
        return e.kind == mrkdwn::Kind::Code;
    }));
    CHECK(mentions("```\n@claude:role:engineer\n```").empty());
    CHECK(mentions("mail x@claude:role:engineer").empty());
    // Word boundaries are Unicode's (roles.h mentionAt): after "é" it's an
    // address too, after a space a mention.
    CHECK(mentions("mail caf\xC3\xA9@claude:role:engineer").empty());
    CHECK(mentions("caf\xC3\xA9 @claude:role:engineer") == V{"claude:role:engineer"});
    CHECK(mentions("@claude:role:engineer\xC3\xA9").empty());
    // A composer pill's raw token (older prompts hold it) is the mention too,
    // with no extra brackets.
    CHECK(mentions("start an <@claude:role:engineer> subagent") == V{"claude:role:engineer"});
    CHECK_STR(
        renderMarkdown("start an <@claude:role:engineer> subagent"),
        "start an <@claude:role:engineer> subagent"
    );
}

// The new UI has no table blocks: a table is fenced (monospace, aligned), in
// place between the text around it.
TEST("render: a markdown table is fenced in reading order") {
    const std::string md   = "Before the table.\n\n"
                             "| Session | Can you write? |\n"
                             "|---|:---:|\n"
                             "| Open in a terminal | **no** |\n"
                             "| Closed \\| gone | `a|b` |\n"
                             "\nAfter it.";
    const std::string text = renderMarkdown(md);
    CHECK_STR(
        text,
        "Before the table.\n\n"
        "```\n"
        "| Session | Can you write? |\n"
        "|---|:---:|\n"
        "| Open in a terminal | **no** |\n"
        "| Closed \\| gone | `a|b` |\n"
        "```\n"
        "\nAfter it."
    );
    const mrkdwn::Rich rich = mrkdwn::parse(text);
    const auto         pre  = std::find_if(rich.entities.begin(), rich.entities.end(), [](auto &e) {
        return e.kind == mrkdwn::Kind::Pre;
    });
    REQUIRE(pre != rich.entities.end());
    const std::string fenced(
        str::trim(std::string_view(rich.text).substr(pre->start, pre->length))
    );
    CHECK(str::startsWith(fenced, "| Session | Can you write? |"));
    CHECK(contains(fenced, "| Closed \\| gone | `a|b` |")); // cells as written, pipes and all
    CHECK(str::startsWith(rich.text, "Before the table."));
    CHECK(str::endsWith(rich.text, "After it."));

    // A lone pipe line is no table; a fenced one stays the code it is.
    CHECK(!contains(renderMarkdown("no table here\n| just a pipe line |"), "```"));
    CHECK_STR(renderMarkdown("```\n| a | b |\n|---|---|\n```"), "```\n| a | b |\n|---|---|\n```");
}

TEST("render: items map to messages: authors, progress subtype, tool card") {
    TranscriptParser p;
    p.feed(sampleTurn());
    const model::UserRef me = 1, claude = 2;
    const auto          &items = p.items();
    REQUIRE(items.size() == 4);
    CHECK(toMessage(items[0], me, claude).user == me);
    const model::Message remark = toMessage(items[1], me, claude);
    CHECK(remark.user == claude);
    CHECK(remark.subtype() == kProgressSubtype);
    const model::Message card = toMessage(items[2], me, claude);
    CHECK(card.subtype() == kProgressSubtype);
    REQUIRE(card.attachments().size() == 1);
    CHECK(contains(card.attachments()[0].text, "*Bash*  Build it"));
    CHECK(contains(card.attachments()[0].text, "\xE2\x9C\x97")); // ✗: the failed Edit
    const model::Message answer = toMessage(items[3], me, claude);
    CHECK(answer.subtype() != kProgressSubtype);
    CHECK_STR(plain(answer), "Fixed: a missing include.");
}

// ── The backend: reading ────────────────────────────────────────────────────

TEST("backend: a terminal session is listed read-only and its answer arrives") {
    FakeClaudeHome home;
    home.writeSession("busy");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z")
    );

    Rig                        rig;
    const model::Conversation *c = onlyConversation(rig.store);
    REQUIRE(c != nullptr);
    const ConvRef ref = rig.ref("S1");
    CHECK(ref != kNoConv);
    CHECK(rig.store.displayName(ref) == "app-1");
    CHECK(c->kind == model::ConvKind::Dm);
    CHECK(contains(c->readOnly, "terminal")); // a terminal drives it
    CHECK(rig.backend->isAgentSession(ref));
    // Working on the turn reads as the session's teammate thinking.
    const model::UserRef agent = rig.store.findUser("claude:agent");
    REQUIRE(agent != model::kNoUser);
    CHECK(!rig.store.typing(ref).empty() && rig.store.typing(ref)[0].user == agent);
    CHECK(rig.store.typing(ref)[0].sinceMs > 0);

    // While Claude works on the turn only the prompt shows.
    const auto &msgs = rig.load(ref);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].user == rig.store.me);
    CHECK(!rig.store.conversation(ref).hasMoreBefore); // the whole session

    // The turn ends: the answer arrives, and the session stops thinking.
    home.append(turnEnd("2026-09-25T10:00:02.000Z"));
    home.writeSession("idle");
    CHECK(rig.wait([&] {
        const auto &m = rig.messages(ref);
        return m.size() == 2 && plain(m[1]) == "Hello!" && m[1].user == agent;
    }));
    CHECK(rig.store.typing(ref).empty());
    CHECK(rig.store.conversation(ref).unread == 1); // Claude's answer, after the last read

    // The terminal session ends: without a claude CLI it stays read-only, for
    // another reason, and it stays listed while its transcript does.
    file::remove(home.dir + "/sessions/1.json");
    CHECK(
        rig.wait([&] { return contains(rig.store.conversation(ref).readOnly, "Install"); }, 15000)
    );
    CHECK(rig.store.conversation(ref).member);
}

TEST("backend: a session not opened yet announces its answers, once") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );

    Rig           rig;
    const ConvRef ref = rig.ref("S1");
    REQUIRE(ref != kNoConv);
    std::vector<std::string> arrived; // what the shell would notify for
    int                      appended = 0;
    const auto               id       = rig.store.observe(ref, [&](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Arrived && rig.store.arrived())
            arrived.push_back(plain(*rig.store.arrived()));
        if (ch.kind == model::ChangeKind::Append)
            ++appended;
    });

    // A new turn: the remark before the tools and the tool card are progress,
    // the answer is news.
    home.append(
        prompt("again", "2026-09-25T10:01:00.000Z") +
        assistantText("Let me look.", "2026-09-25T10:01:01.000Z") +
        toolUse("t1", "Bash", R"({"command":"ls"})", "2026-09-25T10:01:02.000Z") +
        toolResult("t1", "2026-09-25T10:01:03.000Z") +
        assistantText("Found *it*.", "2026-09-25T10:01:04.000Z") +
        turnEnd("2026-09-25T10:01:05.000Z")
    );
    CHECK(rig.wait([&] { return !arrived.empty(); }));
    rig.wait([] { return false; }, 600); // later syncs add nothing
    REQUIRE(arrived.size() == 1);
    CHECK_STR(arrived[0], "Found it.");
    CHECK(rig.store.conversation(ref).mentions == 1);

    // Opened, it's history (the page, the chat on screen): served, not
    // announced again, and nothing added after it.
    CHECK(appended == 0);
    const auto &msgs = rig.load(ref);
    CHECK(std::any_of(msgs.begin(), msgs.end(), [](const model::Message &m) {
        return plain(m) == "Found it.";
    }));
    const int page = appended;
    rig.wait([] { return false; }, 600);
    CHECK(arrived.size() == 1);
    CHECK(appended == page);
    rig.store.unobserve(id);
}

TEST("backend: a session title names the teammates it mentions") {
    FakeClaudeHome home;
    home.writeSession(
        "idle", "Ask @claude:role:engineer and @claude:agent, not caf\xC3\xA9@claude:role:engineer"
    );
    home.append(prompt("hi", "2026-09-25T10:00:00.000Z"));

    Rig rig;
    REQUIRE(rig.listedCount() == 1);
    const ConvRef ref = rig.ref("S1");
    REQUIRE(ref != kNoConv);
    const std::string want = "Ask @Engineer and @Generalist, not caf\xC3\xA9@claude:role:engineer";
    CHECK_STR(rig.store.conversation(ref).name, want);
    const model::User *u = rig.user("claude:S1");
    REQUIRE(u);
    CHECK_STR(u->name, want);
}

TEST(
    "backend: a removal recorded without the transcript's size stays away through an idle-worker "
    "retire"
) {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    // Removed before hide entries kept the transcript's size (seen 2026-09-25:
    // the session came back an hour on, when Claude Code retired its worker).
    const int64_t removedAt = base::nowMicros() / 1000;
    writeFile(
        dirs().data + "/known-sessions.json",
        str::concat(
            {R"({"sessions":[],"hidden":[{"sessionId":"S1","at":)",
             str::number(removedAt),
             R"(,"transcript":)",
             q(home.transcript),
             "}]}"}
        )
    );
    ::usleep(20'000); // the transcript's next write must be newer than the removal

    Rig rig;
    CHECK(rig.listedCount() == 0);

    home.append(
        "{\"type\":\"last-prompt\",\"lastPrompt\":\"hi\",\"sessionId\":\"S1\"}\n"
        "{\"type\":\"cost-state\",\"sessionId\":\"S1\"}\n"
    );
    home.writeSession("idle");
    rig.pump(1500);
    CHECK(rig.listedCount() == 0);

    // A turn after the removal brings it back.
    home.append(prompt("more", isoAt(removedAt + 1000)));
    home.writeSession("busy");
    CHECK(rig.wait([&] { return rig.listedCount() == 1; }, 12000));
}

TEST("backend: a session removed from msga stays away until it gets new activity") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    {
        Rig rig;
        REQUIRE(rig.listedCount() == 1);
        rig.backend->leave(rig.ref("S1"));
        CHECK(rig.listedCount() == 0);
    }
    // Remembered across restarts; Claude Code's own files are left alone.
    CHECK(file::exists(home.transcript));
    CHECK(file::exists(home.dir + "/sessions/1.json"));
    ::usleep(20'000); // the transcript's next write must be newer than the removal

    Rig rig;
    CHECK(rig.listedCount() == 0);

    // Claude Code's daemon retiring the idle worker appends bookkeeping: no
    // activity, it stays away.
    home.append(
        "{\"type\":\"last-prompt\",\"lastPrompt\":\"hi\",\"sessionId\":\"S1\"}\n"
        "{\"type\":\"cost-state\",\"sessionId\":\"S1\"}\n"
    );
    home.writeSession("idle");
    rig.pump(1500);
    CHECK(rig.listedCount() == 0);

    // Someone continues it in the terminal: it's back.
    home.append(prompt("more", "2026-09-25T11:00:00.000Z"));
    home.writeSession("busy");
    CHECK(rig.wait([&] { return rig.listed("S1"); }, 12000));
    CHECK(rig.listedCount() == 1);
}

TEST("backend: a removed background job whose transcript Claude Code deleted stays removed") {
    FakeClaudeHome    home;
    const std::string sid = "8d953db6-f3be-4b02-8f0f-09aea0343b3e";
    home.writeJob(
        "8d953db6",
        str::concat(
            {R"({"state":"done","sessionId":")",
             sid,
             R"(","cwd":"/src/app","name":"no transcript","linkScanPath":)",
             q(home.dir + "/projects/-src-app/gone.jsonl"),
             "}"}
        )
    );
    {
        Rig rig;
        REQUIRE(rig.listedCount() == 1);
        rig.backend->leave(rig.ref(sid));
        // Any roster change triggers a rescan: it must not bring the job back.
        home.writeSession("idle");
        rig.pump(1500);
        CHECK_FALSE(rig.listed(sid));
    }
    Rig rig;
    CHECK_FALSE(rig.listed(sid));
}

TEST("backend: a session renamed in msga is titled by that name") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(prompt("hi", "2026-09-25T10:00:00.000Z"));
    const auto nameOf = [](Rig &rig) {
        const model::User *u = rig.user("claude:S1");
        return u ? u->displayName : std::string();
    };
    {
        Rig rig;
        REQUIRE(nameOf(rig) == "app-1");
        rig.changes.clear();
        rig.backend->setLocalName(rig.ref("S1"), " Refunds ");
        // The DM's title comes from its peer: changed right away.
        CHECK_STR(nameOf(rig), "Refunds");
        CHECK(std::any_of(rig.changes.begin(), rig.changes.end(), [](const model::Change &c) {
            return c.kind == model::ChangeKind::Users;
        }));
        const auto &c = rig.store.conversation(rig.ref("S1"));
        CHECK_STR(c.localName, "Refunds");
        CHECK_STR(c.name, "app-1"); // Claude Code's own name, the dialog's placeholder
    }
    Rig rig;
    CHECK_STR(nameOf(rig), "Refunds"); // kept across restarts
    rig.backend->setLocalName(rig.ref("S1"), "");
    CHECK_STR(nameOf(rig), "app-1");
}

TEST("backend: your name and picture are kept by msga") {
    FakeClaudeHome    home;
    const std::string pics = tempDir("pics");
    const std::string pic  = pics + "/me.png";
    writeFile(pic, "not really a png");
    const auto me = [](Rig &rig) -> const model::User & { return rig.store.user(rig.store.me); };
    {
        Rig rig;
        CHECK_FALSE(rig.backend->capabilities().selfStatus); // a name and a picture, no more
        CHECK(me(rig).avatar.empty());

        bool ok = false, done = false;
        rig.backend->updateProfile("Robin", {}, {}, [&](bool o, const std::string &) {
            ok   = o;
            done = true;
        });
        REQUIRE(rig.wait([&] { return done; }));
        CHECK(ok);
        done = false;
        rig.backend->setPhoto(pic, [&](bool o, const std::string &) {
            ok   = o;
            done = true;
        });
        REQUIRE(rig.wait([&] { return done; }));
        CHECK(ok);
        CHECK(!me(rig).avatar.empty());
        CHECK(file::exists(me(rig).avatar));
    }
    Rig                rig;
    const model::User &after = me(rig);
    CHECK_STR(after.displayName, "Robin");
    CHECK(!after.avatar.empty());
    model::Backend::MyProfile loaded;
    bool                      done = false;
    rig.backend->loadMyProfile([&](model::Backend::MyProfile p) {
        loaded = std::move(p);
        done   = true;
    });
    REQUIRE(rig.wait([&] { return done; }));
    CHECK_STR(loaded.displayName, "Robin");
    CHECK_STR(loaded.avatar, after.avatar);
}

TEST("backend: an idle session with a background command running is not typing") {
    FakeClaudeHome home;
    home.writeSession("shell");
    home.append(
        prompt("watch it", "2026-09-25T10:00:00.000Z") +
        assistantText("Watching.", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    Rig                rig;
    const model::User *peer = rig.user("claude:S1");
    REQUIRE(peer);
    CHECK_FALSE(peer->active); // no "working" dot…
    CHECK(peer->unavailable);  // …but the yellow one: a terminal holds it
    CHECK_STR(peer->statusText, "Running a background command");
    // Its teammate (the generalist) is yellow too: nothing of it works.
    const model::User *mate = rig.user("claude:agent");
    REQUIRE(mate);
    CHECK_FALSE(mate->active);
    CHECK(mate->unavailable);
    // Nobody thinks, not after another look at the roster either.
    home.writeSession("shell");
    rig.pump(1500);
    CHECK(rig.store.typing(rig.ref("S1")).empty());
    CHECK(std::none_of(rig.changes.begin(), rig.changes.end(), [](const model::Change &c) {
        return c.kind == model::ChangeKind::Typing;
    }));
}

TEST("backend: a background job stuck on \"working\" after its turn ended is not typing") {
    // Seen live 2026-09-25: a subagent's notification left the job reading
    // "working" (inFlight.queued 1) while its worker sat idle for good.
    FakeClaudeHome home;
    home.append(
        prompt("look for duplicates", "2026-09-25T10:00:00.000Z") +
        assistantText("Done.", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    home.writeJob(
        "S1",
        str::concat(
            {R"({"state":"working","sessionId":"S1","cwd":"/src/app","name":"duplicates",)"
             R"("linkScanPath":)",
             q(home.transcript),
             "}"}
        )
    );
    home.writeWorker("1.json", getpid(), "S1", "idle", {}, "/src/app");
    Rig                rig;
    const model::User *peer = rig.user("claude:S1");
    REQUIRE(peer);
    CHECK_FALSE(peer->active); // no "working" dot
    home.writeWorker("1.json", getpid(), "S1", "idle", {}, "/src/app");
    rig.pump(1500);
    CHECK(rig.store.typing(rig.ref("S1")).empty());
    CHECK(std::none_of(rig.changes.begin(), rig.changes.end(), [](const model::Change &c) {
        return c.kind == model::ChangeKind::Typing;
    }));
}

TEST("backend: a reply to a background job's question works though the job reads \"blocked\"") {
    // Seen live 2026-09-26: a turn ending on a question left the job "blocked",
    // and the reply's turn never flipped it back to "working" — no dot, no
    // "thinking" for two minutes of work. The worker read "busy" throughout
    // (a subagent of its was running).
    FakeClaudeHome home;
    home.append(
        prompt("look for duplicates", "2026-09-25T10:00:00.000Z") +
        assistantText("Copy it to master?", "2026-09-25T10:00:01.000Z") +
        turnEnd("2026-09-25T10:00:02.000Z")
    );
    home.writeJob(
        "S1",
        str::concat(
            {R"({"state":"blocked","detail":"Copy it to master?",)"
             R"("updatedAt":"2026-09-25T10:00:02.500Z","sessionId":"S1","cwd":"/src/app",)"
             R"("name":"duplicates","linkScanPath":)",
             q(home.transcript),
             "}"}
        )
    );
    home.writeWorker("1.json", getpid(), "S1", "busy", {}, "/src/app");
    Rig           rig;
    const ConvRef ref    = rig.ref("S1");
    const auto    peer   = [&] { return *rig.user("claude:S1"); };
    const auto    active = [&] { return rig.active("claude:S1"); };
    REQUIRE(rig.user("claude:S1"));

    // The question waits: no dot, whatever the busy worker says.
    CHECK_FALSE(peer().active);
    CHECK_STR(peer().statusText, "Waiting for you");

    // The reply's turn is under way: working, thinking since its prompt.
    home.append(
        prompt("copy changes to master", "2026-09-25T10:05:00.000Z") +
        toolUse("b1", "Bash", R"({"command":"git apply"})", "2026-09-25T10:05:01.000Z")
    );
    REQUIRE(rig.wait(active, 5000));
    CHECK_STR(peer().statusText, "Working");
    REQUIRE(rig.wait([&] { return rig.typing(ref).has_value(); }, 5000));
    CHECK(rig.typing(ref)->sinceMs == 1790330700000);

    // A worker gone idle with the turn left open (interrupted): not working.
    home.writeWorker("1.json", getpid(), "S1", "idle", {}, "/src/app");
    REQUIRE(rig.wait([&] { return !active(); }, 5000));
    home.writeWorker("1.json", getpid(), "S1", "busy", {}, "/src/app");
    REQUIRE(rig.wait(active, 5000));

    // The turn ends: back to waiting.
    home.append(
        assistantText("Copied.", "2026-09-25T10:06:00.000Z") + turnEnd("2026-09-25T10:06:01.000Z")
    );
    REQUIRE(rig.wait([&] { return !active(); }, 5000));
}

#ifndef _WIN32 // a `sleep` worker started through /bin/sh
TEST("backend: the suggested reply is offered until the reply is under way") {
    FakeClaudeHome home;
    home.append(
        prompt("look for duplicates", "2026-09-25T10:00:00.000Z") +
        assistantText("Copy it to master?", "2026-09-25T10:00:01.000Z") +
        turnEnd("2026-09-25T10:00:02.000Z")
    );
    home.writeJob(
        "S1",
        str::concat(
            {R"({"state":"blocked","tempo":"blocked","needs":"confirm the copy",)"
             R"("suggestedReply":"copy changes to 'master'","updatedAt":"2026-09-25T10:00:02.500Z",)"
             R"("sessionId":"S1","cwd":"/src/app","name":"duplicates","linkScanPath":)",
             q(home.transcript),
             "}"}
        )
    );
    home.writeWorker("1.json", getpid(), "S1", "busy", {}, "/src/app");
    // A CLI, else nothing is writable from here (it is never run: nothing is sent).
    const std::string work = tempDir("work");
    Rig               rig(Credentials{fakeLoginCli(work, true)});
    REQUIRE(rig.listedCount() == 1);
    const ConvRef ref = rig.ref("S1");
    CHECK(rig.store.conversation(ref).readOnly.empty());
    CHECK_STR(rig.backend->promptSuggestion(ref), "copy changes to 'master'");

    // The reply, typed in a terminal, is under way: nothing left to suggest.
    home.append(
        prompt("copy changes to master", "2026-09-25T10:05:00.000Z") +
        toolUse("b1", "Bash", R"({"command":"ls"})", "2026-09-25T10:05:01.000Z")
    );
    REQUIRE(rig.wait([&] { return rig.backend->promptSuggestion(ref).empty(); }, 5000));
    CHECK_FALSE(file::exists(home.dir + "/calls.log"));
}
#endif

TEST("backend: a branched-off session is a thread in its parent") {
    FakeClaudeHome    home;
    const std::string shared = prompt("hi", "2026-09-25T10:00:00.000Z") +
                               assistantText("Hello!", "2026-09-25T10:00:01.000Z") +
                               turnEnd("2026-09-25T10:00:01.500Z");
    home.writeSession("idle");
    home.append(shared);
    ::usleep(20'000); // the fork's file is the younger one (creation times in ms)
    // The fork: a copy of the parent's records so far, then its own question.
    writeFile(
        home.dir + "/projects/-src-app/S2.jsonl",
        shared + prompt("side question?", "2026-09-25T10:00:03.000Z") +
            assistantText("Side answer.", "2026-09-25T10:00:04.000Z") +
            turnEnd("2026-09-25T10:00:04.500Z")
    );
    writeFile(
        home.dir + "/sessions/2.json",
        str::concat(
            {R"({"pid":)",
             str::number(int64_t(getpid())),
             R"(,"sessionId":"S2","cwd":"/src/app","status":"idle","entrypoint":"cli"})"}
        )
    );
    // The parent went on after the branch.
    home.append(
        prompt("more", "2026-09-25T10:00:05.000Z") +
        assistantText("Sure.", "2026-09-25T10:00:06.000Z") + turnEnd("2026-09-25T10:00:06.500Z")
    );

    Rig rig;
    REQUIRE(rig.listed("S1"));
    CHECK(rig.listedCount() == 1); // the branch isn't a session in the list

    const ConvRef conv = rig.ref("S1");
    const auto   &msgs = rig.load(conv);
    REQUIRE(msgs.size() == 5);
    CHECK_STR(plain(msgs[2]), "side question?"); // in time order, between the turns
    CHECK(msgs[2].user == rig.store.me);
    CHECK(msgs[2].replyCount == 1);
    CHECK_STR(plain(msgs[3]), "more");
    const Ts root = msgs[2].ts;
    CHECK(rig.backend->threadAcceptsReplies(conv, root));
    CHECK_FALSE(rig.backend->threadAcceptsReplies(conv, msgs[0].ts));

    const auto thread = rig.loadThread(conv, root);
    REQUIRE(thread.size() == 1);
    CHECK_STR(plain(*thread[0]), "Side answer.");
    CHECK(thread[0]->threadTs == root);

    // "Open as session": the branch joins the list, its root leaves the parent.
    const ConvRef opened = rig.backend->openThreadAsSession(conv, root);
    REQUIRE(opened != kNoConv);
    CHECK_STR(rig.store.conversation(opened).id, "S2");
    CHECK(rig.listedCount() == 2);
    CHECK(rig.messages(conv).size() == 4); // at once
    CHECK(rig.load(conv).size() == 4);     // and reloaded
}

TEST("backend: /btw is offered with Claude Code's commands") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(prompt("hi", "2026-09-25T10:00:00.000Z"));
    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    const auto    cmds = rig.backend->commands(conv);
    REQUIRE(!cmds.empty());
    // A revision for the "/" list: steady while the folder's list is.
    const uint64_t rev = rig.backend->commandsRevision(conv);
    CHECK(rev != 0);
    CHECK(rig.backend->commandsRevision(conv) == rev);
    CHECK(rig.backend->commandsRevision(kNoConv) == 0);
    CHECK_STR(cmds[0].name, "btw");
    CHECK_STR(cmds[0].source, "msga"); // the row's label: msga's own
    const auto local = [&](std::string_view name) {
        return std::any_of(cmds.begin(), cmds.end(), [&](const auto &c) {
            return c.name == name && c.local;
        });
    };
    CHECK(local("status")); // run by msga itself, never sent to Claude
    CHECK(local("clear"));

    const auto status = rig.backend->runLocalCommand(conv, 0, "status", {});
    REQUIRE(!status.status.empty());
    CHECK(std::any_of(status.status.begin(), status.status.end(), [](const auto &row) {
        return row.second == "S1";
    }));
    // /clear needs the claude tool (none in this test): it says so, opens nothing.
    const auto clear = rig.backend->runLocalCommand(conv, 0, "clear", {});
    CHECK(!clear.error.empty());
    CHECK(clear.open == kNoConv);
}

TEST("backend: a subagent run is a thread holding the subagent's transcript") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("research it", "2026-09-25T10:00:00.000Z") +
        toolUse("a1", "Agent", R"({"description":"Read the docs"})", "2026-09-25T10:00:01.000Z") +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "agent42") +
        assistantText("Done.", "2026-09-25T10:00:10.000Z") + turnEnd("2026-09-25T10:00:11.000Z")
    );
    writeFile(
        home.dir + "/projects/-src-app/S1/subagents/agent-agent42.jsonl",
        prompt("Read the docs and report", "2026-09-25T10:00:02.000Z", false) +
            assistantText("The docs say X.", "2026-09-25T10:00:08.000Z") +
            turnEnd("2026-09-25T10:00:08.500Z")
    );

    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    const auto   &msgs = rig.load(conv);
    const auto    root =
        std::find_if(msgs.begin(), msgs.end(), [](const auto &m) { return m.replyCount > 0; });
    REQUIRE(root != msgs.end());
    CHECK(root->replyCount == 2);
    CHECK_STR(plain(*root), "Subagent: Read the docs");
    const Ts rootTs = root->ts;

    const auto thread = rig.loadThread(conv, rootTs);
    REQUIRE(thread.size() == 2); // prompt + answer (the root stays in the chat)
    CHECK_STR(plain(*thread[1]), "The docs say X.");
    CHECK(thread[1]->threadTs == rootTs);
    // A reply goes on to the subagent (relayed by the session); it isn't a
    // session of its own.
    CHECK(rig.backend->threadAcceptsReplies(conv, rootTs));
    CHECK_FALSE(rig.backend->threadOpensAsSession(conv, rootTs));
}

TEST("backend: a background subagent at work thinks in its thread") {
    FakeClaudeHome home;
    home.writeSession("busy");
    home.append(
        prompt("research it", "2026-09-25T10:00:00.000Z") +
        toolUse(
            "a1",
            "Agent",
            R"({"description":"Read the docs","subagent_type":"designer"})",
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:01.500Z", false, "agent42", "async_launched")
    );
    const std::string sub       = home.dir + "/projects/-src-app/S1/subagents/agent-agent42.jsonl";
    const auto        subAppend = [&](std::string_view bytes) {
        std::string all = readText(sub);
        all.append(bytes);
        writeFile(sub, all);
    };
    subAppend(
        prompt("Read the docs and report", "2026-09-25T10:00:02.000Z", false) +
        toolUse("t1", "Read", R"({"file_path":"/docs"})", "2026-09-25T10:00:03.000Z")
    );

    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    const auto   &msgs = rig.load(conv);
    const auto    root =
        std::find_if(msgs.begin(), msgs.end(), [](const auto &m) { return m.replyCount > 0; });
    REQUIRE(root != msgs.end());
    const Ts rootTs = root->ts;

    // Running: thinking in its thread, as the teammate, since its first record.
    auto typing = rig.typing(conv, rootTs);
    REQUIRE(typing);
    CHECK(typing->user == rig.store.findUser("claude:role:designer"));
    CHECK(typing->sinceMs == 1790330402000);
    // The session thinks in the chat as ever.
    REQUIRE(rig.typing(conv));
    CHECK(rig.typing(conv)->user == rig.store.findUser("claude:agent"));

    // It stops: the session is notified, a moment after its last record.
    subAppend(assistantText("The docs say X.", "2026-09-25T10:00:08.000Z"));
    home.append(taskStopped("agent42", "2026-09-25T10:00:08.050Z"));
    REQUIRE(rig.wait([&] { return !rig.typing(conv, rootTs); }, 5000));

    // A reply relayed to it starts it again — its clock from then. Nothing
    // watches a subagent's own file: the typing pump notices.
    subAppend(coordinatorNote("Which page?", "2026-09-25T10:05:00.000Z"));
    REQUIRE(rig.wait([&] { return rig.typing(conv, rootTs).has_value(); }, 5000));
    CHECK(rig.typing(conv, rootTs)->sinceMs == 1790330700000);

    // A session that's gone runs no subagents, whatever its files say.
    file::remove(home.dir + "/sessions/1.json");
    REQUIRE(rig.wait([&] { return !rig.anyThreadTyping(conv); }, 5000));
}

TEST("backend: a subagent started as a teammate speaks as that teammate") {
    // Seen live 2026-09-26: "start a @Designer subagent" ran subagent_type
    // "designer", yet its thread read as the session's Generalist.
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("draw it", "2026-09-25T10:00:00.000Z") +
        toolUse(
            "a1",
            "Agent",
            R"({"description":"Draw a test image","subagent_type":"designer"})",
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "agent42") +
        assistantText("Done.", "2026-09-25T10:00:10.000Z") + turnEnd("2026-09-25T10:00:11.000Z")
    );
    writeFile(
        home.dir + "/projects/-src-app/S1/subagents/agent-agent42.jsonl",
        prompt("Draw a test image", "2026-09-25T10:00:02.000Z", false) +
            assistantText("Drew it.", "2026-09-25T10:00:08.000Z") +
            turnEnd("2026-09-25T10:00:08.500Z")
    );

    Rig                  rig;
    const ConvRef        conv       = rig.ref("S1");
    const model::UserRef designer   = rig.store.findUser("claude:role:designer");
    const model::UserRef generalist = rig.store.findUser("claude:agent");
    const auto          &msgs       = rig.load(conv);
    const auto           root =
        std::find_if(msgs.begin(), msgs.end(), [](const auto &m) { return m.replyCount > 0; });
    REQUIRE(root != msgs.end());
    CHECK(root->user == designer);
    CHECK(msgs.back().user == generalist); // the session's own answer

    const auto thread = rig.loadThread(conv, root->ts);
    REQUIRE(thread.size() == 2);
    CHECK(thread[0]->user == generalist); // the prompt: the session wrote it
    CHECK(thread[1]->user == designer);
}

TEST("backend: a subagent's hand-back points to its thread") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("draw it", "2026-09-25T10:00:00.000Z") +
        toolUse(
            "a1",
            "Agent",
            R"({"description":"Draw a test image","subagent_type":"designer"})",
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:02.000Z", false, "agent42") +
        assistantText("Started it.", "2026-09-25T10:00:03.000Z") +
        handback("agent42", "Drew it.", "2026-09-25T10:00:08.000Z", true) +
        handback("stranger", "Not ours.", "2026-09-25T10:00:09.000Z", true) +
        assistantText("Done.", "2026-09-25T10:00:10.000Z") + turnEnd("2026-09-25T10:00:11.000Z")
    );

    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    const auto   &msgs = rig.load(conv);
    const auto    find = [&](std::string_view needle) {
        return std::find_if(msgs.begin(), msgs.end(), [&](const auto &m) {
            return m.text.find(needle) != std::string::npos;
        });
    };
    const auto root    = find("Subagent: Draw a test image");
    const auto pointer = find("reported back");
    REQUIRE(root != msgs.end());
    REQUIRE(pointer != msgs.end());
    CHECK(pointer->user == rig.store.findUser("claude:role:designer"));
    CHECK(pointer->text.find("Drew it.") == std::string::npos); // the thread has it
    CHECK_STR(pointer->subtype(), kProgressSubtype);
    const mrkdwn::Rich r    = mrkdwn::parse(pointer->text);
    const auto         link = std::find_if(r.entities.begin(), r.entities.end(), [](const auto &e) {
        return e.kind == mrkdwn::Kind::MessageLink;
    });
    REQUIRE(link != r.entities.end());
    CHECK_STR(r.text.substr(link->start, link->length), "its thread");
    const auto ref = mrkdwn::refFromToken(link->data);
    CHECK(rig.store.findConversation(ref.conv) == conv);
    CHECK(model::parseTs(ref.threadTs) == root->ts);
    // One from a subagent with no thread here is shown whole — as a report.
    const auto stranger = find("Not ours.");
    REQUIRE(stranger != msgs.end());
    CHECK(stranger->user == rig.store.findUser("claude:agent"));
    CHECK(stranger->text.find("Subagent report") != std::string::npos);
    CHECK(find("agent-message") == msgs.end());
}

TEST("backend: a plain subagent isn't taken for its session's teammate") {
    // Seen live 2026-09-26: a Researcher session with no teammate types asked
    // for two @Engineers spawned subagent_type "claude" with a self-written
    // "Role: engineer." prompt — and both threads read as the Researcher.
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("ask engineers", "2026-09-25T10:00:00.000Z") +
        promptSnapshot(
            "# Your role: Researcher (msga: researcher)\nDig.", "2026-09-25T10:00:00.500Z"
        ) +
        toolUse(
            "a1",
            "Agent",
            R"({"description":"Fix it","subagent_type":"claude","prompt":"Role: engineer. Fix it."})",
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:02.000Z", false, "agent1") +
        toolUse(
            "a2",
            "Agent",
            R"({"description":"Look around","subagent_type":"Explore","prompt":"Look."})",
            "2026-09-25T10:00:03.000Z"
        ) +
        toolResult("a2", "2026-09-25T10:00:04.000Z", false, "agent2") +
        assistantText("Started both.", "2026-09-25T10:00:05.000Z") +
        turnEnd("2026-09-25T10:00:06.000Z")
    );

    Rig                                   rig;
    std::map<std::string, model::UserRef> byText;
    for (const auto &m : rig.load(rig.ref("S1")))
        byText[plain(m)] = m.user;
    CHECK(byText["Started both."] == rig.store.findUser("claude:role:researcher"));
    CHECK(byText["Subagent: Fix it"] == rig.store.findUser("claude:role:engineer"));
    CHECK(byText["Subagent: Look around"] == rig.store.findUser("claude:agent"));
}

TEST("backend: a teammate at work as a subagent in another's session shows as working") {
    // Seen live 2026-09-28: an Engineer a Researcher session started was
    // "thinking (2m 47s)…" in its thread, its dot in the team list clear.
    FakeClaudeHome home;
    home.writeSession("idle"); // the Researcher itself waits
    home.append(
        prompt("ask an engineer", "2026-09-25T10:00:00.000Z") +
        promptSnapshot(
            "# Your role: Researcher (msga: researcher)\nDig.", "2026-09-25T10:00:00.500Z"
        ) +
        toolUse(
            "a1",
            "Agent",
            R"({"description":"Fix it","subagent_type":"engineer","prompt":"Fix it."})",
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:01.500Z", false, "eng1", "async_launched") +
        toolUse(
            "a2",
            "Agent",
            R"({"description":"Look around","subagent_type":"Explore","prompt":"Look."})",
            "2026-09-25T10:00:02.000Z"
        ) +
        toolResult("a2", "2026-09-25T10:00:02.500Z", false, "exp1", "async_launched") +
        // A foreground Engineer, done long ago: its result was its stop.
        toolUse(
            "a3",
            "Agent",
            R"({"description":"Check it","subagent_type":"engineer","prompt":"Check it.",)"
            R"("run_in_background":false})",
            "2026-09-25T09:00:00.000Z"
        ) +
        toolResult("a3", "2026-09-25T09:05:00.000Z", false, "eng0", "completed") +
        assistantText("Started them.", "2026-09-25T10:00:03.000Z") +
        turnEnd("2026-09-25T10:00:04.000Z")
    );
    const std::string subDir    = home.dir + "/projects/-src-app/S1/subagents/";
    const auto        subAppend = [&](const std::string &agentId, std::string_view bytes) {
        const std::string path = subDir + "agent-" + agentId + ".jsonl";
        std::string       all  = readText(path);
        all.append(bytes);
        writeFile(path, all);
    };
    subAppend("eng1", prompt("Fix it.", "2026-09-25T10:00:02.000Z", false));
    subAppend("exp1", prompt("Look.", "2026-09-25T10:00:03.000Z", false));
    subAppend(
        "eng0",
        prompt("Check it.", "2026-09-25T09:00:01.000Z", false) +
            assistantText("Checked.", "2026-09-25T09:04:59.000Z")
    );

    Rig               rig;
    const char *const engineer   = "claude:role:engineer";
    const char *const researcher = "claude:role:researcher";
    const char *const generalist = "claude:agent";

    // Its subagent runs: the Engineer is green, dot and card alike — its own
    // session idle, the Researcher's too. The Explore one lights no one.
    REQUIRE(rig.user(engineer));
    CHECK(rig.active(engineer));
    CHECK_FALSE(rig.user(engineer)->unavailable);
    CHECK_FALSE(rig.active(researcher));
    CHECK_FALSE(rig.active(generalist));

    // It stops: the dot clears.
    subAppend("eng1", assistantText("Fixed.", "2026-09-25T10:01:00.000Z"));
    home.append(taskStopped("eng1", "2026-09-25T10:01:00.050Z"));
    REQUIRE(rig.wait([&] { return !rig.active(engineer); }, 5000));

    // A reply relayed to it starts it again. Nothing watches a subagent's own
    // file: the typing pump, ticking for the Explore one, notices.
    subAppend("eng1", coordinatorNote("And the tests?", "2026-09-25T10:05:00.000Z"));
    REQUIRE(rig.wait([&] { return rig.active(engineer); }, 5000));

    // Both stop: the pump's last tick, the one that finds nothing at work,
    // still clears the dot.
    subAppend("eng1", assistantText("Tested.", "2026-09-25T10:06:00.000Z"));
    subAppend("exp1", assistantText("Looked.", "2026-09-25T10:06:00.000Z"));
    home.append(
        taskStopped("eng1", "2026-09-25T10:06:00.050Z") +
        taskStopped("exp1", "2026-09-25T10:06:00.050Z")
    );
    REQUIRE(rig.wait([&] { return !rig.active(engineer); }, 5000));
    CHECK_FALSE(rig.active(generalist));
    // No subagent thinks any more — the finished foreground one neither.
    CHECK(rig.wait([&] { return !rig.anyThreadTyping(rig.ref("S1")); }, 5000));
}

TEST("backend: a reply relayed to a subagent is in its thread, not the chat") {
    FakeClaudeHome home;
    home.writeSession("idle");
    // As Claude Code 2.1.282 records it: the relay is the session's prompt; the
    // subagent gets it as the coordinator's (isMeta, hidden) and answers.
    home.append(
        prompt("research it", "2026-09-25T10:00:00.000Z") +
        toolUse("a1", "Agent", R"({"description":"Read the docs"})", "2026-09-25T10:00:01.000Z") +
        toolResult("a1", "2026-09-25T10:00:02.000Z", false, "agent42") +
        assistantText("Started.", "2026-09-25T10:00:03.000Z") +
        turnEnd("2026-09-25T10:00:04.000Z") +
        prompt(subagentReplyPrompt("agent42", "Which page?"), "2026-09-25T10:01:00.000Z") +
        toolUse("s1", "SendMessage", R"({"to":"agent42"})", "2026-09-25T10:01:01.000Z") +
        toolResult("s1", "2026-09-25T10:01:02.000Z") +
        assistantText("Sent.", "2026-09-25T10:01:03.000Z") + turnEnd("2026-09-25T10:01:04.000Z")
    );
    writeFile(
        home.dir + "/projects/-src-app/S1/subagents/agent-agent42.jsonl",
        prompt("Read the docs", "2026-09-25T10:00:01.500Z", false) +
            assistantText("Reading.", "2026-09-25T10:00:05.000Z") +
            coordinatorNote(
                "The coordinator sent a message while you were working:\nWhich page?\n\n"
                "Address this before completing your current task.",
                "2026-09-25T10:01:02.000Z"
            ) +
            assistantText("Page 3.", "2026-09-25T10:01:05.000Z")
    );

    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    const auto   &msgs = rig.load(conv);
    CHECK(std::none_of(msgs.begin(), msgs.end(), [](const auto &m) {
        return contains(m.text, "Which page?") || m.threadTs != 0;
    }));
    const auto root =
        std::find_if(msgs.begin(), msgs.end(), [](const auto &m) { return m.replyCount > 0; });
    REQUIRE(root != msgs.end());
    CHECK(root->replyCount == 4); // its prompt and 2 answers, and the reply
    const Ts rootTs = root->ts, latest = root->latestReply;

    const auto t = rig.loadThread(conv, rootTs);
    REQUIRE(t.size() == 4);
    CHECK_STR(plain(*t[1]), "Reading.");
    CHECK_STR(plain(*t[2]), "Which page?"); // yours, in time order
    CHECK(t[2]->user == rig.store.me);
    CHECK(t[2]->threadTs == rootTs);
    CHECK_STR(plain(*t[3]), "Page 3.");
    CHECK(latest == t[3]->ts);
}

TEST("backend: deleting a message in a session") {
    FakeClaudeHome    home;
    // A finished background session nothing is writing to…
    const std::string sid          = "8d953db6-f3be-4b02-8f0f-09aea0343b3e";
    const std::string bgTranscript = home.dir + "/projects/-src-app/" + sid + ".jsonl";
    home.writeJob(
        "8d953db6",
        str::concat(
            {R"({"state":"done","sessionId":")",
             sid,
             R"(","cwd":"/src/app","name":"finished","linkScanPath":)",
             q(bgTranscript),
             "}"}
        )
    );
    writeFile(bgTranscript, linkedTurns());
    // …and one open in a terminal, which keeps what it has said.
    home.writeSession("idle");
    home.append(
        linked(prompt("hi", "2026-09-25T09:00:00.000Z"), "q1", nullptr) +
        linked(answer("Hello!", "m9", "2026-09-25T09:00:01.000Z"), "q2", "q1") +
        linked(turnEnd("2026-09-25T09:00:02.000Z"), "q3", "q2")
    );

    Rig rig;
    REQUIRE(rig.listedCount() == 2);

    const ConvRef term     = rig.ref("S1");
    const auto   &termMsgs = rig.load(term);
    REQUIRE(termMsgs.size() == 2);
    CHECK_FALSE(rig.backend->canDeleteMessage(term, termMsgs[0].ts));

    const ConvRef conv = rig.ref(sid);
    const auto   &msgs = rig.load(conv);
    REQUIRE(msgs.size() == 5);
    CHECK(rig.backend->canDeleteMessage(conv, msgs[2].ts));       // a prompt
    CHECK(rig.backend->canDeleteMessage(conv, msgs[4].ts));       // an answer
    CHECK_FALSE(rig.backend->canDeleteMessage(conv, msgs[3].ts)); // tool calls
    CHECK_FALSE(rig.backend->canDeleteMessage(conv, 1000001));

    rig.changes.clear();
    const Ts       gone  = msgs[2].ts;
    const uint64_t read0 = rig.backend->counters().bytesRead;
    rig.backend->remove(conv, gone);
    // Rewritten and read again on a worker: none of it on the UI thread.
    CHECK(rig.wait([&] {
        return std::any_of(rig.changes.begin(), rig.changes.end(), [&](const model::Change &c) {
            return c.kind == model::ChangeKind::Remove && c.conv == conv && c.ts == gone;
        });
    }));
    CHECK(rig.backend->counters().bytesRead == read0);
    const auto &left = rig.load(conv);
    REQUIRE(left.size() == 4);
    CHECK_STR(plain(left[1]), "Kept.");
    CHECK_STR(plain(left[3]), "Noted.");
    CHECK(!contains(readText(bgTranscript), "banana"));
}

TEST("backend: reactions stay in msga") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        linked(prompt("hi", "2026-09-25T09:00:00.000Z"), "q1", nullptr) +
        linked(answer("Hello!", "m9", "2026-09-25T09:00:01.000Z"), "q2", "q1") +
        linked(turnEnd("2026-09-25T09:00:02.000Z"), "q3", "q2")
    );

    Rig rig;
    REQUIRE(rig.listedCount() == 1);
    const ConvRef conv = rig.ref("S1");
    const auto   &msgs = rig.load(conv);
    REQUIRE(msgs.size() == 2);
    const Ts   answerTs = msgs[1].ts;
    const auto updates  = [&] {
        return std::count_if(rig.changes.begin(), rig.changes.end(), [&](const model::Change &c) {
            return c.kind == model::ChangeKind::Update && c.conv == conv && c.ts == answerTs;
        });
    };

    rig.changes.clear();
    rig.backend->react(conv, answerTs, "tada", true);
    rig.backend->react(conv, answerTs, "tada", true); // once is enough
    rig.backend->react(conv, answerTs, "heart", true);
    CHECK(updates() == 2);

    // Served again with the history (a chat reloaded after a switch)…
    {
        const auto &again = rig.load(conv);
        REQUIRE(again.size() == 2);
        REQUIRE(again[1].reactions.size() == 2);
        CHECK_STR(again[1].reactions[0].name, "tada");
        CHECK(again[1].reactions[0].count == 1);
        CHECK(again[1].reactions[0].users == std::vector<model::UserRef>{rig.store.me});
        CHECK(again[0].reactions.empty());
    }

    // …not news when the transcript grows, and never written to it.
    rig.changes.clear();
    home.append(linked(prompt("more", "2026-09-25T09:01:00.000Z"), "q4", "q3"));
    REQUIRE(rig.wait([&] { return rig.messages(conv).size() == 3; }));
    CHECK(std::none_of(rig.changes.begin(), rig.changes.end(), [](const model::Change &c) {
        return c.kind == model::ChangeKind::Update;
    }));
    CHECK(!contains(readText(home.transcript), "tada"));

    rig.changes.clear();
    rig.backend->react(conv, answerTs, "tada", false);
    rig.backend->react(conv, answerTs, "tada", false);
    CHECK(updates() == 1);
    const auto &again = rig.load(conv);
    REQUIRE(again.size() == 3);
    REQUIRE(again[1].reactions.size() == 1);
    CHECK_STR(again[1].reactions[0].name, "heart");
}

TEST("backend: every session is found, and one can be added to the list") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(prompt("listed one", "2026-09-25T09:00:00.000Z"));
    // An ended session msga has never seen, in another folder, and a big one
    // whose middle is never read.
    writeFile(
        home.dir + "/projects/-src-other/aaaaaaaa-0000-0000-0000-000000000001.jsonl",
        R"({"type":"user","cwd":"/src/other","timestamp":"2026-09-25T08:00:00.000Z",)"
        R"("origin":{"kind":"human"},"message":{"content":"old question"}})"
        "\n" +
            assistantText("Old answer.", "2026-09-25T08:00:01.000Z") +
            turnEnd("2026-09-25T08:00:02.000Z") + R"({"type":"ai-title","aiTitle":"Old work"})" +
            "\n"
    );
    std::string big = prompt("big start", "2026-09-25T07:00:00.000Z");
    while (big.size() < 400 * 1024)
        big += assistantText(std::string(500, 'x'), "2026-09-25T07:00:01.000Z");
    big += R"({"type":"last-prompt","lastPrompt":"big end"})"
           "\n";
    writeFile(home.dir + "/projects/-src-big/bbbbbbbb-0000-0000-0000-000000000002.jsonl", big);

    Rig rig;
    REQUIRE(rig.listedCount() == 1);

    std::vector<model::Backend::FoundSession> found;
    bool                                      answered = false;
    rig.backend->findAgentSessions([&](std::vector<model::Backend::FoundSession> f) {
        found    = std::move(f);
        answered = true;
    });
    REQUIRE(rig.wait([&] { return answered; }, 5000));
    REQUIRE(found.size() == 3);
    const auto byId = [&](std::string_view prefix) {
        return *std::find_if(found.begin(), found.end(), [&](const auto &f) {
            return str::startsWith(f.id, prefix);
        });
    };
    CHECK(byId("S1").listed == rig.ref("S1"));
    CHECK_STR(byId("bbbbbbbb").firstPrompt, "big start");
    CHECK_STR(byId("bbbbbbbb").lastPrompt, "big end");
    const auto old = byId("aaaaaaaa");
    CHECK(old.listed == kNoConv);
    CHECK_STR(old.title, "Old work");
    CHECK_STR(old.folder, "/src/other");

    rig.changes.clear();
    const ConvRef conv = rig.backend->addFoundSession(old.id);
    REQUIRE(conv != kNoConv);
    CHECK_STR(rig.store.conversation(conv).id, old.id);
    CHECK_STR(rig.store.conversation(conv).name, "Old work");
    CHECK(rig.store.conversation(conv).member);
    CHECK(rig.store.conversation(conv).unread == 0); // its history isn't news
    CHECK(rig.listedCount() == 2);
    const auto &history = rig.load(conv);
    REQUIRE(history.size() == 2);
    CHECK_STR(plain(history[1]), "Old answer.");
    CHECK(rig.backend->addFoundSession(old.id) == conv); // already there
    CHECK(rig.backend->addFoundSession("nope") == kNoConv);
}

TEST("backend: adding the original of a listed copy keeps both sessions") {
    FakeClaudeHome    home;
    // S1 (listed) is a copy Claude Code made when an ended session was resumed:
    // the original's records, then more. The original itself isn't listed.
    const std::string original = prompt("/clear", "2026-09-25T08:00:00.000Z") +
                                 assistantText("Cleared.", "2026-09-25T08:00:01.000Z") +
                                 turnEnd("2026-09-25T08:00:02.000Z");
    writeFile(home.dir + "/projects/-src-app/cccccccc-0000-0000-0000-000000000003.jsonl", original);
    ::usleep(20'000); // the copy is the younger file
    home.writeSession("idle");
    home.append(
        original + prompt("go on", "2026-09-25T09:00:00.000Z") +
        assistantText("Going.", "2026-09-25T09:00:01.000Z") + turnEnd("2026-09-25T09:00:02.000Z")
    );

    Rig rig;
    REQUIRE(rig.listedCount() == 1);
    const ConvRef conv = rig.backend->addFoundSession("cccccccc-0000-0000-0000-000000000003");
    REQUIRE(conv != kNoConv);
    CHECK_STR(rig.store.conversation(conv).id, "cccccccc-0000-0000-0000-000000000003");
    CHECK(rig.listedCount() == 2); // the copy didn't turn into a thread of the original
    CHECK(rig.load(rig.ref("S1")).size() == 4);
}

TEST("backend: a session's answers carry the files it made, until it's removed") {
    FakeClaudeHome home;
    home.writeSession("idle");
    const int64_t now = base::nowMicros() / 1000;
    writeFile(home.dir + "/work/chart.png", pngBytes(30, 20));
    home.append(
        prompt("make a chart", isoAt(now - 30'000)) +
        assistantText("Saved it to `" + home.dir + "/work/chart.png`.", isoAt(now)) +
        turnEnd(isoAt(now + 1))
    );
    clearOutputs("S1");

    Rig rig;
    // Nothing is copied for a conversation nobody opened (the first scan
    // renders no messages: Backend::seenOf).
    CHECK_FALSE(file::exists(outputsDir("S1")));
    CHECK(rig.store.conversation(rig.ref("S1")).unread == 0);
    const ConvRef conv = rig.ref("S1");
    REQUIRE(!rig.load(conv).empty());
    // Copied on a worker: the answer shows them once they are.
    CHECK(rig.wait([&] { return rig.messages(conv).back().files().size() == 1; }));
    const auto &files = rig.messages(conv).back().files();
    REQUIRE(files.size() == 1);
    CHECK_STR(files[0].name, "chart.png");
    CHECK(files[0].isImage());
    CHECK(files[0].width == 30);
    CHECK(file::exists(outputsDir("S1")));

    rig.backend->leave(rig.ref("S1")); // "Remove from msga"
    CHECK_FALSE(file::exists(outputsDir("S1")));
}

TEST("backend: a live transcript is read as it grows, only what was appended") {
    FakeClaudeHome home;
    home.writeSession("busy");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );

    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    REQUIRE(conv != kNoConv);
    // The first scan read the transcript on a worker, not on the UI thread.
    const auto &n = rig.backend->counters();
    CHECK(n.bytesRead == 0);
    REQUIRE(rig.load(conv).size() == 2);
    CHECK(n.bytesRead == 0);

    // Nothing new: a refresh (the session's status file touched) renders
    // nothing again and copies nothing into the Store.
    const uint64_t read0 = n.bytesRead, renders0 = n.renders, copies0 = n.copies;
    home.writeSession("idle");
    rig.pump(1500);
    home.writeSession("busy");
    rig.pump(1500);
    CHECK(n.bytesRead == read0);
    CHECK(n.renders == renders0);
    CHECK(n.copies == copies0);
    // A refresh that finds nothing changed in it doesn't sync it at all.
    const uint64_t syncs0 = n.syncs;
    home.writeSession("busy"); // the same again: watched, so a refresh
    rig.pump(1500);
    CHECK(n.syncs == syncs0);

    // Half a record, then the rest: only those bytes are read, and the
    // record shows once it's whole.
    const std::string next =
        prompt("again", "2026-09-25T10:01:00.000Z") + turnEnd("2026-09-25T10:01:01.000Z");
    const size_t half = next.size() / 2;
    home.append(std::string_view(next).substr(0, half));
    rig.pump(1500);
    CHECK(n.bytesRead == read0 + half);
    CHECK(rig.messages(conv).size() == 2);
    home.append(std::string_view(next).substr(half));
    CHECK(rig.wait([&] { return rig.messages(conv).size() == 3; }));
    CHECK(n.bytesRead == read0 + next.size());
    CHECK(n.renders == renders0 + 1); // the new prompt alone
    CHECK_STR(plain(rig.messages(conv).back()), "again");

    // Rewritten shorter (truncated, or written anew): read again from the start.
    writeFile(home.transcript, prompt("fresh start", "2026-09-25T11:00:00.000Z"));
    CHECK(rig.wait([&] {
        const auto &m = rig.messages(conv);
        return m.size() == 1 && plain(m[0]) == "fresh start";
    }));
}

TEST("backend: a long session opened renders its newest page only") {
    FakeClaudeHome home;
    home.writeSession("idle");
    std::string records;
    for (int i = 0; i < 300; ++i) {
        const int64_t ms = 1'790'000'000'000 + int64_t(i) * 10'000;
        records += prompt("question " + str::number(i), isoAt(ms)) +
                   assistantText("answer " + str::number(i), isoAt(ms + 1000)) +
                   turnEnd(isoAt(ms + 2000));
    }
    home.append(records);

    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    const auto   &n    = rig.backend->counters();
    REQUIRE(rig.load(conv).size() == 200); // a page of the 600 messages
    CHECK(n.renders == 200);
    CHECK_STR(plain(rig.messages(conv).back()), "answer 299");
    CHECK(rig.store.conversation(conv).hasMoreBefore);

    // The page before: those are rendered then.
    bool done = false;
    rig.backend->loadHistory(conv, rig.messages(conv).front().ts, [&](bool, const std::string &) {
        done = true;
    });
    REQUIRE(rig.wait([&] { return done; }));
    CHECK(rig.messages(conv).size() == 400);
    CHECK(n.renders == 400);
}

TEST("backend: a subagent that stopped isn't read to tell it isn't running") {
    FakeClaudeHome home;
    home.writeSession("busy");
    home.append(
        prompt("research it", "2026-09-25T10:00:00.000Z") +
        toolUse(
            "a1",
            "Agent",
            R"({"description":"Read the docs","subagent_type":"designer"})",
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:01.500Z", false, "agent42", "async_launched") +
        taskStopped("agent42", "2026-09-25T10:00:08.050Z")
    );
    // Its file last written a moment after it was told to have stopped.
    const std::string sub = home.dir + "/projects/-src-app/S1/subagents/agent-agent42.jsonl";
    writeFile(
        sub,
        prompt("Read the docs and report", "2026-09-25T10:00:02.000Z", false) +
            assistantText("The docs say X.", "2026-09-25T10:00:08.000Z")
    );
    utimbuf times;
    times.actime = times.modtime = 1790330408; // 10:00:08Z
    REQUIRE(::utime(sub.c_str(), &times) == 0);

    // The session works (its typing pump looks for running subagents every
    // tick); the stopped one is never read.
    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    REQUIRE(rig.typing(conv));
    rig.pump(300);
    CHECK(rig.backend->counters().bytesRead == 0);
    CHECK_FALSE(rig.anyThreadTyping(conv));

    // Written since (a reply relayed to it): it runs again, read to tell.
    appendFile(sub, coordinatorNote("Which page?", "2026-09-25T10:05:00.000Z"));
    REQUIRE(rig.wait([&] { return rig.anyThreadTyping(conv); }, 5000));
    CHECK(int64_t(rig.backend->counters().bytesRead) == file::size(sub));
}

TEST("backend: a long subagent transcript is read on a worker") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("research it", "2026-09-25T10:00:00.000Z") +
        toolUse("a1", "Agent", R"({"description":"Read the docs"})", "2026-09-25T10:00:01.000Z") +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "agent42", "completed") +
        assistantText("Done.", "2026-09-25T10:00:10.000Z") + turnEnd("2026-09-25T10:00:11.000Z")
    );
    // Over a megabyte: a prompt and 1199 remarks.
    std::string       records = prompt("Read the docs", "2026-09-25T10:00:02.000Z", false);
    const std::string filler(900, 'x');
    for (int i = 1; i < 1200; ++i)
        records += assistantText(filler, isoAt(1'790'330'402'000 + i));
    REQUIRE(records.size() > (1u << 20));
    writeFile(home.dir + "/projects/-src-app/S1/subagents/agent-agent42.jsonl", records);

    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    rig.load(conv);
    // Counted once it's read — not here.
    const auto root = [&]() -> const model::Message * {
        for (const auto &m : rig.messages(conv))
            if (contains(plain(m), "Read the docs"))
                return &m;
        return nullptr;
    };
    REQUIRE(root());
    REQUIRE(rig.wait([&] { return root()->replyCount == 1200; }, 5000));
    CHECK(rig.backend->counters().bytesRead == 0);
    CHECK(rig.loadThread(conv, root()->ts).size() == 1200);
}

TEST("backend: search finds what the lists show, as they show it") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("Fix the **login** page", "2026-09-25T10:00:00.000Z") +
        toolUse("t1", "Bash", R"({"command":"grep login"})", "2026-09-25T10:00:01.000Z") +
        toolResult("t1", "2026-09-25T10:00:02.000Z") +
        toolUse(
            "a1", "Agent", R"({"description":"Check the login docs"})", "2026-09-25T10:00:03.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:04.000Z", false, "agent42", "completed") +
        assistantText(
            "The `login` page works: see <https://x.test/login>.", "2026-09-25T10:00:05.000Z"
        ) +
        turnEnd("2026-09-25T10:00:06.000Z") + prompt("thanks", "2026-09-25T10:00:07.000Z")
    );

    Rig        rig;
    const auto search = [&](std::string q) -> std::vector<Backend::SearchHit> {
        std::vector<Backend::SearchHit> hits;
        bool                            done = false;
        rig.backend->search(std::move(q), [&](std::vector<Backend::SearchHit> h) {
            hits = std::move(h);
            done = true;
        });
        CHECK(rig.wait([&] { return done; }));
        return hits;
    };
    // Nothing of the session rendered yet, then the same once it's shown.
    const auto    before = search("  LOGIN ");
    const ConvRef conv   = rig.ref("S1");
    const auto   &msgs   = rig.load(conv);
    const auto    after  = search("login");
    REQUIRE(before.size() == 3); // the prompt, the subagent, the answer (not the card)
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK(before[i].conv == conv);
        CHECK(before[i].ts == after[i].ts);
        CHECK(before[i].thread == 0);
        CHECK_STR(before[i].text, after[i].text);
        // Newest first, the text as the list has it.
        if (i)
            CHECK(before[i].ts < before[i - 1].ts);
        const auto m = std::find_if(msgs.begin(), msgs.end(), [&](const auto &x) {
            return x.ts == before[i].ts;
        });
        REQUIRE(m != msgs.end());
        CHECK_STR(before[i].text, m->text);
    }
    CHECK(search("nothing like it").empty());
    CHECK(search("  ").empty());
}

TEST("backend: a long transcript found after the first scan is read on a worker") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(prompt("hi", "2026-09-25T10:00:00.000Z"));
    Rig            rig;
    const auto    &n     = rig.backend->counters();
    const uint64_t read0 = n.bytesRead;

    // Another session starts in a terminal, resuming a long one (> 1 MB).
    std::string       records;
    const std::string filler(900, 'x');
    for (int i = 0; records.size() < (3u << 19); ++i) {
        const int64_t ms = 1'790'000'000'000 + int64_t(i) * 10'000;
        records += prompt("q" + str::number(i) + " " + filler, isoAt(ms)) +
                   assistantText("a" + str::number(i), isoAt(ms + 1000)) +
                   turnEnd(isoAt(ms + 2000));
    }
    writeFile(home.dir + "/projects/-src-app/S2.jsonl", records);
    json::Writer w;
    w.beginObject();
    w.key("pid").value(int64_t(getpid()));
    w.key("sessionId").value("S2");
    w.key("cwd").value("/src/app");
    w.key("status").value("idle");
    w.key("entrypoint").value("cli");
    w.endObject();
    file::writeAtomic(home.dir + "/sessions/2.json", w.str());

    REQUIRE(rig.wait([&] { return rig.listed("S2"); }));
    const ConvRef conv = rig.ref("S2");
    REQUIRE(rig.load(conv).size() == 200);
    CHECK_STR(plain(rig.messages(conv).back()).substr(0, 1), "a");
    CHECK(n.bytesRead == read0); // all of it read on the worker
    // Its history is no news: nothing to read, no badge.
    CHECK(rig.store.conversation(conv).unread == 0);
}

TEST("backend: a transcript under a non-ASCII folder is read") {
    FakeClaudeHome home;
    home.writeSession("idle");
    // Not -src-app: the session's transcript is found by its id.
    writeFile(
        home.dir + "/projects/-home-j\xC3\xB6ran-app/S1.jsonl",
        prompt("hej", "2026-09-25T10:00:00.000Z") +
            assistantText("Hej Jöran!", "2026-09-25T10:00:01.000Z") +
            turnEnd("2026-09-25T10:00:02.000Z")
    );
    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    REQUIRE(conv != kNoConv);
    const auto &msgs = rig.load(conv);
    REQUIRE(msgs.size() == 2);
    CHECK_STR(plain(msgs[1]), "Hej Jöran!");
    // …and it's followed as it grows (its size and time looked at each tick).
    appendFile(
        home.dir + "/projects/-home-j\xC3\xB6ran-app/S1.jsonl",
        prompt("again", "2026-09-25T10:01:00.000Z")
    );
    home.writeSession("busy");
    CHECK(rig.wait([&] { return rig.messages(conv).size() == 3; }));
}

TEST("backend: a subagent's thread follows its transcript, reading what it appends") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("research it", "2026-09-25T10:00:00.000Z") +
        toolUse("a1", "Agent", R"({"description":"Read the docs"})", "2026-09-25T10:00:01.000Z") +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "agent42") +
        assistantText("Done.", "2026-09-25T10:00:10.000Z") + turnEnd("2026-09-25T10:00:11.000Z")
    );
    const std::string sub = home.dir + "/projects/-src-app/S1/subagents/agent-agent42.jsonl";
    writeFile(
        sub,
        prompt("Read the docs and report", "2026-09-25T10:00:02.000Z", false) +
            assistantText("The docs say X.", "2026-09-25T10:00:08.000Z")
    );

    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    const auto   &msgs = rig.load(conv);
    const auto    root =
        std::find_if(msgs.begin(), msgs.end(), [](const auto &m) { return m.replyCount > 0; });
    REQUIRE(root != msgs.end());
    CHECK(root->replyCount == 2);
    const Ts rootTs = root->ts;
    REQUIRE(rig.loadThread(conv, rootTs).size() == 2);

    // The subagent goes on: its thread follows, and only the appended record
    // is read.
    const auto       &n     = rig.backend->counters();
    const uint64_t    read0 = n.bytesRead;
    const std::string more  = assistantText("And Y.", "2026-09-25T10:00:08.500Z");
    appendFile(sub, more);
    home.writeSession("idle"); // a refresh (the subagent's file isn't watched)
    CHECK(rig.wait([&] {
        const auto r = rig.replies(conv, rootTs);
        return r.size() == 3 && plain(*r[2]) == "And Y.";
    }));
    CHECK(n.bytesRead == read0 + more.size());
}

TEST("backend: known-sessions.json is written only when what it says changes") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    const std::string known = home.msga + "/data/known-sessions.json";

    Rig           rig;
    const ConvRef conv = rig.ref("S1");
    REQUIRE(conv != kNoConv);
    CHECK(rig.wait([&] { return file::exists(known); }));
    rig.pump(2500); // the first scan's save, if one is still to come
    const int64_t written = mtimeMicros(known);
    REQUIRE(written > 0);

    // Refreshes that change nothing msga keeps write nothing.
    for (int i = 0; i < 2; ++i) {
        home.writeSession(i % 2 ? "idle" : "busy");
        rig.pump(1500);
    }
    home.writeSession("idle");
    rig.pump(3000); // past the save timer
    CHECK(mtimeMicros(known) == written);

    // Something it keeps changes: written.
    rig.backend->setStarred(conv, true);
    CHECK(rig.wait([&] { return mtimeMicros(known) != written; }));
    CHECK(contains(readText(known), "\"starred\":true"));
}

TEST("backend: the sidebar's own sections are kept with the known sessions") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    const std::string known = home.msga + "/data/known-sessions.json";
    {
        Rig           rig;
        const ConvRef conv = rig.ref("S1");
        REQUIRE(conv != kNoConv);
        CHECK(rig.backend->capabilities().customSections);
        rig.backend->setStarred(conv, true);
        rig.backend->createSection("Releases", "ship", conv);
        CHECK_FALSE(rig.store.conversation(conv).starred); // moved out of Starred
        REQUIRE(rig.store.sections().size() == 1);
        CHECK(rig.wait([&] { return contains(readText(known), "\"Releases\""); }));
    }
    // The next run lists them again, the session still in its section.
    Rig rig;
    REQUIRE(rig.store.sections().size() == 1);
    CHECK_STR(rig.store.sections()[0].name, "Releases");
    CHECK_STR(rig.store.sections()[0].emoji, "ship");
    REQUIRE(rig.store.sections()[0].convs.size() == 1);
    CHECK_STR(rig.store.sections()[0].convs[0], "S1");
    rig.backend->deleteSection(rig.store.sections()[0].id);
    CHECK(rig.wait([&] { return !contains(readText(known), "\"Releases\""); }));
}

// POSIX only from here: these cases drive a fake `claude` written as a
// /bin/sh script (and real `sleep` workers); Windows runs no such script.
#ifndef _WIN32

TEST("backend: no session starts while Claude Code is logged out") {
    FakeClaudeHome    home;
    const std::string work = tempDir("work");
    home.trust(work);
    Rig           rig(Credentials{fakeLoginCli(work, false)});
    const ConvRef conv = rig.start(work);
    CHECK_STR(rig.lastStartError, notLoggedInMessage());
    CHECK(conv == kNoConv);
    CHECK_FALSE(file::exists(home.dir + "/calls.log"));
}

// ── The backend: sending and managing, through fake CLIs ───────────────────

#if defined(__linux__)
TEST("backend: removing a session deletes it only when msga, or a session of msga's, started it") {
    FakeClaudeHome    home;
    const std::string work = tempDir("work");
    file::makeDirs(home.dir + "/jobs");
    // A CLI whose `stop` the daemon doesn't act on: the worker lingers, and
    // msga has to end it itself.
    const std::string cli              = writeCli(work, R"SH(#!/bin/sh
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
printf '%s\n' "$*" >> "$CLAUDE_CONFIG_DIR/calls.log"
)SH");
    const std::string parent           = "aaaa1111-0000-4000-8000-000000000001";
    const std::string child            = "bbbb2222-0000-4000-8000-000000000002";
    const std::string other            = "cccc3333-0000-4000-8000-000000000003";
    const std::string helper           = "dddd4444-0000-4000-8000-000000000004";
    const std::string parentTranscript = home.dir + "/projects/-src-app/" + parent + ".jsonl";
    // …and one of its subagents ran `claude --bg`, which started `helper`.
    writeFile(
        Paths::subagentTranscript(parentTranscript, "agent7"),
        "{\"type\":\"user\",\"timestamp\":\"2026-09-25T10:00:02.000Z\",\"message\":{\"role\":"
        "\"user\",\"content\":[{\"tool_use_id\":\"t2\",\"type\":\"tool_result\",\"content\":"
        "\"backgrounded \xc2\xb7 dddd4444\\n  claude agents\"}]}}\n"
    );
    // The session msga started ran `claude --bg`, which started `child`.
    writeFile(
        parentTranscript,
        prompt("start a helper", "2026-09-25T10:00:00.000Z") +
            "{\"type\":\"user\",\"timestamp\":\"2026-09-25T10:00:01.000Z\",\"message\":{\"role\":"
            "\"user\",\"content\":[{\"tool_use_id\":\"t1\",\"type\":\"tool_result\",\"content\":"
            "\"backgrounded \xc2\xb7 bbbb2222\\n  claude agents\"}]}}\n"
    );
    std::vector<int64_t> workers;
    int                  n = 0;
    for (const std::string &sid : {parent, child, other, helper}) {
        const std::string short8 = sid.substr(0, 8);
        home.writeJob(
            short8,
            str::concat(
                {R"({"state":"done","sessionId":")",
                 sid,
                 R"(","cwd":)",
                 q(work),
                 R"(,"name":")",
                 short8,
                 R"(","linkScanPath":)",
                 q(sid == parent ? parentTranscript
                                 : home.dir + "/projects/-src-app/" + sid + ".jsonl"),
                 "}"}
            )
        );
        const int64_t pid = home.spawnWorker();
        REQUIRE(pid > 0);
        workers.push_back(pid);
        home.writeWorker("w" + str::number(int64_t(++n)) + ".json", pid, sid, "idle", short8);
    }
    // msga started `parent` in an earlier run.
    writeFile(
        dirs().data + "/known-sessions.json",
        str::concat({R"({"sessions":[],"started":[")", parent, "\"]}"})
    );
    const auto rmCalls = [&] {
        int k = 0;
        for (const auto &c : home.calls())
            k += str::startsWith(c, "rm ");
        return k;
    };

    Rig rig(Credentials{cli});
    REQUIRE(rig.listedCount() == 4);

    // Started elsewhere: it only leaves the list.
    rig.backend->leave(rig.ref(other));
    rig.pump(1500);
    CHECK(!contains(home.text("calls.log"), "cccc3333"));
    CHECK(isProcessAlive(workers[2]));
    CHECK_FALSE(rig.listed(other));

    // Started by msga's session: deleted (`claude rm`), its lingering worker
    // ended too.
    rig.backend->leave(rig.ref(child));
    REQUIRE(rig.wait([&] { return contains(home.text("calls.log"), "rm bbbb2222"); }, 5000));
    CHECK(rig.wait([&] { return !isProcessAlive(workers[1]); }, 16000));
    CHECK(isProcessAlive(workers[0]));
    CHECK(isProcessAlive(workers[2]));
    CHECK(!contains(home.text("calls.log"), "stop"));
    CHECK(rmCalls() == 1);

    // Started by a subagent of msga's session: deleted too. Which it is was
    // found by reading transcripts on a worker, none of it on the UI thread.
    const uint64_t read0 = rig.backend->counters().bytesRead;
    rig.backend->leave(rig.ref(helper));
    CHECK_FALSE(rig.listed(helper)); // gone from the list at once
    REQUIRE(rig.wait([&] { return contains(home.text("calls.log"), "rm dddd4444"); }, 5000));
    CHECK(rig.wait([&] { return !isProcessAlive(workers[3]); }, 16000));
    CHECK(rig.backend->counters().bytesRead == read0);
    CHECK(rmCalls() == 2);
}

TEST("backend: removing a session msga started deletes every worktree it used") {
    FakeClaudeHome    home;
    const std::string vcs = base::findExecutable("git");
    REQUIRE(!vcs.empty());
    const std::string work = tempDir("work");
    char             *real = ::realpath(work.c_str(), nullptr);
    REQUIRE(real);
    const std::string repo = std::string(real) + "/repo";
    std::free(real);
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
        const base::RunResult r = base::run(vcs, all, o);
        if (r.code != 0)
            ok = false;
        return r.output;
    };
    run(repo, {"init", "-q", "-b", "master"});
    writeFile(repo + "/a.txt", "a\n");
    run(repo, {"add", "a.txt"});
    run(repo, {"commit", "-q", "-m", "a"});
    const std::string trees = repo + "/.claude/worktrees";
    const auto        add   = [&](const std::string &name) {
        run(repo, {"worktree", "add", "-q", "-b", "wt-" + name, trees + "/" + name});
        return trees + "/" + name;
    };
    const std::string inJob     = add("job");     // the job's own, dirty
    const std::string inRecords = add("records"); // only in the transcript, left, unpushed
    const std::string inSub     = add("sub");     // a subagent's
    const std::string inUse     = add("busy");    // entered, but a live session works in it
    const std::string foreign   = add("foreign"); // another session's, not msga's
    writeFile(inJob + "/scratch.txt", "uncommitted\n");
    writeFile(inRecords + "/b.txt", "b\n");
    run(inRecords, {"add", "b.txt"});
    run(inRecords, {"commit", "-q", "-m", "x"});
    writeFile(inRecords + "/a.txt", "changed\n");
    REQUIRE(ok);

    // Claude Code: `rm` keeps the dirty worktree (and the job), as it does.
    const std::string cli        = writeCli(work, R"SH(#!/bin/sh
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
printf '%s\n' "$*" >> "$CLAUDE_CONFIG_DIR/calls.log"
if [ "$1" = rm ]; then
  echo "kept $2 — its worktree is still at somewhere"
  echo "  it has uncommitted changes"
  exit 1
fi
)SH");
    const std::string mine       = "aaaa1111-0000-4000-8000-000000000001";
    const std::string theirs     = "cccc3333-0000-4000-8000-000000000003";
    const std::string busy       = "eeee5555-0000-4000-8000-000000000005";
    const std::string transcript = home.dir + "/projects/-src-app/" + mine + ".jsonl";
    const auto        record =
        [](const std::string &path, const std::string &branch, const std::string &from) {
            return str::concat(
                {R"({"type":"worktree-state","worktreeSession":{"worktreePath":)",
                 q(path),
                 R"(,"worktreeBranch":)",
                 q(branch),
                 R"(,"originalCwd":)",
                 q(from),
                 "}}\n"}
            );
        };
    // Entered, then left (ExitWorktree(keep) records a null session); entered
    // one a live session works in; the main checkout named as if one.
    writeFile(
        transcript,
        prompt("work in a worktree", "2026-09-25T10:00:00.000Z") +
            record(inRecords, "wt-records", repo) +
            "{\"type\":\"worktree-state\",\"worktreeSession\":null}\n" +
            record(inUse, "wt-busy", repo) + record(repo, "master", repo)
    );
    writeFile(
        home.dir + "/projects/-src-app/" + mine + "/subagents/agent-a1.meta.json",
        str::concat({R"({"worktreePath":)", q(inSub), R"(,"worktreeBranch":"wt-sub"})"})
    );
    std::vector<int64_t> workers;
    int                  n = 0;
    for (const std::string &sid : {mine, theirs}) {
        const std::string short8 = sid.substr(0, 8);
        const std::string where  = sid == mine ? inJob : foreign;
        home.writeJob(
            short8,
            str::concat(
                {R"({"state":"done","sessionId":")",
                 sid,
                 R"(","cwd":)",
                 q(where),
                 R"(,"name":")",
                 short8,
                 R"(","linkScanPath":)",
                 q(sid == mine ? transcript : home.dir + "/projects/-src-app/" + sid + ".jsonl"),
                 R"(,"worktreePath":)",
                 q(where),
                 R"(,"worktreeBranch":)",
                 q(sid == mine ? "wt-job" : "wt-foreign"),
                 R"(,"originCwd":)",
                 q(repo),
                 "}"}
            )
        );
        const int64_t pid = home.spawnWorker();
        REQUIRE(pid > 0);
        workers.push_back(pid);
        home.writeWorker(
            "w" + str::number(int64_t(++n)) + ".json", pid, sid, "idle", short8, where
        );
    }
    // A terminal's session working in `inUse`.
    const int64_t terminal = home.spawnWorker();
    REQUIRE(terminal > 0);
    writeFile(
        home.dir + "/sessions/t.json",
        str::concat(
            {R"({"pid":)",
             str::number(terminal),
             R"(,"sessionId":")",
             busy,
             R"(","cwd":)",
             q(inUse + "/src"),
             R"(,"kind":"interactive","entrypoint":"cli","status":"idle"})"}
        )
    );
    writeFile(
        dirs().data + "/known-sessions.json",
        str::concat({R"({"sessions":[],"started":[")", mine, "\"]}"})
    );
    const auto branches = [&] { return lines(run(repo, {"branch", "--format=%(refname:short)"})); };
    const auto has      = [](const std::vector<std::string> &v, std::string_view s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };

    Rig rig(Credentials{cli});
    REQUIRE(rig.listedCount() == 3);

    // Not msga's: it only leaves the list, worktree and all.
    rig.backend->leave(rig.ref(theirs));
    rig.pump(1500);
    CHECK(!contains(home.text("calls.log"), "cccc3333"));
    CHECK(isProcessAlive(workers[1]));
    CHECK(file::exists(foreign));

    // msga's: `claude rm` keeps the dirty worktree; msga deletes it once the
    // worker is gone — and the ones only the records name, and the subagent's.
    rig.backend->leave(rig.ref(mine));
    REQUIRE(rig.wait([&] { return contains(home.text("calls.log"), "rm aaaa1111"); }, 5000));
    CHECK(rig.wait(
        [&] { return !file::exists(inJob) && !file::exists(inRecords) && !file::exists(inSub); },
        30000
    ));
    CHECK(rig.wait([&] { return !has(branches(), "wt-sub"); }, 10000));
    CHECK(rig.wait([&] { return !isProcessAlive(workers[0]); }, 5000));
    const auto left = branches();
    CHECK_FALSE(has(left, "wt-job"));
    CHECK_FALSE(has(left, "wt-records"));
    CHECK_FALSE(has(left, "wt-sub"));
    // Kept: the one in use, the main checkout and its branch, the other session's.
    CHECK(has(left, "wt-busy"));
    CHECK(has(left, "master"));
    CHECK(has(left, "wt-foreign"));
    CHECK(file::exists(inUse));
    CHECK(file::exists(repo + "/a.txt"));
    CHECK(file::exists(foreign));
    const std::string list = run(repo, {"worktree", "list", "--porcelain"});
    CHECK(!contains(list, inJob));
    CHECK(!contains(list, inRecords));
    CHECK(!contains(list, inSub));
    // The transcripts stay, the subagents' too.
    CHECK(file::exists(transcript));
    CHECK(file::exists(home.dir + "/projects/-src-app/" + mine + "/subagents/agent-a1.meta.json"));
    // Nothing failed, so nothing to tell.
    CHECK(rig.errors.empty());
    CHECK(!contains(home.text("calls.log"), "stop"));
}
#endif

TEST("backend: files sent to a session go with its prompt") {
    FakeClaudeHome    home;
    const std::string work = tempDir("work");
    home.trust(work);
    // The CLI only notes what it was asked to do.
    const std::string cli = writeCli(
        work,
        "#!/bin/sh\n[ \"$1\" = auth ] && exit 1\n" // "can't tell": a login isn't required
        "printf '%s\\n' \"$*\" >> \"$CLAUDE_CONFIG_DIR/calls.log\"\nexit 1\n"
    );
    writeFile(work + "/pasted.png", pngBytes(4, 4));

    Rig           rig(Credentials{cli});
    const ConvRef conv = rig.start(work);
    REQUIRE(conv != kNoConv);
    rig.load(conv);

    std::optional<bool> ok;
    rig.backend->sendWithFiles(
        conv, "look", 0, {work + "/pasted.png"}, [&](bool s, const std::string &) { ok = s; }
    );
    // msga's copy of it shows the picture, not the mention — once the file
    // is copied into the cache, which happens off the UI thread.
    CHECK(rig.byText(conv, "look") == nullptr);
    REQUIRE(rig.wait([&] { return rig.byText(conv, "look") != nullptr; }));
    const model::Message *copy = rig.byText(conv, "look");
    REQUIRE(copy);
    CHECK(copy->user == rig.store.me);
    REQUIRE(copy->files().size() == 1);
    CHECK_STR(copy->files()[0].name, "pasted.png");
    REQUIRE(rig.wait([&] { return ok.has_value(); }));
    CHECK(*ok);
    // Claude Code gets the cached copy's mention before the text.
    std::string call;
    // The whole line: the shell creates the file before it writes to it.
    REQUIRE(rig.wait([&] {
        call = home.text("calls.log");
        return str::endsWith(call, "\n");
    }));
    CHECK(contains(call, "-- @" + uploadsDir() + "/"));
    CHECK(str::endsWith(str::trim(call), "/pasted.png look"));
    // The CLI refuses the turn (exit 1): said once.
    REQUIRE(rig.wait([&] { return !rig.errors.empty(); }));
    rig.pump(200);
    CHECK(rig.errors.size() == 1);

    // A file that can't be read fails the send — through done, and once to
    // the user (the composer gives sendWithFiles no done: onError is how it
    // hears of it).
    rig.errors.clear();
    std::optional<bool> failed;
    rig.backend->sendWithFiles(
        conv, "x", 0, {work + "/gone.png"}, [&](bool s, const std::string &) { failed = !s; }
    );
    REQUIRE(rig.wait([&] { return failed.has_value(); }));
    CHECK(*failed);
    CHECK(rig.errors.size() == 1);
    CHECK(rig.byText(conv, "x") == nullptr);
    // Nor is a refused one told twice.
    rig.errors.clear();
    failed.reset();
    rig.backend->sendWithFiles(
        kNoConv, "", 0, {work + "/pasted.png"}, [&](bool s, const std::string &) { failed = !s; }
    );
    REQUIRE(rig.wait([&] { return failed.has_value(); }));
    CHECK(*failed);
    CHECK(rig.errors.size() == 1);
    // A message sent while the files are copied waits its turn behind them.
    std::vector<std::string> order;
    rig.backend->sendWithFiles(
        conv, "first", 0, {work + "/pasted.png"}, [&](bool, const std::string &) {
            order.push_back("first");
        }
    );
    rig.backend->send(conv, "second", 0, [&](bool, const std::string &) {
        order.push_back("second");
    });
    CHECK(rig.byText(conv, "second") == nullptr);
    REQUIRE(rig.wait([&] { return order.size() == 2; }));
    CHECK_STR(order[0], "first");
    CHECK_STR(order[1], "second");
}

TEST("backend: a message that never went out is given back, files and all") {
    FakeClaudeHome    home;
    const std::string work = tempDir("work");
    home.trust(work);
    // Every turn is refused (exit 1).
    const std::string cli = writeCli(work, "#!/bin/sh\n[ \"$1\" = auth ] && exit 1\nexit 1\n");
    writeFile(work + "/pasted.png", pngBytes(4, 4));

    Rig           rig(Credentials{cli});
    const ConvRef conv = rig.start(work);
    REQUIRE(conv != kNoConv);
    rig.load(conv);

    // Taken (done says so), then refused: both come back as the composer
    // gave them — the file as attached, not the cache's copy — the later one
    // first (each goes in front of what the composer holds).
    rig.backend->sendWithFiles(conv, "look *here*", 0, {work + "/pasted.png"}, nullptr);
    rig.backend->send(conv, "and <@claude:role:x>", 0, nullptr);
    REQUIRE(rig.wait([&] { return rig.unsent.size() == 2; }));
    CHECK(rig.unsent[0].conv == conv);
    CHECK(rig.unsent[0].thread == 0);
    CHECK_STR(rig.unsent[0].text, "and <@claude:role:x>");
    CHECK(rig.unsent[0].files.empty());
    CHECK_STR(rig.unsent[1].text, "look *here*");
    REQUIRE(rig.unsent[1].files.size() == 1);
    CHECK_STR(rig.unsent[1].files[0], work + "/pasted.png");
    // Their copies are gone from the chat.
    CHECK(rig.byText(conv, "look here") == nullptr);
    rig.pump(200);
    CHECK(rig.unsent.size() == 2);

    // A file that can't be read: the message comes back at once.
    rig.unsent.clear();
    rig.backend->sendWithFiles(conv, "x", 0, {work + "/gone.png"}, nullptr);
    REQUIRE(rig.wait([&] { return rig.unsent.size() == 1; }));
    CHECK_STR(rig.unsent[0].text, "x");
    REQUIRE(rig.unsent[0].files.size() == 1);
    CHECK_STR(rig.unsent[0].files[0], work + "/gone.png");

    // Refused before it's taken (no such session): back too.
    rig.unsent.clear();
    std::optional<bool> ok;
    rig.backend->send(kNoConv - 1, "lost?", 0, [&](bool s, const std::string &) { ok = s; });
    REQUIRE(rig.wait([&] { return ok.has_value(); }));
    CHECK(!*ok);
    REQUIRE(rig.wait([&] { return rig.unsent.size() == 1; }));
    CHECK_STR(rig.unsent[0].text, "lost?");
    // A /btw that can't be asked yet: the question comes back as typed.
    rig.unsent.clear();
    rig.backend->send(conv, "/btw", 0, nullptr);
    REQUIRE(rig.wait([&] { return rig.unsent.size() == 1; }));
    CHECK_STR(rig.unsent[0].text, "/btw");
}

namespace {

// Stand-in for the CLI, following what Claude Code 2.1.282 was seen doing:
// `--bg … -- <prompt>` creates a job + worker (a real process: a stop waits
// for it to exit) and answers, printing "backgrounded · <short>"; `stop
// <short>` ends the worker; `--bg --resume <id> -- <prompt>` continues — or,
// with a copy-next file present, starts a copy of the session (records
// repeated, uuids and all) and continues there. Every call is logged to
// calls.log.
constexpr const char *kSendingCli = R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
if [ "$1" = attach ]; then
  printf '%s\n' "$*" >> "$H/attach.log"
  echo "no such session"; exit 1
fi
printf '%s\n' "$*" >> "$H/calls.log" # echo would expand \n
if [ "$1" = stop ]; then
  rm -f "$H/sessions/w$2.json"
  kill $(cat "$H/wpid-$2" 2>/dev/null) 2>/dev/null
  sed 's/"state":"[a-z]*"/"state":"stopped"/' "$H/jobs/$2/state.json" > "$H/state.tmp" &&
    mv "$H/state.tmp" "$H/jobs/$2/state.json"
  echo "stopped $2"; exit 0
fi
sid=""; prompt=""; copied=""
while [ $# -gt 0 ]; do
  case "$1" in
    --resume) sid="$2"; shift ;;
    --) prompt="$2"; shift ;;
  esac; shift
done
if [ -n "$sid" ] && [ -f "$H/copy-next" ]; then
  rm -f "$H/copy-next"; copied="$H/projects/-fake/$sid.jsonl"; sid=""
fi
if [ -z "$sid" ]; then
  n=$(cat "$H/counter" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$H/counter"
  sid="abcdef1$n-0000-4000-8000-00000000000$n"
fi
short=$(echo "$sid" | cut -c1-8)
T="$H/projects/-fake/$sid.jsonl"
[ -n "$copied" ] && cp "$copied" "$T"
# Milliseconds: perl (no %N in macOS's date), python3 where it's missing (Alpine).
ts=$(perl -MTime::HiRes=time -MPOSIX=strftime -e '$t = time; printf "%s.%03dZ", strftime("%Y-%m-%dT%H:%M:%S", gmtime $t), ($t - int $t) * 1000' 2>/dev/null || python3 -c 'import datetime as d; print(d.datetime.now(d.timezone.utc).isoformat(timespec="milliseconds")[:-6] + "Z")')
u=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
mkdir -p "$H/jobs/$short"
echo "{\"state\":\"done\",\"sessionId\":\"$sid\",\"cwd\":\"$PWD\",\"name\":\"fake-$short\",\"linkScanPath\":\"$T\"}" > "$H/jobs/$short/state.json"
kill $(cat "$H/wpid-$short" 2>/dev/null) 2>/dev/null
sleep 60 </dev/null >/dev/null 2>&1 &
echo $! > "$H/wpid-$short"
echo "{\"pid\":$!,\"sessionId\":\"$sid\",\"kind\":\"bg\",\"status\":\"idle\"}" > "$H/sessions/w$short.json"
echo "{\"type\":\"user\",\"uuid\":\"$u-1\",\"timestamp\":\"$ts\",\"origin\":{\"kind\":\"human\"},\"message\":{\"content\":\"$prompt\"}}" >> "$T"
echo "{\"type\":\"assistant\",\"uuid\":\"$u-2\",\"timestamp\":\"$ts\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"echo $prompt\"}]}}" >> "$T"
echo "{\"type\":\"system\",\"subtype\":\"turn_duration\",\"uuid\":\"$u-3\",\"timestamp\":\"$ts\"}" >> "$T"
[ -n "$copied" ] && echo "Worker still running; started a copy of the session."
echo "backgrounded · $short"
)SH";

} // namespace

TEST("backend: sending runs background sessions: start, then stop + resume per turn, queued") {
    FakeClaudeHome    home;
    const std::string work      = tempDir("work");
    const std::string untrusted = tempDir("untrusted");
    file::makeDirs(home.dir + "/jobs");
    file::makeDirs(home.dir + "/projects/-fake");
    home.trust(work); // not `untrusted`, a repository of its own
    file::makeDirs(untrusted + "/.git");
    const std::string cli = writeCli(work, kSendingCli);

    Rig rig(Credentials{cli});

    // Claude Code never trusts the home folder for background sessions: said
    // up front. Any other folder msga trusts itself when the session starts.
    CHECK(rig.start(base::homeDir()) == kNoConv);
    CHECK(contains(rig.lastStartError, "home folder"));

    const ConvRef conv = rig.start(work);
    REQUIRE(conv != kNoConv);
    REQUIRE(str::startsWith(rig.store.conversation(conv).id, "new-")); // Claude Code picks the id
    CHECK(rig.store.conversation(conv).member);
    CHECK(rig.store.conversation(conv).readOnly.empty());
    rig.load(conv);
    const auto answered = [&](ConvRef c, std::string_view text) {
        return rig.wait([&] { return rig.answered(c, text); }, 8000);
    };

    bool ok1 = false, ok2 = false, ok3 = false;
    rig.send(conv, "first", &ok1);
    REQUIRE(answered(conv, "echo first"));
    CHECK(ok1);
    // Two more at once: the second waits for its turn, one prompt per turn.
    rig.send(conv, "second", &ok2);
    rig.send(conv, "third", &ok3);
    // The waiting one is a message of its own at once, and stays one across a
    // reload of the chat.
    using V = std::vector<std::string>;
    CHECK(rig.ownTexts(conv) == (V{"first", "second", "third"}));
    REQUIRE(answered(conv, "echo second"));
    REQUIRE(answered(conv, "echo third"));
    CHECK(ok2);
    CHECK(ok3);
    // …and gives way to its prompt in the transcript: no doubles.
    CHECK(rig.ownTexts(conv) == (V{"first", "second", "third"}));
    CHECK(rig.errors.empty());

    const std::string sid   = "abcdef11-0000-4000-8000-000000000001";
    const auto        calls = home.calls();
    // A sixth, the stop after the third turn, may be in already.
    REQUIRE(calls.size() >= 5);
    // A new session gets the team as subagent types, so "use @Engineer" works.
    CHECK_STR(
        calls[0],
        "--bg --disallowedTools AskUserQuestion --agents " + subagentsJson(builtInRoles()) +
            " -- first"
    );
    CHECK_STR(calls[1], "stop abcdef11");                       // the worker idles on after a turn
    CHECK_STR(calls[2], "--bg --resume " + sid + " -- second"); // no flags: keeps its options
    CHECK_STR(calls[3], "stop abcdef11");
    CHECK_STR(calls[4], "--bg --resume " + sid + " -- third");

    // A message still waiting for its turn can be taken back.
    rig.send(conv, "fourth");
    rig.send(conv, "fifth");
    const model::Message *fifthMsg = rig.byText(conv, "fifth");
    REQUIRE(fifthMsg);
    const Ts fifth = fifthMsg->ts;
    REQUIRE(rig.backend->canDeleteMessage(conv, fifth));
    rig.changes.clear();
    rig.backend->remove(conv, fifth);
    CHECK(std::any_of(rig.changes.begin(), rig.changes.end(), [&](const model::Change &c) {
        return c.kind == model::ChangeKind::Remove && c.ts == fifth;
    }));
    REQUIRE(answered(conv, "echo fourth"));
    rig.pump(500); // a turn for fifth would have started by now
    CHECK(!contains(home.text("calls.log"), "fifth"));
    const auto own = rig.ownTexts(conv);
    CHECK(std::find(own.begin(), own.end(), "fifth") == own.end());

    // One conversation for it, under its "+" id; the name comes from Claude Code.
    const auto named = [&](std::string_view prefix) {
        int k = 0;
        for (ConvRef c = 0; c < rig.store.conversationCount(); ++c)
            k += rig.store.conversation(c).member &&
                 str::startsWith(rig.store.conversation(c).name, prefix);
        return k;
    };
    CHECK(named("fake-abcdef11") == 1);

    // Claude Code started a copy after all (a resume racing the old worker's
    // exit, seen 2026-09-25): the chat goes on in it — no failure, no history
    // twice, no second session or thread — and the next message resumes the copy.
    writeFile(home.dir + "/copy-next", "");
    bool ok6 = false;
    rig.send(conv, "sixth", &ok6);
    REQUIRE(answered(conv, "echo sixth"));
    CHECK(ok6);
    CHECK(rig.errors.empty());
    CHECK(rig.ownTexts(conv) == (V{"first", "second", "third", "fourth", "sixth"}));
    CHECK(named("fake-abcdef1") == 1);
    rig.send(conv, "seventh");
    REQUIRE(answered(conv, "echo seventh"));
    const std::string copyLog = home.text("calls.log");
    CHECK(contains(copyLog, "--bg --resume abcdef12-0000-4000-8000-000000000002 -- seventh"));
    CHECK(contains(copyLog, "stop abcdef11\n")); // the original is stopped, not left idling

    // Skipping permission checks is a start option, saved with the session.
    // (In a folder Claude Code doesn't trust yet: msga trusts it on launch.)
    CHECK_FALSE(isFolderTrusted(untrusted));
    const ConvRef noChecks = rig.start(untrusted, true);
    REQUIRE(noChecks != kNoConv);
    rig.load(noChecks);
    rig.send(noChecks, "go");
    REQUIRE(rig.wait(
        [&] {
            return contains(
                home.text("calls.log"),
                "--dangerously-skip-permissions --agents " + subagentsJson(builtInRoles()) +
                    " -- go"
            );
        },
        8000
    ));
    // Let that launch finish before the backend goes away with it.
    CHECK(answered(noChecks, "echo go"));
    CHECK(isFolderTrusted(untrusted));
    CHECK(isFolderTrusted(work)); // the trust already there is kept
    // Claude gets the text as typed: fences and backticks too.
    rig.send(noChecks, "quotes ```test``` `code`");
    CHECK(
        rig.wait([&] { return contains(home.text("calls.log"), "quotes ```test``` `code`"); }, 8000)
    );

    // A teammate: its few lines go after Claude Code's own prompt, its session
    // carries its picture, and Claude answers as the teammate.
    const ConvRef engineer = rig.start(work, false, "engineer");
    REQUIRE(engineer != kNoConv);
    rig.load(engineer);
    const std::string  engConv = rig.store.conversation(engineer).id;
    const model::User *engUser = rig.user("claude:role:engineer");
    const model::User *engPeer = rig.user("claude:" + engConv);
    REQUIRE(engUser);
    REQUIRE(engPeer);
    CHECK_STR(engUser->name, "Engineer");
    CHECK_STR(rig.backend->agentSessionRole(engineer), "engineer");
    CHECK(!engPeer->avatar.empty());
    CHECK_STR(engPeer->avatar, engUser->avatar);
    rig.send(engineer, "hi");
    REQUIRE(answered(engineer, "echo hi"));
    CHECK(rig.byText(engineer, "echo hi")->user == rig.store.findUser("claude:role:engineer"));
    CHECK(contains(
        home.text("calls.log"), "--append-system-prompt # Your role: Engineer (msga: engineer)\n"
    ));

    // An added teammate. Editing its instructions reaches new sessions only —
    // one already started goes on without flags, keeping what it began with —
    // and removing it leaves its session its name and picture.
    model::Backend::AgentRole copy;
    copy.name   = "Copywriter";
    copy.glyph  = "pen-tool";
    copy.color  = 0x0e8c9a;
    copy.prompt = "Write copy v1.";
    std::string err;
    REQUIRE(rig.backend->saveAgentRole(copy, &err) == "copywriter");
    const ConvRef cw = rig.start(work, false, "copywriter");
    REQUIRE(cw != kNoConv);
    rig.load(cw);
    rig.send(cw, "tagline");
    REQUIRE(answered(cw, "echo tagline"));
    for (const auto &r : rig.backend->agentRoles())
        if (r.id == "copywriter")
            copy = r;
    copy.prompt = "Write copy v2.";
    REQUIRE(rig.backend->saveAgentRole(copy, &err) == "copywriter");
    rig.send(cw, "again");
    REQUIRE(answered(cw, "echo again"));
    const ConvRef cw2 = rig.start(work, false, "copywriter");
    REQUIRE(cw2 != kNoConv);
    rig.load(cw2);
    rig.send(cw2, "fresh");
    REQUIRE(answered(cw2, "echo fresh"));
    const std::string        all = home.text("calls.log");
    // Both its prompt and its subagent type carry the text: count launches.
    // (A call's log entry runs over lines where its prompt does.)
    std::vector<std::string> launches;
    for (size_t at = 0;;) {
        const size_t next = all.find("\n--bg", at + 1);
        launches.push_back(
            all.substr(at, next == std::string::npos ? std::string::npos : next - at)
        );
        if (next == std::string::npos)
            break;
        at = next;
    }
    const auto launched = [&](std::string_view text) {
        return std::count_if(launches.begin(), launches.end(), [&](const std::string &l) {
            return contains(l, text);
        });
    };
    CHECK(launched("Write copy v1.") == 1);
    CHECK(launched("Write copy v2.") == 1);
    CHECK(contains(all, R"("copywriter":{"description":"Copywriter, a teammate)"));
    CHECK(all.find("Write copy v2.") > all.find(" -- again")); // only the new session
    for (const auto &l : lines(all))
        if (str::endsWith(l, " -- again"))
            CHECK(str::startsWith(l, "--bg --resume ")); // no flags: its own prompt stays

    rig.backend->removeAgentRole("copywriter");
    for (const auto &r : rig.backend->agentRoles())
        CHECK(r.id != "copywriter");
    const model::User *cwPeer = rig.user("claude:" + rig.store.conversation(cw).id);
    REQUIRE(cwPeer);
    CHECK(rig.store.conversation(cw).member);
    CHECK_STR(rig.backend->agentSessionRole(cw), "copywriter");
    CHECK(str::endsWith(cwPeer->avatar, "pen-tool-0e8c9a.svg"));
    const model::User *cwMate = rig.user("claude:role:copywriter");
    REQUIRE(cwMate);
    CHECK_STR(cwMate->name, "Copywriter");
}

#if defined(__linux__)
TEST("backend: Stop cuts a session's turn short and drops what waits") {
    FakeClaudeHome home;
    home.writeSession("busy"); // S1: a terminal's session, working
    const std::string work = tempDir("work");
    file::makeDirs(home.dir + "/jobs");
    file::makeDirs(home.dir + "/projects/-fake");
    home.trust(work);
    // Every turn this CLI starts goes on until it is stopped. Its worker is a
    // real process: a stop waits for it to exit.
    const std::string cli = writeCli(work, R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
if [ "$1" = attach ]; then
  printf '%s\n' "$*" >> "$H/attach.log"
  echo "no such session"; exit 1
fi
printf '%s\n' "$*" >> "$H/calls.log" # echo would expand \n
if [ "$1" = stop ] || [ "$1" = rm ]; then
  # The worker writes its last records as it exits.
  T=$(sed -n 's/.*"linkScanPath":"\([^"]*\)".*/\1/p' "$H/jobs/$2/state.json")
  echo '{"type":"last-prompt","lastPrompt":"x"}' >> "$T"
  rm -f "$H/sessions/w$2.json"
  kill $(cat "$H/wpid-$2" 2>/dev/null) 2>/dev/null
  if [ "$1" = rm ]; then rm -rf "$H/jobs/$2"; echo "removed $2"; exit 0; fi
  sed 's/"state":"[a-z]*"/"state":"stopped"/' "$H/jobs/$2/state.json" > "$H/state.tmp" &&
    mv "$H/state.tmp" "$H/jobs/$2/state.json"
  echo "stopped $2"; exit 0
fi
sid=""; prompt=""
while [ $# -gt 0 ]; do
  case "$1" in
    --resume) sid="$2"; shift ;;
    --) prompt="$2"; shift ;;
  esac; shift
done
[ -z "$sid" ] && sid="$FAKE_SID"
short=$(echo "$sid" | cut -c1-8)
T="$H/projects/-fake/$sid.jsonl"
ts=$(perl -MTime::HiRes=time -MPOSIX=strftime -e '$t = time; printf "%s.%03dZ", strftime("%Y-%m-%dT%H:%M:%S", gmtime $t), ($t - int $t) * 1000' 2>/dev/null || python3 -c 'import datetime as d; print(d.datetime.now(d.timezone.utc).isoformat(timespec="milliseconds")[:-6] + "Z")')
mkdir -p "$H/jobs/$short"
echo "{\"state\":\"working\",\"sessionId\":\"$sid\",\"cwd\":\"$PWD\",\"name\":\"fake-$short\",\"linkScanPath\":\"$T\"}" > "$H/jobs/$short/state.json"
kill $(cat "$H/wpid-$short" 2>/dev/null) 2>/dev/null
sleep 60 </dev/null >/dev/null 2>&1 &
echo $! > "$H/wpid-$short"
echo "{\"pid\":$!,\"sessionId\":\"$sid\",\"kind\":\"bg\",\"status\":\"busy\"}" > "$H/sessions/w$short.json"
echo "{\"type\":\"user\",\"timestamp\":\"$ts\",\"origin\":{\"kind\":\"human\"},\"message\":{\"content\":\"$prompt\"}}" >> "$T"
echo "backgrounded · $short"
)SH");

    // The session id is this run's own: the leftover lookup scans every
    // process on the machine, and another claude_tests run at the same time
    // (another checkout) must not be taken for this session's.
    char tail[16];
    std::snprintf(tail, sizeof tail, "%012llx", static_cast<unsigned long long>(getpid()));
    const std::string sid = str::concat({"abcdef11-0000-4000-8000-", tail});
    base::test::setEnv("FAKE_SID", sid);
    struct Unset {
        ~Unset() { base::test::unsetEnv("FAKE_SID"); }
    } unsetSid;
    Rig rig(Credentials{cli});

    // A terminal's session is the terminal's to stop.
    CHECK_FALSE(rig.backend->canStopSession(rig.ref("S1")));

    const ConvRef conv = rig.start(work);
    REQUIRE(conv != kNoConv);
    CHECK_FALSE(rig.backend->canStopSession(conv)); // nothing sent yet
    rig.load(conv);
    const std::string assistant = "claude:" + rig.store.conversation(conv).id;
    const auto        working   = [&] { return rig.active(assistant); };
    using V                     = std::vector<std::string>;

    rig.send(conv, "first");
    // Claude is on it: the prompt is in and the worker reads busy.
    REQUIRE(rig.wait([&] { return rig.ownTexts(conv) == V{"first"}; }, 8000));
    rig.pump(300);            // the launcher has reported back
    rig.send(conv, "second"); // waits for the turn to end
    CHECK(rig.ownTexts(conv) == (V{"first", "second"}));
    CHECK(working());
    REQUIRE(rig.backend->canStopSession(conv));

    rig.backend->stopSession(conv);
    CHECK_FALSE(working()); // at once
    CHECK_FALSE(rig.backend->canStopSession(conv));
    CHECK(rig.ownTexts(conv) == V{"first"}); // the waiting one is dropped
    REQUIRE(rig.wait([&] { return home.calls().size() == 2; }, 8000));
    CHECK_STR(home.calls()[1], "stop abcdef11");
    rig.pump(1000); // stopped, and nothing more goes out
    CHECK(home.calls().size() == 2);
    CHECK_FALSE(working());
    CHECK(rig.errors.empty());

    // The next message continues it: the worker is gone, so no stop first.
    rig.send(conv, "third");
    REQUIRE(rig.wait([&] { return home.calls().size() == 3; }, 8000));
    CHECK_STR(home.calls()[2], "--bg --resume " + sid + " -- third");
    // Stopped while the CLI is still starting the turn: stopped once it has.
    rig.backend->stopSession(conv);
    REQUIRE(rig.wait([&] { return home.calls().size() == 4; }, 8000));
    CHECK_STR(home.calls()[3], "stop abcdef11");
    rig.pump(1000);
    CHECK_FALSE(working());
    CHECK_FALSE(rig.backend->canStopSession(conv));

    // An idle worker can be stopped too: a subagent's result or a scheduled
    // prompt would wake it without anyone sending a thing.
    rig.send(conv, "fourth");
    REQUIRE(rig.wait([&] { return home.calls().size() == 5; }, 8000));
    const std::string workerFile = home.dir + "/sessions/wabcdef11.json";
    REQUIRE(rig.wait([&] { return file::exists(workerFile); }, 8000));
    rig.pump(300); // the launcher has reported back
    home.writeWorker("wabcdef11.json", std::atoll(home.text("wpid-abcdef11").c_str()), sid, "idle");
    {
        // …and the turn is over.
        std::string  state = home.text("jobs/abcdef11/state.json");
        const size_t at    = state.find("\"working\"");
        REQUIRE(at != std::string::npos);
        state.replace(at, 9, "\"done\"");
        writeFile(home.dir + "/jobs/abcdef11/state.json", state);
        appendFile(
            home.dir + "/projects/-fake/" + sid + ".jsonl",
            "{\"type\":\"system\",\"subtype\":\"turn_duration\"}\n"
        );
    }
    REQUIRE(rig.wait([&] { return !working(); }, 8000));
    CHECK(rig.backend->canStopSession(conv));

    // What the session left running outside its worker is ended with it —
    // only that: a process that merely inherited the session id is no leftover.
    const int64_t leftover = home.spawnWorker(
        {"CLAUDE_CODE_SESSION_ID=" + sid, "CLAUDE_JOB_DIR=" + home.dir + "/jobs/abcdef11"}
    );
    const int64_t bystander = home.spawnWorker({"CLAUDE_CODE_SESSION_ID=" + sid});
    REQUIRE(leftover > 0);
    REQUIRE(bystander > 0);
    REQUIRE(rig.wait(
        [&] { return leftoverProcesses(sid, "abcdef11") == std::vector<int64_t>{leftover}; }, 3000
    ));
    rig.backend->stopSession(conv);
    REQUIRE(rig.wait([&] { return home.calls().size() == 6; }, 8000));
    CHECK_STR(home.calls()[5], "stop abcdef11");
    CHECK(rig.wait([&] { return !isProcessAlive(leftover); }, 5000));
    CHECK(isProcessAlive(bystander));

    // "Remove from msga" deletes it (`claude rm`), live worker and all, and the
    // session stays away though the worker still writes to its transcript as
    // it goes.
    rig.send(conv, "fifth");
    REQUIRE(rig.wait([&] { return home.calls().size() == 7; }, 8000));
    REQUIRE(rig.wait(working, 8000));
    rig.pump(300); // the launcher has reported back
    rig.backend->leave(conv);
    REQUIRE(rig.wait([&] { return home.calls().size() == 8; }, 8000));
    CHECK_STR(home.calls()[7], "rm abcdef11");
    rig.pump(1500);
    CHECK_FALSE(rig.store.conversation(conv).member);
    // Nothing resumes it. A removal that lands while the turn is still being
    // launched stops the worker once more when the launch reports back — so a
    // second "rm" is fine, anything else not.
    const auto after = home.calls();
    REQUIRE(after.size() >= 8);
    for (size_t i = 8; i < after.size(); ++i)
        CHECK_STR(after[i], "rm abcdef11");
}
#endif

namespace {

// `--bg` starts a session whose worker (a real process) idles on after the
// turn; `attach` is the fake terminal UI (test_terminal.cpp's).
constexpr const char *kLiveCli = R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
if [ "$1" = attach ]; then
  printf '%s\n' "$*" >> "$H/attach.log"
  MSGA_FAKE_ATTACH_RUN="$2" exec "$FAKE_ATTACH"
fi
printf '%s\n' "$*" >> "$H/calls.log"
if [ "$1" = stop ]; then
  rm -f "$H/sessions/w$2.json"; kill $(cat "$H/wpid-$2") 2>/dev/null; exit 0
fi
prompt=""
while [ $# -gt 0 ]; do [ "$1" = -- ] && prompt="$2"; shift; done
sid="abcdef11-0000-4000-8000-000000000001"; short=abcdef11
T="$H/projects/-fake/$sid.jsonl"
mkdir -p "$H/jobs/$short"
echo "{\"state\":\"done\",\"sessionId\":\"$sid\",\"cwd\":\"$PWD\",\"name\":\"fake\",\"linkScanPath\":\"$T\"}" > "$H/jobs/$short/state.json"
sleep 60 </dev/null >/dev/null 2>&1 &
echo $! > "$H/wpid-$short"
echo "{\"pid\":$!,\"sessionId\":\"$sid\",\"kind\":\"bg\",\"status\":\"idle\"}" > "$H/sessions/w$short.json"
echo "{\"type\":\"user\",\"uuid\":\"s-$$-1\",\"origin\":{\"kind\":\"human\"},\"message\":{\"content\":\"$prompt\"}}" >> "$T"
echo "{\"type\":\"assistant\",\"uuid\":\"s-$$-2\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"echo $prompt\"}]}}" >> "$T"
echo "{\"type\":\"system\",\"subtype\":\"turn_duration\",\"uuid\":\"s-$$-3\"}" >> "$T"
echo "backgrounded · $short"
)SH";

// A started session on a CLI whose `attach` is the fake terminal UI.
struct LiveRig {
    FakeClaudeHome       home;
    std::string          work = tempDir("work");
    std::string          cli;
    std::unique_ptr<Rig> rig;
    ConvRef              conv = kNoConv;

    LiveRig() {
        file::makeDirs(home.dir + "/jobs");
        file::makeDirs(home.dir + "/projects/-fake");
        home.trust(work);
        base::test::setEnv("FAKE_ATTACH", fakeAttachProgram());
        AttachInput::setAttachTimeoutMs(1500);
        cli  = writeCli(work, kLiveCli);
        rig  = std::make_unique<Rig>(Credentials{cli});
        conv = rig->start(work);
        if (conv != kNoConv)
            rig->load(conv);
    }
    ~LiveRig() {
        rig.reset();
        AttachInput::setAttachTimeoutMs(15'000);
        AttachInput::setSubmitTimeoutMs(10'000);
    }
    // A `stop` call (the first call's --agents JSON can say "stop" too).
    bool stopped() const {
        for (const auto &l : home.calls())
            if (str::startsWith(l, "stop "))
                return true;
        return false;
    }
    bool answered(std::string_view text, int ms = 8000) {
        return rig->wait([&] { return rig->answered(conv, text); }, ms);
    }
};

} // namespace

TEST("backend: a live background session is typed to, not stopped, even while it works") {
    LiveRig live;
    REQUIRE(live.conv != kNoConv);
    Rig            &rig  = *live.rig;
    FakeClaudeHome &home = live.home;
    const ConvRef   conv = live.conv;

    rig.send(conv, "first"); // a new session: started the usual way
    REQUIRE(live.answered("echo first"));
    const std::string worker(str::trim(home.text("wpid-abcdef11")));
    REQUIRE(!worker.empty());

    // A subagent runs: the worker reads busy though Claude waits for input.
    rig.pump(300);
    home.writeWorker(
        "wabcdef11.json", std::atoll(worker.c_str()), "abcdef11-0000-4000-8000-000000000001", "busy"
    );
    rig.pump(500); // the roster has seen it
    rig.send(conv, "second");
    REQUIRE(live.answered("echo second"));
    CHECK_STR(home.text("typed.log"), "second\n");
    CHECK(contains(home.text("attach.log"), "attach abcdef11"));
    CHECK_FALSE(live.stopped()); // the worker (and its subagent) live on
    CHECK(isProcessAlive(std::atoll(worker.c_str())));

    // Several lines, one starting with "!": typed as they are, as plain text.
    rig.send(conv, "!make it\nwork");
    REQUIRE(live.answered("echo  !make it\nwork"));
    CHECK(str::endsWith(home.text("typed.log"), " !make it\\nwork\n"));

    // A permission question has the keyboard: nothing is typed, the message
    // waits — no stop, no resume — and goes once the prompt is back.
    writeFile(home.dir + "/attach-mode", "question");
    rig.send(conv, "third");
    rig.pump(2500);
    CHECK(!contains(home.text("typed.log"), "third"));
    CHECK_FALSE(file::exists(home.dir + "/question-keys.log"));
    CHECK_FALSE(live.stopped());
    writeFile(home.dir + "/attach-mode", "");
    REQUIRE(live.answered("echo third", 10000));
    CHECK_FALSE(live.stopped());
    CHECK(rig.errors.empty());

    // Waiting on the permission question its job names: the question's
    // options, read off the screen, are buttons on "Waiting for your
    // approval", and a press picks that option there. A message sent
    // meanwhile waits for the answer instead of being refused.
    const std::string jobPath = home.dir + "/jobs/abcdef11/state.json";
    const std::string jobDone = readText(jobPath);
    writeFile(home.dir + "/attach-mode", "question");
    {
        std::string job = jobDone;
        job.replace(job.find("\"done\""), 6, "\"working\"");
        job.insert(job.rfind('}'), R"(,"needs":"approve Bash: rm -rf build")");
        writeFile(jobPath, job);
    }
    const model::Message *waiting = nullptr;
    REQUIRE(rig.wait(
        [&] {
            for (const auto &m : rig.messages(conv))
                if (m.extra && m.extra->buttons.size() == 3)
                    waiting = &m;
            return waiting != nullptr;
        },
        10000
    ));
    CHECK_STR(waiting->text, "Waiting for your approval: Bash: rm -rf build");
    CHECK(waiting->user == rig.store.findUser("claude:agent"));
    const auto buttons = waiting->extra->buttons;
    const Ts   waitTs  = waiting->ts;
    CHECK_STR(buttons[0].label, "Yes");
    CHECK(buttons[0].style == model::Button::Style::Primary);
    CHECK_STR(buttons[1].label, "Yes, and don't ask again for rm commands");
    CHECK_STR(buttons[2].label, "No");
    CHECK(buttons[2].style == model::Button::Style::Danger);
    CHECK_FALSE(file::exists(home.dir + "/answered.log")); // reading it pressed nothing
    CHECK(!contains(home.text("question-keys.log"), "\r"));
    rig.send(conv, "fifth");
    bool pressed = false, ok = false;
    rig.backend->pressButton(conv, waitTs, buttons[2].id, [&](bool o, const std::string &) {
        pressed = true;
        ok      = o;
    });
    CHECK_FALSE(pressed); // never from inside the call
    REQUIRE(rig.wait([&] { return pressed; }, 10000));
    CHECK(ok);
    CHECK_STR(home.text("answered.log"), "3\n"); // "No", moved to with ↓↓ before Enter
    CHECK(!contains(home.text("typed.log"), "fifth"));
    writeFile(jobPath, jobDone);
    REQUIRE(live.answered("echo fifth", 10000));
    CHECK_FALSE(live.stopped());
    CHECK(rig.errors.empty());

    // Started with msga's permission hook (approvals.h): the question comes
    // through it, options and all, and a press is left there as the hook's
    // answer — the screen isn't read (it shows no question here at all).
    writeFile(home.dir + "/attach-mode", "");
    const std::string approvals = dirs().data + "/approvals";
    REQUIRE(file::makeDirs(approvals));
    writeFile(
        approvals + "/7-1.json",
        R"({"session_id":"abcdef11-0000-4000-8000-000000000001","cwd":"/w",)"
        R"("hook_event_name":"PermissionRequest","tool_name":"Bash",)"
        R"("tool_input":{"command":"rm -f nm"},"permission_suggestions":[)"
        R"({"type":"addDirectories","directories":["/w/outside"],"destination":"session"}]})"
    );
    {
        std::string job = jobDone;
        job.replace(job.find("\"done\""), 6, "\"working\"");
        job.insert(
            job.rfind('}'),
            R"(,"needs":"approve Bash: rm -f nm","respawnFlags":["--settings",)"
            R"("{\"args\":[\"--claude-permission-hook\"]}"])"
        );
        writeFile(jobPath, job);
    }
    const model::Message *hooked = nullptr;
    REQUIRE(rig.wait(
        [&] {
            for (const auto &m : rig.messages(conv))
                if (m.extra && m.extra->buttons.size() == 3 &&
                    m.text == "Waiting for your approval: Bash: rm -f nm")
                    hooked = &m;
            return hooked != nullptr;
        },
        10000
    ));
    const auto hookButtons = hooked->extra->buttons;
    CHECK_STR(hookButtons[0].label, "Yes");
    CHECK_STR(hookButtons[1].label, "Yes, and always allow access to /w/outside from this project");
    CHECK_STR(hookButtons[2].label, "No");
    CHECK(file::exists(approvals + "/7-1.seen")); // msga has it: the hook waits
    bool hookPressed = false, hookOk = false;
    rig.backend->pressButton(conv, hooked->ts, hookButtons[1].id, [&](bool o, const std::string &) {
        hookPressed = true;
        hookOk      = o;
    });
    REQUIRE(rig.wait([&] { return hookPressed; }, 5000));
    CHECK(hookOk);
    CHECK(contains(readText(approvals + "/7-1.answer"), R"("directories":["/w/outside"])"));
    CHECK_STR(home.text("answered.log"), "3\n");          // nothing pressed on the screen
    for (const char *ext : {".json", ".seen", ".answer"}) // the hook takes it and goes
        file::remove(approvals + "/7-1" + ext);
    writeFile(jobPath, jobDone);
    rig.pump(500);
    CHECK(rig.errors.empty());

    // Mid-turn Claude Code queues the prompt and may draw the box as it likes:
    // a hint in it, or the prompt seemingly still there. Delivered either way
    // (the transcript has it) — never "didn't take the message".
    AttachInput::setSubmitTimeoutMs(1000);
    writeFile(home.dir + "/attach-mode", "hint");
    rig.send(conv, "queued one");
    REQUIRE(live.answered("echo queued one"));
    writeFile(home.dir + "/attach-mode", "sticky");
    rig.send(conv, "queued two");
    REQUIRE(live.answered("echo queued two"));
    rig.pump(1500); // past the box's deadline
    writeFile(home.dir + "/attach-mode", "");
    rig.send(conv, "queued three"); // and the next one still goes live
    REQUIRE(live.answered("echo queued three", 10000));
    CHECK(str::endsWith(home.text("typed.log"), "queued one\nqueued two\nqueued three\n"));
    CHECK_FALSE(live.stopped());
    CHECK(rig.errors.empty());
    AttachInput::setSubmitTimeoutMs(10'000);

    // Stopped by hand, the session takes the old way again.
    rig.backend->stopSession(conv);
    REQUIRE(rig.wait([&] { return live.stopped(); }, 8000));
    rig.pump(500);
    rig.send(conv, "fourth");
    REQUIRE(live.answered("echo fourth"));
    CHECK(contains(home.text("calls.log"), "-- fourth"));
}

// AttachInput's done can run from inside the call that starts it and from
// cancel(): Stop while a message waits in `attach` for the prompt box is that
// path — the worker is stopped, nothing typed, and the session goes on.
TEST("backend: Stop while a message waits for the prompt box stops the worker, types nothing") {
    LiveRig live;
    REQUIRE(live.conv != kNoConv);
    Rig            &rig  = *live.rig;
    FakeClaudeHome &home = live.home;
    const ConvRef   conv = live.conv;
    rig.send(conv, "first");
    REQUIRE(live.answered("echo first"));
    rig.pump(300);

    // A question has the keyboard: the message waits in `attach` for the box.
    AttachInput::setAttachTimeoutMs(8000);
    writeFile(home.dir + "/attach-mode", "question");
    rig.send(conv, "second");
    REQUIRE(rig.wait([&] { return contains(home.text("attach.log"), "attach abcdef11"); }));
    rig.pump(300);
    REQUIRE(rig.backend->canStopSession(conv));
    rig.backend->stopSession(conv); // cancels the typing: its done runs right here
    REQUIRE(rig.wait([&] { return live.stopped(); }, 8000));
    CHECK_FALSE(rig.active("claude:" + rig.store.conversation(conv).id));
    CHECK(!contains(home.text("typed.log"), "second"));
    CHECK(rig.ownTexts(conv) == std::vector<std::string>{"first"}); // what waited is gone
    CHECK(rig.errors.empty());

    // The session goes on as before: the next message is sent the old way.
    writeFile(home.dir + "/attach-mode", "");
    rig.pump(500);
    rig.send(conv, "third");
    REQUIRE(live.answered("echo third"));
    CHECK(rig.errors.empty());
}

// A background command keeps the worker from being stopped (it would die
// with it), so a slash command, which otherwise goes by stop and resume,
// waits for it to end — forever, for one that never does. /compact runs
// without taking the keyboard: it is typed to the worker instead.
TEST("backend: /compact is typed to a worker running a background command, other commands wait") {
    LiveRig live;
    REQUIRE(live.conv != kNoConv);
    Rig            &rig  = *live.rig;
    FakeClaudeHome &home = live.home;
    const ConvRef   conv = live.conv;
    rig.send(conv, "first");
    REQUIRE(live.answered("echo first"));
    const std::string worker(str::trim(home.text("wpid-abcdef11")));
    REQUIRE(!worker.empty());
    const char *sid = "abcdef11-0000-4000-8000-000000000001";
    rig.pump(300);
    home.writeWorker("wabcdef11.json", std::atoll(worker.c_str()), sid, "shell");
    rig.pump(1500); // the roster has seen it

    rig.send(conv, "/compact keep the API notes");
    REQUIRE(live.answered("echo /compact keep the API notes"));
    CHECK_STR(home.text("typed.log"), "/compact keep the API notes\n"); // no space before it
    CHECK_FALSE(live.stopped());
    CHECK(isProcessAlive(std::atoll(worker.c_str())));

    // Any other command waits for the background command, untyped.
    rig.send(conv, "/cost");
    rig.pump(2500);
    CHECK(!contains(home.text("typed.log"), "cost"));
    CHECK_FALSE(live.stopped());
    CHECK(!contains(home.text("calls.log"), "-- /cost"));

    // Over: it goes the old way.
    home.writeWorker("wabcdef11.json", std::atoll(worker.c_str()), sid, "idle");
    REQUIRE(rig.wait([&] { return contains(home.text("calls.log"), "-- /cost"); }, 10000));
    CHECK(live.stopped());
    CHECK(!contains(home.text("typed.log"), "cost"));
    CHECK(rig.errors.empty());
}

namespace {

// Background sessions with no live worker (each turn is a launch), whose
// turns read a file before they answer: the prompt and the Read call are
// written at once, the rest 1.5 s later. `--fork-session` copies the
// session's transcript (uuids and all) before the new prompt.
constexpr const char *kBranchCli = R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
if [ "$1" = auth ]; then echo '{"loggedIn":true,"authMethod":"claude.ai"}'; exit 0; fi
printf '%s\n' "$*" >> "$H/calls.log"
[ "$1" = stop ] && exit 0
sid=""; prompt=""; fork=""
while [ $# -gt 0 ]; do
  case "$1" in
    --resume) sid="$2"; shift ;;
    --fork-session) fork=1 ;;
    --) prompt="$2"; shift ;;
  esac; shift
done
parent="$sid"
if [ -z "$sid" ] || [ -n "$fork" ]; then
  n=$(cat "$H/counter" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$H/counter"
  sid="abcdef1$n-0000-4000-8000-00000000000$n"
fi
short=$(echo "$sid" | cut -c1-8)
T="$H/projects/-fake/$sid.jsonl"
[ -n "$fork" ] && cp "$H/projects/-fake/$parent.jsonl" "$T"
now() { perl -MTime::HiRes=time -MPOSIX=strftime -e '$t = time; printf "%s.%03dZ", strftime("%Y-%m-%dT%H:%M:%S", gmtime $t), ($t - int $t) * 1000' 2>/dev/null || python3 -c 'import datetime as d; print(d.datetime.now(d.timezone.utc).isoformat(timespec="milliseconds")[:-6] + "Z")'; }
ts=$(now)
u=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
mkdir -p "$H/jobs/$short"
echo "{\"state\":\"done\",\"sessionId\":\"$sid\",\"cwd\":\"$PWD\",\"name\":\"fake-$short\",\"linkScanPath\":\"$T\"}" > "$H/jobs/$short/state.json"
echo "{\"type\":\"user\",\"uuid\":\"$u-1\",\"timestamp\":\"$ts\",\"origin\":{\"kind\":\"human\"},\"message\":{\"content\":\"$prompt\"}}" >> "$T"
echo "{\"type\":\"assistant\",\"uuid\":\"$u-2\",\"timestamp\":\"$ts\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"t$u\",\"name\":\"Read\",\"input\":{\"file_path\":\"/x\"}}]}}" >> "$T"
(
  sleep 1.5; ts=$(now)
  echo "{\"type\":\"user\",\"uuid\":\"$u-3\",\"timestamp\":\"$ts\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t$u\",\"is_error\":false}]}}" >> "$T"
  echo "{\"type\":\"assistant\",\"uuid\":\"$u-4\",\"timestamp\":\"$ts\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"answer: $prompt\"}]}}" >> "$T"
  echo "{\"type\":\"system\",\"subtype\":\"turn_duration\",\"uuid\":\"$u-5\",\"timestamp\":\"$ts\"}" >> "$T"
) </dev/null >/dev/null 2>&1 &
echo "backgrounded · $short"
)SH";

// What a branch's turn told, in order.
struct TurnLog {
    std::vector<model::Backend::AgentTurn> updates;
    model::Backend::AgentTurnFn            fn() {
        return [this](const model::Backend::AgentTurn &t) { updates.push_back(t); };
    }
    bool done() const { return !updates.empty() && updates.back().done; }
    bool phased(model::Backend::AgentPhase p) const {
        return std::any_of(updates.begin(), updates.end(), [&](const auto &u) {
            return !u.done && u.phase == p;
        });
    }
};

// The uuid of the transcript's last record.
std::string lastUuid(const std::string &path) {
    const auto     all = lines(readText(path));
    json::Document doc;
    return !all.empty() && doc.parse(all.back()) ? std::string(doc.root()["uuid"].str())
                                                 : std::string();
}

} // namespace

TEST("backend: an agent branch runs without tools, answers each turn, and keeps them off") {
    FakeClaudeHome    home;
    const std::string work = tempDir("work");
    file::makeDirs(home.dir + "/jobs");
    file::makeDirs(home.dir + "/projects/-fake");
    home.trust(work);
    const std::string cli                 = writeCli(work, kBranchCli);
    using Phase                           = model::Backend::AgentPhase;
    const std::vector<std::string> denied = {"Bash", "Edit", "Write", "NotebookEdit"};
    const std::string flags = " --fork-session --disallowedTools AskUserQuestion Bash Edit Write "
                              "NotebookEdit -- ";

    auto          rig  = std::make_unique<Rig>(Credentials{cli});
    const ConvRef conv = rig->start(work);
    REQUIRE(conv != kNoConv);
    rig->load(conv);
    rig->send(conv, "first");
    REQUIRE(rig->wait([&] { return rig->answered(conv, "answer: first"); }));
    const std::string session = rig->store.conversation(conv).id;
    const std::string sid     = "abcdef11-0000-4000-8000-000000000001";
    const std::string sPath   = home.dir + "/projects/-fake/" + sid + ".jsonl";
    rig->pump(300); // the turn settled

    // No such session, or one that can't be branched: said, nothing launched.
    std::optional<std::string> refused;
    rig->backend->startAgentBranch(
        "nope",
        "x",
        {},
        denied,
        [&](model::Backend::AgentBranch, const std::string &e) { refused = e; },
        {}
    );
    REQUIRE(rig->wait([&] { return refused.has_value(); }));
    CHECK(!refused->empty());

    // A branch: started off the session as it is, its first turn followed.
    std::optional<model::Backend::AgentBranch> branch;
    std::string                                startError;
    TurnLog                                    first;
    rig->backend->startAgentBranch(
        session,
        "what is up",
        {},
        denied,
        [&](model::Backend::AgentBranch b, const std::string &e) {
            branch     = std::move(b);
            startError = e;
            CHECK(first.updates.empty()); // the start is told before its turn
        },
        first.fn()
    );
    REQUIRE(rig->wait([&] { return branch.has_value(); }));
    CHECK_STR(startError, "");
    REQUIRE(!branch->id.empty());
    CHECK_STR(branch->forkPoint, lastUuid(sPath)); // the session's last record then
    CHECK(branch->conv == conv);
    CHECK(branch->root != 0);
    REQUIRE(rig->wait([&] { return first.done(); }));
    CHECK(first.phased(Phase::Reading)); // the Read call, while it ran
    CHECK_STR(first.updates.back().error, "");
    CHECK_STR(first.updates.back().answer, "answer: what is up");
    CHECK(!first.updates.back().answerId.empty());
    CHECK(contains(home.text("calls.log"), "--bg --resume " + sid + flags + "what is up\n"));
    // It's a thread under the session, never a session in the list.
    CHECK(rig->listedCount() == 1);
    const model::Message *root = nullptr;
    for (const auto &m : rig->load(conv))
        if (m.ts == branch->root)
            root = &m;
    REQUIRE(root);
    CHECK_STR(plain(*root), "what is up");
    // A label in the long prompt's place on the root, and back.
    rig->backend->setAgentBranchLabel(branch->id, "\xF0\x9F\x94\x97 Mira in Lumen: up?");
    REQUIRE(rig->wait([&] {
        for (const auto &m : rig->load(conv))
            if (m.ts == branch->root)
                return plain(m) == "\xF0\x9F\x94\x97 Mira in Lumen: up?";
        return false;
    }));
    rig->backend->setAgentBranchLabel(branch->id, {});
    REQUIRE(rig->wait([&] {
        for (const auto &m : rig->load(conv))
            if (m.ts == branch->root)
                return plain(m) == "what is up";
        return false;
    }));
    auto replies = rig->loadThread(conv, branch->root);
    REQUIRE(!replies.empty());
    CHECK_STR(plain(*replies.back()), "answer: what is up");

    // The session hasn't moved on since.
    std::optional<bool> moved;
    rig->backend->agentSessionMovedOn(session, branch->forkPoint, [&](bool m) { moved = m; });
    REQUIRE(rig->wait([&] { return moved.has_value(); }));
    CHECK_FALSE(*moved);

    // The next turn: a fork of the branch passing the tools again, taken as
    // the branch going on — one thread still, its replies in order.
    const std::string bSid = "abcdef12-0000-4000-8000-000000000002";
    TurnLog           second;
    rig->backend->continueAgentBranch(branch->id, "and more", {}, second.fn());
    REQUIRE(rig->wait([&] { return second.done(); }));
    CHECK_STR(second.updates.back().error, "");
    CHECK_STR(second.updates.back().answer, "answer: and more");
    CHECK(contains(home.text("calls.log"), "--bg --resume " + bSid + flags + "and more\n"));
    CHECK(rig->listedCount() == 1);
    replies = rig->loadThread(conv, branch->root);
    std::vector<std::string> texts;
    for (const auto *m : replies)
        if (!plain(*m).empty()) // the Read calls are cards
            texts.push_back(plain(*m));
    CHECK(
        texts == (std::vector<std::string>{"answer: what is up", "and more", "answer: and more"})
    );

    // The session goes on: from now on the branch is behind it.
    rig->send(conv, "second");
    REQUIRE(rig->wait([&] { return rig->answered(conv, "answer: second"); }));
    moved.reset();
    rig->backend->agentSessionMovedOn(session, branch->forkPoint, [&](bool m) { moved = m; });
    REQUIRE(rig->wait([&] { return moved.has_value(); }));
    CHECK(*moved);

    // Followed with no turn running: its last answer at once, the same one.
    TurnLog later;
    rig->backend->watchAgentBranch(branch->id, later.fn());
    REQUIRE(rig->wait([&] { return later.done(); }));
    CHECK_STR(later.updates.back().answer, "answer: and more");
    CHECK_STR(later.updates.back().answerId, second.updates.back().answerId);

    // After a restart the branch still goes without them.
    rig->pump(300);
    rig.reset();
    CHECK(contains(
        readText(dirs().data + "/known-sessions.json"),
        R"("deniedTools":["Bash","Edit","Write","NotebookEdit"])"
    ));
    rig = std::make_unique<Rig>(Credentials{cli});
    TurnLog third;
    rig->backend->continueAgentBranch(branch->id, "once more", {}, third.fn());
    REQUIRE(rig->wait([&] { return third.done(); }));
    CHECK_STR(third.updates.back().answer, "answer: once more");
    CHECK(contains(home.text("calls.log"), flags + "once more\n"));

    // A branch that isn't there says so through its turn.
    TurnLog gone;
    rig->backend->continueAgentBranch("nope", "x", {}, gone.fn());
    REQUIRE(rig->wait([&] { return gone.done(); }));
    CHECK(!gone.updates.back().error.empty());

    // Files go first, as mentions of copies in the cache (one ending the
    // prompt would eat the Enter when typed live).
    writeFile(work + "/notes.txt", "hi");
    std::optional<model::Backend::AgentBranch> withFile;
    TurnLog                                    fileTurn;
    rig->backend->startAgentBranch(
        session,
        "read this",
        {work + "/notes.txt"},
        denied,
        [&](model::Backend::AgentBranch b, const std::string &) { withFile = std::move(b); },
        fileTurn.fn()
    );
    REQUIRE(rig->wait([&] { return fileTurn.done(); }));
    REQUIRE(withFile.has_value());
    CHECK(withFile->root != branch->root); // a thread of its own
    CHECK(contains(home.text("calls.log"), flags + "@" + uploadsDir() + "/"));
    CHECK(contains(home.text("calls.log"), "/notes.txt read this\n"));
    CHECK(rig->errors.empty());
}

TEST("backend: a tool call's name says what an agent turn is doing") {
    using Phase = model::Backend::AgentPhase;
    CHECK(toolPhase("Read") == Phase::Reading);
    CHECK(toolPhase("Grep") == Phase::Searching);
    CHECK(toolPhase("WebFetch") == Phase::Browsing);
    CHECK(toolPhase("Bash") == Phase::Running);
    CHECK(toolPhase("Agent") == Phase::Delegating);
    CHECK(toolPhase("mcp__x__y") == Phase::Working);
    CHECK_STR(phaseStatus(Phase::Reading), "Reading files…");
    CHECK_STR(phaseStatus(Phase::WaitingForApproval), "Waiting for approval…");
}

#endif // !_WIN32

// Not a test: how the backend does on the real ~/.claude (read only — no
// send, no CLI). MSGA_CC_REAL_HOME=<dir> runs it.
TEST("backend: probe of a real Claude Code home") {
    const std::string real = base::env("MSGA_CC_REAL_HOME");
    if (real.empty())
        return;
    auto              app  = plat::App::create();
    const std::string msga = tempDir("msga-probe");
    base::test::setEnv("CLAUDE_CONFIG_DIR", real);
    setDirs({msga + "/data", msga + "/cache"});
    model::Store  store;
    const int64_t t0 = base::monotonicMs();
    {
        Backend backend(store, *app, Credentials{});
        bool    connected = false;
        backend.connect([&](bool, const std::string &) { connected = true; });
        waitFor(*app, [&] { return connected; });
        const int64_t t1     = base::monotonicMs();
        int           listed = 0;
        for (ConvRef c = 0; c < store.conversationCount(); ++c)
            listed += store.conversation(c).member;
        std::printf(
            "probe: %d sessions listed, %zu users, first scan %lld ms\n",
            listed,
            store.userCount(),
            (long long)(t1 - t0)
        );
        for (ConvRef c = 0; c < store.conversationCount() && c < 15; ++c) {
            const auto &cv = store.conversation(c);
            std::printf(
                "  %-40s unread %u mentions %u %s\n",
                store.displayName(c).c_str(),
                cv.unread,
                cv.mentions,
                cv.readOnly.empty() ? "" : "(read-only)"
            );
        }
        if (store.conversationCount()) {
            bool          loaded = false;
            const int64_t t2     = base::monotonicMs();
            backend.loadHistory(0, 0, [&](bool, const std::string &) { loaded = true; });
            waitFor(*app, [&] { return loaded; });
            std::printf(
                "probe: first history %zu messages in %lld ms\n",
                store.conversation(0).messages.size(),
                (long long)(base::monotonicMs() - t2)
            );
        }
        backend.close();
    }
    base::test::unsetEnv("CLAUDE_CONFIG_DIR");
}

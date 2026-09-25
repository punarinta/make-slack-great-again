// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Claude Code backend: transcript parsing, roster parsing, and the backend
// end to end against a fake ~/.claude (CLAUDE_CONFIG_DIR) — docs/backend-modules-plan.md §5.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QFileInfo>
#include <QUrl>

#include "backend/claude_code/cc_catalog.h"
#include "backend/claude_code/cc_roster.h"
#include "backend/claude_code/cc_transcript.h"
#include "backend/claude_code/claude_code_backend.h"

using namespace claude_code;
using Kind  = TranscriptItem::Kind;
using State = TranscriptItem::State;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");
    QStandardPaths::setTestModeEnabled(true);
    return Catch::Session().run(argc, argv);
}

namespace {

QByteArray line(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray prompt(const QString &text, const char *ts, bool viaOrigin = true) {
    QJsonObject o{
        {"type", "user"},
        {"timestamp", ts},
        {"message", QJsonObject{{"role", "user"}, {"content", text}}},
    };
    if (viaOrigin)
        o["origin"] = QJsonObject{{"kind", "human"}};
    return line(o);
}

QByteArray assistantText(const QString &text, const char *ts) {
    return line({
        {"type", "assistant"},
        {"timestamp", ts},
        {"message",
         QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}}},
    });
}

QByteArray
toolUse(const QString &id, const QString &name, const QJsonObject &input, const char *ts) {
    return line({
        {"type", "assistant"},
        {"timestamp", ts},
        {"message",
         QJsonObject{
             {"content",
              QJsonArray{
                  QJsonObject{{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", input}}
              }}
         }},
    });
}

QByteArray
toolResult(const QString &id, const char *ts, bool error = false, const QString &agentId = {}) {
    QJsonObject o{
        {"type", "user"},
        {"timestamp", ts},
        {"message",
         QJsonObject{
             {"content",
              QJsonArray{
                  QJsonObject{{"type", "tool_result"}, {"tool_use_id", id}, {"is_error", error}}
              }}
         }},
    };
    if (!agentId.isEmpty())
        o["toolUseResult"] = QJsonObject{{"agentId", agentId}};
    return line(o);
}

QByteArray turnEnd(const char *ts) {
    return line({{"type", "system"}, {"subtype", "turn_duration"}, {"timestamp", ts}});
}

// One realistic turn: prompt, a remark, two tool calls, the answer, turn end.
QByteArray sampleTurn() {
    return prompt("fix the build", "2026-09-25T10:00:00.000Z") +
           assistantText("Let me look.", "2026-09-25T10:00:01.000Z") +
           toolUse(
               "t1",
               "Bash",
               {{"command", "make"}, {"description", "Build it"}},
               "2026-09-25T10:00:02.000Z"
           ) +
           toolResult("t1", "2026-09-25T10:00:03.000Z") +
           toolUse("t2", "Edit", {{"file_path", "/x/main.cpp"}}, "2026-09-25T10:00:04.000Z") +
           toolResult("t2", "2026-09-25T10:00:05.000Z", /*error=*/true) +
           assistantText("Fixed: a missing **include**.", "2026-09-25T10:00:06.000Z") +
           turnEnd("2026-09-25T10:00:07.000Z");
}

} // namespace

// ── Transcript parsing ────────────────────────────────────────────────────────

TEST_CASE(
    "a turn becomes prompt, progress remark, tool card and final answer", "[claude][transcript]"
) {
    TranscriptParser p;
    p.feed(sampleTurn());
    const auto &items = p.items();
    REQUIRE(items.size() == 4);
    CHECK(items[0].kind == Kind::UserPrompt);
    CHECK(items[0].text == "fix the build");
    CHECK(items[1].kind == Kind::AssistantText);
    CHECK(items[1].state == State::Progress); // Claude kept working after it
    CHECK(items[2].kind == Kind::ToolGroup);
    REQUIRE(items[2].tools.size() == 2);
    CHECK(items[2].tools[0].summary == "Build it");
    CHECK_FALSE(items[2].tools[0].error);
    CHECK(items[2].tools[1].error);
    CHECK(items[3].kind == Kind::AssistantText);
    CHECK(items[3].state == State::Final);
    CHECK_FALSE(p.turnOpen());
}

TEST_CASE(
    "the last text of a live turn stays pending until the turn resolves it", "[claude][transcript]"
) {
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

TEST_CASE("feeding byte by byte yields the same items as one piece", "[claude][transcript]") {
    const QByteArray all = sampleTurn();
    TranscriptParser whole;
    whole.feed(all);
    TranscriptParser pieces;
    for (qsizetype i = 0; i < all.size(); i += 7)
        pieces.feed(all.mid(i, 7));
    CHECK(pieces.items() == whole.items());
}

TEST_CASE("system text recorded as user turns is hidden", "[claude][transcript]") {
    TranscriptParser p;
    QJsonObject      meta{
        {"type", "user"},
        {"isMeta", true},
        {"timestamp", "2026-09-25T10:00:00.000Z"},
        {"message", QJsonObject{{"content", "caveat"}}},
    };
    QJsonObject notification{
        {"type", "user"},
        {"timestamp", "2026-09-25T10:00:01.000Z"},
        {"origin", QJsonObject{{"kind", "task-notification"}}},
        {"message", QJsonObject{{"content", "<task-notification>done</task-notification>"}}},
    };
    p.feed(
        line(meta) + line(notification) +
        prompt("[Request interrupted by user]", "2026-09-25T10:00:03.000Z") +
        prompt(
            "<command-name>/model</command-name>\n<command-args>opus</command-args>",
            "2026-09-25T10:00:04.000Z"
        )
    );
    REQUIRE(p.items().size() == 1);
    CHECK(p.items()[0].text == "/model opus"); // a slash command reads as typed
}

TEST_CASE("a command Claude Code runs itself shows with its output", "[claude][transcript]") {
    TranscriptParser p;
    // /context: both halves are "local_command" system records.
    p.feed(
        line({
            {"type", "system"},
            {"subtype", "local_command"},
            {"timestamp", "2026-09-25T10:00:00.000Z"},
            {"content",
             "<command-name>/context</command-name>\n<command-message>context</command-message>\n"
             "<command-args></command-args>"},
        }) +
        line({
            {"type", "system"},
            {"subtype", "local_command"},
            {"timestamp", "2026-09-25T10:00:01.000Z"},
            {"content",
             "<local-command-stdout>\x1b[1mContext Usage\x1b[22m\n42k / 1m</local-command-stdout>"},
        })
    );
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[0].kind == Kind::UserPrompt);
    CHECK(p.items()[0].text == "/context");
    CHECK(p.items()[1].kind == Kind::AssistantText);
    CHECK(p.items()[1].state == State::Final);
    CHECK(p.items()[1].text == "```\nContext Usage\n42k / 1m\n```"); // colours gone
    CHECK_FALSE(p.turnOpen()); // no turn_duration comes: the output ends it

    // …followed by the same report as markdown for the model, which is shown instead.
    p.feed(line({
        {"type", "user"},
        {"isMeta", true},
        {"timestamp", "2026-09-25T10:00:01.500Z"},
        {"message", QJsonObject{{"content", "## Context Usage\n\n**Tokens:** 42k / 1m"}}},
    }));
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[1].text == "## Context Usage\n\n**Tokens:** 42k / 1m");

    // /compact: user records, after the summary it starts the session over from.
    QJsonObject summary{
        {"type", "user"},
        {"isCompactSummary", true},
        {"timestamp", "2026-09-25T10:01:00.000Z"},
        {"message", QJsonObject{{"content", "This session is being continued…"}}},
    };
    QJsonObject synthetic{
        {"type", "assistant"},
        {"timestamp", "2026-09-25T10:00:59.000Z"},
        {"message",
         QJsonObject{
             {"model", "<synthetic>"},
             {"content",
              QJsonArray{QJsonObject{{"type", "text"}, {"text", "No response requested."}}}},
         }},
    };
    p.feed(
        line(synthetic) + prompt("/compact keep it short", "2026-09-25T10:00:59.500Z") +
        line(summary) +
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
    CHECK(p.items()[2].text == "/compact keep it short");
    CHECK(p.items()[3].text == "Compacted");
    CHECK_FALSE(p.turnOpen());
}

TEST_CASE("Claude Code's command list", "[claude][commands]") {
    const QByteArray out =
        R"j({"type":"control_response","response":{"subtype":"success","request_id":"msga",)j"
        R"j("response":{"commands":[)j"
        R"j({"name":"compact","description":"Free up context","argumentHint":"<instructions>","builtin":true},)j"
        R"j({"name":"verify","description":"Drive the app (project)","argumentHint":""},)j"
        R"j({"name":"gui-sudo","description":"Run a root command (user)","argumentHint":""},)j"
        R"j({"name":"__remote-workflow","description":"internal","builtin":true},)j"
        R"j({"name":"agents","description":"(removed) Ask Claude","builtin":true},)j"
        R"j({"name":"extra-usage","description":"Renamed to /usage-credits","builtin":true}]}}})j"
        "\n";
    const auto cmds = parseCommandList(out);
    REQUIRE(cmds.size() == 3);
    CHECK(cmds[0].name == "compact");
    CHECK(cmds[0].usage == "<instructions>");
    CHECK(cmds[0].source == "Claude Code");
    CHECK(cmds[1].desc == "Drive the app");
    CHECK(cmds[1].source == "Project skill");
    CHECK(cmds[2].source == "Skill");
    CHECK(parseCommandList("not json\n").empty());
}

TEST_CASE("a prompt queued mid-turn shows without ending the turn", "[claude][transcript]") {
    TranscriptParser p;
    p.feed(
        prompt("go", "2026-09-25T10:00:00.000Z") +
        assistantText("Working on it", "2026-09-25T10:00:01.000Z")
    );
    p.feed(line({
        {"type", "attachment"},
        {"timestamp", "2026-09-25T10:00:02.000Z"},
        {"attachment",
         QJsonObject{{"type", "queued_command"}, {"commandMode", "prompt"}, {"prompt", "also X"}}},
    }));
    REQUIRE(p.items().size() == 3);
    CHECK(p.items()[2].kind == Kind::UserPrompt);
    CHECK(p.items()[2].text == "also X");
    CHECK(p.items()[1].state == State::Pending); // the turn goes on
}

TEST_CASE("an Agent call becomes a subagent item linked by its agent id", "[claude][transcript]") {
    TranscriptParser p;
    p.feed(
        prompt("research", "2026-09-25T10:00:00.000Z") +
        toolUse(
            "a1",
            "Agent",
            {{"description", "Search the docs"}, {"prompt", "…"}},
            "2026-09-25T10:00:01.000Z"
        ) +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "abc123")
    );
    REQUIRE(p.items().size() == 2);
    CHECK(p.items()[1].kind == Kind::Subagent);
    CHECK(p.items()[1].text == "Search the docs");
    CHECK(p.items()[1].agentId == "abc123");
}

TEST_CASE(
    "records in the same millisecond still get distinct, ordered ids", "[claude][transcript]"
) {
    TranscriptParser p;
    p.feed(
        prompt("a", "2026-09-25T10:00:00.000Z") + prompt("b", "2026-09-25T10:00:00.000Z") +
        prompt("c", "2026-09-25T09:00:00.000Z")
    ); // even a clock step back
    REQUIRE(p.items().size() == 3);
    CHECK(p.items()[0].ts < p.items()[1].ts);
    CHECK(p.items()[1].ts < p.items()[2].ts);
    CHECK(p.items()[0].ts.size() == p.items()[2].ts.size()); // fixed width: string order works
}

TEST_CASE("torn and foreign lines are skipped, not fatal", "[claude][transcript]") {
    TranscriptParser p;
    p.feed(
        "{not json\n" + line({{"type", "file-history-snapshot"}}) +
        prompt("still here", "2026-09-25T10:00:00.000Z")
    );
    REQUIRE(p.items().size() == 1);
    CHECK(p.items()[0].text == "still here");
}

TEST_CASE(
    "markdown renders headings bold, keeps angle brackets, fences tables", "[claude][markdown]"
) {
    const auto heading = renderMarkdown("## Result\nuse a < b && c > d");
    CHECK(heading.text.startsWith("Result\n"));
    CHECK(heading.text.contains("use a < b && c > d"));
    bool bold = false;
    for (const auto &e : heading.entities)
        bold = bold || (e.type == EntityType::Bold && e.offset == 0 && e.length == 6);
    CHECK(bold);

    const auto table = renderMarkdown("| a | b |\n|---|---|\n| 1 | 2 |");
    bool       pre   = false;
    for (const auto &e : table.entities)
        pre = pre || e.type == EntityType::Pre;
    CHECK(pre); // monospace keeps the columns aligned
}

TEST_CASE("bare URLs in Claude's text are links", "[claude][message]") {
    auto links = [](const QString &md) {
        const TextWithEntities t = renderMarkdown(md);
        QStringList            out;
        for (const auto &e : t.entities)
            if (e.type == EntityType::Link)
                out << e.data;
        return out;
    };
    CHECK(
        links("Here: https://ex.example.com/iA09a_HA?x=1&y=2.") ==
        QStringList{"https://ex.example.com/iA09a_HA?x=1&y=2"}
    );
    CHECK(links("(see https://a.example/b)") == QStringList{"https://a.example/b"});
    CHECK(
        links("https://en.wikipedia.org/wiki/Foo_(bar)") ==
        QStringList{"https://en.wikipedia.org/wiki/Foo_(bar)"}
    );
    CHECK(links("<https://auto.example/x>") == QStringList{"https://auto.example/x"});
    CHECK(links("[the docs](https://docs.example/p)") == QStringList{"https://docs.example/p"});
    CHECK(links("run `curl https://code.example/`").isEmpty());
    CHECK(links("```\nhttps://fenced.example/\n```").isEmpty());
    const TextWithEntities t = renderMarkdown("go to https://x.example/a now");
    CHECK(t.text == "go to https://x.example/a now");
}

TEST_CASE("a teammate mention renders as a mention", "[claude][message]") {
    auto mentions = [](const QString &md) {
        QStringList out;
        for (const auto &e : renderMarkdown(md).entities)
            if (e.type == EntityType::UserMention)
                out << e.data;
        return out;
    };
    CHECK(
        mentions("Do you know who @claude:role:engineer is?") == QStringList{"claude:role:engineer"}
    );
    CHECK(
        mentions("ask @claude:agent and @claude:role:data-analyst.") ==
        QStringList{"claude:agent", "claude:role:data-analyst"}
    );
    CHECK(mentions("run `echo @claude:role:engineer`").isEmpty());
    // Quoted on its own, it's still the teammate (Claude's habit).
    const auto quoted = renderMarkdown("Yes. `@claude:role:researcher` is the id.");
    CHECK(quoted.text == "Yes. @claude:role:researcher is the id.");
    CHECK(
        mentions("Yes. `@claude:role:researcher` is the id.") ==
        QStringList{"claude:role:researcher"}
    );
    CHECK(std::none_of(quoted.entities.begin(), quoted.entities.end(), [](const auto &e) {
        return e.type == EntityType::Code;
    }));
    CHECK(mentions("```\n@claude:role:engineer\n```").isEmpty());
    CHECK(mentions("mail x@claude:role:engineer").isEmpty());
}

TEST_CASE("a pasted image is attached to the prompt", "[claude][message]") {
    QImage img(3, 2, QImage::Format_RGB32);
    img.fill(Qt::red);
    QByteArray png;
    QBuffer    buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    QJsonObject rec{
        {"type", "user"},
        {"timestamp", "2026-09-25T10:00:00.000Z"},
        {"origin", QJsonObject{{"kind", "human"}}},
        {"imagePasteIds", QJsonArray{14}},
        {"message",
         QJsonObject{
             {"content",
              QJsonArray{
                  QJsonObject{{"type", "text"}, {"text", "look at this: [Image #14] please"}},
                  QJsonObject{
                      {"type", "image"},
                      {"source",
                       QJsonObject{
                           {"type", "base64"},
                           {"media_type", "image/png"},
                           {"data", QString::fromLatin1(png.toBase64())}
                       }}
                  },
              }}
         }},
    };
    TranscriptParser p;
    p.feed(line(rec));
    REQUIRE(p.items().size() == 1);
    const auto &item = p.items()[0];
    REQUIRE(item.images.size() == 1);
    CHECK(QFile::exists(item.images[0]));
    CHECK(item.text == "look at this: please"); // the placeholder goes
    const Message m = toMessage(item, UserId{"me"}, UserId{"claude:agent"});
    REQUIRE(m.files.size() == 1);
    CHECK(m.files[0].name == "Image 14.png");
    CHECK(m.files[0].imageWidth == 3);
    CHECK(m.files[0].imageHeight == 2);
    CHECK(m.files[0].thumbUrl.startsWith("file:"));
    // The same image again is the same cached file.
    CHECK(cachePastedImage("image/png", png.toBase64()) == item.images[0]);
}

TEST_CASE("markdown tables become table blocks in reading order", "[claude][message]") {
    const QString md     = "Before the table.\n\n"
                           "| Session | Can you write? |\n"
                           "|---|:---:|\n"
                           "| Open in a terminal | **no** |\n"
                           "| Closed \\| gone | `a|b` |\n"
                           "\nAfter it.";
    const auto    blocks = markdownBlocks(md);
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[0].typeStr == "rich_text");
    CHECK(blocks[0].text.text == "Before the table.");
    REQUIRE(blocks[1].typeStr == "table");
    const auto &rows = blocks[1].tableRows;
    REQUIRE(rows.size() == 3); // header + 2, the separator dropped
    REQUIRE(rows[0].size() == 2);
    CHECK(rows[0][0].text == "Session");
    CHECK(std::any_of(rows[0][0].entities.begin(), rows[0][0].entities.end(), [](const auto &e) {
        return e.type == EntityType::Bold; // header row is bold
    }));
    CHECK(rows[1][1].text == "no");
    CHECK(std::any_of(rows[1][1].entities.begin(), rows[1][1].entities.end(), [](const auto &e) {
        return e.type == EntityType::Bold; // the cell's own **bold**
    }));
    CHECK(rows[2][0].text == "Closed | gone"); // escaped pipe stays in the cell
    CHECK(rows[2][1].text == "a|b");           // so does one inside code
    CHECK(blocks[2].text.text == "After it.");

    CHECK(markdownBlocks("no table here\n| just a pipe line |").empty());
    CHECK(markdownBlocks("```\n| a | b |\n|---|---|\n```").empty()); // fenced stays code
}

TEST_CASE("items map to messages: authors, progress subtype, tool card", "[claude][message]") {
    TranscriptParser p;
    p.feed(sampleTurn());
    const UserId me{"me"}, claude{"claude:S1"};
    const auto   items = p.items();
    CHECK(toMessage(items[0], me, claude).author == me);
    const Message remark = toMessage(items[1], me, claude);
    CHECK(remark.author == claude);
    CHECK(isProgressMessage(remark));
    const Message card = toMessage(items[2], me, claude);
    CHECK(isProgressMessage(card));
    REQUIRE(card.attachments.size() == 1);
    CHECK(card.attachments[0].text.text.contains("Bash  Build it"));
    CHECK(card.attachments[0].text.text.contains("✗")); // the failed Edit
    const Message answer = toMessage(items[3], me, claude);
    CHECK_FALSE(isProgressMessage(answer));
    CHECK(answer.text.text == "Fixed: a missing include.");
}

// ── Roster ────────────────────────────────────────────────────────────────────

TEST_CASE("roster files parse into sessions", "[claude][roster]") {
    const auto live = parseInteractiveSession(
        R"({"pid":42,"sessionId":"S1","cwd":"/src/app","name":"app-1","status":"waiting",
            "statusUpdatedAt":1790000000000,"entrypoint":"cli","kind":"interactive"})"
    );
    REQUIRE(live);
    CHECK(live->sessionId == "S1");
    CHECK(live->kind == SessionInfo::Kind::Interactive);
    CHECK(live->entrypoint == "cli");
    CHECK(statusNeedsUser(live->status));

    const auto job = parseBackgroundJob(
        R"({"state":"done","sessionId":"S2","name":"Refactor","cwd":"/src/x","needs":null,
            "linkScanPath":"/p/S2.jsonl","updatedAt":"2026-09-02T06:56:01.982Z"})"
    );
    REQUIRE(job);
    CHECK(job->kind == SessionInfo::Kind::Background);
    CHECK_FALSE(job->running); // done: msga may continue it
    CHECK(job->transcriptPath == "/p/S2.jsonl");
    CHECK(job->statusSinceMs > 0);

    // Waiting for an approval: "working" plus a needs line (verified live).
    const auto approval = parseBackgroundJob(
        R"({"state":"working","sessionId":"S3","needs":"approve Bash: touch x"})"
    );
    REQUIRE(approval);
    CHECK(statusNeedsUser(approval->status));
    CHECK_FALSE(statusIsBusy(approval->status));

    // A done job whose worker is still alive (idle) counts as running — resuming
    // it would only start a copy — and a busy worker makes it busy.
    auto done   = *job;
    auto worker = parseInteractiveSession(
        R"({"pid":7,"sessionId":"S2","kind":"bg","status":"busy","entrypoint":"cli"})"
    );
    REQUIRE(worker);
    CHECK(worker->kind == SessionInfo::Kind::Background);
    applyWorker(done, *worker);
    CHECK(done.running);
    CHECK(statusIsBusy(done.status));

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
    CHECK(statusIsBusy("busy"));
    CHECK(statusIsBusy("working"));
    CHECK_FALSE(statusIsBusy("idle"));
    // Idle with a background command running: nobody is typing.
    CHECK_FALSE(statusIsBusy("shell"));
    CHECK(statusHasShell("shell"));
}

// ── The backend against a fake ~/.claude ──────────────────────────────────────

namespace {

struct FakeClaudeHome {
    QTemporaryDir dir;
    QString       transcript;

    FakeClaudeHome() {
        qputenv("CLAUDE_CONFIG_DIR", dir.path().toUtf8());
        QDir(dir.path()).mkpath("sessions");
        QDir(dir.path()).mkpath("projects/-src-app");
        transcript = dir.path() + "/projects/-src-app/S1.jsonl";
        // A known-sessions file from an earlier test run would leak in.
        QFile::remove(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
            "/claude-code/known-sessions.json"
        );
    }
    ~FakeClaudeHome() { qunsetenv("CLAUDE_CONFIG_DIR"); }

    void writeSession(const QString &status, const QString &name = "app-1") {
        QFile f(dir.path() + "/sessions/1.json");
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"pid", QCoreApplication::applicationPid()}, // alive
                        {"sessionId", "S1"},
                        {"cwd", "/src/app"},
                        {"name", name},
                        {"status", status},
                        {"entrypoint", "cli"},
                    }
        )
                    .toJson());
    }
    void append(const QByteArray &bytes) {
        QFile f(transcript);
        REQUIRE(f.open(QIODevice::Append));
        f.write(bytes);
    }
};

template <typename T>
std::vector<T> collect(rpl::producer<T> p) {
    std::vector<T> out;
    rpl::lifetime  lt;
    std::move(p) | rpl::on_next([&](T v) { out.push_back(std::move(v)); }, lt);
    return out;
}

} // namespace

TEST_CASE("a session title names the teammates it mentions", "[claude][backend]") {
    FakeClaudeHome home;
    home.writeSession("idle", "Ask @claude:role:engineer and @claude:agent");
    home.append(prompt("hi", "2026-09-25T10:00:00.000Z"));

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const auto convs = collect(backend.loadConversations());
    REQUIRE(convs.size() == 1);
    REQUIRE(convs[0].size() == 1);
    CHECK(convs[0][0].name == "Ask @Engineer and @Generalist");
    const auto users = collect(backend.loadUsers());
    CHECK(std::any_of(users[0].begin(), users[0].end(), [](const User &u) {
        return u.id.value == "claude:S1" && u.name == "Ask @Engineer and @Generalist";
    }));
}

TEST_CASE(
    "backend lists a terminal session read-only and announces its answer", "[claude][backend]"
) {
    FakeClaudeHome home;
    home.writeSession("busy");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z")
    );

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();

    const auto convs = collect(backend.loadConversations());
    REQUIRE(convs.size() == 1);
    REQUIRE(convs[0].size() == 1);
    const Conversation c = convs[0][0];
    // Working on the turn reads as the session typing (the terminal's spinner).
    CHECK(std::any_of(events.begin(), events.end(), [](const Event &e) {
        const auto *t = std::get_if<EvTyping>(&e);
        return t && t->conv.value == "S1" && t->user.value == "claude:agent";
    }));
    CHECK(c.name == "app-1");
    CHECK(c.kind == ConvKind::Im);
    CHECK(c.readOnlyReason.contains("terminal")); // a terminal drives it

    // While Claude works on the turn only the prompt shows.
    auto history = collect(backend.loadHistory(c.id, std::nullopt));
    REQUIRE(history.size() == 1);
    REQUIRE(history[0].messages.size() == 1);
    CHECK(history[0].messages[0].author == UserId{"me"});

    // The turn ends: the answer is announced (once), and the dot goes idle.
    home.append(turnEnd("2026-09-25T10:00:02.000Z"));
    home.writeSession("idle");
    const bool announced = QTest::qWaitFor(
        [&] {
            for (const auto &e : events)
                if (const auto *n = std::get_if<EvMessageNew>(&e))
                    return n->msg.text.text == "Hello!";
            return false;
        },
        5000
    );
    CHECK(announced);

    // The terminal session ends: without a claude CLI it stays read-only, but
    // for a different reason, and the change is pushed as a conversation update.
    events.clear();
    QFile::remove(home.dir.path() + "/sessions/1.json");
    const bool updated = QTest::qWaitFor(
        [&] {
            for (const auto &e : events)
                if (const auto *u = std::get_if<EvChannelCreated>(&e))
                    return u->conv.readOnlyReason.contains("Install");
            return false;
        },
        12000
    );
    CHECK(updated);
    // …and it is still listed: ended sessions stay while their transcript does.
    CHECK(collect(backend.loadConversations())[0].size() == 1);
}

TEST_CASE(
    "a session removed from msga stays away until it gets new activity", "[claude][backend]"
) {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("hi", "2026-09-25T10:00:00.000Z") +
        assistantText("Hello!", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    const ConversationId conv{"S1"};
    {
        claude_code::Backend backend(Credentials{});
        backend.connectRealtime();
        REQUIRE(collect(backend.loadConversations())[0].size() == 1);
        backend.leaveConversation(conv);
        CHECK(collect(backend.loadConversations())[0].empty());
    }
    // Remembered across restarts; Claude Code's own files are left alone.
    CHECK(QFile::exists(home.transcript));
    CHECK(QFile::exists(home.dir.path() + "/sessions/1.json"));
    QTest::qWait(20); // the transcript's next write must be newer than the removal

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    CHECK(collect(backend.loadConversations())[0].empty());

    // Claude Code's daemon retiring the idle worker appends bookkeeping: no
    // activity, it stays away.
    home.append(
        "{\"type\":\"last-prompt\",\"lastPrompt\":\"hi\",\"sessionId\":\"S1\"}\n"
        "{\"type\":\"cost-state\",\"sessionId\":\"S1\"}\n"
    );
    home.writeSession("idle");
    QTest::qWait(1500);
    CHECK(collect(backend.loadConversations())[0].empty());

    // Someone continues it in the terminal: it's back.
    home.append(prompt("more", "2026-09-25T11:00:00.000Z"));
    home.writeSession("busy");
    CHECK(
        QTest::qWaitFor(
            [&] {
                for (const auto &e : events)
                    if (const auto *c = std::get_if<EvChannelCreated>(&e); c && c->conv.id == conv)
                        return true;
                return false;
            },
            12000
        )
    );
    CHECK(collect(backend.loadConversations())[0].size() == 1);
}

TEST_CASE(
    "a removed background job whose transcript Claude Code deleted stays removed",
    "[claude][backend]"
) {
    FakeClaudeHome home;
    QDir(home.dir.path()).mkpath("jobs/8d953db6");
    {
        QFile f(home.dir.path() + "/jobs/8d953db6/state.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"state", "done"},
                        {"sessionId", "8d953db6-f3be-4b02-8f0f-09aea0343b3e"},
                        {"cwd", "/src/app"},
                        {"name", "no transcript"},
                        {"linkScanPath", home.dir.path() + "/projects/-src-app/gone.jsonl"},
                    }
        )
                    .toJson());
    }
    const ConversationId conv{"8d953db6-f3be-4b02-8f0f-09aea0343b3e"};
    {
        claude_code::Backend backend(Credentials{});
        backend.connectRealtime();
        REQUIRE(collect(backend.loadConversations())[0].size() == 1);
        backend.leaveConversation(conv);
        // Any roster change triggers a rescan: it must not bring the job back.
        home.writeSession("idle");
        QTest::qWait(1500);
        const auto convs = collect(backend.loadConversations())[0];
        CHECK(std::none_of(convs.begin(), convs.end(), [&](const Conversation &c) {
            return c.id == conv;
        }));
    }
    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const auto convs = collect(backend.loadConversations())[0];
    CHECK(std::none_of(convs.begin(), convs.end(), [&](const Conversation &c) {
        return c.id == conv;
    }));
}

TEST_CASE("a session renamed in msga is titled by that name", "[claude][backend]") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(prompt("hi", "2026-09-25T10:00:00.000Z"));
    const ConversationId conv{"S1"};
    const UserId         peer{"claude:S1"};
    auto                 nameOf = [&](claude_code::Backend &b) {
        const auto users = collect(b.loadUsers());
        for (const auto &u : users[0])
            if (u.id == peer)
                return u.displayName;
        return QString();
    };
    {
        claude_code::Backend backend(Credentials{});
        std::vector<Event>   events;
        rpl::lifetime        lt;
        backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
        backend.connectRealtime();
        REQUIRE(nameOf(backend) == "app-1");
        backend.setConversationLocalName(conv, " Refunds ");
        // The DM's title comes from its peer: pushed right away.
        CHECK(std::any_of(events.begin(), events.end(), [&](const Event &e) {
            const auto *u = std::get_if<EvUserChanged>(&e);
            return u && u->user.id == peer && u->user.displayName == "Refunds";
        }));
        const auto convs = collect(backend.loadConversations())[0];
        REQUIRE(convs.size() == 1);
        CHECK(convs[0].localName == "Refunds");
        CHECK(convs[0].name == "app-1"); // Claude Code's own name, the dialog's placeholder
    }
    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    CHECK(nameOf(backend) == "Refunds"); // kept across restarts
    backend.setConversationLocalName(conv, "");
    CHECK(nameOf(backend) == "app-1");
}

TEST_CASE("your name and picture are kept by msga", "[claude][backend][profile]") {
    FakeClaudeHome home;
    const QString  appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QFile::remove(appData + "/claude-code/profile.json");
    QTemporaryDir pics;
    const QString pic = pics.path() + "/me.png";
    {
        QFile f(pic);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not really a png");
    }
    auto meOf = [](claude_code::Backend &b) {
        const auto users = collect(b.loadUsers());
        return users[0][0]; // you come first
    };
    {
        claude_code::Backend backend(Credentials{});
        CHECK_FALSE(backend.capabilities().profileContact);
        const User before = meOf(backend);
        CHECK(before.avatarUrl.isEmpty());

        bool ok = false;
        backend.updateProfile({{"display_name", "Robin"}}, [&](bool o, QString) { ok = o; });
        CHECK(ok);
        QString url;
        backend.setPhoto(pic, [&](bool o, QString, QString u) {
            ok  = o;
            url = u;
        });
        CHECK(ok);
        CHECK(url.startsWith("file:"));
        CHECK(QFile::exists(QUrl(url).toLocalFile()));
    }
    claude_code::Backend backend(Credentials{});
    const User           after = meOf(backend);
    CHECK(after.displayName == "Robin");
    CHECK(!after.avatarUrl.isEmpty());
    MyProfile loaded;
    backend.loadMyProfile([&](MyProfile p) { loaded = p; });
    CHECK(loaded.displayName == "Robin");
    CHECK(loaded.avatarUrl == after.avatarUrl);
    QFile::remove(appData + "/claude-code/profile.json");
    QFile::remove(QUrl(after.avatarUrl).toLocalFile());
}

TEST_CASE("an idle session with a background command running is not typing", "[claude][backend]") {
    FakeClaudeHome home;
    home.writeSession("shell");
    home.append(
        prompt("watch it", "2026-09-25T10:00:00.000Z") +
        assistantText("Watching.", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    const auto users = collect(backend.loadUsers());
    const auto peer  = std::find_if(users[0].begin(), users[0].end(), [](const User &u) {
        return u.id == UserId{"claude:S1"};
    });
    REQUIRE(peer != users[0].end());
    CHECK_FALSE(peer->isActive); // no "working" dot…
    CHECK(peer->unavailable);    // …but the yellow one: a terminal holds it
    CHECK(peer->statusText == "Running a background command");
    // Its teammate (the generalist) is yellow too: nothing of it works.
    const auto mate = std::find_if(users[0].begin(), users[0].end(), [](const User &u) {
        return u.id == UserId{"claude:agent"};
    });
    REQUIRE(mate != users[0].end());
    CHECK_FALSE(mate->isActive);
    CHECK(mate->unavailable);
    CHECK_FALSE(collect(backend.loadPresence(UserId{"claude:S1"}))[0]);
    QTest::qWait(3500); // the typing pump runs every 3 s while anything is busy
    CHECK(std::none_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvTyping>(e);
    }));
}

TEST_CASE(
    "a background job stuck on \"working\" after its turn ended is not typing",
    "[claude][backend][bg]"
) {
    // Seen live 2026-09-25: a subagent's notification left the job reading
    // "working" (inFlight.queued 1) while its worker sat idle for good.
    FakeClaudeHome home;
    home.append(
        prompt("look for duplicates", "2026-09-25T10:00:00.000Z") +
        assistantText("Done.", "2026-09-25T10:00:01.000Z") + turnEnd("2026-09-25T10:00:02.000Z")
    );
    QDir(home.dir.path()).mkpath("jobs/S1");
    {
        QFile f(home.dir.path() + "/jobs/S1/state.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"state", "working"},
                        {"sessionId", "S1"},
                        {"cwd", "/src/app"},
                        {"name", "duplicates"},
                        {"linkScanPath", home.transcript},
                    }
        )
                    .toJson());
    }
    {
        QFile f(home.dir.path() + "/sessions/1.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"pid", QCoreApplication::applicationPid()}, // alive
                        {"sessionId", "S1"},
                        {"cwd", "/src/app"},
                        {"kind", "bg"},
                        {"status", "idle"},
                        {"entrypoint", "cli"},
                    }
        )
                    .toJson());
    }
    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    const auto users = collect(backend.loadUsers());
    const auto peer  = std::find_if(users[0].begin(), users[0].end(), [](const User &u) {
        return u.id == UserId{"claude:S1"};
    });
    REQUIRE(peer != users[0].end());
    CHECK_FALSE(peer->isActive); // no "working" dot
    QTest::qWait(3500);          // the typing pump runs every 3 s while anything is busy
    CHECK(std::none_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvTyping>(e);
    }));
}

#if !defined(Q_OS_WIN)
// ── Team roles ────────────────────────────────────────────────────────────────

TEST_CASE("the team: a generalist first, then the specialists", "[claude][roles]") {
    const auto &all = builtInRoles();
    REQUIRE(all.size() == 5);
    CHECK(all[0].id == "generalist");
    CHECK(appendedPrompt(all[0]).isEmpty()); // plain Claude Code
    for (size_t i = 1; i < all.size(); ++i)
        CHECK(appendedPrompt(all[i]).startsWith(
            "# Your role: " + all[i].promptName + " (msga: " + all[i].id + ")\n"
        ));
}

TEST_CASE("a session's role is read back from its recorded system prompt", "[claude][roles]") {
    const Role      &engineer = builtInRoles()[1];
    const QJsonArray withRole{"You are an interactive agent…", appendedPrompt(engineer)};
    CHECK(roleInSystemPrompt(withRole).id == "engineer");
    CHECK(roleInSystemPrompt(QJsonArray{"You are an interactive agent…"}).id.isEmpty());
    // Before ids were written: a built-in's English name alone.
    CHECK(roleInSystemPrompt(QJsonArray{"# Your role: Engineer\nYou are…"}).id == "engineer");
    CHECK(roleInSystemPrompt(QJsonArray{"# Your role: Astronaut\nFly."}).id.isEmpty());
    // An added teammate, with spaces in its name.
    const RoleMark added =
        roleInSystemPrompt(QJsonArray{"# Your role: Data analyst (msga: data-analyst)\nDig."});
    CHECK(added.id == "data-analyst");
    CHECK(added.name == "Data analyst");

    // In a transcript: the prompt_snapshot attachment Claude Code records.
    const QByteArray snapshot = line(
        {{"type", "attachment"},
         {"timestamp", "2026-09-25T10:00:02.000Z"},
         {"attachment", QJsonObject{{"type", "prompt_snapshot"}, {"systemPrompt", withRole}}}}
    );
    TranscriptParser p;
    p.feed(prompt("hello", "2026-09-25T10:00:01.000Z") + snapshot);
    CHECK(p.role() == "engineer");
    TranscriptParser plain;
    plain.feed(prompt("hello", "2026-09-25T10:00:01.000Z"));
    CHECK(plain.role().isEmpty());

    // Raw bytes, as "Find a session" reads a transcript's ends — even cut off
    // right after the header line.
    CHECK(roleInTranscriptBytes(snapshot).id == "engineer");
    const qsizetype at = snapshot.indexOf("engineer)\\n");
    CHECK(roleInTranscriptBytes(snapshot.left(at + 11)).id == "engineer");
    CHECK(roleInTranscriptBytes(snapshot.left(at + 5)).id.isEmpty()); // the line isn't whole
    CHECK(roleInTranscriptBytes(prompt("# Your role: Engineer", "2026-09-25T10:00:01.000Z"))
              .id.isEmpty()); // typed, not a part of the system prompt

    CatalogEntry e;
    REQUIRE(catalogEntryFrom(prompt("hello", "2026-09-25T10:00:01.000Z") + snapshot, "\n", e));
    CHECK(e.role == "engineer");
}

TEST_CASE("teammates are added, edited, restored and removed", "[claude][roles]") {
    QTemporaryDir dir;
    Team          team(dir.path());
    REQUIRE(team.listed().size() == 5);

    Role copy;
    copy.name        = "Copy writer!";
    copy.description = "Writes copy.";
    copy.glyph       = "pen-tool";
    copy.color       = QColor("#0e8c9a");
    copy.prompt      = "You write copy.";
    QString       error;
    const QString id = team.save(copy, &error);
    REQUIRE(id == "copy-writer");
    CHECK(team.find(id)->avatarUrl.endsWith("/avatars/pen-tool-0e8c9a.svg"));
    CHECK(QFileInfo::exists(QUrl(team.find(id)->avatarUrl).toLocalFile()));
    CHECK(
        appendedPrompt(*team.find(id)) ==
        "# Your role: Copy writer! (msga: copy-writer)\nYou write copy."
    );
    // A second one by the same name gets an id of its own.
    CHECK(team.save(copy, &error) == "copy-writer-2");
    CHECK(team.save(Role{}, &error).isEmpty()); // no name
    CHECK_FALSE(error.isEmpty());

    // Renaming keeps the id — the one its sessions carry.
    Role renamed = *team.find(id);
    renamed.name = "Writer";
    CHECK(team.save(renamed, &error) == id);
    CHECK(appendedPrompt(*team.find(id)).startsWith("# Your role: Writer (msga: copy-writer)"));

    // A built-in, edited then restored.
    Role engineer   = *team.find("engineer");
    engineer.prompt = "Only Rust.";
    REQUIRE(team.save(engineer, &error) == "engineer");
    CHECK(team.find("engineer")->edited);
    CHECK(team.find("engineer")->avatarUrl == "qrc:/roles/engineer.svg"); // same picture
    CHECK(
        appendedPrompt(*team.find("engineer")) ==
        "# Your role: Engineer (msga: engineer)\nOnly Rust."
    );
    CHECK_FALSE(team.remove("engineer")); // built-ins stay
    CHECK(team.restore("engineer"));
    CHECK_FALSE(team.find("engineer")->edited);
    CHECK(team.find("engineer")->prompt == builtInRoles()[1].prompt);

    // The team as subagent types: every role with a prompt, under its id, with
    // that prompt — but none shadowing one of Claude Code's own types.
    Role plan;
    plan.name   = "Plan";
    plan.prompt = "You plan.";
    REQUIRE(team.save(plan, &error) == "plan");
    const QJsonObject agents =
        QJsonDocument::fromJson(subagentsJson(team.listed()).toUtf8()).object();
    CHECK(
        agents.keys() ==
        QStringList{
            "copy-writer", "copy-writer-2", "designer", "engineer", "marketer", "researcher"
        }
    );
    CHECK(agents["engineer"]["prompt"].toString() == appendedPrompt(*team.find("engineer")));
    CHECK(
        agents["copy-writer"]["description"].toString() ==
        "Writer, a teammate (mentioned as @claude:role:copy-writer). Writes copy."
    );
    CHECK(subagentsJson({builtInRoles()[0]}).isEmpty()); // the Generalist adds nothing
    team.remove("plan");

    // Removed: off the list, still known to its sessions.
    CHECK(team.remove(id));
    CHECK(team.listed().size() == 6);
    REQUIRE(team.find(id));
    CHECK(team.find(id)->removed);
    CHECK(team.resolve(id).name == "Writer");

    // All of it read back from disk.
    Team again(dir.path());
    CHECK(again.find(id)->removed);
    CHECK(again.find(id)->name == "Writer");
    CHECK(again.find("copy-writer-2")->prompt == "You write copy.");
    CHECK_FALSE(again.find("engineer")->edited);
    // Added teammates come after the built-ins, in the order they were added.
    CHECK(again.roles()[5].id == id);

    // A role nobody here knows: a former teammate, named by its sessions.
    CHECK(again.noteFormer("astronaut", "Astronaut"));
    CHECK_FALSE(again.noteFormer("astronaut", "Astronaut"));
    const Role former = again.resolve("astronaut");
    CHECK(former.former);
    CHECK(former.name == "Astronaut");
    CHECK_FALSE(former.avatarUrl.isEmpty());
    // …and its id is never given to a new teammate.
    Role astro;
    astro.name = "Astronaut";
    CHECK(again.save(astro, &error) == "astronaut-2");
}

TEST_CASE(
    "sending runs background sessions: start, then stop + resume per turn, queued",
    "[claude][backend][bg]"
) {
    FakeClaudeHome home;
    QTemporaryDir  work, untrusted;
    // No teammates left over from an earlier run.
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/claude-code/team")
        .removeRecursively();
    QDir(home.dir.path()).mkpath("jobs");
    QDir(home.dir.path()).mkpath("projects/-fake");
    // Claude Code trusts `work` (a parent folder would do too), not `untrusted`.
    {
        QFile f(home.dir.path() + "/.claude.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"projects",
                         QJsonObject{
                             {QDir(work.path()).absolutePath(),
                              QJsonObject{{"hasTrustDialogAccepted", true}}}
                         }},
                    }
        )
                    .toJson());
    }
    // Stand-in for the CLI, following what Claude Code 2.1.282 was seen doing:
    // `--bg … -- <prompt>` creates a job + worker (a real process: a stop waits
    // for it to exit) and answers, printing "backgrounded · <short>"; `stop
    // <short>` ends the worker; `--bg --resume <id> -- <prompt>` continues —
    // or, with a copy-next file present, starts a copy of the session (records
    // repeated, uuids and all) and continues there. Every call is logged to
    // calls.log.
    const QString cli = work.path() + "/claude";
    {
        QFile f(cli);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
printf '%s\n' "$*" >> "$H/calls.log" # echo would expand \n
if [ "$1" = stop ]; then
  rm -f "$H/sessions/w$2.json"
  kill $(cat "$H/wpid-$2" 2>/dev/null) 2>/dev/null
  sed -i 's/"state":"[a-z]*"/"state":"stopped"/' "$H/jobs/$2/state.json"
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
ts=$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)
u=$(cat /proc/sys/kernel/random/uuid)
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
)SH");
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
    }

    claude_code::Backend backend(Credentials{cli});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();

    // Background sessions refuse untrusted folders: said up front.
    QString error;
    backend.startAgentSession(untrusted.path(), false, {}, {}, [&](QString e) { error = e; });
    CHECK(error.contains("trust"));

    ConversationId conv;
    backend.startAgentSession(work.path(), false, {}, [&](ConversationId id) { conv = id; }, {});
    REQUIRE(conv.value.startsWith("new-")); // Claude Code picks the id on the first message
    const auto listed = collect(backend.loadConversations());
    CHECK(std::any_of(listed[0].begin(), listed[0].end(), [&](const Conversation &c) {
        return c.id == conv && c.readOnlyReason.isEmpty();
    }));

    auto sendText = [&](const char *text, bool *ok) {
        OutgoingMessage out;
        out.text = {text, {}};
        backend.sendMessage(conv, out, [ok](bool success, QString) { *ok = success; });
    };
    auto answered = [&](const QString &text) {
        return QTest::qWaitFor(
            [&] {
                for (const auto &e : events)
                    if (const auto *n = std::get_if<EvMessageNew>(&e);
                        n && n->conv == conv && n->msg.text.text == text)
                        return true;
                return false;
            },
            8000
        );
    };

    bool ok1 = false, ok2 = false, ok3 = false;
    sendText("first", &ok1);
    REQUIRE(answered("echo first"));
    CHECK(ok1);
    // Two more at once: the second waits for its turn, one prompt per turn.
    sendText("second", &ok2);
    sendText("third", &ok3);
    // The waiting one is a message of its own at once, and stays one across a
    // reload of the chat — the Session's optimistic copy wouldn't.
    CHECK(ok3);
    const auto ownTexts = [&] {
        QStringList texts;
        const auto  pages = collect(backend.loadHistory(conv, std::nullopt));
        for (const auto &m : pages[0].messages)
            if (m.author.value == "me")
                texts << m.text.text;
        return texts;
    };
    CHECK(ownTexts() == QStringList{"first", "second", "third"});
    REQUIRE(answered("echo second"));
    REQUIRE(answered("echo third"));
    CHECK(ok2);
    // …and gives way to its prompt in the transcript: no doubles.
    CHECK(ownTexts() == QStringList{"first", "second", "third"});

    QFile log(home.dir.path() + "/calls.log");
    REQUIRE(log.open(QIODevice::ReadOnly));
    const QStringList calls = QString::fromUtf8(log.readAll()).split('\n', Qt::SkipEmptyParts);
    const QString     sid   = "abcdef11-0000-4000-8000-000000000001";
    REQUIRE(calls.size() == 5);
    // A new session gets the team as subagent types, so "use @Engineer" works.
    CHECK(
        calls[0] == "--bg --disallowedTools AskUserQuestion --agents " +
                        subagentsJson(builtInRoles()) + " -- first"
    );
    CHECK(calls[1] == "stop abcdef11");                       // the worker idles on after a turn
    CHECK(calls[2] == "--bg --resume " + sid + " -- second"); // no flags: keeps its options
    CHECK(calls[3] == "stop abcdef11");
    CHECK(calls[4] == "--bg --resume " + sid + " -- third");
    log.close();

    // A message still waiting for its turn can be taken back.
    bool ok4 = false, ok5 = false;
    sendText("fourth", &ok4);
    sendText("fifth", &ok5);
    Ts fifth;
    for (const auto &e : events)
        if (const auto *n = std::get_if<EvMessageNew>(&e); n && n->msg.text.text == "fifth")
            fifth = n->msg.ts;
    REQUIRE_FALSE(fifth.isEmpty());
    REQUIRE(backend.canDeleteMessage(conv, fifth));
    backend.deleteMessage(conv, fifth);
    CHECK(std::any_of(events.begin(), events.end(), [&](const Event &e) {
        const auto *d = std::get_if<EvMessageDeleted>(&e);
        return d && d->ts == fifth;
    }));
    REQUIRE(answered("echo fourth"));
    QTest::qWait(500); // a turn for fifth would have started by now
    REQUIRE(log.open(QIODevice::ReadOnly));
    CHECK_FALSE(log.readAll().contains("fifth"));
    log.close();
    CHECK_FALSE(ownTexts().contains("fifth"));

    // One conversation for it, under its "+" id; the name comes from Claude Code.
    const auto after = collect(backend.loadConversations());
    CHECK(std::count_if(after[0].begin(), after[0].end(), [&](const Conversation &c) {
              return c.name == "fake-abcdef11";
          }) == 1);

    // Claude Code started a copy after all (a resume racing the old worker's
    // exit, seen 2026-09-25): the chat goes on in it — no failure, no history
    // twice, no second session or thread — and the next message resumes the copy.
    {
        QFile f(home.dir.path() + "/copy-next");
        REQUIRE(f.open(QIODevice::WriteOnly));
    }
    bool ok6 = false, ok7 = false;
    sendText("sixth", &ok6);
    REQUIRE(answered("echo sixth"));
    CHECK(ok6);
    CHECK_FALSE(std::any_of(events.begin(), events.end(), [&](const Event &e) {
        return std::holds_alternative<EvSendFailed>(e);
    }));
    CHECK(ownTexts() == QStringList{"first", "second", "third", "fourth", "sixth"});
    const auto copied = collect(backend.loadConversations());
    CHECK(std::count_if(copied[0].begin(), copied[0].end(), [&](const Conversation &c) {
              return c.name.startsWith("fake-abcdef1");
          }) == 1);
    sendText("seventh", &ok7);
    REQUIRE(answered("echo seventh"));
    REQUIRE(log.open(QIODevice::ReadOnly));
    const QString copyLog = QString::fromUtf8(log.readAll());
    log.close();
    CHECK(copyLog.contains("--bg --resume abcdef12-0000-4000-8000-000000000002 -- seventh"));
    CHECK(copyLog.contains("stop abcdef11\n")); // the original is stopped, not left idling

    // Skipping permission checks is a start option, saved with the session.
    ConversationId noChecks;
    backend.startAgentSession(work.path(), true, {}, [&](ConversationId id) { noChecks = id; }, {});
    OutgoingMessage out;
    out.text = {"go", {}};
    backend.sendMessage(noChecks, out, {});
    REQUIRE(
        QTest::qWaitFor(
            [&] {
                QFile f(home.dir.path() + "/calls.log");
                return f.open(QIODevice::ReadOnly) &&
                       f.readAll().contains(
                           "--dangerously-skip-permissions --agents " +
                           subagentsJson(builtInRoles()).toUtf8() + " -- go"
                       );
            },
            8000
        )
    );
    // Let that launch finish before the backend goes away with it.
    CHECK(
        QTest::qWaitFor(
            [&] {
                for (const auto &e : events)
                    if (const auto *n = std::get_if<EvMessageNew>(&e);
                        n && n->conv == noChecks && n->msg.text.text == "echo go")
                        return true;
                return false;
            },
            8000
        )
    );
    // Claude gets the text as typed, not the parsed copy the fences and
    // backticks were stripped from.
    OutgoingMessage fenced;
    fenced.text         = {"quotes test code", {}};
    fenced.rawText      = "quotes ```test``` `code`";
    fenced.composerText = "quotes ```test``` `code`";
    backend.sendMessage(noChecks, fenced, {});
    CHECK(
        QTest::qWaitFor(
            [&] {
                QFile f(home.dir.path() + "/calls.log");
                return f.open(QIODevice::ReadOnly) &&
                       f.readAll().contains("quotes ```test``` `code`");
            },
            8000
        )
    );

    // A teammate: its few lines go after Claude Code's own prompt, its session
    // carries its role and picture, and Claude answers as the teammate.
    ConversationId engineer;
    backend.startAgentSession(
        work.path(), false, "engineer", [&](ConversationId id) { engineer = id; }, {}
    );
    REQUIRE_FALSE(engineer.value.isEmpty());
    const auto convs = collect(backend.loadConversations());
    CHECK(std::any_of(convs[0].begin(), convs[0].end(), [&](const Conversation &c) {
        return c.id == engineer && c.agentRole == "engineer";
    }));
    const auto users = collect(backend.loadUsers());
    CHECK(std::any_of(users[0].begin(), users[0].end(), [&](const User &u) {
        return u.id.value == "claude:" + engineer.value && u.avatarUrl == "qrc:/roles/engineer.svg";
    }));
    CHECK(std::any_of(users[0].begin(), users[0].end(), [](const User &u) {
        return u.id.value == "claude:role:engineer" && u.name == "Engineer";
    }));
    out.text = {"hi", {}};
    backend.sendMessage(engineer, out, {});
    CHECK(
        QTest::qWaitFor(
            [&] {
                for (const auto &e : events)
                    if (const auto *n = std::get_if<EvMessageNew>(&e);
                        n && n->conv == engineer && n->msg.text.text == "echo hi")
                        return n->msg.author == UserId{"claude:role:engineer"};
                return false;
            },
            8000
        )
    );
    QFile calls2(home.dir.path() + "/calls.log");
    REQUIRE(calls2.open(QIODevice::ReadOnly));
    CHECK(
        calls2.readAll().contains("--append-system-prompt # Your role: Engineer (msga: engineer)\n")
    );
    calls2.close();

    // An added teammate. Editing its instructions reaches new sessions only —
    // one already started goes on without flags, keeping what it began with —
    // and removing it leaves its session its name and picture.
    auto waitFor = [&](const ConversationId &c, const QString &text) {
        return QTest::qWaitFor(
            [&] {
                for (const auto &e : events)
                    if (const auto *n = std::get_if<EvMessageNew>(&e);
                        n && n->conv == c && n->msg.text.text == text)
                        return true;
                return false;
            },
            8000
        );
    };
    AgentRole copy;
    copy.name   = "Copywriter";
    copy.glyph  = "pen-tool";
    copy.color  = "#0e8c9a";
    copy.prompt = "Write copy v1.";
    QString err;
    REQUIRE(backend.saveAgentRole(copy, &err) == "copywriter");
    ConversationId cw;
    backend.startAgentSession(
        work.path(), false, "copywriter", [&](ConversationId id) { cw = id; }, {}
    );
    out.text = {"tagline", {}};
    backend.sendMessage(cw, out, {});
    REQUIRE(waitFor(cw, "echo tagline"));
    for (const AgentRole &r : backend.agentRoles())
        if (r.id == "copywriter")
            copy = r;
    copy.prompt = "Write copy v2.";
    REQUIRE(backend.saveAgentRole(copy, &err) == "copywriter");
    out.text = {"again", {}};
    backend.sendMessage(cw, out, {});
    REQUIRE(waitFor(cw, "echo again"));
    ConversationId cw2;
    backend.startAgentSession(
        work.path(), false, "copywriter", [&](ConversationId id) { cw2 = id; }, {}
    );
    out.text = {"fresh", {}};
    backend.sendMessage(cw2, out, {});
    REQUIRE(waitFor(cw2, "echo fresh"));
    QFile calls3(home.dir.path() + "/calls.log");
    REQUIRE(calls3.open(QIODevice::ReadOnly));
    const QString     calls3Text = QString::fromUtf8(calls3.readAll());
    // Both its prompt and its subagent type carry the text: count launches.
    // (A call's log entry runs over lines where its prompt does.)
    const QStringList calls3List = calls3Text.split("\n--bg");
    auto              launches   = [&](const QString &text) {
        return std::count_if(calls3List.begin(), calls3List.end(), [&](const QString &call) {
            return call.contains(text);
        });
    };
    CHECK(launches("Write copy v1.") == 1);
    CHECK(launches("Write copy v2.") == 1);
    CHECK(calls3Text.contains(R"("copywriter":{"description":"Copywriter, a teammate)"));
    CHECK(
        calls3Text.indexOf("Write copy v2.") > calls3Text.indexOf(" -- again")
    ); // only the new session
    for (const QString &l : calls3Text.split('\n'))
        if (l.endsWith(" -- again"))
            CHECK(l.startsWith("--bg --resume ")); // no flags: its own prompt stays

    backend.removeAgentRole("copywriter");
    for (const AgentRole &r : backend.agentRoles())
        CHECK(r.id != "copywriter");
    const auto afterRemove = collect(backend.loadConversations());
    CHECK(std::any_of(afterRemove[0].begin(), afterRemove[0].end(), [&](const Conversation &c) {
        return c.id == cw && c.agentRole == "copywriter";
    }));
    const auto usersAfter = collect(backend.loadUsers());
    CHECK(std::any_of(usersAfter[0].begin(), usersAfter[0].end(), [&](const User &u) {
        return u.id.value == "claude:" + cw.value && u.avatarUrl.endsWith("pen-tool-0e8c9a.svg");
    }));
    CHECK(std::any_of(usersAfter[0].begin(), usersAfter[0].end(), [](const User &u) {
        return u.id.value == "claude:role:copywriter" && u.name == "Copywriter";
    }));
}

TEST_CASE("Stop cuts a session's turn short and drops what waits", "[claude][backend][bg]") {
    FakeClaudeHome home;
    home.writeSession("busy"); // S1: a terminal's session, working
    QTemporaryDir work;
    QDir(home.dir.path()).mkpath("jobs");
    QDir(home.dir.path()).mkpath("projects/-fake");
    {
        QFile f(home.dir.path() + "/.claude.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"projects",
                         QJsonObject{
                             {QDir(work.path()).absolutePath(),
                              QJsonObject{{"hasTrustDialogAccepted", true}}}
                         }},
                    }
        )
                    .toJson());
    }
    qputenv("FAKE_WORKER_PID", QByteArray::number(QCoreApplication::applicationPid()));
    // Every turn this CLI starts goes on until it is stopped.
    const QString cli = work.path() + "/claude";
    {
        QFile f(cli);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(R"SH(#!/bin/sh
H="$CLAUDE_CONFIG_DIR"
printf '%s\n' "$*" >> "$H/calls.log" # echo would expand \n
if [ "$1" = stop ]; then
  # The worker writes its last records as it exits.
  T=$(sed -n 's/.*"linkScanPath":"\([^"]*\)".*/\1/p' "$H/jobs/$2/state.json")
  echo '{"type":"last-prompt","lastPrompt":"x"}' >> "$T"
  rm -f "$H/sessions/w$2.json"
  sed -i 's/"state":"[a-z]*"/"state":"stopped"/' "$H/jobs/$2/state.json"
  echo "stopped $2"; exit 0
fi
sid=""; prompt=""
while [ $# -gt 0 ]; do
  case "$1" in
    --resume) sid="$2"; shift ;;
    --) prompt="$2"; shift ;;
  esac; shift
done
[ -z "$sid" ] && sid="abcdef11-0000-4000-8000-000000000001"
short=$(echo "$sid" | cut -c1-8)
T="$H/projects/-fake/$sid.jsonl"
ts=$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)
mkdir -p "$H/jobs/$short"
echo "{\"state\":\"working\",\"sessionId\":\"$sid\",\"cwd\":\"$PWD\",\"name\":\"fake-$short\",\"linkScanPath\":\"$T\"}" > "$H/jobs/$short/state.json"
echo "{\"pid\":$FAKE_WORKER_PID,\"sessionId\":\"$sid\",\"kind\":\"bg\",\"status\":\"busy\"}" > "$H/sessions/w$short.json"
echo "{\"type\":\"user\",\"timestamp\":\"$ts\",\"origin\":{\"kind\":\"human\"},\"message\":{\"content\":\"$prompt\"}}" >> "$T"
echo "backgrounded · $short"
)SH");
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
    }

    claude_code::Backend backend(Credentials{cli});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();

    // A terminal's session is the terminal's to stop.
    CHECK_FALSE(backend.canStopAgentSession(ConversationId{"S1"}));

    ConversationId conv;
    backend.startAgentSession(work.path(), false, {}, [&](ConversationId id) { conv = id; }, {});
    REQUIRE_FALSE(conv.value.isEmpty());
    CHECK_FALSE(backend.canStopAgentSession(conv)); // nothing sent yet

    const QString sid   = "abcdef11-0000-4000-8000-000000000001";
    auto          calls = [&] {
        QFile f(home.dir.path() + "/calls.log");
        return f.open(QIODevice::ReadOnly)
                   ? QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts)
                   : QStringList{};
    };
    auto send = [&](const char *text) {
        OutgoingMessage out;
        out.text = {text, {}};
        backend.sendMessage(conv, out, {});
    };
    auto ownTexts = [&] {
        QStringList texts;
        const auto  pages = collect(backend.loadHistory(conv, std::nullopt));
        for (const auto &m : pages[0].messages)
            if (m.author.value == "me")
                texts << m.text.text;
        return texts;
    };
    const UserId assistant{"claude:" + conv.value};
    auto         working = [&] {
        const auto presence = collect(backend.loadPresence(assistant)); // vector<bool>: bind it
        return bool(presence[0]);
    };

    send("first");
    // Claude is on it: the prompt is in and the worker reads busy.
    REQUIRE(QTest::qWaitFor([&] { return ownTexts() == QStringList{"first"}; }, 8000));
    QTest::qWait(300); // the launcher has reported back
    send("second");    // waits for the turn to end
    CHECK(ownTexts() == QStringList{"first", "second"});
    CHECK(working());
    REQUIRE(backend.canStopAgentSession(conv));

    backend.stopAgentSession(conv);
    CHECK_FALSE(working()); // at once
    CHECK_FALSE(backend.canStopAgentSession(conv));
    CHECK(ownTexts() == QStringList{"first"}); // the waiting one is dropped
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 2; }, 8000));
    CHECK(calls()[1] == "stop abcdef11");
    QTest::qWait(1000); // stopped, and nothing more goes out
    CHECK(calls().size() == 2);
    CHECK_FALSE(working());
    CHECK(std::none_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvSendFailed>(e);
    }));

    // The next message continues it: the worker is gone, so no stop first.
    send("third");
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 3; }, 8000));
    CHECK(calls()[2] == "--bg --resume " + sid + " -- third");
    // Stopped while the CLI is still starting the turn: stopped once it has.
    backend.stopAgentSession(conv);
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 4; }, 8000));
    CHECK(calls()[3] == "stop abcdef11");
    QTest::qWait(1000);
    CHECK_FALSE(working());
    CHECK_FALSE(backend.canStopAgentSession(conv));

    // An idle worker can be stopped too: a subagent's result or a scheduled
    // prompt would wake it without anyone sending a thing.
    send("fourth");
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 5; }, 8000));
    const QString workerFile = home.dir.path() + "/sessions/wabcdef11.json";
    REQUIRE(QTest::qWaitFor([&] { return QFile::exists(workerFile); }, 8000));
    QTest::qWait(300); // the launcher has reported back
    {
        QFile f(workerFile);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QString(R"({"pid":%1,"sessionId":"%2","kind":"bg","status":"idle"})")
                    .arg(QCoreApplication::applicationPid())
                    .arg(sid)
                    .toUtf8());
        // …and the turn is over.
        QFile job(home.dir.path() + "/jobs/abcdef11/state.json");
        REQUIRE(job.open(QIODevice::ReadOnly));
        const QByteArray state = job.readAll().replace("\"working\"", "\"done\"");
        job.close();
        REQUIRE(job.open(QIODevice::WriteOnly | QIODevice::Truncate));
        job.write(state);
        QFile t(home.dir.path() + "/projects/-fake/" + sid + ".jsonl");
        REQUIRE(t.open(QIODevice::Append));
        t.write("{\"type\":\"system\",\"subtype\":\"turn_duration\"}\n");
    }
    REQUIRE(QTest::qWaitFor([&] { return !working(); }, 8000));
    CHECK(backend.canStopAgentSession(conv));

#if defined(Q_OS_LINUX)
    // What the session left running outside its worker is ended with it —
    // only that: a process that merely inherited the session id is no leftover.
    auto spawn = [&](bool inJob, qint64 *pid) {
        QProcess            p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("CLAUDE_CODE_SESSION_ID", sid);
        if (inJob)
            env.insert("CLAUDE_JOB_DIR", home.dir.path() + "/jobs/abcdef11");
        p.setProcessEnvironment(env);
        p.setProgram("sleep");
        p.setArguments({"30"});
        return p.startDetached(pid); // not msga's child: those are spared
    };
    qint64 leftover = 0, bystander = 0;
    REQUIRE(spawn(true, &leftover));
    REQUIRE(spawn(false, &bystander));
    REQUIRE(
        QTest::qWaitFor(
            [&] { return leftoverProcesses(sid, "abcdef11") == std::vector<qint64>{leftover}; },
            3000
        )
    );
#endif
    backend.stopAgentSession(conv);
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 6; }, 8000));
    CHECK(calls()[5] == "stop abcdef11");
#if defined(Q_OS_LINUX)
    CHECK(QTest::qWaitFor([&] { return !isProcessAlive(leftover); }, 5000));
    CHECK(isProcessAlive(bystander));
    signalProcess(bystander, true);
#endif

    // "Remove from msga" stops a live worker too, and the session stays away
    // though the worker still writes to its transcript as it goes.
    send("fifth");
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 7; }, 8000));
    REQUIRE(QTest::qWaitFor([&] { return working(); }, 8000));
    QTest::qWait(300); // the launcher has reported back
    backend.leaveConversation(conv);
    REQUIRE(QTest::qWaitFor([&] { return calls().size() == 8; }, 8000));
    CHECK(calls()[7] == "stop abcdef11");
    QTest::qWait(1500);
    const auto convs = collect(backend.loadConversations());
    CHECK(std::none_of(convs[0].begin(), convs[0].end(), [&](const Conversation &c) {
        return c.id == conv;
    }));
    CHECK(calls().size() == 8);
}
#endif

TEST_CASE("a branched-off session is a thread in its parent", "[claude][backend][thread]") {
    FakeClaudeHome home;
    home.writeSession("idle");
    const QByteArray shared = prompt("hi", "2026-09-25T10:00:00.000Z") +
                              assistantText("Hello!", "2026-09-25T10:00:01.000Z") +
                              turnEnd("2026-09-25T10:00:01.500Z");
    home.append(shared);
    QTest::qWait(20); // the fork's file is the younger one (creation times in ms)
    // The fork: a copy of the parent's records so far, then its own question.
    {
        QFile f(home.dir.path() + "/projects/-src-app/S2.jsonl");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(
            shared + prompt("side question?", "2026-09-25T10:00:03.000Z") +
            assistantText("Side answer.", "2026-09-25T10:00:04.000Z") +
            turnEnd("2026-09-25T10:00:04.500Z")
        );
        QFile s(home.dir.path() + "/sessions/2.json");
        REQUIRE(s.open(QIODevice::WriteOnly));
        s.write(QJsonDocument(
                    QJsonObject{
                        {"pid", QCoreApplication::applicationPid()},
                        {"sessionId", "S2"},
                        {"cwd", "/src/app"},
                        {"status", "idle"},
                        {"entrypoint", "cli"},
                    }
        )
                    .toJson());
    }
    // The parent went on after the branch.
    home.append(
        prompt("more", "2026-09-25T10:00:05.000Z") +
        assistantText("Sure.", "2026-09-25T10:00:06.000Z") + turnEnd("2026-09-25T10:00:06.500Z")
    );

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const auto convs = collect(backend.loadConversations());
    REQUIRE(convs.size() == 1);
    REQUIRE(convs[0][0].id.value == "S1");
    CHECK(convs[0].size() == 1); // the branch isn't a session in the list

    const ConversationId conv{"S1"};
    const auto           page = collect(backend.loadHistory(conv, std::nullopt));
    REQUIRE(page.size() == 1);
    const auto &msgs = page[0].messages;
    REQUIRE(msgs.size() == 5);
    CHECK(msgs[2].text.text == "side question?"); // in time order, between the turns
    CHECK(msgs[2].author == UserId{"me"});
    CHECK(msgs[2].replyCount == 1);
    CHECK(msgs[3].text.text == "more");
    const Ts root = msgs[2].ts;
    CHECK(backend.threadAcceptsReplies(conv, root));
    CHECK_FALSE(backend.threadAcceptsReplies(conv, msgs[0].ts));

    const auto thread = collect(backend.loadThread(conv, root, std::nullopt));
    REQUIRE(thread.size() == 1);
    REQUIRE(thread[0].messages.size() == 2);
    CHECK(thread[0].messages[0].ts == root);
    CHECK(thread[0].messages[1].text.text == "Side answer.");
    CHECK(thread[0].messages[1].threadRoot == root);
    CHECK(thread[0].messages[1].parentUserId == UserId{"me"}); // notifies as a reply to you

    // "Open as session": the branch joins the list, its root leaves the parent.
    CHECK(backend.openThreadAsSession(conv, root).value == "S2");
    const auto after = collect(backend.loadConversations());
    REQUIRE(after.size() == 1);
    CHECK(after[0].size() == 2);
    const auto parent = collect(backend.loadHistory(conv, std::nullopt));
    REQUIRE(parent.size() == 1);
    CHECK(parent[0].messages.size() == 4);
}

TEST_CASE("/btw is offered with Claude Code's commands", "[claude][commands]") {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(prompt("hi", "2026-09-25T10:00:00.000Z"));
    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const auto cmds = backend.conversationCommands(ConversationId{"S1"});
    REQUIRE_FALSE(cmds.empty());
    CHECK(cmds[0].name == "btw");
    const auto local = [&](const QString &name) {
        return std::any_of(cmds.begin(), cmds.end(), [&](const SlashCommand &c) {
            return c.name == name && c.local;
        });
    };
    CHECK(local("status")); // run by msga itself, never sent to Claude
    CHECK(local("clear"));

    const ConversationId conv{"S1"};
    const auto           status = backend.runLocalCommand(conv, "status", {});
    REQUIRE_FALSE(status.status.empty());
    CHECK(std::any_of(status.status.begin(), status.status.end(), [](const auto &row) {
        return row.second == "S1";
    }));
    // /clear needs the claude tool (none in this test): it says so, opens nothing.
    const auto clear = backend.runLocalCommand(conv, "clear", {});
    CHECK_FALSE(clear.error.isEmpty());
    CHECK(clear.open.value.isEmpty());
}

TEST_CASE(
    "a subagent run is a thread holding the subagent's transcript", "[claude][backend][thread]"
) {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(
        prompt("research it", "2026-09-25T10:00:00.000Z") +
        toolUse("a1", "Agent", {{"description", "Read the docs"}}, "2026-09-25T10:00:01.000Z") +
        toolResult("a1", "2026-09-25T10:00:09.000Z", false, "agent42") +
        assistantText("Done.", "2026-09-25T10:00:10.000Z") + turnEnd("2026-09-25T10:00:11.000Z")
    );
    QDir().mkpath(home.dir.path() + "/projects/-src-app/S1/subagents");
    {
        QFile f(home.dir.path() + "/projects/-src-app/S1/subagents/agent-agent42.jsonl");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(
            prompt("Read the docs and report", "2026-09-25T10:00:02.000Z", false) +
            assistantText("The docs say X.", "2026-09-25T10:00:08.000Z") +
            turnEnd("2026-09-25T10:00:08.500Z")
        );
    }

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    const ConversationId conv{"S1"};
    const auto           page = collect(backend.loadHistory(conv, std::nullopt));
    REQUIRE(page.size() == 1);
    const auto root =
        std::find_if(page[0].messages.begin(), page[0].messages.end(), [](const Message &m) {
            return m.replyCount > 0;
        });
    REQUIRE(root != page[0].messages.end());
    CHECK(root->replyCount == 2);
    CHECK(root->text.text == "Subagent: Read the docs");

    const auto thread = collect(backend.loadThread(conv, root->ts, std::nullopt));
    REQUIRE(thread.size() == 1);
    REQUIRE(thread[0].messages.size() == 3); // root + prompt + answer
    CHECK(thread[0].messages[0].ts == root->ts);
    CHECK(thread[0].messages[2].text.text == "The docs say X.");
    CHECK(thread[0].messages[2].threadRoot == root->ts);
    CHECK(backend.capabilities().agentSessions); // → the thread composer is hidden
}

// ── Deleting messages ─────────────────────────────────────────────────────────

namespace {

// A record as Claude Code links them: uuid, and the record before it.
QByteArray linked(QByteArray rec, const char *uuid, const char *parent) {
    QJsonObject o   = QJsonDocument::fromJson(rec).object();
    o["uuid"]       = uuid;
    o["parentUuid"] = parent ? QJsonValue(parent) : QJsonValue();
    return line(o);
}

// One block of an assistant message (Claude Code writes each as a record).
QByteArray assistantBlock(const QJsonObject &block, const char *msgId, const char *ts) {
    return line({
        {"type", "assistant"},
        {"timestamp", ts},
        {"message", QJsonObject{{"id", msgId}, {"content", QJsonArray{block}}}},
    });
}

QByteArray thinking(const char *msgId, const char *ts) {
    return assistantBlock({{"type", "thinking"}, {"thinking", ""}, {"signature", "x"}}, msgId, ts);
}

QByteArray answer(const QString &text, const char *msgId, const char *ts) {
    return assistantBlock({{"type", "text"}, {"text", text}}, msgId, ts);
}

// Two turns: the second prompt thinks, calls a tool, thinks again, answers.
QByteArray linkedTurns() {
    return linked(prompt("keep this", "2026-09-25T10:00:00.000Z"), "p1", nullptr) +
           linked(thinking("m1", "2026-09-25T10:00:01.000Z"), "t1", "p1") +
           linked(answer("Kept.", "m1", "2026-09-25T10:00:02.000Z"), "a1", "t1") +
           linked(turnEnd("2026-09-25T10:00:03.000Z"), "e1", "a1") +
           linked(prompt("the secret is banana", "2026-09-25T10:01:00.000Z"), "p2", "e1") +
           linked(thinking("m2", "2026-09-25T10:01:01.000Z"), "t2", "p2") +
           linked(
               toolUse("tu", "Bash", {{"command", "ls"}}, "2026-09-25T10:01:02.000Z"), "u2", "t2"
           ) +
           linked(toolResult("tu", "2026-09-25T10:01:03.000Z"), "r2", "u2") +
           linked(thinking("m3", "2026-09-25T10:01:04.000Z"), "t3", "r2") +
           linked(answer("Noted.", "m3", "2026-09-25T10:01:05.000Z"), "a3", "t3") +
           linked(turnEnd("2026-09-25T10:01:06.000Z"), "e2", "a3") +
           R"({"type":"last-prompt","leafUuid":"e2"})"
           "\n" +
           // Claude Code's bookkeeping copies of the second prompt.
           R"({"type":"queue-operation","operation":"enqueue","content":"the secret is banana"})"
           "\n"
           R"({"type":"last-prompt","lastPrompt":"the secret is banana","leafUuid":"e2"})"
           "\n";
}

// uuid → parentUuid of every record in the file ("-" for none), in file order.
QList<std::pair<QString, QString>> chain(const QString &path) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    QList<std::pair<QString, QString>> out;
    for (const QByteArray &l : f.readAll().split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(l).object();
        if (o.contains("uuid"))
            out.append({o["uuid"].toString(), o["parentUuid"].toString("-")});
    }
    return out;
}

} // namespace

TEST_CASE("a prompt leaves the transcript with its turn's thinking", "[claude][delete]") {
    QTemporaryDir dir;
    const QString path = dir.path() + "/s.jsonl";
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(linkedTurns());
    }
    TranscriptParser before;
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::ReadOnly));
        before.feed(f.readAll());
    }
    REQUIRE(before.items().size() == 5); // prompt, answer, prompt, tool calls, answer
    CHECK(before.items()[2].uuid == "p2");
    CHECK(before.items()[3].uuid.isEmpty()); // tool calls can't go on their own
    CHECK(before.items()[4].uuid == "a3");

    REQUIRE(removeFromTranscript(path, "p2"));
    // The prompt and both thoughts of its turn are gone; the tool call and its
    // result stay, and what followed a removed record now follows its parent.
    CHECK(
        chain(path) == QList<std::pair<QString, QString>>{
                           {"p1", "-"},
                           {"t1", "p1"},
                           {"a1", "t1"},
                           {"e1", "a1"},
                           {"u2", "e1"},
                           {"r2", "u2"},
                           {"a3", "r2"},
                           {"e2", "a3"},
                       }
    );
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray after = f.readAll();
    CHECK_FALSE(after.contains("banana"));
    // Records that needed no new link are kept byte for byte.
    CHECK(after.startsWith(linked(prompt("keep this", "2026-09-25T10:00:00.000Z"), "p1", nullptr)));
    CHECK(after.endsWith(
        R"({"type":"last-prompt","leafUuid":"e2"})"
        "\n"
    )); // its copies are gone
    f.close();

    CHECK_FALSE(removeFromTranscript(path, "p2")); // no longer there
}

TEST_CASE("an answer leaves the transcript with its own thinking", "[claude][delete]") {
    QTemporaryDir dir;
    const QString path = dir.path() + "/s.jsonl";
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(linkedTurns().replace(R"("leafUuid":"e2")", R"("leafUuid":"a1")"));
    }
    REQUIRE(removeFromTranscript(path, "a1"));
    const auto c = chain(path);
    REQUIRE(c.size() == 9);
    CHECK(c[0] == std::pair<QString, QString>{"p1", "-"});
    CHECK(c[1] == std::pair<QString, QString>{"e1", "p1"}); // t1 and a1 are gone
    CHECK(c[3] == std::pair<QString, QString>{"t2", "p2"}); // the next turn's thinking stays
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    CHECK(f.readAll().contains(R"("leafUuid":"p1")")); // pointers along the chain move too
}

TEST_CASE("deleting a message in a session", "[claude][backend][delete]") {
    FakeClaudeHome home;
    // A finished background session nothing is writing to…
    const QString  sid          = "8d953db6-f3be-4b02-8f0f-09aea0343b3e";
    const QString  bgTranscript = home.dir.path() + "/projects/-src-app/" + sid + ".jsonl";
    QDir(home.dir.path()).mkpath("jobs/8d953db6");
    {
        QFile f(home.dir.path() + "/jobs/8d953db6/state.json");
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(
                    QJsonObject{
                        {"state", "done"},
                        {"sessionId", sid},
                        {"cwd", "/src/app"},
                        {"name", "finished"},
                        {"linkScanPath", bgTranscript},
                    }
        )
                    .toJson());
        QFile t(bgTranscript);
        REQUIRE(t.open(QIODevice::WriteOnly));
        t.write(linkedTurns());
    }
    // …and one open in a terminal, which keeps what it has said.
    home.writeSession("idle");
    home.append(
        linked(prompt("hi", "2026-09-25T09:00:00.000Z"), "q1", nullptr) +
        linked(answer("Hello!", "m9", "2026-09-25T09:00:01.000Z"), "q2", "q1") +
        linked(turnEnd("2026-09-25T09:00:02.000Z"), "q3", "q2")
    );

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    REQUIRE(collect(backend.loadConversations())[0].size() == 2);
    CHECK(backend.capabilities().deleteMessage);
    CHECK(backend.capabilities().deleteAnyMessage); // Claude's answers too

    const ConversationId term{"S1"};
    const auto           termMsgs = collect(backend.loadHistory(term, std::nullopt))[0].messages;
    REQUIRE(termMsgs.size() == 2);
    CHECK_FALSE(backend.canDeleteMessage(term, termMsgs[0].ts));

    const ConversationId conv{sid};
    const auto           msgs = collect(backend.loadHistory(conv, std::nullopt))[0].messages;
    REQUIRE(msgs.size() == 5);
    CHECK(backend.canDeleteMessage(conv, msgs[2].ts));       // a prompt
    CHECK(backend.canDeleteMessage(conv, msgs[4].ts));       // an answer
    CHECK_FALSE(backend.canDeleteMessage(conv, msgs[3].ts)); // tool calls
    CHECK_FALSE(backend.canDeleteMessage(conv, "1.000001"));

    events.clear();
    backend.deleteMessage(conv, msgs[2].ts);
    CHECK(std::any_of(events.begin(), events.end(), [&](const Event &e) {
        const auto *d = std::get_if<EvMessageDeleted>(&e);
        return d && d->conv == conv && d->ts == msgs[2].ts;
    }));
    const auto left = collect(backend.loadHistory(conv, std::nullopt))[0].messages;
    REQUIRE(left.size() == 4);
    CHECK(left[1].text.text == "Kept.");
    CHECK(left[3].text.text == "Noted.");
    QFile f(bgTranscript);
    REQUIRE(f.open(QIODevice::ReadOnly));
    CHECK_FALSE(f.readAll().contains("banana"));
}

// ── Finding sessions ("Find a session") ───────────────────────────────────────

namespace {

QByteArray titled(const char *type, const char *key, const QString &value) {
    return line({{"type", type}, {key, value}});
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

} // namespace

TEST_CASE("the catalog reads a transcript's title and prompts", "[claude][catalog]") {
    const QByteArray head =
        line(
            {{"type", "user"},
             {"cwd", "/src/app"},
             {"timestamp", "2026-09-25T10:00:00.000Z"},
             {"message",
              QJsonObject{{"content", "<local-command-caveat>x</local-command-caveat>"}}}}
        ) +
        prompt("fix   the\nbuild", "2026-09-25T10:00:01.000Z") +
        titled("ai-title", "aiTitle", "Build fix");
    CatalogEntry e;
    REQUIRE(catalogEntryFrom(head, "\n" + head, e));
    CHECK(e.cwd == "/src/app");
    CHECK(e.firstPrompt == "fix the build"); // one line; the caveat isn't a prompt
    CHECK(e.lastPrompt == "fix the build");
    CHECK(e.title == "Build fix");

    // The tail: a cut first line, the latest title (a /rename wins) and prompt.
    const QByteArray tail = R"(t":"cut off"})"
                            "\n" +
                            titled("custom-title", "customTitle", "My name") +
                            titled("last-prompt", "lastPrompt", "and the tests") +
                            titled("ai-title", "aiTitle", "Later title");
    CatalogEntry     e2;
    REQUIRE(catalogEntryFrom(head, tail, e2));
    CHECK(e2.title == "My name");
    CHECK(e2.firstPrompt == "fix the build");
    CHECK(e2.lastPrompt == "and the tests");

    CatalogEntry none;
    CHECK_FALSE(catalogEntryFrom(titled("ai-title", "aiTitle", "x"), "\n", none)); // no prompt
}

TEST_CASE(
    "every session is found, and one can be added to the list", "[claude][catalog][backend]"
) {
    FakeClaudeHome home;
    home.writeSession("idle");
    home.append(prompt("listed one", "2026-09-25T09:00:00.000Z"));
    // An ended session msga has never seen, in another folder, and a big one
    // whose middle is never read.
    const QString gone =
        home.dir.path() + "/projects/-src-other/aaaaaaaa-0000-0000-0000-000000000001.jsonl";
    writeFile(
        gone,
        line(
            {{"type", "user"},
             {"cwd", "/src/other"},
             {"timestamp", "2026-09-25T08:00:00.000Z"},
             {"origin", QJsonObject{{"kind", "human"}}},
             {"message", QJsonObject{{"content", "old question"}}}}
        ) + assistantText("Old answer.", "2026-09-25T08:00:01.000Z") +
            turnEnd("2026-09-25T08:00:02.000Z") + titled("ai-title", "aiTitle", "Old work")
    );
    QByteArray big = prompt("big start", "2026-09-25T07:00:00.000Z");
    while (big.size() < 400 * 1024)
        big += assistantText(QString(500, 'x'), "2026-09-25T07:00:01.000Z");
    big += titled("last-prompt", "lastPrompt", "big end");
    writeFile(
        home.dir.path() + "/projects/-src-big/bbbbbbbb-0000-0000-0000-000000000002.jsonl", big
    );

    const auto catalog = scanCatalog(home.dir.path() + "/projects");
    REQUIRE(catalog.size() == 3);
    const auto bigEntry = std::find_if(catalog.begin(), catalog.end(), [](const CatalogEntry &e) {
        return e.sessionId.startsWith("bbbbbbbb");
    });
    REQUIRE(bigEntry != catalog.end());
    CHECK(bigEntry->firstPrompt == "big start");
    CHECK(bigEntry->lastPrompt == "big end");

    claude_code::Backend backend(Credentials{});
    std::vector<Event>   events;
    rpl::lifetime        lt;
    backend.events() | rpl::on_next([&](Event e) { events.push_back(std::move(e)); }, lt);
    backend.connectRealtime();
    REQUIRE(collect(backend.loadConversations())[0].size() == 1);

    std::vector<FoundSession> found;
    bool                      answered = false;
    backend.findAgentSessions([&](std::vector<FoundSession> f) {
        found    = std::move(f);
        answered = true;
    });
    REQUIRE(QTest::qWaitFor([&] { return answered; }, 5000));
    REQUIRE(found.size() == 3);
    const auto byId = [&](const QString &prefix) {
        return *std::find_if(found.begin(), found.end(), [&](const FoundSession &f) {
            return f.id.startsWith(prefix);
        });
    };
    CHECK(byId("S1").listed == ConversationId{"S1"});
    const FoundSession old = byId("aaaaaaaa");
    CHECK(old.listed.value.isEmpty());
    CHECK(old.title == "Old work");
    CHECK(old.folder == "/src/other");

    events.clear();
    const ConversationId conv = backend.addFoundSession(old.id);
    CHECK(conv.value == old.id);
    CHECK(std::any_of(events.begin(), events.end(), [&](const Event &e) {
        const auto *c = std::get_if<EvChannelCreated>(&e);
        return c && c->conv.id == conv && c->conv.name == "Old work";
    }));
    CHECK(std::none_of(events.begin(), events.end(), [](const Event &e) {
        return std::holds_alternative<EvMessageNew>(e); // its history isn't news
    }));
    CHECK(collect(backend.loadConversations())[0].size() == 2);
    const auto history = collect(backend.loadHistory(conv, std::nullopt))[0].messages;
    REQUIRE(history.size() == 2);
    CHECK(history[1].text.text == "Old answer.");
    CHECK(backend.addFoundSession(old.id) == conv); // already there
    CHECK(backend.addFoundSession("nope").value.isEmpty());
}

TEST_CASE(
    "adding the original of a listed copy keeps both sessions", "[claude][catalog][backend]"
) {
    FakeClaudeHome   home;
    // S1 (listed) is a copy Claude Code made when an ended session was resumed:
    // the original's records, then more. The original itself isn't listed.
    const QByteArray original = prompt("/clear", "2026-09-25T08:00:00.000Z") +
                                assistantText("Cleared.", "2026-09-25T08:00:01.000Z") +
                                turnEnd("2026-09-25T08:00:02.000Z");
    const QString    origPath =
        home.dir.path() + "/projects/-src-app/cccccccc-0000-0000-0000-000000000003.jsonl";
    writeFile(origPath, original);
    QTest::qWait(20); // the copy is the younger file
    home.writeSession("idle");
    home.append(
        original + prompt("go on", "2026-09-25T09:00:00.000Z") +
        assistantText("Going.", "2026-09-25T09:00:01.000Z") + turnEnd("2026-09-25T09:00:02.000Z")
    );

    claude_code::Backend backend(Credentials{});
    backend.connectRealtime();
    REQUIRE(collect(backend.loadConversations())[0].size() == 1);

    const ConversationId conv = backend.addFoundSession("cccccccc-0000-0000-0000-000000000003");
    CHECK(conv.value == "cccccccc-0000-0000-0000-000000000003");
    const auto convs = collect(backend.loadConversations())[0];
    CHECK(convs.size() == 2); // the copy didn't turn into a thread of the original
    CHECK(collect(backend.loadHistory(ConversationId{"S1"}, std::nullopt))[0].messages.size() == 4);
}

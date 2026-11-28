// "Find a session": a transcript's title, prompts and role read from its two
// ends — against a throwaway projects folder (never the user's ~/.claude).
#include "app/claude/catalog.h"
#include "app/claude/outputs.h"
#include "app/claude/roles.h"

#include "base/file.h"
#include "base/json.h"
#include "base/str.h"
#include "support/test.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <sys/time.h>
#include <unistd.h>

using namespace claude;

namespace {

std::string tempDir() {
    return base::test::makeTempDir("claude_catalog_test_");
}

// A user record as Claude Code writes one.
std::string prompt(std::string_view text, std::string_view ts, std::string_view cwd = {}) {
    json::Writer w;
    w.beginObject();
    w.key("type").value("user");
    if (!cwd.empty())
        w.key("cwd").value(cwd);
    w.key("timestamp").value(ts);
    w.key("message").beginObject();
    w.key("role").value("user");
    w.key("content").value(text);
    w.endObject();
    w.key("origin").beginObject().key("kind").value("human").endObject();
    w.endObject();
    return w.take() + "\n";
}

std::string assistantText(std::string_view text, std::string_view ts) {
    json::Writer w;
    w.beginObject();
    w.key("type").value("assistant");
    w.key("timestamp").value(ts);
    w.key("message").beginObject().key("content").beginArray();
    w.beginObject().key("type").value("text").key("text").value(text).endObject();
    w.endArray().endObject();
    w.endObject();
    return w.take() + "\n";
}

std::string titled(const char *type, const char *key, std::string_view value) {
    json::Writer w;
    w.beginObject().key("type").value(type).key(key).value(value).endObject();
    return w.take() + "\n";
}

void setModifiedSecs(const std::string &path, int64_t secs) {
    base::test::setModifiedTime(path, secs);
}

} // namespace

TEST("catalog: a transcript's title and prompts") {
    const std::string head =
        R"({"type":"user","cwd":"/src/app","timestamp":"2026-09-25T10:00:00.000Z",)"
        R"("message":{"content":"<local-command-caveat>x</local-command-caveat>"}})"
        "\n" +
        prompt("fix   the\nbuild", "2026-09-25T10:00:01.000Z") +
        titled("ai-title", "aiTitle", "Build fix");
    CatalogEntry e;
    REQUIRE(catalogEntryFrom(head, "\n" + head, e));
    CHECK_STR(e.cwd, "/src/app");
    CHECK_STR(e.firstPrompt, "fix the build"); // one line; the caveat isn't a prompt
    CHECK_STR(e.lastPrompt, "fix the build");
    CHECK_STR(e.title, "Build fix");

    // The tail: a cut first line, the latest title (a /rename wins) and prompt.
    const std::string tail = R"(t":"cut off"})"
                             "\n" +
                             titled("custom-title", "customTitle", "My name") +
                             titled("last-prompt", "lastPrompt", "and the tests") +
                             titled("ai-title", "aiTitle", "Later title");
    CatalogEntry      e2;
    REQUIRE(catalogEntryFrom(head, tail, e2));
    CHECK_STR(e2.title, "My name");
    CHECK_STR(e2.firstPrompt, "fix the build");
    CHECK_STR(e2.lastPrompt, "and the tests");

    CatalogEntry none;
    CHECK_FALSE(catalogEntryFrom(titled("ai-title", "aiTitle", "x"), "\n", none)); // no prompt

    // A slash command reads as its name; tool output and meta records are none.
    CatalogEntry cmd;
    REQUIRE(catalogEntryFrom(
        prompt("<command-name>/compact</command-name><command-args></command-args>", "t") +
            R"({"type":"user","message":{"content":[{"type":"tool_result","content":"x"}]}})"
            "\n"
            R"({"type":"user","isMeta":true,"message":{"content":"meta"}})"
            "\n",
        "\n",
        cmd
    ));
    CHECK_STR(cmd.firstPrompt, "/compact");
    // A long prompt is cut to 200 characters.
    CatalogEntry longOne;
    std::string  wide;
    for (int i = 0; i < 300; ++i)
        wide += "é";
    REQUIRE(catalogEntryFrom(prompt(wide, "t"), "\n", longOne));
    CHECK(str::endsWith(longOne.firstPrompt, "…"));
    CHECK(longOne.firstPrompt.size() == 199 * 2 + 3);
}

TEST("catalog: the role a session was started with") {
    const Role  &engineer = builtInRoles()[1];
    json::Writer w;
    w.beginArray()
        .value("You are an interactive agent…")
        .value(appendedPrompt(engineer))
        .endArray();
    const std::string snapshot = str::concat(
        {R"({"type":"attachment","timestamp":"2026-09-25T10:00:02.000Z",)",
         R"("attachment":{"type":"prompt_snapshot","systemPrompt":)",
         w.str(),
         "}}\n"}
    );
    CatalogEntry e;
    REQUIRE(catalogEntryFrom(prompt("hello", "2026-09-25T10:00:01.000Z") + snapshot, "\n", e));
    CHECK_STR(e.role, "engineer");
    CHECK_STR(e.roleName, "Engineer");
}

TEST("catalog: every session is found, newest first, a big one read at its ends") {
    const std::string root     = tempDir();
    const std::string projects = root + "/projects";
    // An ended session in one folder, a big one whose middle is never read in
    // another, one with no prompt at all, and a stray file.
    const std::string gone = projects + "/-src-other/aaaaaaaa-0000-0000-0000-000000000001.jsonl";
    file::writeAtomic(
        gone,
        prompt("old question", "2026-09-25T08:00:00.000Z", "/src/other") +
            assistantText("Old answer.", "2026-09-25T08:00:01.000Z") +
            titled("ai-title", "aiTitle", "Old work")
    );
    std::string big = prompt("big start", "2026-09-25T07:00:00.000Z");
    while (big.size() < 400 * 1024)
        big += assistantText(std::string(500, 'x'), "2026-09-25T07:00:01.000Z");
    big += titled("last-prompt", "lastPrompt", "big end");
    const std::string bigPath = projects + "/-src-big/bbbbbbbb-0000-0000-0000-000000000002.jsonl";
    file::writeAtomic(bigPath, big);
    file::writeAtomic(
        projects + "/-src-big/cccccccc-0000-0000-0000-000000000003.jsonl",
        titled("ai-title", "aiTitle", "nothing typed")
    );
    file::writeAtomic(projects + "/-src-big/notes.txt", prompt("not a transcript", "t"));
    setModifiedSecs(gone, 1'790'000'000);
    setModifiedSecs(bigPath, 1'790'000'100);

    const auto catalog = scanCatalog(projects);
    REQUIRE(catalog.size() == 2);
    const CatalogEntry &first = catalog[0];
    CHECK_STR(first.sessionId, "bbbbbbbb-0000-0000-0000-000000000002");
    CHECK_STR(first.transcriptPath, bigPath);
    CHECK_STR(first.firstPrompt, "big start");
    CHECK_STR(first.lastPrompt, "big end");
    CHECK(first.modifiedMs == 1'790'000'100'000LL);
    const CatalogEntry &second = catalog[1];
    CHECK_STR(second.title, "Old work");
    CHECK_STR(second.cwd, "/src/other");
    CHECK_STR(second.firstPrompt, "old question");

    CatalogEntry missing;
    CHECK_FALSE(readCatalogEntry(projects + "/nope.jsonl", missing));
    file::removeTree(root);
}

// The team: built-in roles, the header line a session's prompt carries, the
// teammate note, and teammate files — against a throwaway data folder.
#include "app/claude/avatar_glyphs.h"
#include "app/claude/common.h"
#include "app/claude/outputs.h"
#include "app/claude/roles.h"

#include "base/file.h"
#include "base/json.h"
#include "base/str.h"
#include "support/test.h"

#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace claude;

namespace {

// A temp folder msga's own data and cache point into, removed afterwards.
struct TempDirs {
    std::string root;
    TempDirs() {
        root = base::test::makeTempDir("claude_roles_test_");
        setDirs({root + "/data", root + "/cache"});
    }
    ~TempDirs() {
        file::removeTree(root);
        setDirs({});
    }
};

const Role *findBuiltIn(std::string_view id) {
    for (const Role &r : builtInRoles())
        if (r.id == id)
            return &r;
    return nullptr;
}

size_t count(std::string_view hay, std::string_view needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string_view::npos; at = hay.find(needle, at + 1))
        ++n;
    return n;
}

bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

// A user record as Claude Code writes one.
std::string prompt(std::string_view text, std::string_view ts) {
    json::Writer w;
    w.beginObject();
    w.key("type").value("user");
    w.key("timestamp").value(ts);
    w.key("message").beginObject();
    w.key("role").value("user");
    w.key("content").value(text);
    w.endObject();
    w.key("origin").beginObject().key("kind").value("human").endObject();
    w.endObject();
    return w.take() + "\n";
}

json::Value parsed(json::Document &doc, std::string text) {
    doc.parse(std::move(text));
    return doc.root();
}

} // namespace

TEST("roles: a prompt mentioning teammates says how to spawn them") {
    const std::string typed =
        "ask one @claude:role:engineer to fix 1-4, another @claude:role:engineer for p. 11";
    const std::string note = teammateNote(typed, findBuiltIn);
    // Once per teammate, its prompt included: a session started before the team
    // were subagent types (or a copy of one) has no "engineer" type to pick.
    CHECK(count(note, "# Your role: Engineer (msga: engineer)") == 1);
    CHECK(contains(note, "subagent_type \"engineer\""));
    CHECK(contains(note, "subagent_type \"claude\""));
    CHECK_STR(withoutTeammateNote(typed + note), typed);
    CHECK_STR(withoutTeammateNote(typed), typed);
    // The Generalist and unknown ids add nothing; nor does a bare word.
    CHECK(teammateNote("ask @claude:agent and @claude:role:astronaut", findBuiltIn).empty());
    CHECK(teammateNote("ask an engineer", findBuiltIn).empty());

    // A subagent's teammate, from its prompt: our header, or Claude's own line.
    CHECK_STR(
        roleInAgentPrompt("# Your role: Engineer (msga: engineer)\nYou are…\n\nFix it."), "engineer"
    );
    CHECK_STR(roleInAgentPrompt("Role: engineer. Repo: msga …"), "engineer");
    CHECK_STR(roleInAgentPrompt("  Role:Data-Analyst\nDig."), "data-analyst");
    CHECK(roleInAgentPrompt("Fix the role: engineer bug").empty());
}

TEST("roles: one mention grammar, its word boundaries Unicode's") {
    const auto at = [](std::string_view text) {
        const size_t i = text.find('@');
        return mentionAt(text, i == std::string_view::npos ? 0 : i);
    };
    CHECK(at("ask @claude:role:engineer now").len == 21);
    CHECK_STR(at("ask @claude:role:engineer now").roleId, "engineer");
    CHECK(at("@claude:agent?").len == 13);
    CHECK(at("@claude:agent?").roleId.empty()); // the Generalist
    CHECK(at("<@claude:role:data-analyst>").len == 25);
    // Inside a word, a path or an address: none. A letter is a letter in any
    // script: "café@…" is an address, as "cafe@…" is.
    CHECK(at("mail x@claude:role:engineer").len == 0);
    CHECK(at("mail cafe@claude:role:engineer").len == 0);
    CHECK(at("mail caf\xC3\xA9@claude:role:engineer").len == 0);
    CHECK(at("ask \xC3\xA9 @claude:role:engineer").len == 21);
    CHECK(at("see /x/@claude:role:engineer").len == 0);
    // Running on into a longer word: none.
    CHECK(at("@claude:role:engineer_x").len == 0);
    CHECK(at("@claude:role:engineer\xC3\xA9").len == 0);
    CHECK(at("@claude:agent-x").len == 0);
    CHECK(at("@claude:role:").len == 0);
    CHECK(at("@claude:role:Engineer").len == 0);
    // The note goes by the same rule.
    CHECK(teammateNote("ask caf\xC3\xA9@claude:role:engineer", findBuiltIn).empty());
    CHECK_FALSE(teammateNote("ask \xC3\xA9 @claude:role:engineer", findBuiltIn).empty());
}

TEST("roles: teammates a session has no types for count when merely named") {
    const std::vector<Role> &team = builtInRoles();

    // "Spawn a marketer agent" in a session started without --agents: Claude
    // would otherwise spawn a general-purpose one and write its own role.
    const std::string typed = "Spawn a marketer agent to do the investigation.";
    const std::string note  = teammateNote(typed, findBuiltIn, team);
    CHECK(count(note, "# Your role: Marketer (msga: marketer)") == 1);
    CHECK(contains(note, "subagent_type \"claude\""));
    CHECK(!contains(note, "msga: engineer"));
    CHECK_STR(withoutTeammateNote(typed + note), typed);
    CHECK_STR(
        roleInAgentPrompt(appendedPrompt(*findBuiltIn("marketer")) + "\n\nFind links."), "marketer"
    );

    // Any case, plural, the id; a mention and the name make one entry.
    CHECK(contains(teammateNote("ask two Engineers", findBuiltIn, team), "msga: engineer"));
    CHECK(contains(teammateNote("the RESEARCHER", findBuiltIn, team), "msga: researcher"));
    CHECK(
        count(
            teammateNote("@claude:role:designer — the designer", findBuiltIn, team),
            "# Your role: Designer"
        ) == 1
    );
    // Not inside other words, paths or mentions of something else; the
    // Generalist adds nothing; a type the session offers isn't in `byName`.
    CHECK(teammateNote("engineering the design", findBuiltIn, team).empty());
    CHECK(teammateNote("see src/engineer/ and @claude:engineer", findBuiltIn, team).empty());
    CHECK(teammateNote("the generalist", findBuiltIn, team).empty());
    CHECK(teammateNote("spawn a marketer", findBuiltIn).empty());

    // An added teammate with a two-word name.
    Role analyst;
    analyst.id     = "data-analyst";
    analyst.name   = "Data analyst";
    analyst.prompt = "Dig.";
    CHECK(contains(
        teammateNote("ask the data analysts", findBuiltIn, {analyst}), "msga: data-analyst"
    ));
    CHECK(contains(teammateNote("the Data-Analyst", findBuiltIn, {analyst}), "msga: data-analyst"));
    CHECK(teammateNote("the data", findBuiltIn, {analyst}).empty());
}

TEST("roles: the team, a generalist first, then the specialists") {
    const auto &all = builtInRoles();
    REQUIRE(all.size() == 5);
    CHECK_STR(all[0].id, "generalist");
    CHECK(appendedPrompt(all[0]).empty()); // plain Claude Code
    for (size_t i = 1; i < all.size(); ++i)
        CHECK(
            str::startsWith(
                appendedPrompt(all[i]),
                str::concat({"# Your role: ", all[i].promptName, " (msga: ", all[i].id, ")\n"})
            )
        );
}

TEST("roles: the built-ins' pictures are written to the avatars folder") {
    TempDirs          tmp;
    const std::string avatars = tmp.root + "/data/team/avatars";
    const auto       &all     = builtInRoles();
    for (const Role &r : all) {
        CHECK(str::startsWith(r.avatar, avatars + "/"));
        CHECK(file::exists(r.avatar));
    }
    CHECK_STR(file::baseName(all[1].avatar), "role-engineer.svg");
    const std::string agent = agentAvatarPath();
    CHECK_STR(agent, all[0].avatar);
    std::string svg;
    REQUIRE(file::readAll(agent, &svg));
    CHECK(str::startsWith(svg, "<svg "));
    CHECK(contains(svg, "#D97757"));

    // A glyph tile is its own picture.
    const std::string tile = avatar_glyphs::svg("code-xml", 0x2F6FDB);
    CHECK(contains(tile, "fill=\"#2f6fdb\""));
    CHECK(contains(tile, "m14.5 4-5 16"));
    CHECK(contains(avatar_glyphs::svg("no-such-glyph", 0x123456), "M12 19h8")); // the first
    CHECK(avatar_glyphs::hasGlyph("telescope"));
    CHECK_FALSE(avatar_glyphs::hasGlyph("telescopes"));
    CHECK(avatar_glyphs::colors().front() == 0xD97757);
}

TEST("roles: a session's role is read back from its recorded system prompt") {
    const Role    &engineer = builtInRoles()[1];
    json::Document d1, d2, d3, d4, d5, d6;
    json::Writer   w;
    w.beginArray()
        .value("You are an interactive agent…")
        .value(appendedPrompt(engineer))
        .endArray();
    const std::string withRole = w.take();
    CHECK_STR(roleInSystemPrompt(parsed(d1, withRole)).id, "engineer");
    CHECK(roleInSystemPrompt(parsed(d2, R"(["You are an interactive agent…"])")).id.empty());
    // Before ids were written: a built-in's English name alone.
    CHECK_STR(
        roleInSystemPrompt(parsed(d3, R"(["# Your role: Engineer\nYou are…"])")).id, "engineer"
    );
    CHECK(roleInSystemPrompt(parsed(d4, R"(["# Your role: Astronaut\nFly."])")).id.empty());
    // An added teammate, with spaces in its name — and as a {"text"} part.
    const RoleMark added = roleInSystemPrompt(parsed(
        d5, R"([{"type":"text","text":"# Your role: Data analyst (msga: data-analyst)\nDig."}])"
    ));
    CHECK_STR(added.id, "data-analyst");
    CHECK_STR(added.name, "Data analyst");
    CHECK(roleInSystemPrompt(parsed(d6, "{}")).id.empty());

    // In a transcript: the prompt_snapshot attachment Claude Code records.
    const std::string snapshot = str::concat(
        {R"({"type":"attachment","timestamp":"2026-09-25T10:00:02.000Z",)",
         R"("attachment":{"type":"prompt_snapshot","systemPrompt":)",
         withRole,
         "}}\n"}
    );
    // Raw bytes, as "Find a session" reads a transcript's ends — even cut off
    // right after the header line.
    CHECK_STR(roleInTranscriptBytes(snapshot).id, "engineer");
    const size_t at = snapshot.find("engineer)\\n");
    REQUIRE(at != std::string::npos);
    CHECK_STR(roleInTranscriptBytes(snapshot.substr(0, at + 11)).id, "engineer");
    CHECK(roleInTranscriptBytes(snapshot.substr(0, at + 5)).id.empty()); // the line isn't whole
    CHECK(roleInTranscriptBytes(prompt("# Your role: Engineer", "2026-09-25T10:00:01.000Z"))
              .id.empty()); // typed, not a part of the system prompt
}

TEST("roles: teammates are added, edited, restored and removed") {
    TempDirs          tmp;
    const std::string dir = tmp.root + "/data/team";
    Team              team(dir);
    REQUIRE(team.listed().size() == 5);

    Role copy;
    copy.name        = "Copy writer!";
    copy.description = "Writes copy.";
    copy.glyph       = "pen-tool";
    copy.color       = 0x0E8C9A;
    copy.prompt      = "You write copy.";
    std::string       error;
    const std::string id = team.save(copy, &error);
    REQUIRE(id == "copy-writer");
    CHECK(str::endsWith(team.find(id)->avatar, "/avatars/pen-tool-0e8c9a.svg"));
    CHECK(file::exists(team.find(id)->avatar));
    CHECK_STR(
        appendedPrompt(*team.find(id)),
        "# Your role: Copy writer! (msga: copy-writer)\nYou write copy."
    );
    // A second one by the same name gets an id of its own.
    CHECK_STR(team.save(copy, &error), "copy-writer-2");
    CHECK(team.save(Role{}, &error).empty()); // no name
    CHECK_FALSE(error.empty());

    // Renaming keeps the id — the one its sessions carry.
    Role renamed = *team.find(id);
    renamed.name = "Writer";
    CHECK_STR(team.save(renamed, &error), id);
    CHECK(
        str::startsWith(appendedPrompt(*team.find(id)), "# Your role: Writer (msga: copy-writer)")
    );

    // A built-in, edited then restored.
    Role engineer   = *team.find("engineer");
    engineer.prompt = "Only Rust.";
    REQUIRE(team.save(engineer, &error) == "engineer");
    CHECK(team.find("engineer")->edited);
    CHECK_STR(team.find("engineer")->avatar, builtInRoles()[1].avatar); // same picture
    CHECK_STR(
        appendedPrompt(*team.find("engineer")), "# Your role: Engineer (msga: engineer)\nOnly Rust."
    );
    CHECK_FALSE(team.remove("engineer")); // built-ins stay
    CHECK(team.restore("engineer"));
    CHECK_FALSE(team.find("engineer")->edited);
    CHECK_STR(team.find("engineer")->prompt, builtInRoles()[1].prompt);

    // The team as subagent types: every role with a prompt, under its id, with
    // that prompt — but none shadowing one of Claude Code's own types.
    Role plan;
    plan.name   = "Plan";
    plan.prompt = "You plan.";
    REQUIRE(team.save(plan, &error) == "plan");
    json::Document agents;
    REQUIRE(agents.parse(subagentsJson(team.listed()), nullptr));
    std::vector<std::string> keys;
    for (json::Value v : agents.root())
        keys.emplace_back(v.key());
    const std::vector<std::string> want = {
        "copy-writer", "copy-writer-2", "designer", "engineer", "marketer", "researcher"
    };
    CHECK(keys == want);
    CHECK_STR(agents.root()["engineer"]["prompt"].str(), appendedPrompt(*team.find("engineer")));
    CHECK_STR(
        agents.root()["copy-writer"]["description"].str(),
        "Writer, a teammate (mentioned as @claude:role:copy-writer). Writes copy."
    );
    CHECK(subagentsJson({builtInRoles()[0]}).empty()); // the Generalist adds nothing
    team.remove("plan");

    // Removed: off the list, still known to its sessions.
    CHECK(team.remove(id));
    CHECK(team.listed().size() == 6);
    REQUIRE(team.find(id));
    CHECK(team.find(id)->removed);
    CHECK_STR(team.resolve(id).name, "Writer");

    // All of it read back from disk.
    Team again(dir);
    REQUIRE(again.find(id));
    CHECK(again.find(id)->removed);
    CHECK_STR(again.find(id)->name, "Writer");
    CHECK_STR(again.find("copy-writer-2")->prompt, "You write copy.");
    CHECK(again.find("copy-writer-2")->color == 0x0E8C9A);
    CHECK_FALSE(again.find("engineer")->edited);
    // Added teammates come after the built-ins, in the order they were added.
    CHECK_STR(again.roles()[5].id, id);

    // A role nobody here knows: a former teammate, named by its sessions.
    CHECK(again.noteFormer("astronaut", "Astronaut"));
    CHECK_FALSE(again.noteFormer("astronaut", "Astronaut"));
    const Role former = again.resolve("astronaut");
    CHECK(former.former);
    CHECK_STR(former.name, "Astronaut");
    CHECK_FALSE(former.avatar.empty());
    CHECK(file::exists(former.avatar));
    // …and its id is never given to a new teammate.
    Role astro;
    astro.name = "Astronaut";
    CHECK_STR(again.save(astro, &error), "astronaut-2");
    // The Generalist for no id at all.
    CHECK_STR(again.resolve("").id, "generalist");
}

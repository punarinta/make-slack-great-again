// The team: roles a Claude Code session can be started with — the Generalist
// (plain Claude Code), the built-in specialists, and teammates the user adds.
//
// A role's few lines are passed with --append-system-prompt, under a header
// line naming it: "# Your role: Copywriter (msga: copywriter)". Not --agent:
// an agent's prompt replaces Claude Code's instead of adding to it (verified
// 2026-09-25 — the recorded system prompt was the agent's text alone). Claude
// Code records the rendered system prompt in the transcript (an attachment of
// type "prompt_snapshot", its `systemPrompt` a list of parts, ours last) and
// sends that record again on every resume. So a session keeps the prompt it
// started with, whatever is edited later, and its role is read back from the
// header's id — which never changes, so renaming a teammate keeps its sessions.
// Sessions from before the id was written carry "# Your role: Engineer" only,
// matched by the built-in's English name.
//
// Teammates live in msga's data, one file each (<dirs().data>/team/<id>.md):
// a few "key: value" lines between "---" lines, then the prompt. A built-in
// has a file only once edited ("Restore default" deletes it). A removed
// teammate's file stays, marked removed: its sessions keep its name and
// picture. A role nothing here knows (another machine's, a deleted file) is a
// former teammate, named from the transcript's header.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace json {
class Value;
}

namespace claude {

struct Role {
    std::string id;          // stable: "engineer", "copywriter"
    std::string name;        // "Engineer", shown (a built-in's is translated)
    std::string description; // one line for the teammate's page
    std::string avatar;      // a local image file (SVG for glyph tiles)
    std::string glyph;       // avatar_glyphs id
    uint32_t    color = 0;   // 0xRRGGBB of the glyph tile
    std::string prompt;      // what the role adds to Claude Code's prompt; "" = nothing
    std::string promptName;  // the name in the header line (a built-in's stays English)
    bool        builtIn = false;
    bool        edited  = false; // a built-in changed from its default
    bool        removed = false; // no longer on the team; its sessions keep it
    bool        former  = false; // only known from a session's transcript
    int64_t     created = 0;     // epoch ms; orders the added teammates
};

// What --append-system-prompt gets for `role`: the header line, then its
// prompt; "" when the role adds nothing (the plain Generalist).
std::string appendedPrompt(const Role &role);
// What --agents gets: `roles` (the listed team) as Claude Code subagent types,
// one per role that adds a prompt, named by its id — so "use @Engineer", which
// reaches Claude as "@claude:role:engineer", finds subagent_type "engineer".
// For a subagent --agent's replacing the prompt is the point: every subagent
// type works that way. An id Claude Code's own types already use is left out.
std::string subagentsJson(const std::vector<Role> &roles);

// What msga adds to a prompt that mentions teammates ("@claude:role:x"): a
// note saying how to spawn each, their prompts included. --agents alone isn't
// enough — sessions started before it, and copies Claude Code makes on
// resume, have no such subagent types, and Claude then spawns a plain
// "claude" subagent with a self-written "Role: engineer" line. "" when
// `prompt` mentions none that adds a prompt. `find` looks a role up by id.
// `byName` are teammates the session has no subagent types for: those are
// noted when merely named, too ("spawn a marketer"), since nothing else tells
// Claude they exist — a stray "our marketer said" only costs a note it ignores.
std::string teammateNote(
    std::string_view                                        prompt,
    const std::function<const Role *(std::string_view id)> &find,
    const std::vector<Role>                                &byName = {}
);
// `prompt` without the note, as the transcript has it back.
std::string withoutTeammateNote(std::string_view prompt);

// A teammate mention at text[i] (its '@'): "@claude:agent" (the Generalist)
// or "@claude:role:<id>" ([a-z0-9-]+), standing on its own — not inside a
// word, a path or an address (no word character nor one of "@/:.-" before
// it), nor running on into a longer word (no word character nor '-' after
// it). Word characters are Unicode's (\w): "café@claude:role:x" is none.
// The one grammar for mentions: rendered, titles, the composer's pills,
// teammateNote. A token "<@claude:…>" is this with '<' before and '>' after.
struct Mention {
    size_t      len = 0; // from the '@' on; 0 = no mention here
    std::string roleId;  // "" = the Generalist (claude:agent)
};
Mention     mentionAt(std::string_view text, size_t i);
// The teammate a subagent was spawned as, from its Agent call's prompt: our
// header line (a teammateNote fallback), or a leading "Role: engineer" line
// Claude wrote itself; "" = none.
std::string roleInAgentPrompt(std::string_view prompt);

// The role named by our part of a recorded system prompt.
struct RoleMark {
    std::string id;   // "" = none of ours
    std::string name; // as the header has it
};
// From a prompt_snapshot's `systemPrompt` (a JSON array of strings, or of
// {"text": …} parts).
RoleMark roleInSystemPrompt(const json::Value &systemPrompt);
// From raw transcript bytes (the JSON-escaped header, possibly cut off after
// its line) — for reading only a transcript's ends.
RoleMark roleInTranscriptBytes(std::string_view bytes);

// The built-ins as they come, the Generalist first. Their pictures are
// written to <dirs().data>/team/avatars on first use (none before the
// directories are set).
const std::vector<Role> &builtInRoles();
// The Generalist's picture — Claude Code's own, the workspace icon too: its
// file, written on the first call; "" before the directories are set.
std::string              agentAvatarPath();

class Team {
public:
    // `dir` holds the teammate files (created on the first save).
    explicit Team(std::string dir);

    // Everyone known: the built-ins, then the added teammates (removed ones
    // included) in the order they were added.
    const std::vector<Role> &roles() const { return _roles; }
    // The team as listed: without the removed ones.
    const std::vector<Role> &listed() const { return _listed; }
    const Role              &generalist() const { return _roles.front(); }
    // nullptr when unknown.
    const Role              *find(std::string_view id) const;
    // `id` as shown: a known role, a former teammate (named `nameHint`, or as
    // noted before), or the Generalist for "". Valid until the team or its
    // formers change (save, remove, restore, noteFormer).
    const Role              &resolve(std::string_view id, std::string_view nameHint = {}) const;
    // Remembers what a transcript calls a role nobody here knows; true when
    // that's news.
    bool                     noteFormer(std::string_view id, std::string_view name);
    // Former teammates seen so far: id → name.
    const std::unordered_map<std::string, std::string> &formers() const { return _formers; }

    // Adds (`role.id` empty) or updates a teammate; its id, "" on failure
    // (*error says why).
    std::string save(Role role, std::string *error = nullptr);
    // Takes an added teammate off the team (not a built-in).
    bool        remove(std::string_view id);
    // A built-in back to how it comes.
    bool        restore(std::string_view id);

private:
    void        load();
    bool        write(const Role &role, std::string *error);
    std::string avatarFor(const Role &role) const;
    std::string newId(std::string_view name) const;

    std::string                                   _dir;
    std::vector<Role>                             _roles;
    std::vector<Role>                             _listed; // _roles but the removed (load)
    std::unordered_map<std::string, std::string>  _formers;
    // Former teammates as resolve() made them, by id + '\n' + name hint
    // (node-based: references stay valid as more are added).
    mutable std::unordered_map<std::string, Role> _resolved;
};

} // namespace claude

// Permission questions answered through Claude Code's PermissionRequest hook,
// rather than read off its screen (attach.h). Background sessions msga starts
// carry the hook in `--settings` (saved with the job like any start option):
// when Claude Code would ask, it runs `msga --claude-permission-hook <dir>`,
// which leaves the question in <dir> for the running msga and prints the
// answer msga leaves back. msga shows the question's options as buttons.
//
// Verified with Claude Code 2.1.314 (2026-10-30):
//   • The hook gets session_id, cwd, tool_name, tool_input and
//     permission_suggestions — the "Yes, and don't ask again…" options of
//     the dialog — on stdin, in every permission mode (auto too), while the
//     job's state.json already reads "approve <tool>: …".
//   • Its answer {"hookSpecificOutput":{"hookEventName":"PermissionRequest",
//     "decision":{…}}}: "allow" (with updatedPermissions: a suggestion, which
//     Claude Code then saves where it says), or "deny" + interrupt, which
//     ends the turn as the dialog's "No" does.
//   • The dialog is up while the hook runs, and the first answer counts (one
//     in a terminal too); a hook that prints nothing leaves it to the
//     dialog. The exec form ("command" + "args") runs it without a shell.
//
// The files, in <dir>, per question <id>:
//   <id>.json     the hook's input, written by the hook; touched while it waits
//   <id>.seen     msga has it (touched while msga runs)
//   <id>.answer   what the hook prints ("" = the dialog asks after all)
// No running msga takes it within a few seconds, or msga goes away: the hook
// prints nothing, and the dialog alone asks — which msga reads off the screen.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace claude {

inline constexpr std::string_view kPermissionHookFlag = "--claude-permission-hook";

// The `--settings` JSON that has background sessions ask through `exe`'s hook
// mode, leaving questions in `dir`.
std::string permissionHookSettings(std::string_view exe, std::string_view dir);
// Whether a job's saved start options carry it.
bool        hasPermissionHook(std::string_view settingsJson);

// The hook itself (`msga --claude-permission-hook <dir>`): reads the question
// on stdin, waits for msga's answer, prints it. Its exit code: always 0 —
// a failing hook mustn't decide anything.
int runPermissionHook(std::string_view dir);

struct HookQuestion {
    struct Option {
        std::string label;
        std::string answer; // what the hook prints when it's picked
    };
    std::string         id;
    std::string         sessionId;
    std::vector<Option> options; // "Yes", its suggestions, "No"
};

// The question in a hook's input (`json`), as the dialog would offer it.
// False when it isn't one.
bool parseHookQuestion(std::string_view json, HookQuestion *out);

// The questions whose hook still waits (the others' files removed).
std::vector<HookQuestion> pendingHookQuestions(std::string_view dir);
// msga has `id`: from now on the hook waits for its answer. Called again,
// it says msga is still there.
void                      takeHookQuestion(std::string_view dir, std::string_view id);
// Its answer; "" leaves it to the dialog.
bool answerHookQuestion(std::string_view dir, std::string_view id, std::string_view answer);

} // namespace claude

// What a session at work shows beside "thinking": how its long jobs are going.
// Two sources, both read by Backend::pumpTyping:
//
//   • A command it runs in the background (TranscriptParser::runningCommands)
//     writes its output to a file as it comes; its last line is shown as is,
//     with a bar when the line reads "N/M" or "NN%". Nothing to ask of anyone:
//     a test runner's "9846/10000 passed" is already there.
//   • A report of its own: `msga --claude-status "<line>" [--progress <p>]
//     [--name <job>]`,
//     run by the session (or a script of its) — the sessions msga starts are
//     told how in their system prompt (progressNote). It leaves the report in
//     the job's folder ($CLAUDE_JOB_DIR, set for everything a background
//     session runs), where msga reads it.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace claude {

inline constexpr std::string_view kStatusFlag = "--claude-status";

// `msga --claude-status …` (args after the flag): writes the report, prints
// usage for --help or a mistake. Exit code 0 on success, 2 on a usage error,
// 1 when not inside a background session (no CLAUDE_JOB_DIR).
int runStatusCommand(int argc, char **argv);

// A report's file in a job's folder (jobs/<short>): one per job name ("" =
// the unnamed one), each written whole, so jobs side by side never clash.
std::string reportPath(std::string_view jobDir, std::string_view name = {});

struct Report {
    std::string name; // --name's, "" = none
    std::string text;
    std::string ratio;         // "6200/10000" when --progress said so
    float       progress = -1; // 0…1, < 0 = not given
    int64_t     atMs     = 0;  // when it was made, epoch ms
};
// The unnamed report in a job's folder; false when there is none (or no valid one).
bool                readReport(std::string_view jobDir, Report *out);
// Every report in a job's folder, by name.
std::vector<Report> readReports(std::string_view jobDir);
// The JSON a report is stored as (runStatusCommand writes it).
std::string         reportJson(const Report &r);

// A fraction as --progress takes it: "9846/10000", "98.5%", "0.985" (≤ 1).
// < 0 when it isn't one.
// `ratio` (optional) gets "9846/10000" when it was one.
float parseProgress(std::string_view s, std::string *ratio = nullptr);
// How far along a line of output says it is: its first "N/M" (N ≤ M, M > 0,
// not part of a path or a date) or "NN%"; < 0 when it says nothing of it.
float progressInLine(std::string_view line, std::string *ratio = nullptr);

// A background job's short name, from its command line: the program's name
// without its folder and extension ("./scripts/fuzz-checkout.sh" →
// "fuzz-checkout"), and for a generic runner its next word ("npm test",
// "cmake --build"); a script run by an interpreter names itself ("python3
// tools/gen.py" → "gen").
std::string jobName(std::string_view command);

// The line to show from the end of a command's output (`tail`): the last one
// with something on it — a progress bar redrawn with "\r" counts its last
// drawing — without terminal colours, trimmed, at most `maxChars` code points.
std::string lastOutputLine(std::string_view tail, size_t maxChars = 160);

// The paragraph msga adds to the system prompt of the sessions it starts:
// how to report progress with `exe` (msga's own path).
std::string progressNote(std::string_view exe);

} // namespace claude

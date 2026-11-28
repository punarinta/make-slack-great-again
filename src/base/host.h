// Inside a Flatpak sandbox the programs the app runs — the claude CLI, git, a
// browser for the Slack sign-in — are the host's: process.h starts them there
// through flatpak-spawn --host (the org.freedesktop.Flatpak permission), and
// this is how. The sandbox has a process namespace of its own, so what runs on
// the host is signalled and looked at (/proc) through the host too.
//
// Measured on Flatpak 1.16: a host command costs ~7 ms. flatpak-spawn passes
// SIGTERM on to the host process alone, not its group, and when flatpak-spawn
// dies (--watch-bus) only that process goes, not what it started — so the
// commands report their host pid (wrap's pidFd) and their group is signalled
// on the host (signal).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Flatpak is Linux's: elsewhere (macOS, Windows, the BSDs, a port of
// src/port) sandboxed() is false and nothing here is reached.
#if defined(__linux__) && !defined(MSGA_PORT)
#define MSGA_HOST_EXEC 1
#endif

namespace base {
struct RunResult;
}

namespace base::host {

// Running in a Flatpak sandbox (/.flatpak-info) that may run programs on the
// host, or a test standing in for one: what everything here is for.
bool sandboxed();

// In a Flatpak sandbox the user has taken the host away from (flatpak
// override, Flatseal): programs then run in the sandbox, where there is no
// claude, git or browser, and the features that need them say so.
bool hostAccessRevoked();

// Whether a /.flatpak-info grants what running on the host takes: talking to
// org.freedesktop.Flatpak (flatpak-spawn --host) and the host's files
// (Claude Code's sessions and projects). A user override shows here too.
bool hostAccessFrom(std::string_view flatpakInfo);

// Tests: `spawner` plays flatpak-spawn — it gets the same options and runs the
// command (here, on this machine) — and sandboxed() is true; "" ends it.
void setSpawnerForTest(std::string spawner);

struct Command {
    std::string              exe; // flatpak-spawn
    std::vector<std::string> args;
};

// The command that runs `exe` with `args` on the host: from `cwd` (the home
// folder when empty — the sandbox's own folder may not exist there), with
// `env` ("NAME=value" sets, "NAME=" unsets) on top of the host session's
// environment, and the sandbox's file descriptors `forwardFds` passed along
// as themselves. pidFd >= 0: a shell writes its host pid there (a line), then
// execs the program — whose pid it then is, leading a session of its own.
Command wrap(
    const std::string              &exe,
    const std::vector<std::string> &args,
    const std::string              &cwd,
    const std::vector<std::string> &env,
    int                             pidFd,
    const std::vector<int>         &forwardFds = {}
);

// The pid wrap's shell writes on the read end of its pidFd pipe; 0 when the
// pipe ends first (flatpak-spawn failed) or nothing comes within timeoutMs.
int64_t readPid(int fd, int timeoutMs);

// A POSIX sh script run on the host, with `args` as $1…; its stdout (stderr
// is dropped). BLOCKING (~7 ms): keep it off the UI thread where it repeats.
RunResult sh(const std::string &script, const std::vector<std::string> &args = {});

// sig (SIGTERM, SIGKILL, …) to the host process `pid` — to the group it leads
// when `group` — and whether that was delivered.
bool signal(int64_t pid, int sig, bool group);

// What a host program runs to reach this app (Claude Code's permission
// hook, `msga --claude-status`): our own path, or in a Flatpak sandbox —
// where /app/bin is no host path — the installation's exported launcher,
// <installation>/exports/bin/<app id>, which starts us in a sandbox of our
// own. "" when it can't be told.
std::string selfCommand();
// …the launcher named by a /.flatpak-info: its [Application] name and its
// [Instance] app-path, <installation>/app/<name>/<arch>/<branch>/<commit>/files.
std::string exportedLauncher(std::string_view flatpakInfo);

} // namespace base::host

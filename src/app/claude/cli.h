// Adding the Claude Code workspace. There is nothing to sign in to: the
// workspace is "the Claude Code sessions on this machine", read from Claude
// Code's state directory, plus the `claude` CLI for the sessions msga starts
// itself. The add flow checks both exist and that the CLI is logged in (msga
// can't log it in: that's interactive), and records where the CLI is.
#pragma once

#include "app/auth/workspaces.h"

#include <functional>
#include <string>

namespace plat {
class App;
}

namespace claude {

// This backend's service. The token is stored in workspace handles — never change it.
inline constexpr const char *kService = "claude-code";

// One workspace per machine.
inline constexpr const char *kWorkspaceId = "local";

struct Credentials {
    std::string claudePath; // the `claude` executable; empty = not found (read-only use)
    // "Runs on": "" = this computer, else the SSH destination ("user@box",
    // a Host alias) whose Claude Code sessions msga drives through its relay
    // (host.h), with msga there at `remoteMsga` ("" = on its PATH).
    std::string sshDestination;
    std::string remoteMsga;
    bool        operator==(const Credentials &) const = default;
};

auth::WorkspaceRecord toRecord(const Credentials &creds);
Credentials           fromRecord(const auth::WorkspaceRecord &rec);

// The `claude` CLI: PATH first, then where its installers put it. On Windows
// an npm install is `claude.cmd`, the native one `claude.exe`. Empty if absent.
std::string findClaudeExecutable();

// What to tell the user when the CLI isn't there, and when it isn't logged in:
// both are fixed outside msga, so both say how.
std::string notInstalledMessage();
std::string notLoggedInMessage();

// The add flow: the CLI found, Claude Code's state directory there, the CLI
// logged in. done(creds, "") when the workspace can be added, done({}, why)
// when not. On the plat loop, never re-entrantly.
void checkSetup(plat::App &app, std::function<void(Credentials creds, std::string error)> done);

} // namespace claude

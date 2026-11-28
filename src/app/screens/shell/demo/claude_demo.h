// The Claude Code demo workspace (`msga --demo demo/claude`): the real Claude
// Code workspace (app/claude) run against demo/claude/fake-claude, a stand-in
// for the `claude` CLI that plays scripted answers from the fixture
// (demo/claude/claude-code.json) — no Claude, no account, and nothing of the
// real ~/.claude: its Claude Code home is the demo run's throwaway HOME.
// Compiled only into demo builds (-DMSGA_DEMO=ON).
#pragma once

#include <string>

namespace plat {
class App;
}
namespace shell {
struct Settings;
}

namespace demo {

// Whether `dir` holds a Claude Code fixture (claude-code.json).
bool isClaudeFixture(const std::string &dir);

struct ClaudeDemo {
    std::string claudePath;   // the stand-in CLI
    std::string startSession; // the conversation opened first ("" = none)
};

// Points Claude Code's home at <HOME>/.claude (HOME is the demo run's own),
// sets msga's Claude Code folders, writes the fixture's sessions, folders and
// profile there (fake-claude --demo-seed) and makes the fixture's folder the
// one new sessions start in. False (and *error says why) when that fails.
bool prepareClaudeDemo(
    const std::string &dir,
    plat::App         &app,
    shell::Settings   &settings,
    ClaudeDemo        *out,
    std::string       *error
);

} // namespace demo

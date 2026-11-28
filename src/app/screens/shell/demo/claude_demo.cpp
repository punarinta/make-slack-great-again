#include "screens/shell/demo/claude_demo.h"

#include "app/claude/common.h"
#include "app/identity.h"
#include "base/file.h"
#include "base/json.h"
#include "base/process.h"
#include "base/str.h"
#include "screens/shell/recent_folders.h"
#include "screens/shell/settings.h"

#include <cstdlib>

namespace demo {

bool isClaudeFixture(const std::string &dir) {
    return file::exists(file::join(dir, "claude-code.json"));
}

bool prepareClaudeDemo(
    const std::string &dir,
    plat::App         &app,
    shell::Settings   &settings,
    ClaudeDemo        *out,
    std::string       *error
) {
    const std::string fixture = file::absolute(dir);
    const std::string home    = base::env("HOME"); // the demo run's own (isolateDemoState)
    const std::string cc      = file::join(home, ".claude");
    if (home.empty() || !file::makeDirs(cc)) {
        *error = "cannot create " + cc;
        return false;
    }
    setenv("CLAUDE_CONFIG_DIR", cc.c_str(), 1);

    // msga's own Claude Code folders, as a real workspace has them (Accounts).
    claude::Dirs d;
    d.data  = file::join(identity::dataDir(app), "claude-code");
    d.cache = file::join(identity::cacheDir(app), "claude-code");
    claude::setDirs(d);

    out->claudePath = file::join(fixture, "fake-claude");
    base::RunOptions o;
    o.env                   = {"MSGA_DEMO_CLAUDE_DATA=" + d.data};
    o.timeoutMs             = 30'000;
    const base::RunResult r = base::run(out->claudePath, {"--demo-seed"}, o);
    if (!r.started || r.code != 0) {
        *error = str::concat({"fake-claude --demo-seed failed: ", r.output});
        return false;
    }

    json::Document doc;
    if (!doc.parseFile(file::join(fixture, "claude-code.json"), error))
        return false;
    out->startSession            = std::string(doc.root()["startSession"].str());
    // New sessions start in the fixture's "startFolder", not in the bare HOME
    // (which Claude Code doesn't trust).
    const std::string_view start = doc.root()["startFolder"].str();
    if (!start.empty()) {
        const std::string folder =
            str::startsWith(start, "~/") ? file::join(home, start.substr(2)) : std::string(start);
        settings.claudeLastDir = folder;
        shell::recent_folders::bump(settings, folder);
    }
    return true;
}

} // namespace demo

// A CLI run (a git command, `claude …`) off the UI thread, on a
// model::runInBackground worker; its result goes to the loop. Callbacks run
// on the UI thread, never re-entrantly from inside the call.
//
// Nothing is cancelled: a caller that may be gone by then guards its callback
// (a shared alive flag, as SlackBackend does).
#pragma once

#include "base/process.h"

#include <functional>
#include <string>
#include <vector>

namespace plat {
class App;
}

namespace claude {

using RunDone = std::function<void(base::RunResult)>;
// base::run(exe, args, opts) on a worker thread; done(result) on the loop.
void runAsync(
    plat::App               &app,
    std::string              exe,
    std::vector<std::string> args,
    base::RunOptions         opts,
    RunDone                  done
);

} // namespace claude

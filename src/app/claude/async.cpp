#include "app/claude/async.h"

#include "app/model/jobs.h"

#include <memory>

namespace claude {

void runAsync(
    plat::App               &app,
    std::string              exe,
    std::vector<std::string> args,
    base::RunOptions         opts,
    RunDone                  done
) {
    auto r = std::make_shared<base::RunResult>();
    model::runInBackground(
        app,
        [r, exe = std::move(exe), args = std::move(args), opts = std::move(opts)] {
            *r = base::run(exe, args, opts);
        },
        [r, done = std::move(done)] {
            if (done)
                done(std::move(*r));
        }
    );
}

} // namespace claude

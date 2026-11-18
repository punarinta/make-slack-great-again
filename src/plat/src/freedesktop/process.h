// Child processes for the Linux pieces that drive desktop command-line tools
// (zenity/kdialog file dialogs, the sound server's audio helpers): finding a
// program on a search path, spawning it with piped stdio, and reaping it.
#pragma once

#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace plat::linux_process {

// $PATH, or the usual system directories when it is unset or empty.
std::string_view defaultSearchPath();

// Absolute path of the regular, executable file `name` in the first directory
// of the ':'-separated `searchPath` that has one (empty entries skipped), or "".
std::string findExecutable(const char *name, std::string_view searchPath);

void closeFd(int &fd); // closes fd if open and sets it to -1

enum Pipes : unsigned { kStdin = 1, kStdout = 2, kStderr = 4 };

struct Child {
    pid_t pid = -1;
    int   in = -1, out = -1, err = -1; // our ends, non-blocking
};

// Starts `exe` (an absolute path) with `args`, argv[0] being `argv0` (exe when
// null). The requested stdio streams become pipes (stdin a socket, so a dead
// reader can't SIGPIPE us), the rest the null device; the child starts with
// no blocked signals and SIGPIPE/SIGTERM/SIGINT at their defaults. False when
// it could not be started.
bool spawn(
    const std::string              &exe,
    const std::vector<std::string> &args,
    unsigned                        pipes,
    Child                          *c,
    const char                     *argv0 = nullptr
);

// Kills and reaps the child (SIGKILL lands at once) and closes our ends.
void kill(Child &c);

// Non-blocking: true once the child has exited (and is reaped); *ok = it
// exited normally with status 0.
bool reaped(Child &c, bool *ok);

} // namespace plat::linux_process

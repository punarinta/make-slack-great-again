// Child processes the app launches and later stops (a browser for the Slack
// sign-in), and short commands whose output it reads (the claude CLI, see
// run()). A Process's output goes to the null device, and the child runs in
// its own session / process group so the whole tree can be signalled —
// wrappers like /usr/bin/google-chrome start the real browser and do not
// forward SIGTERM.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace base {

class Process {
public:
    Process() = default;
    ~Process(); // forgets the child (it keeps running); call kill() first to stop it
    Process(const Process &)            = delete;
    Process &operator=(const Process &) = delete;

    // `exe` is a path (see findExecutable); args exclude argv[0]. False when
    // it could not be started.
    bool    start(const std::string &exe, const std::vector<std::string> &args);
    // Polls (never blocks): true until the child has exited (and reaps it).
    bool    running();
    // force = false: SIGTERM to the group (Windows has no gentle signal for
    // a GUI app and terminates anyway); true: SIGKILL / TerminateProcess.
    void    kill(bool force);
    int64_t pid() const { return _pid; }
    // After running() saw the exit: the exit code, 128 + signal; -1 before.
    int     exitStatus() const { return _status; }

private:
    int64_t _pid    = 0;
    int     _status = -1;
#ifdef _WIN32
    void *_handle = nullptr; // process HANDLE
#endif
    bool _exited = false;
};

// A command run to completion with its output captured. BLOCKING: the UI
// thread runs it on a worker thread (app/claude/async.h). The child gets its
// own session / process group, as Process does.
struct RunOptions {
    std::string              cwd;   // "" = ours
    std::string              input; // written to its stdin, which is then closed ("" = null device)
    std::vector<std::string> env;   // "NAME=value" set on top of ours; "NAME=" unsets
    bool                     mergeStderr = true; // stderr into `output`; false = discarded
    int                      timeoutMs   = 0;    // > 0: the group is killed then (timedOut)
};
struct RunResult {
    bool        started  = false; // false: it could not be started at all
    bool        timedOut = false;
    int         code     = -1; // exit code, 128 + signal; -1 when it never ran
    std::string output;        // stdout (+ stderr when merged), as written; !started: why
};
RunResult run(const std::string &exe, const std::vector<std::string> &args, const RunOptions &o);

// Our environment with `overrides` applied ("NAME=value" sets, "NAME="
// unsets; the last word on a name wins).
#ifdef _WIN32
// As CreateProcessW takes it (CREATE_UNICODE_ENVIRONMENT): "K=V\0" strings
// ending in an empty one; names compare case-insensitively, as Windows does.
std::wstring envBlock(const std::vector<std::string> &overrides);
// Appends `arg` to a command line (a space first unless it is empty), quoted
// so CommandLineToArgvW reads it back as one argument.
void         appendQuoted(std::wstring &cmd, const std::wstring &arg);
#else
// As execve takes it once made into pointers: "K=V" strings.
std::vector<std::string> mergedEnv(const std::vector<std::string> &overrides);
#endif

// The full path of `name` found on $PATH (Windows: also tries ".exe"); ""
// when absent. A name containing a separator is checked as is.
std::string findExecutable(std::string_view name);
// getenv as a string ("" when unset).
std::string env(const char *name);
// The user's home directory: $HOME, on Windows %USERPROFILE% with '/'
// separators; "" when unset.
std::string homeDir();

// A test process: one the test harness marked (base::test, before main —
// for good, whatever a test does to the environment), or a child of one
// (MSGA_TEST_ISOLATED). It never touches the user's keychain or native
// settings store (secret.h, old_settings.h).
bool testProcess();
void markTestProcess();

// A process whose stores are throwaway: a test process, or one marked here
// (a demo run, whose HOME and XDG dirs are temporary). Its credentials and
// settings go to the INI file under $XDG_CONFIG_HOME, never the user's
// keychain or native settings store (secret.h, old_settings.h).
bool isolatedStorage();
void markIsolatedStorage();

// The running executable's absolute path ("" when the OS won't say).
std::string executablePath();
// A restart: once the app has exited — main
// returned, the window, the tray and the single-instance claim gone — the
// same executable starts again with `args` (argv without argv[0]): exec on
// Linux and macOS (an updated binary is the new one), a new process on
// Windows. Call before quitting; a later call replaces the arguments.
void        relaunchOnExit(std::vector<std::string> args);

} // namespace base

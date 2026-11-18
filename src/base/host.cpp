#include "base/host.h"

#include "base/file.h"
#include "base/process.h"
#include "base/str.h"
#include "base/time.h"

#ifdef MSGA_HOST_EXEC
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <unistd.h>
#endif

namespace base::host {

#ifdef MSGA_HOST_EXEC

namespace {

std::string g_testSpawner; // set by tests only, before they spawn anything

} // namespace

bool sandboxed() {
    if (!g_testSpawner.empty())
        return true;
    static const bool inFlatpak = ::access("/.flatpak-info", F_OK) == 0;
    return inFlatpak;
}

void setSpawnerForTest(std::string spawner) {
    g_testSpawner = std::move(spawner);
}

Command wrap(
    const std::string              &exe,
    const std::vector<std::string> &args,
    const std::string              &cwd,
    const std::vector<std::string> &env,
    int                             pidFd,
    const std::vector<int>         &forwardFds
) {
    Command c;
    c.exe  = g_testSpawner.empty() ? std::string("/usr/bin/flatpak-spawn") : g_testSpawner;
    // --watch-bus: what we started ends with us, never left running on the host.
    c.args = {"--host", "--watch-bus"};
    const std::string dir = cwd.empty() ? homeDir() : cwd;
    if (!dir.empty())
        c.args.push_back(str::concat({"--directory=", dir}));
    for (const std::string &e : env) {
        const size_t eq = e.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        if (eq + 1 == e.size())
            c.args.push_back(str::concat({"--unset-env=", std::string_view(e).substr(0, eq)}));
        else
            c.args.push_back(str::concat({"--env=", e}));
    }
    for (const int fd : forwardFds)
        c.args.push_back(str::concat({"--forward-fd=", str::number(fd)}));
    if (pidFd >= 0) {
        const std::string n = str::number(pidFd);
        c.args.push_back(str::concat({"--forward-fd=", n}));
        // exec keeps the pid: the one written is the program's. Its copy of
        // the pipe is closed first, so the pipe ends with the line.
        c.args.insert(
            c.args.end(),
            {"/bin/sh",
             "-c",
             str::concat({"echo $$ >&", n, "; exec ", n, ">&-; exec \"$@\""}),
             "sh"}
        );
    }
    c.args.push_back(exe);
    c.args.insert(c.args.end(), args.begin(), args.end());
    return c;
}

int64_t readPid(int fd, int timeoutMs) {
    std::string   line;
    const int64_t deadline = monotonicMs() + timeoutMs;
    while (line.find('\n') == std::string::npos) {
        const int64_t left = deadline - monotonicMs();
        if (left <= 0)
            return 0;
        pollfd    p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, int(left));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return 0;
        char          buf[32];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0; // ended without a pid: flatpak-spawn failed
        line.append(buf, size_t(n));
        if (line.size() > 64)
            return 0;
    }
    int64_t pid = 0;
    for (const char ch : line) {
        if (ch < '0' || ch > '9')
            break;
        pid = pid * 10 + (ch - '0');
    }
    return pid;
}

RunResult sh(const std::string &script, const std::vector<std::string> &args) {
    Command c = wrap("/bin/sh", {}, {}, {}, -1);
    c.args.insert(c.args.end(), {"-c", script, "sh"});
    c.args.insert(c.args.end(), args.begin(), args.end());
    RunOptions o;
    o.here        = true; // it is flatpak-spawn that runs here
    o.mergeStderr = false;
    o.timeoutMs   = 10'000;
    return run(c.exe, c.args, o);
}

bool signal(int64_t pid, int sig, bool group) {
    if (pid <= 1)
        return false;
    const char *name = nullptr;
    switch (sig) {
    case SIGTERM:
        name = "TERM";
        break;
    case SIGKILL:
        name = "KILL";
        break;
    case SIGINT:
        name = "INT";
        break;
    case SIGHUP:
        name = "HUP";
        break;
    default:
        return false;
    }
    // "-s NAME --": the form dash's kill takes a negative (group) pid in;
    // BusyBox's rejects the "--" and takes it without.
    const std::string target = group ? str::concat({"-", str::number(pid)}) : str::number(pid);
    const RunResult   r =
        sh("kill -s \"$1\" -- \"$2\" 2>/dev/null || kill -s \"$1\" \"$2\"", {name, target});
    return r.started && !r.timedOut && r.code == 0;
}

std::string selfCommand() {
    if (::access("/.flatpak-info", F_OK) != 0)
        return executablePath(); // not in a sandbox (or a test standing in for one)
    std::string info;
    file::readAll("/.flatpak-info", &info);
    return exportedLauncher(info);
}

#else // no Flatpak here: never sandboxed, so none of this is reached

bool sandboxed() {
    return false;
}

void setSpawnerForTest(std::string) {}

Command wrap(
    const std::string              &exe,
    const std::vector<std::string> &args,
    const std::string &,
    const std::vector<std::string> &,
    int,
    const std::vector<int> &
) {
    return {exe, args};
}

int64_t readPid(int, int) {
    return 0;
}

RunResult sh(const std::string &, const std::vector<std::string> &) {
    return {};
}

bool signal(int64_t, int, bool) {
    return false;
}

std::string selfCommand() {
    return executablePath();
}

#endif

std::string exportedLauncher(std::string_view flatpakInfo) {
    // An ini file: the keys looked for under their sections.
    std::string   section, name, appPath;
    str::Splitter lines(flatpakInfo, '\n');
    for (std::string_view line; lines.next(&line);) {
        line = str::trim(line);
        if (!line.empty() && line.front() == '[') {
            section = std::string(line);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = line.substr(0, eq), value = line.substr(eq + 1);
        if (section == "[Application]" && key == "name")
            name = std::string(value);
        else if (section == "[Instance]" && key == "app-path")
            appPath = std::string(value);
    }
    if (name.empty() || appPath.empty())
        return {};
    const size_t at = appPath.find(str::concat({"/app/", name, "/"}));
    if (at == std::string::npos || at == 0)
        return {}; // a build directory (flatpak-builder --run): nothing exported
    return str::concat({std::string_view(appPath).substr(0, at), "/exports/bin/", name});
}

} // namespace base::host

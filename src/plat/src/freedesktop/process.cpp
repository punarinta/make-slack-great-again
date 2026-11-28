#include "freedesktop/process.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace plat::linux_process {

std::string_view defaultSearchPath() {
    const char *path = std::getenv("PATH");
    return path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
}

std::string findExecutable(const char *name, std::string_view rest) {
    while (!rest.empty()) {
        const size_t colon = rest.find(':');
        std::string  dir(rest.substr(0, colon));
        rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
        if (dir.empty())
            continue;
        const std::string full = dir + "/" + name;
        struct stat       st{};
        if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            ::access(full.c_str(), X_OK) == 0)
            return full;
    }
    return {};
}

void closeFd(int &fd) {
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

bool spawn(
    const std::string              &exe,
    const std::vector<std::string> &args,
    unsigned                        pipes,
    Child                          *c,
    const char                     *argv0
) {
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    if ((pipes & kStdin) && ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, in) != 0)
        return false;
    if ((pipes & kStdout) && ::pipe2(out, O_CLOEXEC) != 0) {
        closeFd(in[0]), closeFd(in[1]);
        return false;
    }
    if ((pipes & kStderr) && ::pipe2(err, O_CLOEXEC) != 0) {
        closeFd(in[0]), closeFd(in[1]), closeFd(out[0]), closeFd(out[1]);
        return false;
    }
    // dup2 clears CLOEXEC on the child's copy.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (in[1] >= 0)
        posix_spawn_file_actions_adddup2(&fa, in[1], 0);
    else
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out[1] >= 0)
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    else
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (err[1] >= 0)
        posix_spawn_file_actions_adddup2(&fa, err[1], 2);
    else
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    // A clean signal state: our blocked/ignored signals are not the child's.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, dfl;
    sigemptyset(&none);
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGPIPE);
    sigaddset(&dfl, SIGTERM);
    sigaddset(&dfl, SIGINT);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &dfl);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<char *> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char *>(argv0 ? argv0 : exe.c_str()));
    for (const auto &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    pid_t     pid = -1;
    const int rc  = ::posix_spawn(&pid, exe.c_str(), &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    closeFd(in[1]), closeFd(out[1]), closeFd(err[1]);
    if (rc != 0) {
        closeFd(in[0]), closeFd(out[0]), closeFd(err[0]);
        return false;
    }
    for (int fd : {in[0], out[0], err[0]})
        if (fd >= 0)
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    *c = {pid, in[0], out[0], err[0]};
    return true;
}

void kill(Child &c) {
    if (c.pid > 0) {
        ::kill(c.pid, SIGKILL);
        while (::waitpid(c.pid, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
    c.pid = -1;
    closeFd(c.in), closeFd(c.out), closeFd(c.err);
}

bool reaped(Child &c, bool *ok) {
    if (c.pid <= 0)
        return true;
    int         st = 0;
    const pid_t r  = ::waitpid(c.pid, &st, WNOHANG);
    if (r == 0 || (r < 0 && errno == EINTR))
        return false;
    *ok   = r == c.pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    c.pid = -1;
    return true;
}

} // namespace plat::linux_process

#include "freedesktop/posix_app.h"

#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

namespace plat::linux_services {

bool PosixServicesApp::openUrl(std::string_view url) {
    // Through a shell that backgrounds xdg-open and exits at once: we reap
    // the shell right here and the opener is re-parented to init, so no
    // zombie, no SIGCHLD handling in a library, and no blocking on a browser
    // that xdg-open runs in the foreground. The URL is $1, never interpolated
    // into the script.
    const std::string u(url);
    const char       *argv[] = {
        "/bin/sh", "-c", "xdg-open \"$1\" >/dev/null 2>&1 &", "sh", u.c_str(), nullptr
    };
    pid_t pid = 0;
    if (posix_spawn(&pid, "/bin/sh", nullptr, nullptr, const_cast<char **>(argv), environ) != 0)
        return false;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace plat::linux_services

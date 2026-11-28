#include "support/fake_llm_server.h"

#include "plat/plat.h"

#include <chrono>

#ifndef _WIN32
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fakellm {

namespace {

std::string g_base;

#ifndef _WIN32
pid_t g_pid   = -1;
int   g_stdin = -1;

void stop() {
    if (g_stdin >= 0)
        close(g_stdin); // fake_llm.py exits at EOF on stdin
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        waitpid(g_pid, nullptr, 0);
    }
}
#endif

} // namespace

const std::string &server() {
#ifndef _WIN32
    if (!g_base.empty() || g_pid == 0)
        return g_base;
    g_pid = 0; // tried
    int toChild[2], fromChild[2];
    if (pipe(toChild) != 0 || pipe(fromChild) != 0)
        return g_base;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, toChild[0], 0);
    posix_spawn_file_actions_adddup2(&fa, fromChild[1], 1);
    posix_spawn_file_actions_addclose(&fa, toChild[1]);
    posix_spawn_file_actions_addclose(&fa, fromChild[0]);
    char      py[] = "python3", script[] = FAKE_LLM_SCRIPT;
    char     *argv[] = {py, script, nullptr};
    const int rc     = posix_spawnp(&g_pid, "python3", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(toChild[0]);
    close(fromChild[1]);
    if (rc != 0) {
        g_pid = 0;
        close(toChild[1]);
        close(fromChild[0]);
        return g_base;
    }
    g_stdin = toChild[1];
    std::atexit(stop);
    std::string out;
    char        buf[128];
    while (out.find("READY\n") == std::string::npos) {
        const ssize_t n = read(fromChild[0], buf, sizeof buf);
        if (n <= 0)
            break;
        out.append(buf, size_t(n));
    }
    close(fromChild[0]);
    int port = 0;
    if (std::sscanf(out.c_str(), "PORT %d", &port) == 1 && port > 0)
        g_base = "http://127.0.0.1:" + std::to_string(port);
#endif
    return g_base;
}

net::Response ctl(plat::App &app, const char *method, const std::string &path, std::string body) {
    net::Client   c(app);
    net::Request  req;
    net::Response out;
    bool          done = false;
    req.method         = method;
    req.url            = server() + path;
    req.body           = std::move(body);
    req.headers.push_back({"Content-Type", "application/json"});
    c.send(std::move(req), [&](net::Response r) {
        out  = std::move(r);
        done = true;
    });
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done && std::chrono::steady_clock::now() < end)
        app.pump(10);
    return out;
}

void script(plat::App &app, const char *path, const char *responsesJson) {
    ctl(app,
        "POST",
        "/_ctl/script",
        std::string("{\"path\":\"") + path + "\",\"responses\":" + responsesJson + "}");
}

} // namespace fakellm

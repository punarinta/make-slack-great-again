// base/process.h's children over the port contract (MSGA_PORT builds). A
// port without process spawn (PORT_CAP_PROCESS clear) makes start() and
// run() fail cleanly, and the features that need a child say so.
#include "base/process.h"

#include "base/time.h"

#include "port/port.h"

#include <algorithm>

namespace base {

namespace {

std::vector<const char *> cArgs(const std::vector<std::string> &args) {
    std::vector<const char *> out;
    out.reserve(args.size());
    for (const auto &a : args)
        out.push_back(a.c_str());
    return out;
}

} // namespace

bool Process::start(const std::string &exe, const std::vector<std::string> &args) {
    const std::vector<const char *> argv = cArgs(args);
    port_spawn_desc                 d{};
    d.size       = sizeof d;
    d.exe        = exe.c_str();
    d.argv       = argv.data();
    d.argc       = argv.size();
    // Output to the null device, a group of its own (see process.h).
    d.flags      = PORT_SPAWN_NEW_GROUP;
    port_proc *p = nullptr;
    if (port_proc_spawn(&d, &p) != PORT_OK)
        return false;
    port_proc_release(static_cast<port_proc *>(_handle));
    _handle = p;
    _pid    = port_proc_pid(p);
    _exited = false;
    _status = -1;
    return true;
}

bool Process::running() {
    if (!_handle || _exited)
        return false;
    int code = -1;
    if (port_proc_wait(static_cast<port_proc *>(_handle), 0, &code) == PORT_OK)
        _exited = true, _status = code;
    return !_exited;
}

void Process::kill(bool force) {
    if (running())
        port_proc_kill(static_cast<port_proc *>(_handle), force ? 1 : 0);
}

std::vector<std::string> mergedEnv(const std::vector<std::string> &overrides) {
    auto                     name = [](std::string_view kv) { return kv.substr(0, kv.find('=')); };
    std::vector<std::string> out;
    struct Ctx {
        const std::vector<std::string> *overrides;
        std::vector<std::string>       *out;
    } ctx{&overrides, &out};
    port_env_list(
        [](void *user, const char *kv) {
            auto            *c = static_cast<Ctx *>(user);
            std::string_view v(kv);
            for (const auto &o : *c->overrides)
                if (o.substr(0, o.find('=')) == v.substr(0, v.find('=')))
                    return;
            c->out->emplace_back(v);
        },
        &ctx
    );
    for (size_t i = 0; i < overrides.size(); ++i) {
        const std::string     &o = overrides[i];
        const std::string_view n = name(o);
        if (n.empty() || n.size() + 1 >= o.size())
            continue;       // no name, or "NAME=": unset
        bool later = false; // the last word on a name wins
        for (size_t j = i + 1; j < overrides.size(); ++j)
            later = later || name(overrides[j]) == n;
        if (!later)
            out.push_back(o);
    }
    return out;
}

RunResult run(const std::string &exe, const std::vector<std::string> &args, const RunOptions &o) {
    RunResult                       r;
    const bool                      feed = !o.input.empty();
    const std::vector<const char *> argv = cArgs(args);
    const std::vector<const char *> envv = cArgs(o.env);
    port_spawn_desc                 d{};
    d.size  = sizeof d;
    d.exe   = exe.c_str();
    d.argv  = argv.data();
    d.argc  = argv.size();
    d.cwd   = o.cwd.empty() ? nullptr : o.cwd.c_str();
    d.env   = envv.data();
    d.envc  = envv.size();
    d.flags = PORT_SPAWN_STDOUT_PIPE | PORT_SPAWN_NEW_GROUP | PORT_SPAWN_SEARCH_PATH |
              (o.mergeStderr ? PORT_SPAWN_STDERR_MERGE : 0) | (feed ? PORT_SPAWN_STDIN_PIPE : 0);
    port_proc *p  = nullptr;
    const int  rc = port_proc_spawn(&d, &p);
    if (rc != PORT_OK) {
        r.output = port_strerror(rc);
        return r;
    }
    r.started = true;

    const int64_t deadline = o.timeoutMs > 0 ? monotonicMs() + o.timeoutMs : 0;
    auto   left    = [&]() -> int64_t { return deadline ? deadline - monotonicMs() : INT64_MAX; };
    size_t written = 0;
    bool   inOpen = feed, outOpen = true, exited = false;
    int    code = -1;
    char   buf[8192];
    // Its output until it ends — or until the child has exited: a process it
    // left running in the background may hold the pipe open for good. The
    // input goes in alongside, a pipe's worth at a time, so neither side
    // waits on the other.
    while (outOpen && !exited && left() > 0) {
        if (inOpen) {
            const int64_t w =
                port_proc_write(p, o.input.data() + written, o.input.size() - written, 0);
            if (w > 0)
                written += size_t(w);
            if ((w < 0 && w != PORT_E_TIMEOUT && w != PORT_E_AGAIN) || written == o.input.size()) {
                port_proc_close_stdin(p); // the end of its input (or it closed its end)
                inOpen = false;
            }
        }
        const int64_t n = port_proc_read(
            p, PORT_STDOUT, buf, sizeof buf, int32_t(std::min<int64_t>(left(), inOpen ? 5 : 50))
        );
        if (n > 0)
            r.output.append(buf, size_t(n));
        else if (n == 0 || (n < 0 && n != PORT_E_TIMEOUT && n != PORT_E_AGAIN))
            outOpen = false;
        exited = port_proc_wait(p, 0, &code) == PORT_OK;
    }
    if (inOpen)
        port_proc_close_stdin(p);
    // The output has ended: the exit follows within moments.
    if (!exited && left() > 0)
        exited = port_proc_wait(p, int32_t(std::min<int64_t>(left(), INT32_MAX)), &code) == PORT_OK;
    if (!exited) {
        r.timedOut = true;
        port_proc_kill(p, 1);
        port_proc_wait(p, -1, &code);
    }
    // What it wrote before it exited.
    for (int64_t n; outOpen && (n = port_proc_read(p, PORT_STDOUT, buf, sizeof buf, 0)) > 0;)
        r.output.append(buf, size_t(n));
    port_proc_release(p);
    r.code = code;
    return r;
}

} // namespace base

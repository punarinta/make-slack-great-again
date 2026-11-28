#include "freedesktop/file_dialog.h"

#include "freedesktop/process.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace plat::linux_services {

namespace {

constexpr const char *kPortal         = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath     = "/org/freedesktop/portal/desktop";
constexpr const char *kChooserIface   = "org.freedesktop.portal.FileChooser";
constexpr const char *kRequestIface   = "org.freedesktop.portal.Request";
// How long a request waits for a bus that is still saying Hello.
constexpr int         kBusWaitMs      = 2000;
// A portal's code 2 within this long of the call means its backend failed
// before showing anything; after it, a dialog was up and the user closed it.
constexpr auto        kNoDialogWindow = std::chrono::milliseconds(1000);

// "x11:1a2b" -> 6699 (kdialog --attach wants the XID in decimal).
std::string x11Xid(const std::string &parent) {
    if (parent.rfind("x11:", 0) != 0)
        return {};
    char         *end = nullptr;
    unsigned long v   = std::strtoul(parent.c_str() + 4, &end, 16);
    return v && end && !*end ? std::to_string(v) : std::string();
}

std::string joined(const std::vector<std::string> &v, const char *sep) {
    std::string out;
    for (const auto &s : v) {
        if (!out.empty())
            out += sep;
        out += s;
    }
    return out;
}

// The tools' arguments (after argv[0]).
std::vector<std::string> zenityArgs(const FileDialogDesc &d) {
    using Mode                 = FileDialogDesc::Mode;
    std::vector<std::string> a = {"--file-selection"};
    if (!d.title.empty())
        a.push_back("--title=" + d.title);
    if (d.mode == Mode::OpenMultiple) {
        a.push_back("--multiple");
        a.push_back("--separator=\n");
    }
    if (d.mode == Mode::PickFolder)
        a.push_back("--directory");
    if (d.mode == Mode::Save) {
        a.push_back("--save");
        a.push_back("--confirm-overwrite"); // a no-op warning on zenity 4, still accepted
    }
    // A trailing slash makes zenity open the folder rather than select it.
    std::string start = d.initialDir.empty() ? std::string() : d.initialDir + "/";
    if (d.mode == Mode::Save)
        start += d.suggestedName;
    if (!start.empty())
        a.push_back("--filename=" + start);
    for (const auto &f : d.filters)
        a.push_back("--file-filter=" + f.name + " | " + joined(f.patterns, " "));
    return a;
}

std::vector<std::string> kdialogArgs(const FileDialogDesc &d, const std::string &parent) {
    using Mode = FileDialogDesc::Mode;
    std::vector<std::string> a;
    if (!d.title.empty()) {
        a.push_back("--title");
        a.push_back(d.title);
    }
    if (const std::string xid = x11Xid(parent); !xid.empty()) {
        a.push_back("--attach");
        a.push_back(xid);
    }
    std::string start = d.initialDir;
    if (start.empty()) {
        const char *home = std::getenv("HOME");
        start            = home && *home ? home : "/";
    }
    std::string filter;
    for (const auto &f : d.filters) {
        if (!filter.empty())
            filter += '\n';
        filter += f.name + " (" + joined(f.patterns, " ") + ")";
    }
    switch (d.mode) {
    case Mode::Open:
    case Mode::OpenMultiple:
        a.push_back("--getopenfilename");
        a.push_back(start);
        if (!filter.empty())
            a.push_back(filter);
        if (d.mode == Mode::OpenMultiple) {
            a.push_back("--multiple");
            a.push_back("--separate-output");
        }
        break;
    case Mode::Save:
        a.push_back("--getsavefilename");
        a.push_back(start + "/" + d.suggestedName);
        if (!filter.empty())
            a.push_back(filter);
        break;
    case Mode::PickFolder:
        a.push_back("--getexistingdirectory");
        a.push_back(start);
        break;
    }
    return a;
}

FileDialogResult chosen(std::vector<std::string> paths) {
    FileDialogResult r;
    r.status =
        paths.empty() ? FileDialogResult::Status::Cancelled : FileDialogResult::Status::Chosen;
    r.paths = std::move(paths);
    return r;
}

} // namespace

FileChooser::FileChooser(BackendApp &app, Bus &bus) : _app(app), _bus(bus) {}

FileChooser::~FileChooser() {
    for (auto &[id, r] : _requests) {
        if (r.sub)
            _bus.unsubscribe(r.sub);
        if (r.timer)
            _app.cancelTimer(r.timer);
    }
    // A dialog still open when the app goes away is closed with it.
    for (auto &[id, c] : _children) {
        if (c.watch)
            _app.unwatchFd(c.watch);
        if (c.fd >= 0)
            ::close(c.fd);
        if (c.pid > 0) {
            ::kill(c.pid, SIGTERM);
            ::waitpid(c.pid, nullptr, WNOHANG);
        }
    }
}

void FileChooser::show(const FileDialogDesc &d, std::string parent, bool tools, Callback cb) {
    const uint64_t id = _next++;
    _requests[id] = {.desc = d, .parent = std::move(parent), .cb = std::move(cb), .tools = tools};
    if (_bus.ready()) {
        portal(id);
    } else if (_bus.connected()) {
        // Still saying Hello: the Request path needs our unique name.
        std::weak_ptr<int> weak = _alive;
        _requests[id].timer     = _app.addTimer(kBusWaitMs, false, [this, weak, id] {
            if (weak.expired())
                return;
            if (auto it = _requests.find(id); it != _requests.end()) {
                it->second.timer = 0;
                fallback(id);
            }
        });
    } else {
        fallback(id);
    }
}

void FileChooser::busReady() {
    std::vector<uint64_t> waiting;
    for (auto &[id, r] : _requests)
        if (r.timer)
            waiting.push_back(id);
    for (uint64_t id : waiting) {
        _app.cancelTimer(_requests[id].timer);
        _requests[id].timer = 0;
        portal(id);
    }
}

void FileChooser::portal(uint64_t id) {
    Request &r = _requests[id];
    using Mode = FileDialogDesc::Mode;

    // Subscribe to the Response of the Request object before calling, or a
    // fast portal could answer before our match rule exists. Its path is
    // predictable from our unique name and the handle_token we choose.
    const std::string token  = "plat_" + std::to_string(::getpid()) + "_" + std::to_string(id);
    std::string       sender = _bus.uniqueName();
    if (!sender.empty() && sender[0] == ':')
        sender.erase(0, 1);
    for (char &c : sender)
        if (c == '.')
            c = '_';
    r.path = std::string(kPortalPath) + "/request/" + sender + "/" + token;

    std::weak_ptr<int> weak        = _alive;
    auto               subscribeTo = [this, weak, id](const std::string &path) {
        return _bus.subscribe(
            "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" +
                path + "'",
            kRequestIface,
            "Response",
            [this, weak, id](DBusMessage *m) {
                if (!weak.expired())
                    onResponse(id, m);
            }
        );
    };
    r.sub = subscribeTo(r.path);

    r.asked         = std::chrono::steady_clock::now();
    const bool save = r.desc.mode == Mode::Save;
    auto       m = methodCall(kPortal, kPortalPath, kChooserIface, save ? "SaveFile" : "OpenFile");
    MsgWriter  w(m.get());
    w.str(r.parent);
    w.str(
        r.desc.title.empty() ? (save                              ? "Save file"
                                : r.desc.mode == Mode::PickFolder ? "Choose folder"
                                                                  : "Open file")
                             : r.desc.title
    );
    w.array("{sv}", [&](MsgWriter &o) {
        o.entry("handle_token", "s", [&](MsgWriter &v) { v.str(token); });
        o.entry("modal", "b", [&](MsgWriter &v) { v.boolean(!r.parent.empty()); });
        if (r.desc.mode == Mode::OpenMultiple)
            o.entry("multiple", "b", [](MsgWriter &v) { v.boolean(true); });
        if (r.desc.mode == Mode::PickFolder) // FileChooser v3; older portals pick a file
            o.entry("directory", "b", [](MsgWriter &v) { v.boolean(true); });
        if (save && !r.desc.suggestedName.empty())
            o.entry("current_name", "s", [&](MsgWriter &v) { v.str(r.desc.suggestedName); });
        if (!r.desc.initialDir.empty()) {
            // A NUL-terminated byte string: paths need not be UTF-8.
            o.entry("current_folder", "ay", [&](MsgWriter &v) {
                v.bytes(
                    reinterpret_cast<const uint8_t *>(r.desc.initialDir.c_str()),
                    r.desc.initialDir.size() + 1
                );
            });
        }
        if (!r.desc.filters.empty()) {
            o.entry("filters", "a(sa(us))", [&](MsgWriter &v) {
                v.array("(sa(us))", [&](MsgWriter &list) {
                    for (const auto &f : r.desc.filters) {
                        list.structure([&](MsgWriter &s) {
                            s.str(f.name);
                            s.array("(us)", [&](MsgWriter &pats) {
                                for (const auto &p : f.patterns)
                                    pats.structure([&](MsgWriter &e) {
                                        e.u32(0); // 0 = glob, 1 = MIME type
                                        e.str(p);
                                    });
                            });
                        });
                    }
                });
            });
        }
    });

    _bus.call(
        m, [this, weak, id, subscribeTo](DBusMessage *reply, const char *errName, const char *) {
            if (weak.expired())
                return;
            auto it = _requests.find(id);
            if (it == _requests.end())
                return; // answered already
            Request    &r = it->second;
            std::string handle;
            if (!reply || !MsgReader(reply).str(&handle)) {
                // No portal, or one without a FileChooser backend (wlr-only
                // setups): the fallback decides.
                if (r.sub)
                    _bus.unsubscribe(r.sub);
                r.sub = 0;
                (void)errName;
                fallback(id);
                return;
            }
            if (handle != r.path) {
                // Portals before 0.9 ignore handle_token; follow the real path.
                _bus.unsubscribe(r.sub);
                r.path = handle;
                r.sub  = subscribeTo(handle);
            }
        }
    );
}

void FileChooser::onResponse(uint64_t id, DBusMessage *m) {
    auto it = _requests.find(id);
    if (it == _requests.end())
        return;
    const char *path = dbus_message_get_path(m);
    if (!path || it->second.path != path)
        return;
    MsgReader r(m);
    uint32_t  response = 2;
    r.u32(&response);
    std::vector<std::string> paths;
    // 0 = success, 1 = cancelled by the user, 2 = ended some other way: the
    // backend failed (nothing was shown, answered at once) or, on GNOME/GTK,
    // the dialog was closed with Escape or its close button (a cancel).
    if (response == 2 && std::chrono::steady_clock::now() - it->second.asked < kNoDialogWindow) {
        finish(id, {.status = FileDialogResult::Status::Unavailable});
        return;
    }
    if (response == 0) {
        MsgReader results = r.enter();
        while (results.type() == DBUS_TYPE_DICT_ENTRY) {
            MsgReader   e = results.enter();
            std::string key;
            e.str(&key);
            std::vector<std::string> uris;
            if (key != "uris" || !e.value().strings(&uris))
                continue;
            for (const auto &u : uris) {
                // Only local files are paths; anything else (a GVfs
                // sftp:// pick) cannot be handed back as one.
                std::string p = pathFromFileUri(u);
                if (!p.empty())
                    paths.push_back(std::move(p));
            }
        }
    }
    finish(id, chosen(std::move(paths)));
}

void FileChooser::fallback(uint64_t id) {
    const char                                *env  = std::getenv("PLAT_FILE_DIALOG_FALLBACK");
    std::string_view                           mode = env ? env : "";
    std::vector<std::pair<const char *, bool>> order; // tool, is kdialog
    if (mode == "0" || (mode.empty() && !_requests[id].tools)) {
        // Disabled, or the app shows its own chooser.
    } else if (mode == "zenity") {
        order = {{"zenity", false}};
    } else if (mode == "kdialog") {
        order = {{"kdialog", true}};
    } else {
        const char *de  = std::getenv("XDG_CURRENT_DESKTOP");
        const bool  kde = de && std::strstr(de, "KDE");
        order           = kde ? decltype(order){{"kdialog", true}, {"zenity", false}}
                              : decltype(order){{"zenity", false}, {"kdialog", true}};
    }
    for (auto [name, kdialog] : order) {
        const std::string tool =
            linux_process::findExecutable(name, linux_process::defaultSearchPath());
        if (!tool.empty() && spawn(id, tool, kdialog))
            return;
    }
    finish(id, {.status = FileDialogResult::Status::Unavailable});
}

bool FileChooser::spawn(uint64_t id, const std::string &tool, bool kdialog) {
    const Request           &r    = _requests[id];
    std::vector<std::string> args = kdialog ? kdialogArgs(r.desc, r.parent) : zenityArgs(r.desc);
    linux_process::Child     p;
    if (!linux_process::spawn(
            tool, args, linux_process::kStdout, &p, kdialog ? "kdialog" : "zenity"
        ))
        return false;
    Child &c                = _children[id];
    c                       = {.request = id, .pid = p.pid, .fd = p.out, .kdialog = kdialog};
    std::weak_ptr<int> weak = _alive;
    c.watch                 = _app.watchFd(p.out, FdRead, [this, weak, id](uint32_t) {
        if (!weak.expired())
            childReadable(id);
    });
    return true;
}

void FileChooser::childReadable(uint64_t id) {
    auto it = _children.find(id);
    if (it == _children.end())
        return;
    Child &c = it->second;
    char   buf[4096];
    for (;;) {
        const ssize_t n = ::read(c.fd, buf, sizeof buf);
        if (n > 0) {
            c.out.append(buf, size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return; // more later
        break;      // EOF (the dialog closed) or a read error
    }
    _app.unwatchFd(c.watch);
    c.watch = 0;
    ::close(c.fd);
    c.fd = -1;
    reap(id, 0);
}

// The child closed stdout, so it is exiting; poll for its status instead of
// blocking the loop on waitpid.
void FileChooser::reap(uint64_t id, int attempt) {
    auto it = _children.find(id);
    if (it == _children.end())
        return;
    Child &c      = it->second;
    int    status = 0;
    pid_t  got    = ::waitpid(c.pid, &status, WNOHANG);
    if (got == 0) {
        if (attempt == 100) // 2 s after closing stdout: it is not coming back
            ::kill(c.pid, SIGKILL);
        std::weak_ptr<int> weak = _alive;
        _app.addTimer(20, false, [this, weak, id, attempt] {
            if (!weak.expired())
                reap(id, attempt + 1);
        });
        return;
    }
    // Exit 0 = chosen; 1 = cancelled; anything else = it failed.
    // ECHILD: the app ignores SIGCHLD, so the status is gone; trust the output
    // (a cancelled zenity/kdialog prints nothing).
    const bool ok = got == c.pid ? WIFEXITED(status) && WEXITSTATUS(status) == 0 : errno == ECHILD;
    // Exit 1 is the tools' cancel; a crash or another code (no display, bad
    // arguments) means no dialog was shown.
    const bool cancelled = got == c.pid && WIFEXITED(status) && WEXITSTATUS(status) == 1;
    std::vector<std::string> paths;
    if (ok) {
        size_t start = 0;
        while (start < c.out.size()) {
            size_t end = c.out.find('\n', start);
            if (end == std::string::npos)
                end = c.out.size();
            std::string line = c.out.substr(start, end - start);
            if (!line.empty() && line[0] == '/')
                paths.push_back(std::move(line));
            start = end + 1;
        }
    }
    _children.erase(it);
    if (!ok && !cancelled)
        finish(id, {.status = FileDialogResult::Status::Unavailable});
    else
        finish(id, chosen(std::move(paths)));
}

void FileChooser::finish(uint64_t id, FileDialogResult res) {
    auto it = _requests.find(id);
    if (it == _requests.end())
        return;
    if (it->second.sub)
        _bus.unsubscribe(it->second.sub);
    if (it->second.timer)
        _app.cancelTimer(it->second.timer);
    Callback cb = std::move(it->second.cb);
    _requests.erase(it);
    // Posted: the contract says never re-entrant, and we may be inside a
    // D-Bus dispatch here.
    std::weak_ptr<int> weak = _alive;
    _app.post([weak, cb = std::move(cb), res = std::move(res)]() mutable {
        if (!weak.expired() && cb)
            cb(std::move(res));
    });
}

} // namespace plat::linux_services

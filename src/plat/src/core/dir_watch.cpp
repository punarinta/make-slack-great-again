// App::watchDirectory's default: inotify on Linux, read through the
// backend's own watchFd, so every POSIX loop (Wayland, X11, msga's relay
// loop) gets it without code of its own. Where watchFd has no fds (the
// headless test backend) a timer reads it instead. Elsewhere: not watched
// (the caller looks on a timer); a port build's adapter overrides it.
#include "plat/plat.h"

#include <algorithm>
#include <utility>

#if defined(__linux__) && !defined(PLAT_PORT)
#include <sys/inotify.h>
#include <unistd.h>
#define PLAT_INOTIFY 1
#endif

namespace plat {

#ifdef PLAT_INOTIFY

struct App::DirWatches {
    struct Watch {
        uint64_t                               id;
        int                                    wd;
        std::function<void(const DirChange &)> fn;
    };
    int                fd      = -1; // -2: inotify can't be had
    uint64_t           fdWatch = 0, timer = 0, nextId = 0;
    std::vector<Watch> watches;

    ~DirWatches() {
        if (fd >= 0)
            ::close(fd);
    }
};

namespace {
constexpr uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB |
                           IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
constexpr uint32_t kCameOrWent =
    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
} // namespace

App::App()  = default;
App::~App() = default;

uint64_t App::watchDirectory(std::string_view path, std::function<void(const DirChange &)> fn) {
    if (!_dirWatchState)
        _dirWatchState = std::make_unique<DirWatches>();
    DirWatches &w = *_dirWatchState;
    if (w.fd == -2)
        return 0;
    if (w.fd == -1) {
        w.fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (w.fd < 0) {
            w.fd = -2;
            return 0;
        }
    }
    const int wd = inotify_add_watch(w.fd, std::string(path).c_str(), kMask);
    if (wd < 0)
        return 0; // not there (yet)
    if (!w.fdWatch && !w.timer) {
        // Everything inotify has queued, then the callbacks: each watch
        // found again by id, as one may unwatch others.
        const auto read = [this] {
            DirWatches &d = *_dirWatchState;
            struct Event {
                int         wd;
                uint32_t    mask;
                std::string name;
            };
            std::vector<Event>          events;
            alignas(inotify_event) char buf[4096];
            for (;;) {
                const ssize_t n = ::read(d.fd, buf, sizeof buf);
                if (n <= 0)
                    break;
                for (ssize_t at = 0; at < n;) {
                    const auto *e = reinterpret_cast<const inotify_event *>(buf + at);
                    at += ssize_t(sizeof(inotify_event) + e->len);
                    events.push_back(
                        {e->wd, e->mask, e->len ? std::string(e->name) : std::string()}
                    );
                }
            }
            for (const Event &e : events) {
                // Lost events concern every watch; the rest, the watches on
                // that directory (one per caller that asked for it).
                std::vector<uint64_t> ids;
                for (const auto &x : d.watches)
                    if ((e.mask & IN_Q_OVERFLOW) || x.wd == e.wd)
                        ids.push_back(x.id);
                DirChange c;
                c.name       = e.name;
                c.cameOrWent = (e.mask & kCameOrWent) != 0;
                c.gone       = (e.mask & IN_IGNORED) != 0; // removed, or the folder went
                for (uint64_t id : ids) {
                    const auto it = std::find_if(d.watches.begin(), d.watches.end(), [&](auto &x) {
                        return x.id == id;
                    });
                    if (it == d.watches.end())
                        continue;
                    auto f = it->fn; // the callback may unwatch itself
                    if (c.gone)
                        d.watches.erase(it);
                    f(c);
                }
            }
        };
        w.fdWatch = watchFd(w.fd, FdRead, [read](uint32_t) { read(); });
        if (!w.fdWatch) // no fds on this loop: looked at instead
            w.timer = addTimer(250, true, read);
    }
    w.watches.push_back({++w.nextId, wd, std::move(fn)});
    return w.nextId;
}

void App::unwatchDirectory(uint64_t id) {
    if (!_dirWatchState)
        return;
    DirWatches &w  = *_dirWatchState;
    const auto  it = std::find_if(w.watches.begin(), w.watches.end(), [id](const auto &x) {
        return x.id == id;
    });
    if (it == w.watches.end())
        return;
    const int wd = it->wd;
    w.watches.erase(it);
    // inotify gives the same directory one descriptor: kept while another
    // caller watches it.
    if (std::none_of(w.watches.begin(), w.watches.end(), [wd](const auto &x) {
            return x.wd == wd;
        }))
        inotify_rm_watch(w.fd, wd);
}

#else

struct App::DirWatches {};

App::App()  = default;
App::~App() = default;

uint64_t App::watchDirectory(std::string_view, std::function<void(const DirChange &)>) {
    return 0;
}

void App::unwatchDirectory(uint64_t) {}

#endif

} // namespace plat

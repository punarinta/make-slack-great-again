// Single-instance channel and URL-scheme registration for both Linux
// backends (the XDG directories are in xdg_dirs.cpp).
//
// Single instance: the primary listens on a Unix stream socket named after
// the key in a per-user 0700 directory ($XDG_RUNTIME_DIR, else a private
// /tmp/plat-<uid>). A later launch connects, sends one length-prefixed
// message (cwd, activation token, argv) and waits for a one-byte ack, so it
// never exits before the primary has the message. A filesystem socket rather
// than an abstract one: abstract names are per network namespace and carry no
// permissions, so any local user could connect or squat on the name. Under
// Flatpak the socket goes to $XDG_RUNTIME_DIR/app/$FLATPAK_ID, the one runtime
// directory every instance of the same app shares.
//
// Who is primary is decided under an flock() on a lock file next to the
// socket: "connect refused → unlink the stale socket → bind" is otherwise a
// race in which two simultaneous launches can both end up primary.
#include "freedesktop/instance.h"

#include "core/hash.h"
#include "core/strings.h"
#include "core/wire.h"
#include "freedesktop/files.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <optional>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h> // timeval (glibc's sys/socket.h brings it, musl's doesn't)
#include <sys/un.h>
#include <unistd.h>

namespace plat::linux_instance {

namespace {

// The uid of the process at the other end of a Unix socket, or false.
// getpeereid is the BSDs' (and musl's); glibc has only SO_PEERCRED.
bool peerUid(int fd, uid_t *uid) {
#if defined(SO_PEERCRED) && defined(__linux__)
    ucred     cr{};
    socklen_t len = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0)
        return false;
    *uid = cr.uid;
    return true;
#else
    gid_t gid;
    return getpeereid(fd, uid, &gid) == 0;
#endif
}

constexpr char     kMagic[4]    = {'P', 'L', 'A', 'T'};
constexpr uint32_t kVersion     = 1;
constexpr size_t   kHeader      = 12;      // magic, version, payload length
constexpr size_t   kMaxPayload  = 1 << 20; // argv of a launch, generously
constexpr int      kSecondaryMs = 3000;    // send + ack budget for a later launch
constexpr int      kConnIdleMs  = 5000;    // primary drops a client that stalls
constexpr size_t   kMaxConns    = 16;
constexpr char     kAck         = 'A';

// Schemes registerUrlScheme() was called for in this process (lower case).
std::vector<std::string> &schemes() {
    static std::vector<std::string> s;
    return s;
}

// ── wire format ─────────────────────────────────────────────────────────────
// header: "PLAT", u32 version, u32 payload length (little endian)
// payload: strings as u32 length + bytes — cwd, token, then each argument.
using core::getU32;
using core::putU32;

std::string
encode(const std::string &cwd, const std::string &token, const std::vector<std::string> &args) {
    std::string payload;
    core::putString(payload, cwd);
    core::putString(payload, token);
    for (auto &a : args)
        core::putString(payload, a);
    std::string msg(kMagic, 4);
    putU32(msg, kVersion);
    putU32(msg, uint32_t(payload.size()));
    return msg + payload;
}

bool decode(
    std::string_view p, std::string *cwd, std::string *token, std::vector<std::string> *args
) {
    std::vector<std::string> fields;
    while (!p.empty())
        if (!core::takeString(p, &fields.emplace_back()))
            return false;
    if (fields.size() < 2)
        return false;
    *cwd   = std::move(fields[0]);
    *token = std::move(fields[1]);
    args->assign(
        std::make_move_iterator(fields.begin() + 2), std::make_move_iterator(fields.end())
    );
    return true;
}

// ── where the socket lives ──────────────────────────────────────────────────

// A directory only we can enter: create it 0700 if missing, and refuse one
// that is a symlink, someone else's, or open to others (a pre-planted /tmp
// directory would let another user intercept our launches).
bool privateDir(const std::string &dir) {
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
        return false;
    struct stat st{};
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid())
        return false;
    if ((st.st_mode & 077) != 0 && chmod(dir.c_str(), 0700) != 0)
        return false;
    return true;
}

bool isOurDir(const char *dir) {
    struct stat st{};
    return dir && dir[0] == '/' && stat(dir, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == getuid();
}

std::string socketDir() {
    const char *rt = std::getenv("XDG_RUNTIME_DIR");
    if (isOurDir(rt)) {
        if (const char *fp = std::getenv("FLATPAK_ID"); fp && *fp) {
            const std::string app = std::string(rt) + "/app/" + fp;
            if (isOurDir(app.c_str()))
                return app;
        }
        return rt;
    }
    const std::string tmp = "/tmp/plat-" + std::to_string(getuid());
    return privateDir(tmp) ? tmp : std::string();
}

// Key → file name: safe characters only, and short enough for sun_path
// (108 bytes) even under a long runtime dir; long keys keep a hash suffix so
// two of them never collide by truncation.
std::string socketName(std::string_view key) {
    std::string s;
    for (char c : key)
        s += (std::isalnum(uint8_t(c)) || c == '.' || c == '-' || c == '_') ? c : '_';
    if (s.size() > 40) {
        const uint64_t h = core::fnv1a(key);
        char           hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
        s = s.substr(0, 23) + "-" + hex;
    }
    return "plat-" + s;
}

bool makeAddr(const std::string &path, sockaddr_un *a) {
    std::memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (path.size() >= sizeof a->sun_path)
        return false;
    std::memcpy(a->sun_path, path.c_str(), path.size());
    return true;
}

std::string currentDir() {
    std::vector<char> buf(4096);
    while (!getcwd(buf.data(), buf.size())) {
        if (errno != ERANGE || buf.size() > (1u << 20))
            return {};
        buf.resize(buf.size() * 2);
    }
    return buf.data();
}

// ── primary side ────────────────────────────────────────────────────────────

class SocketServer final : public Server {
public:
    SocketServer(BackendApp &app, int fd, std::string path)
        : _app(app), _fd(fd), _path(std::move(path)) {
        struct stat st{};
        if (stat(_path.c_str(), &st) == 0) {
            _dev = st.st_dev;
            _ino = st.st_ino;
        }
        _watch = _app.watchFd(_fd, FdRead, [this](uint32_t) { acceptAll(); });
    }

    ~SocketServer() override {
        for (auto &[fd, c] : _conns)
            drop(*c, false);
        _conns.clear();
        _app.unwatchFd(_watch);
        close(_fd);
        // Only if the name still points at our socket: a successor that took
        // over after a stale-socket check must keep its own.
        struct stat st{};
        if (_ino && stat(_path.c_str(), &st) == 0 && st.st_dev == _dev && st.st_ino == _ino)
            unlink(_path.c_str());
    }

private:
    struct Conn {
        int         fd    = -1;
        uint64_t    watch = 0;
        TimerId     timer = 0;
        std::string buf;
    };

    void acceptAll() {
        for (;;) {
            const int c = accept4(_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (c < 0)
                return; // EAGAIN, or a transient error: the next wakeup retries
            // The directory is private already; the peer check keeps a
            // mis-set $XDG_RUNTIME_DIR from opening us to other users.
            uid_t uid = 0;
            if (!peerUid(c, &uid) || uid != getuid() || _conns.size() >= kMaxConns) {
                close(c);
                continue;
            }
            auto  conn = std::make_unique<Conn>();
            Conn *p    = conn.get();
            p->fd      = c;
            p->watch   = _app.watchFd(c, FdRead, [this, c](uint32_t) { readFrom(c); });
            p->timer   = _app.addTimer(kConnIdleMs, false, [this, c] {
                if (auto it = _conns.find(c); it != _conns.end()) {
                    it->second->timer = 0;
                    drop(*it->second, true);
                }
            });
            _conns[c]  = std::move(conn);
        }
    }

    // Unwatch and close; erase = also remove it from _conns (invalidates it).
    void drop(Conn &c, bool erase) {
        _app.unwatchFd(c.watch);
        if (c.timer)
            _app.cancelTimer(c.timer);
        const int fd = c.fd;
        close(fd);
        if (erase)
            _conns.erase(fd);
    }

    void readFrom(int fd) {
        auto it = _conns.find(fd);
        if (it == _conns.end())
            return;
        Conn &c   = *it->second;
        bool  eof = false;
        char  chunk[4096];
        for (;;) {
            const ssize_t n = read(fd, chunk, sizeof chunk);
            if (n > 0) {
                c.buf.append(chunk, size_t(n));
                if (c.buf.size() > kHeader + kMaxPayload)
                    break;
                continue;
            }
            if (n == 0)
                eof = true;
            else if (errno == EINTR)
                continue;
            else if (errno != EAGAIN && errno != EWOULDBLOCK)
                eof = true;
            break;
        }
        if (c.buf.size() >= kHeader) {
            const bool good =
                std::memcmp(c.buf.data(), kMagic, 4) == 0 && getU32(c.buf.data() + 4) == kVersion;
            const uint32_t len = getU32(c.buf.data() + 8);
            if (!good || len > kMaxPayload) {
                drop(c, true);
                return;
            }
            if (c.buf.size() >= kHeader + len) {
                std::string              cwd, token;
                std::vector<std::string> args;
                const bool               ok =
                    decode(std::string_view(c.buf).substr(kHeader, len), &cwd, &token, &args);
                if (ok) {
                    // Ack before emitting: the handler may run long, and the
                    // other launch only waits to know we have the message.
                    const char a = kAck;
                    (void)!send(fd, &a, 1, MSG_NOSIGNAL);
                }
                drop(c, true);
                if (ok)
                    deliver(std::move(cwd), std::move(token), std::move(args));
                return;
            }
        }
        if (eof || c.buf.size() > kHeader + kMaxPayload)
            drop(c, true);
    }

    void deliver(std::string cwd, std::string token, std::vector<std::string> args) {
        std::vector<std::string> urls = core::schemeUrls(args, schemes());
        _app.emit(
            {.type            = EventType::InstanceActivated,
             .text            = std::move(cwd),
             .strings         = std::move(args),
             .activationToken = token}
        );
        if (!urls.empty())
            _app.emit(
                {.type            = EventType::OpenUrls,
                 .strings         = std::move(urls),
                 .activationToken = std::move(token)}
            );
    }

    BackendApp                          &_app;
    int                                  _fd;
    std::string                          _path;
    dev_t                                _dev   = 0;
    ino_t                                _ino   = 0;
    uint64_t                             _watch = 0;
    std::map<int, std::unique_ptr<Conn>> _conns;
};

// ── secondary side ──────────────────────────────────────────────────────────

// Hand our launch to the primary on the connected socket; true once it
// acknowledged. Blocking with timeouts: this runs before the app does anything.
bool forward(int fd, const std::vector<std::string> &args) {
    timeval tv{kSecondaryMs / 1000, (kSecondaryMs % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string token;
    if (const char *t = std::getenv("XDG_ACTIVATION_TOKEN"); t && *t)
        token = t;
    else if (const char *s = std::getenv("DESKTOP_STARTUP_ID"); s && *s)
        token = s;
    const std::string msg = encode(currentDir(), token, args);
    for (size_t off = 0; off < msg.size();) {
        const ssize_t n = send(fd, msg.data() + off, msg.size() - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        off += size_t(n);
    }
    char a = 0;
    for (;;) {
        const ssize_t n = recv(fd, &a, 1, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n == 1 && a == kAck;
    }
}

// ── URL schemes ─────────────────────────────────────────────────────────────

// Desktop Entry Exec quoting: the argument goes in double quotes with ", `,
// $ and \ backslash-escaped; then the value itself is a string, whose own
// escaping doubles every backslash; % is a field-code prefix and becomes %%.
std::string execQuote(const std::string &path) {
    std::string arg = "\"";
    for (char c : path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\')
            arg += '\\';
        arg += c;
    }
    arg += '"';
    std::string out;
    for (char c : arg) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '%')
            out += "%%";
        else
            out += c;
    }
    return out;
}

using linux_files::readFile;

// `s` cut at every `sep`, std::getline-style (no empty piece after a final one).
std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    for (size_t at = 0, end; at < s.size(); at = end + 1) {
        end = s.find(sep, at);
        if (end == std::string::npos)
            end = s.size();
        out.push_back(s.substr(at, end - at));
    }
    return out;
}

using prim::file::makeDirs;

// Write via a temp file + rename, so a crash never leaves a half-written
// mimeapps.list. A symlinked file (dotfile managers) is written through.
bool writeAtomically(std::string path, const std::string &data) {
    struct stat st;
    if (lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        char *real = realpath(path.c_str(), nullptr);
        if (!real)
            return false;
        path = real;
        free(real);
    }
    return prim::file::writeAtomic(path, data);
}

std::string trim(std::string_view s) {
    return std::string(core::trim(s, " \t\r"));
}

// mimeapps.list with `mime` defaulting to `desktopId` (ours first, earlier
// choices kept after it); every other line and section is left as it was.
std::string
withDefault(const std::string &text, const std::string &mime, const std::string &desktopId) {
    std::vector<std::string> lines = split(text, '\n');
    auto                     value = [&](const std::string &old) {
        std::string v = desktopId + ";";
        for (std::string id : split(old, ';'))
            if (id = trim(id); !id.empty() && id != desktopId)
                v += id + ";";
        return v;
    };
    size_t section = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
        if (trim(lines[i]) == "[Default Applications]") {
            section = i;
            break;
        }
    if (section == lines.size()) {
        if (!lines.empty() && !trim(lines.back()).empty())
            lines.emplace_back();
        lines.push_back("[Default Applications]");
        lines.push_back(mime + "=" + value(""));
    } else {
        size_t insertAt = section + 1;
        bool   done     = false;
        for (size_t i = section + 1; i < lines.size(); ++i) {
            const std::string t = trim(lines[i]);
            if (!t.empty() && t[0] == '[')
                break;
            if (!t.empty())
                insertAt = i + 1;
            const size_t eq = t.find('=');
            if (eq != std::string::npos && trim(t.substr(0, eq)) == mime) {
                lines[i] = mime + "=" + value(t.substr(eq + 1));
                done     = true;
                break;
            }
        }
        if (!done)
            lines.insert(lines.begin() + long(insertAt), mime + "=" + value(""));
    }
    std::string out;
    for (auto &l : lines)
        out += l + "\n";
    return out;
}

} // namespace

bool claim(
    BackendApp                     &app,
    std::string_view                key,
    const std::vector<std::string> &args,
    std::unique_ptr<Server>        *server
) {
    if (*server)
        return true; // already primary in this process
    // Without a usable directory there is no channel: run as primary rather
    // than refuse to start.
    const std::string dir = socketDir();
    if (dir.empty())
        return true;
    const std::string path = dir + "/" + socketName(key) + ".sock";
    sockaddr_un       addr{};
    if (!makeAddr(path, &addr))
        return true;

    const std::string lockPath = dir + "/" + socketName(key) + ".lock";
    const int         lock     = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock >= 0)
        while (flock(lock, LOCK_EX) != 0 && errno == EINTR) {
        }
    auto unlock = [&] {
        if (lock >= 0)
            close(lock); // releases the flock
    };

    for (int attempt = 0; attempt < 3; ++attempt) {
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0)
            break;
        if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0) {
            if (listen(fd, 16) != 0) {
                close(fd);
                unlink(path.c_str());
                break;
            }
            *server = std::make_unique<SocketServer>(app, fd, path);
            unlock();
            return true;
        }
        const int bindErr = errno;
        close(fd);
        if (bindErr != EADDRINUSE)
            break;
        // The name exists: a live primary, or a socket left by a crash.
        const int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (c < 0)
            break;
        if (connect(c, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0) {
            unlock(); // the primary is up; later launches need not wait for us
            forward(c, args);
            close(c);
            // Even without an ack another instance owns the key: a second
            // primary would be worse than a lost activation.
            return false;
        }
        const int connErr = errno;
        close(c);
        if (connErr == ECONNREFUSED)
            unlink(path.c_str()); // nobody listening: stale
        else if (connErr != ENOENT)
            break; // EACCES etc.: not ours to fix
    }
    unlock();
    return true;
}

bool registerUrlScheme(BackendApp &app, std::string_view schemeIn) {
    if (!core::validScheme(schemeIn))
        return false;
    const std::string scheme = core::asciiLower(schemeIn);
    // Remembered whatever happens below: a handler may already be installed
    // (a distro package), and forwarded URLs should still reach OpenUrls.
    core::rememberScheme(schemes(), scheme);

    const std::string exePath = prim::file::executablePath();
    if (exePath.empty() || exePath.find_first_of("\n\r") != std::string::npos)
        return false;

    const std::string dataHome = standardDir(StandardDir::Data);
    const std::string cfgHome  = standardDir(StandardDir::Config);
    if (dataHome.empty() || cfgHome.empty())
        return false;

    // Desktop file ids allow [A-Za-z0-9_-.]; reverse-DNS app ids already fit.
    std::string id;
    for (char c : app.appInfo().id)
        id += (std::isalnum(uint8_t(c)) || c == '-' || c == '_' || c == '.') ? c : '_';
    std::string fileScheme = scheme; // '+' is legal in schemes, not in desktop ids
    std::replace(fileScheme.begin(), fileScheme.end(), '+', '_');
    std::string       desktopId = id + "-url-" + fileScheme + ".desktop";
    const std::string appsDir   = dataHome + "/applications";
    std::string       name      = app.appInfo().name;
    std::replace(name.begin(), name.end(), '\n', ' ');

    // The app's own launcher entry (<id>.desktop, which the app installed,
    // or its package did system-wide: the first one in the XDG data dirs is
    // the one in effect) already handles the scheme: it is the handler, and
    // no hidden one is needed (one left from before goes).
    std::string       own;
    const char       *sysDirs = std::getenv("XDG_DATA_DIRS");
    const std::string dataDirs =
        dataHome + ":" + (sysDirs && *sysDirs ? sysDirs : "/usr/local/share:/usr/share");
    for (const std::string &dir : split(dataDirs, ':'))
        if (auto text = readFile(dir + "/applications/" + id + ".desktop")) {
            own = std::move(*text);
            break;
        }
    if (own.find("x-scheme-handler/" + scheme + ";") != std::string::npos) {
        unlink((appsDir + "/" + desktopId).c_str());
        desktopId = id + ".desktop";
    } else {

        const std::string entry = "[Desktop Entry]\n"
                                  "Type=Application\n"
                                  "Name=" +
                                  name +
                                  "\n"
                                  "Exec=" +
                                  execQuote(exePath) +
                                  " %u\n"
                                  "MimeType=x-scheme-handler/" +
                                  scheme +
                                  ";\n"
                                  "NoDisplay=true\n"
                                  "Terminal=false\n";
        makeDirs(appsDir);
        const std::string entryPath = appsDir + "/" + desktopId;
        // Rewrite only on change: this runs on every launch.
        if (readFile(entryPath) != entry && !writeAtomically(entryPath, entry))
            return false;
    }

    makeDirs(cfgHome);
    const std::string listPath = cfgHome + "/mimeapps.list";
    const std::string old      = readFile(listPath).value_or("");
    const std::string updated  = withDefault(old, "x-scheme-handler/" + scheme, desktopId);
    if (updated != old && !writeAtomically(listPath, updated))
        return false;
    // No update-desktop-database: the [Default Applications] entry is read
    // directly by GIO, KIO and xdg-open; the cache only matters for
    // "what can open this type" lists.
    return true;
}

} // namespace plat::linux_instance

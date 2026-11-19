#include "app/identity.h"

#include "base/file.h"
#include "base/process.h"
#include "base/utf8.h"
#include "plat/plat.h"

#ifdef _WIN32
#include "base/winstr.h"

#include <windows.h>
#elif !defined(MSGA_PORT)
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace identity {

namespace {

std::string under(const std::string &base, const char *rel) {
    return base.empty() ? std::string() : file::join(base, rel);
}

} // namespace

const char *appId() {
#if defined(MSGA_FLATPAK)
    // The Flatpak's: its .desktop file is named by it, and the shell matches
    // the window (Wayland app_id) and the notifications to that.
    return "com.nisdos.msga";
#elif defined(MSGA_FREEDESKTOP)
    return "msga";
#else
    return "com.nisdos.msga";
#endif
}

const char *instanceKey() {
    return appId();
}

std::string configDir(plat::App &app) {
#if defined(MSGA_FREEDESKTOP)
    return under(app.standardDir(plat::StandardDir::Config), "msga");
#else
    return dataDir(app);
#endif
}

std::string dataDir(plat::App &app) {
#ifdef _WIN32
    // The data directory is in the roaming profile; plat's Data is the local one.
    return under(app.standardDir(plat::StandardDir::Config), "msga/MSGA");
#else
    return under(app.standardDir(plat::StandardDir::Data), "msga/MSGA");
#endif
}

std::string cacheDir(plat::App &app) {
#ifdef _WIN32
    return under(app.standardDir(plat::StandardDir::Cache), "msga/MSGA/cache");
#else
    return under(app.standardDir(plat::StandardDir::Cache), "msga/MSGA");
#endif
}

#if !defined(MSGA_PORT)
namespace {

// A string as the hand-off carries it: a big-endian byte length, then UTF-16BE.
std::string utf16BeString(const std::string &s) {
    std::string out(4, '\0');
    for (size_t i = 0; i < s.size();) {
        uint32_t   cp   = utf8::decode(s, i);
        const auto unit = [&out](uint32_t u) {
            out.push_back(char(u >> 8));
            out.push_back(char(u & 0xFF));
        };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 + (cp >> 10));
            unit(0xDC00 + (cp & 0x3FF));
        } else {
            unit(cp);
        }
    }
    const uint32_t n = uint32_t(out.size() - 4);
    for (int b = 0; b < 4; ++b)
        out[size_t(b)] = char(n >> (24 - 8 * b));
    return out;
}

// The home folder's name (its last path component).
std::string homeName() {
    std::string home = base::homeDir();
    while (home.size() > 1 && (home.back() == '/' || home.back() == '\\'))
        home.pop_back();
    return std::string(file::baseName(home));
}

} // namespace
#endif

bool handOffToEarlierVersion(const std::string &url) {
#if defined(MSGA_PORT)
    // No earlier version ever ran on a port's OS: second launches are the
    // port's (plat::App::claimSingleInstance, port_instance_claim).
    (void)url;
    return false;
#else
    const std::string name = "msga-" + homeName();
    const std::string data = url.empty() ? std::string() : utf16BeString(url);
#ifdef _WIN32
    const std::wstring w = base::wide("\\\\.\\pipe\\" + name);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    if (!data.empty())
        WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    return true;
#else
    // The socket: <TMPDIR, else /tmp>/<name>.
    std::string tmp = base::env("TMPDIR");
    while (tmp.size() > 1 && tmp.back() == '/')
        tmp.pop_back();
    const std::string path = (tmp.empty() ? std::string("/tmp") : tmp) + "/" + name;
    sockaddr_un       addr{};
    if (path.size() >= sizeof addr.sun_path)
        return false;
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    const int one = 1; // macOS: no MSG_NOSIGNAL
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    // A stale socket file refuses at once: no earlier version is running.
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        close(fd);
        return false;
    }
    for (size_t put = 0; put < data.size();) {
#ifdef MSG_NOSIGNAL
        const ssize_t w = send(fd, data.data() + put, data.size() - put, MSG_NOSIGNAL);
#else
        const ssize_t w = send(fd, data.data() + put, data.size() - put, 0);
#endif
        if (w <= 0)
            break;
        put += size_t(w);
    }
    // Give it a moment to read before the connection goes (it reads when
    // data arrives; a hang-up first could lose the URL).
    pollfd p{fd, POLLIN, 0};
    poll(&p, 1, 300);
    close(fd);
    return true;
#endif
#endif // MSGA_PORT
}

std::string crashLogPath(plat::App &app) {
    return under(dataDir(app), "crash.log");
}

} // namespace identity

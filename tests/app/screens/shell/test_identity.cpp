// The app's identity, shared with earlier versions (app/identity.h): its
// directories, its launcher entry, and handing a launch to a running earlier
// version. (The import of earlier versions' settings and caches is
// app/legacy, tested there.)
#include "app/auth/workspaces.h"
#include "app/identity.h"
#include "base/file.h"
#include "base/old_settings.h"
#include "base/process.h"
#include "base/str.h"
#include "support/test.h"
#include "screens/shell/desktop_entry.h"
#include "screens/shell/settings.h"
#include "ui/ui.h"

#include <cstring>
#include <memory>
#include <thread>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

[[maybe_unused]] ui::App &app() {
    static std::unique_ptr<ui::App> a = [] {
        std::string err;
        return ui::App::create(&err);
    }();
    return *a;
}

[[maybe_unused]] std::string read(const std::string &path) {
    std::string s;
    file::readAll(path, &s);
    return s;
}

} // namespace

#ifdef MSGA_FREEDESKTOP // a port build keeps none of them (no earlier version ran there)
TEST("identity: the directories earlier versions used") {
    // plat's own directories (the headless backend's in tests) + the
    // organization "msga" and application "MSGA" names under them.
    plat::App        &pa     = app().platform();
    const std::string config = pa.standardDir(plat::StandardDir::Config);
    const std::string data   = pa.standardDir(plat::StandardDir::Data);
#ifdef MSGA_FLATPAK
    CHECK_STR(identity::appId(), "com.nisdos.msga"); // the Flatpak's .desktop id
#else
    CHECK_STR(identity::appId(), "msga");
#endif
    CHECK_STR(identity::configDir(pa), config + "/msga");
    CHECK_STR(identity::dataDir(pa), data + "/msga/MSGA");
    CHECK_STR(identity::cacheDir(pa), pa.standardDir(plat::StandardDir::Cache) + "/msga/MSGA");
    CHECK_STR(identity::crashLogPath(pa), data + "/msga/MSGA/crash.log");
    CHECK_STR(shell::Settings::defaultPath(pa), config + "/msga/settings.json");
    CHECK_STR(auth::WorkspaceStore::defaultPath(pa), config + "/msga/workspaces.json");
    // Earlier versions' INI settings file is XDG_CONFIG_HOME/msga/msga.conf.
    CHECK_STR(oldsettings::iniPath(), base::env("XDG_CONFIG_HOME") + "/msga/msga.conf");
}
#endif

TEST("desktop entry: the launcher earlier versions installed") {
    const std::string e = shell::desktopEntry("/opt/my apps/msga", "/d/icons/msga.png");
    CHECK(e.find("Exec=\"/opt/my apps/msga\" %u\n") != std::string::npos);
    CHECK(e.find("Icon=/d/icons/msga.png\n") != std::string::npos);
    CHECK(e.find("Name=MSGA\n") != std::string::npos);
    CHECK(e.find("StartupWMClass=msga\n") != std::string::npos);
    CHECK(e.find("MimeType=x-scheme-handler/msga;\n") != std::string::npos);
    CHECK(
        shell::desktopEntry("/usr/bin/msga", "msga").find("Exec=/usr/bin/msga %u\n") !=
        std::string::npos
    );
#ifdef MSGA_FREEDESKTOP
    plat::App        &pa   = app().platform();
    const std::string data = pa.standardDir(plat::StandardDir::Data);
    const std::string path = file::join(data, "applications/msga.desktop");
    const std::string icon = file::join(data, "icons/hicolor/256x256/apps/msga.png");
    file::remove(path);
    file::remove(icon);
    shell::installDesktopEntry(pa);
#ifdef MSGA_INSTALL_LAUNCHER
    for (int i = 0; i < 500 && !file::exists(path); ++i)
        app().pump(10);
    REQUIRE(file::exists(path));
    CHECK(read(path).find("Icon=" + icon + "\n") != std::string::npos);
    CHECK(str::startsWith(read(icon), "\x89PNG"));
#else
    // A package installs the entry: the app writes none of its own.
    for (int i = 0; i < 50; ++i)
        app().pump(10);
    CHECK_FALSE(file::exists(path));
    CHECK_FALSE(file::exists(icon));
#endif
#endif
}

#if !defined(_WIN32)
TEST("identity: a running earlier version takes the launch") {
    // No earlier version running: nobody listens, the launch is ours.
    CHECK_FALSE(identity::handOffToEarlierVersion("msga://x"));
    // An earlier version's local socket: $TMPDIR/msga-<home folder name>,
    // reading one string (a big-endian byte length, then UTF-16BE; checked
    // against a running earlier version by hand).
    std::string tmp = base::env("TMPDIR");
    if (tmp.empty())
        tmp = "/tmp";
    const std::string path = tmp + "/msga-" + std::string(file::baseName(base::env("HOME")));
    ::unlink(path.c_str());
    const int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(srv >= 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, path.c_str(), path.size());
    REQUIRE(bind(srv, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0);
    REQUIRE(listen(srv, 1) == 0);
    std::string got;
    std::thread t([&] {
        const int c = accept(srv, nullptr, nullptr);
        char      buf[256];
        ssize_t   n;
        while ((n = ::read(c, buf, sizeof buf)) > 0)
            got.append(buf, size_t(n));
        close(c);
    });
    CHECK(identity::handOffToEarlierVersion("msga://o?\xC3\xA9"));
    t.join();
    close(srv);
    ::unlink(path.c_str());
    // Byte length 20, then UTF-16BE.
    CHECK(got == std::string("\0\0\0\x14\0m\0s\0g\0a\0:\0/\0/\0o\0?\0\xE9", 24));
}
#endif

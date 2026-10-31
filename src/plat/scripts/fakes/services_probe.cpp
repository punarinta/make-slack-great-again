// Checks what plat_selftest cannot, against plat_fake_desktop.py:
//  - the portal colour scheme: the startup read, and a SettingChanged flip
//    (via the fake's test method) arriving as ThemeChanged with darkMode()
//    updated. Expects the fake portal to start with color-scheme 1.
//  - with $PLAT_PROBE_FULL set: the other portal settings (a SettingChanged
//    for the text scale -> ThemeChanged) and the xdg-activation tokens that
//    a notification click (ActivationToken signal) and a tray click
//    (ProvideXdgActivationToken) hand over.
//  - with $PLAT_PROBE_RESTART_CMD set: that the tray and the badge come back
//    by themselves after the session bus restarts (the command restarts the
//    bus and the fakes, blocking; the address must carry no guid).
//  - with $PLAT_PROBE_NO_SERVICES set (nothing on the buses): a file dialog
//    answers {} at once and networkOnline() stays unknown; nothing else runs.
// Built by src/linux/services.cmake, run by plat-selftest-linux-services.sh.
#include "plat/plat.h"
#include "plat/testing.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace plat;
using Clock = std::chrono::steady_clock;

namespace {

std::unique_ptr<App> g_app;
int                  g_themeChanges = 0;
int                  g_failures     = 0;
std::vector<Event>   g_events;

bool pumpUntil(const std::function<bool()> &pred, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (!pred() && Clock::now() < end)
        g_app->pump(10);
    return pred();
}

void report(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!ok)
        ++g_failures;
}

bool setScheme(int v) {
    char cmd[256];
    std::snprintf(
        cmd,
        sizeof cmd,
        "dbus-send --session --type=method_call --dest=org.freedesktop.portal.Desktop "
        "/org/freedesktop/portal/desktop org.nisdos.PlatTest.SetColorScheme uint32:%d",
        v
    );
    return std::system(cmd) == 0;
}

void checkTheme() {
    // ThemeChanged is posted after darkMode() flips, so wait for both.
    report(
        pumpUntil([] { return g_app->darkMode() && g_themeChanges > 0; }, 3000),
        "portal read at startup: dark, ThemeChanged sent"
    );

    g_themeChanges = 0;
    report(setScheme(2), "fake portal flipped to light");
    report(
        pumpUntil([] { return g_themeChanges > 0 && !g_app->darkMode(); }, 3000),
        "SettingChanged -> ThemeChanged, darkMode() false"
    );

    g_themeChanges = 0;
    setScheme(1);
    report(
        pumpUntil([] { return g_themeChanges > 0 && g_app->darkMode(); }, 3000),
        "flip back -> ThemeChanged, darkMode() true"
    );

    pumpUntil([] { return false; }, 200);
    g_themeChanges = 0;
    setScheme(1); // unchanged value: no spurious ThemeChanged
    pumpUntil([] { return false; }, 300);
    report(g_themeChanges == 0, "same value again sends no ThemeChanged");
}

bool dbusSend(const char *args) {
    std::string cmd = "dbus-send --session --print-reply=literal --type=method_call ";
    cmd += args;
    cmd += " >/dev/null";
    return std::system(cmd.c_str()) == 0;
}

// Portal settings beyond the colour scheme: a SettingChanged for one of them
// is a ThemeChanged with systemSettings() updated.
void checkSettings() {
    report(
        pumpUntil([] { return g_app->systemSettings().textScale == 1.25; }, 3000),
        "portal settings read at startup (text scale 1.25)"
    );
    g_themeChanges = 0;
    report(
        dbusSend(
            "--dest=org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop "
            "org.nisdos.PlatTest.SetTextScale double:1.5"
        ),
        "fake portal text scale -> 1.5"
    );
    report(
        pumpUntil(
            [] { return g_themeChanges > 0 && g_app->systemSettings().textScale == 1.5; }, 3000
        ),
        "SettingChanged -> ThemeChanged, textScale 1.5"
    );
}

// The xdg-activation tokens a click hands over reach the event.
void checkActivationTokens() {
    TestHooks *hooks = g_app->testHooks();
    if (!hooks) {
        report(false, "test hooks available");
        return;
    }
    g_events.clear();
    const uint64_t id = 1;
    const bool     ok = g_app->notify(id, {.title = "token", .body = "click me"});
    report(ok && hooks->notificationInvoke(id, ""), "notification clicked through the fake server");
    const Event *e = nullptr;
    pumpUntil(
        [&] {
            for (auto &ev : g_events)
                if (ev.type == EventType::NotificationActivated && ev.id == id)
                    e = &ev;
            return e != nullptr;
        },
        3000
    );
    report(
        e && e->activationToken.rfind("fake-token-", 0) == 0,
        "NotificationActivated carries the server's ActivationToken"
    );

    auto tray = g_app->createTray();
    report(tray && pumpUntil([&] { return tray->isVisible(); }, 3000), "tray visible");
    g_events.clear();
    report(
        dbusSend(
            "--dest=org.nisdos.PlatTest /org/nisdos/PlatTest "
            "org.nisdos.PlatTest.ActivateTrayWithToken string:plat-tray-token"
        ),
        "host provides a token, then activates"
    );
    const Event *t = nullptr;
    pumpUntil(
        [&] {
            for (auto &ev : g_events)
                if (ev.type == EventType::TrayActivated)
                    t = &ev;
            return t != nullptr;
        },
        3000
    );
    report(t && t->activationToken == "plat-tray-token", "TrayActivated carries the token");
}

// With nothing on the buses (and the zenity/kdialog fallback off), a file
// dialog must answer "nothing chosen" at once instead of hanging.
void checkNoServices() {
    std::optional<std::vector<std::string>> got;
    const auto                              start = Clock::now();
    g_app->showFileDialog({.mode = FileDialogDesc::Mode::Open}, [&](auto p) {
        got = std::move(p);
    });
    pumpUntil([&] { return got.has_value(); }, 5000);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    std::printf("    file dialog answered after %lld ms\n", (long long)ms);
    report(got && got->empty() && ms < 3000, "file dialog without portal or fallback: {} promptly");
    report(!g_app->networkOnline(), "networkOnline() unknown without portal/NetworkManager");
}

void checkReconnect(const char *restartCmd) {
    TestHooks *hooks = g_app->testHooks();
    auto       tray  = g_app->createTray();
    if (!tray || !hooks) {
        report(false, "tray and test hooks available");
        return;
    }
    tray->setTooltip("reconnect");
    g_app->setBadgeCount(5);
    report(pumpUntil([&] { return tray->isVisible(); }, 3000), "tray visible before restart");
    report(pumpUntil([&] { return hooks->badgeCount() == 5; }, 3000), "badge 5 before restart");

    report(std::system(restartCmd) == 0, "session bus and fakes restarted");
    // Reconnect backs off 1 s, 2 s, 4 s…; the bus is back within a second.
    report(
        pumpUntil([&] { return tray->isVisible(); }, 15000),
        "tray re-registered with the new watcher"
    );
    report(
        pumpUntil([&] { return hooks->badgeCount() == 5; }, 5000), "badge re-sent on the new bus"
    );
    TestHooks::TrayProbe probe;
    report(
        hooks->trayProbe(*tray, &probe) && probe.tooltip == "reconnect",
        "tray serves its state on the new bus"
    );
    report(g_app->notificationsAvailable(), "notifications available again");
}

} // namespace

int main() {
    std::string err;
    g_app = App::create(&err);
    if (!g_app) {
        std::printf("FAIL create app: %s\n", err.c_str());
        return 1;
    }
    std::printf("# backend: %s\n", g_app->backendName());
    g_app->setAppInfo({.name = "plat services probe", .id = "org.nisdos.plat-services-probe"});
    g_app->setEventHandler([](const Event &e) {
        g_events.push_back(e);
        if (e.type == EventType::ThemeChanged) {
            ++g_themeChanges;
            if (std::getenv("PLAT_PROBE_VERBOSE"))
                std::printf("    ThemeChanged (dark=%d)\n", int(g_app->darkMode()));
        }
    });
    auto win = g_app->createWindow({.title = "plat services probe", .size = {120, 80}});

    if (std::getenv("PLAT_PROBE_NO_SERVICES")) {
        checkNoServices();
        win.reset();
        g_app.reset();
        std::printf("\n%d failed\n", g_failures);
        return g_failures ? 1 : 0;
    }
    checkTheme();
    if (std::getenv("PLAT_PROBE_FULL")) {
        checkSettings();
        checkActivationTokens();
    }
    if (const char *cmd = std::getenv("PLAT_PROBE_RESTART_CMD"); cmd && *cmd)
        checkReconnect(cmd);

    win.reset();
    g_app.reset();
    std::printf("\n%d failed\n", g_failures);
    return g_failures ? 1 : 0;
}

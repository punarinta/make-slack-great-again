// App-level system events for the Linux backends:
//  - network reachability: xdg-desktop-portal NetworkMonitor on the session
//    bus (the only source inside Flatpak), else NetworkManager on the system
//    bus. Portal: online = available && connectivity != local-only.
//    NetworkManager: online = State >= 60 (CONNECTED_SITE; 70 is GLOBAL), 0
//    (UNKNOWN) = no answer. NetworkChanged goes out only when the value
//    actually flips between two known states.
//  - sleep: logind PrepareForSleep(b) -> Suspending / Resumed. We hold a
//    "delay" sleep inhibitor so Suspending is dispatched before the machine
//    sleeps, release it right after, and take a new one on resume.
// Everything is passive listening; nothing here blocks.
#pragma once

#include "freedesktop/dbus_conn.h"

#include <memory>
#include <optional>

namespace plat::linux_services {

class SystemMonitor {
public:
    SystemMonitor(BackendApp &app, Bus &session);
    ~SystemMonitor();

    std::optional<bool> online() const;

    // For the test hooks: which network source is in use ("portal", "nm" or
    // "" while none answered), and whether the logind signal is wired up.
    std::string networkSource() const;
    bool        sleepWatchReady() const { return _sys.ready() && !_login1Owner.empty(); }

private:
    void queryPortalNetwork();
    void queryNm();
    void applyNmState(int64_t state);
    void updateNetwork();

    void login1Changed(const std::string &owner);
    void takeInhibitor();
    void releaseInhibitor();
    void prepareForSleep(bool sleeping);

    void postEmit(Event e);

    BackendApp &_app;
    Bus        &_session;
    Bus         _sys;

    // network
    bool                _portalNet = false; // the portal answered GetAvailable
    std::optional<bool> _portalOnline, _nmOnline, _lastKnown;
    uint64_t            _portalGen = 0;
    std::string         _nmOwner;

    // logind
    std::string _login1Owner;
    int         _inhibitFd       = -1;
    bool        _inhibitInFlight = false;
    bool        _sleeping        = false;

    std::shared_ptr<int> _alive = std::make_shared<int>(0);
};

} // namespace plat::linux_services

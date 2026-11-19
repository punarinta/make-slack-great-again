#include "freedesktop/system_events.h"

#include <unistd.h>

namespace plat::linux_services {

namespace {

constexpr const char *kPortal       = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath   = "/org/freedesktop/portal/desktop";
constexpr const char *kNetMonIface  = "org.freedesktop.portal.NetworkMonitor";
constexpr const char *kNm           = "org.freedesktop.NetworkManager";
constexpr const char *kNmPath       = "/org/freedesktop/NetworkManager";
constexpr const char *kLogin1       = "org.freedesktop.login1";
constexpr const char *kLogin1Path   = "/org/freedesktop/login1";
constexpr const char *kManagerIface = "org.freedesktop.login1.Manager";
constexpr const char *kPropsIface   = "org.freedesktop.DBus.Properties";

// NM_STATE_CONNECTED_SITE: a network with a gateway. LOCAL (50) is a link
// with no route anywhere, which is offline for a chat client.
constexpr int64_t  kNmOnlineState   = 60;
// NetworkMonitor connectivity 1 = local only; 2 limited and 3 captive portal
// still count, since reconnect attempts are how the app finds out.
constexpr uint32_t kPortalLocalOnly = 1;

bool senderIs(DBusMessage *m, const std::string &owner) {
    const char *s = dbus_message_get_sender(m);
    return s && !owner.empty() && owner == s;
}

} // namespace

SystemMonitor::SystemMonitor(BackendApp &app, Bus &session)
    : _app(app), _session(session), _sys(app, systemBusAddress()) {
    std::weak_ptr<int> weak = _alive;

    // ── network: portal ─────────────────────────────────────────────────────
    _session.subscribe(
        "type='signal',interface='org.freedesktop.portal.NetworkMonitor',member='changed',"
        "path='/org/freedesktop/portal/desktop'",
        kNetMonIface,
        "changed",
        [this](DBusMessage *) { queryPortalNetwork(); } // v1 sent (b), v3 nothing: re-read
    );
    _session.watchName(kPortal, [this](const std::string &owner) {
        if (owner.empty()) {
            _portalNet = false;
            _portalOnline.reset();
            updateNetwork();
        } else {
            queryPortalNetwork();
        }
    });

    // ── network: NetworkManager ─────────────────────────────────────────────
    _sys.watchName(kNm, [this](const std::string &owner) {
        _nmOwner = owner;
        if (owner.empty()) {
            _nmOnline.reset();
            updateNetwork();
        } else {
            queryNm();
        }
    });
    _sys.subscribe(
        "type='signal',sender='org.freedesktop.NetworkManager',"
        "interface='org.freedesktop.NetworkManager',member='StateChanged'",
        kNm,
        "StateChanged",
        [this](DBusMessage *m) {
            uint32_t state = 0;
            if (senderIs(m, _nmOwner) && MsgReader(m).u32(&state))
                applyNmState(state);
        }
    );
    _sys.subscribe(
        "type='signal',sender='org.freedesktop.NetworkManager',"
        "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
        "path='/org/freedesktop/NetworkManager',arg0='org.freedesktop.NetworkManager'",
        kPropsIface,
        "PropertiesChanged",
        [this](DBusMessage *m) {
            const char *path = dbus_message_get_path(m);
            if (!senderIs(m, _nmOwner) || !path || std::string_view(path) != kNmPath)
                return;
            MsgReader   r(m);
            std::string iface;
            if (!r.str(&iface) || iface != kNm)
                return;
            MsgReader props = r.enter();
            while (props.type() == DBUS_TYPE_DICT_ENTRY) {
                MsgReader   e = props.enter();
                std::string key;
                int64_t     v = 0;
                if (e.str(&key) && key == "State" && e.value().integer(&v))
                    applyNmState(v);
            }
        }
    );

    // ── logind ──────────────────────────────────────────────────────────────
    _sys.watchName(kLogin1, [this](const std::string &owner) { login1Changed(owner); });
    _sys.subscribe(
        "type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Manager',"
        "member='PrepareForSleep',path='/org/freedesktop/login1'",
        kManagerIface,
        "PrepareForSleep",
        [this](DBusMessage *m) {
            bool sleeping = false;
            if (senderIs(m, _login1Owner) && MsgReader(m).boolean(&sleeping))
                prepareForSleep(sleeping);
        }
    );
    _sys.onLost = [this] {
        // logind drops our inhibitor with the connection; the name watch
        // takes a new one when the bus is back.
        releaseInhibitor();
        _login1Owner.clear();
    };
    _sys.start();
}

SystemMonitor::~SystemMonitor() {
    releaseInhibitor();
}

std::optional<bool> SystemMonitor::online() const {
    return _portalNet ? _portalOnline : _nmOnline;
}

std::string SystemMonitor::networkSource() const {
    if (_portalNet && _portalOnline)
        return "portal";
    if (_nmOnline)
        return "nm";
    return {};
}

// ── network ─────────────────────────────────────────────────────────────────

void SystemMonitor::queryPortalNetwork() {
    if (!_session.connected())
        return;
    const uint64_t gen = ++_portalGen; // only the newest answer counts
    _session.call(
        methodCall(kPortal, kPortalPath, kNetMonIface, "GetAvailable"),
        [this, gen](DBusMessage *reply, const char *, const char *) {
            bool available = false;
            if (gen != _portalGen)
                return;
            if (!reply || !MsgReader(reply).boolean(&available)) {
                // No portal, or one without NetworkMonitor: NetworkManager decides.
                _portalNet = false;
                _portalOnline.reset();
                updateNetwork();
                return;
            }
            _session.call(
                methodCall(kPortal, kPortalPath, kNetMonIface, "GetConnectivity"),
                [this, gen, available](DBusMessage *reply, const char *, const char *) {
                    if (gen != _portalGen)
                        return;
                    uint32_t connectivity = 0; // v1 portals lack it: availability alone
                    if (reply)
                        MsgReader(reply).u32(&connectivity);
                    _portalNet    = true;
                    _portalOnline = available && connectivity != kPortalLocalOnly;
                    updateNetwork();
                }
            );
        }
    );
}

void SystemMonitor::queryNm() {
    auto      m = methodCall(kNm, kNmPath, kPropsIface, "Get");
    MsgWriter w(m.get());
    w.str(kNm);
    w.str("State");
    _sys.call(m, [this](DBusMessage *reply, const char *, const char *) {
        int64_t state = 0;
        if (reply && MsgReader(reply).value().integer(&state))
            applyNmState(state);
    });
}

void SystemMonitor::applyNmState(int64_t state) {
    if (state == 0)
        _nmOnline.reset(); // NM_STATE_UNKNOWN
    else
        _nmOnline = state >= kNmOnlineState;
    updateNetwork();
}

void SystemMonitor::updateNetwork() {
    const std::optional<bool> now = online();
    if (!now)
        return;
    // The first answer is the starting state, not a change; an unknown
    // stretch in between (a portal restart) is not one either.
    const bool changed = _lastKnown && *_lastKnown != *now;
    _lastKnown         = now;
    if (changed)
        postEmit({.type = EventType::NetworkChanged, .online = *now});
}

// ── logind ──────────────────────────────────────────────────────────────────

void SystemMonitor::login1Changed(const std::string &owner) {
    if (owner == _login1Owner)
        return;
    _login1Owner = owner;
    releaseInhibitor(); // held by the old logind, if any
    if (!owner.empty())
        takeInhibitor();
}

void SystemMonitor::takeInhibitor() {
    if (_inhibitFd >= 0 || _inhibitInFlight || _sleeping || !_sys.ready())
        return;
    _inhibitInFlight = true;
    auto      m      = methodCall(kLogin1, kLogin1Path, kManagerIface, "Inhibit");
    MsgWriter w(m.get());
    w.str("sleep");
    w.str(_app.appInfo().name);
    w.str("Saving state before the system sleeps");
    w.str("delay"); // logind waits (up to InhibitDelayMaxSec) until we close it
    std::weak_ptr<int> weak = _alive;
    _sys.call(m, [this, weak](DBusMessage *reply, const char *, const char *) {
        if (weak.expired())
            return;
        _inhibitInFlight = false;
        int fd           = -1;
        if (!reply || !MsgReader(reply).unixFd(&fd))
            return; // not allowed (polkit) or no logind: Suspending is best effort
        if (_sleeping || _inhibitFd >= 0) {
            ::close(fd); // too late for this sleep, or a duplicate
            return;
        }
        _inhibitFd = fd;
    });
}

void SystemMonitor::releaseInhibitor() {
    if (_inhibitFd >= 0) {
        ::close(_inhibitFd);
        _inhibitFd = -1;
    }
}

void SystemMonitor::prepareForSleep(bool sleeping) {
    if (sleeping) {
        if (_sleeping)
            return;
        _sleeping = true;
        // Emit first, release after: the handler runs (and may flush state)
        // while logind still waits for us; the second closure runs on the
        // next loop iteration, after the handler returned.
        postEmit({.type = EventType::Suspending});
        std::weak_ptr<int> weak = _alive;
        _app.post([this, weak] {
            if (!weak.expired())
                releaseInhibitor();
        });
        return;
    }
    // Resumed also goes out without a Suspending before it (we may have
    // missed it across a reconnect): sockets are stale either way.
    _sleeping = false;
    postEmit({.type = EventType::Resumed});
    takeInhibitor();
}

void SystemMonitor::postEmit(Event e) {
    linux_services::postEmit(_app, _alive, std::move(e));
}

} // namespace plat::linux_services

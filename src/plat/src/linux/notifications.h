// org.freedesktop.Notifications client. notify()'s id is the caller's (see
// plat::App::notify), mapped to the server's id when the (async) Notify
// reply lands.
#pragma once

#include "linux/dbus_conn.h"

#include <deque>
#include <map>
#include <memory>

namespace plat::linux_services {

class Notifier {
public:
    static constexpr const char *kName  = "org.freedesktop.Notifications";
    static constexpr const char *kPath  = "/org/freedesktop/Notifications";
    static constexpr const char *kIface = "org.freedesktop.Notifications";

    Notifier(BackendApp &app, Bus &bus);

    // Call from the shared bus's onReady / onLost.
    void busReady();
    void busLost();

    // Optimistic until the bus has told us whether a server exists (a few
    // ms after start, at most kResolveMs): an app asking right at launch must
    // not be told "no notifications" just because D-Bus hasn't answered yet.
    // notify() queues meanwhile; if no server turns up, it gets NotificationFailed.
    bool available() const {
        return !_resolved || (_bus.ready() && (!_owner.empty() || _activatable));
    }
    bool notify(uint64_t id, const Notification &n);

    // The server's id for one of ours, once the Notify reply arrived.
    bool serverId(uint64_t id, uint32_t *out) const;

private:
    struct Entry {
        uint32_t server = 0; // 0 until the Notify reply
        bool     closed = false;
    };
    struct Queued {
        uint64_t     id;
        Notification n;
    };

    void ownerChanged(const std::string &owner);
    void requestCapabilities();
    void send(uint64_t id, const Notification &n);
    void onActionInvoked(DBusMessage *m);
    void onClosed(DBusMessage *m);
    bool fromServer(DBusMessage *m) const;
    void forget(uint64_t id);
    void dropServerIds();
    void postEmit(Event e);

    BackendApp                     &_app;
    Bus                            &_bus;
    std::string                     _owner; // unique name of the current server
    bool                            _activatable  = false;
    bool                            _capsKnown    = false;
    bool                            _capsInFlight = false;
    bool                            _markup       = false; // server parses body markup: escape ours
    static constexpr int            kResolveMs    = 2000;
    bool                            _resolved     = false; // owner and activatable both known
    bool                            _ownerKnown   = false;
    bool                            _activatableKnown = false;
    void                            maybeResolve();
    void                            resolveNow(); // gives up waiting: fail what's queued if none
    std::map<uint64_t, Entry>       _entries;     // ours -> state, oldest first
    std::map<uint32_t, uint64_t>    _byServer;
    std::map<uint32_t, std::string> _tokens; // ActivationToken, until its ActionInvoked
    std::deque<uint64_t>            _closedOrder;
    std::vector<Queued>             _queue; // waiting for GetCapabilities
    std::shared_ptr<int>            _alive = std::make_shared<int>(0);
};

} // namespace plat::linux_services

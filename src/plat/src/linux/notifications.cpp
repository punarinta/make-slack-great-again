#include "linux/notifications.h"

#include "core/image_util.h"
#include "core/strings.h"

#include <algorithm>
#include <unistd.h>

namespace plat::linux_services {

namespace {

// Closed notifications stay mapped: Plasma keeps expired popups in its
// history, and clicking one there re-emits ActionInvoked with the old id
// (msga issue #40 — dropping the mapping on close made them unclickable).
constexpr size_t kRetainClosed = 64;
// Some servers never report a close for persistent notifications; cap the
// open ones too so a long-running app cannot grow the maps without bound.
constexpr size_t kMaxEntries   = 512;

} // namespace

Notifier::Notifier(BackendApp &app, Bus &bus) : _app(app), _bus(bus) {
    _bus.watchName(kName, [this](const std::string &owner) {
        _ownerKnown = true;
        ownerChanged(owner);
        maybeResolve();
    });
    // No bus at all (or one that never answers): stop being optimistic.
    _app.addTimer(kResolveMs, false, [this] { resolveNow(); });
    const std::string rule =
        "type='signal',sender='org.freedesktop.Notifications',interface='org.freedesktop."
        "Notifications',path='/org/freedesktop/Notifications'";
    _bus.subscribe(rule, kIface, "ActionInvoked", [this](DBusMessage *m) { onActionInvoked(m); });
    _bus.subscribe(rule, kIface, "NotificationClosed", [this](DBusMessage *m) { onClosed(m); });
    // Spec 1.2: sent right before ActionInvoked for the same click.
    _bus.subscribe(rule, kIface, "ActivationToken", [this](DBusMessage *m) {
        if (!fromServer(m))
            return;
        MsgReader   r(m);
        uint32_t    server = 0;
        std::string token;
        // Only ours, so the map cannot fill up with other apps' clicks.
        if (r.u32(&server) && r.str(&token) && _byServer.count(server))
            _tokens[server] = token;
    });
}

void Notifier::busReady() {
    // A server that is not running yet but D-Bus-activatable counts as
    // available: the first call starts it.
    auto m =
        methodCall(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "ListActivatableNames");
    _bus.call(m, [this](DBusMessage *reply, const char *, const char *) {
        std::vector<std::string> names;
        if (reply)
            MsgReader(reply).strings(&names);
        _activatable      = std::find(names.begin(), names.end(), kName) != names.end();
        _activatableKnown = true;
        if (_activatable && !_capsKnown)
            requestCapabilities();
        maybeResolve();
    });
}

void Notifier::maybeResolve() {
    if (!_resolved && _ownerKnown && _activatableKnown)
        resolveNow();
}

void Notifier::resolveNow() {
    if (_resolved)
        return;
    _resolved = true;
    if (available())
        return; // the GetCapabilities reply flushes the queue
    auto queue = std::move(_queue);
    _queue.clear();
    for (auto &q : queue) {
        _entries.erase(q.id);
        postEmit(
            {.type = EventType::NotificationFailed,
             .text = "no notification server on the session bus",
             .id   = q.id}
        );
    }
}

void Notifier::busLost() {
    _owner.clear();
    _activatable = false;
    _capsKnown   = false;
    dropServerIds();
}

// Server ids die with the server (or our connection to it). Ours stay valid
// but can no longer be clicked or closed — and must not be: a new server
// numbers from scratch, so an old id would close or match someone else's.
void Notifier::dropServerIds() {
    _byServer.clear();
    _tokens.clear();
    for (auto &[id, e] : _entries)
        e.server = 0;
}

void Notifier::ownerChanged(const std::string &owner) {
    if (owner == _owner)
        return;
    const bool restarted = !_owner.empty();
    _owner               = owner;
    if (restarted) {
        dropServerIds();
        _capsKnown = false;
    }
    if (!owner.empty() && !_capsKnown)
        requestCapabilities();
}

void Notifier::requestCapabilities() {
    if (_capsInFlight)
        return;
    _capsInFlight = true;
    _bus.call(
        methodCall(kName, kPath, kIface, "GetCapabilities"),
        [this](DBusMessage *reply, const char *, const char *) {
            _capsInFlight = false;
            std::vector<std::string> caps;
            if (reply)
                MsgReader(reply).strings(&caps);
            _markup    = std::find(caps.begin(), caps.end(), "body-markup") != caps.end();
            _capsKnown = true;
            // Held back until now so the body is escaped (or not) correctly. On
            // an error the Notify calls below fail and report NotificationFailed.
            auto queue = std::move(_queue);
            _queue.clear();
            for (auto &q : queue)
                send(q.id, q.n);
        }
    );
}

bool Notifier::notify(uint64_t id, const Notification &n) {
    if (!available())
        return false;
    _entries[id] = {};
    while (_entries.size() > kMaxEntries)
        forget(_entries.begin()->first);
    if (_capsKnown) {
        send(id, n);
    } else {
        _queue.push_back({id, n});
        requestCapabilities();
    }
    return true;
}

void Notifier::send(uint64_t id, const Notification &n) {
    auto      m = methodCall(kName, kPath, kIface, "Notify");
    MsgWriter w(m.get());
    w.str(_app.appInfo().name);
    w.u32(0);  // replaces_id
    w.str(""); // app_icon: the desktop-entry hint names our icon
    w.str(n.title);
    w.str(_markup ? core::escapeMarkup(n.body) : n.body); // str() sanitizes either way
    w.array("s", [&](MsgWriter &a) {
        // "default" is the body click; servers do not draw it as a button.
        a.str("default");
        a.str("");
        for (const auto &act : n.actions) {
            a.str(act.key);
            a.str(act.label);
        }
    });
    w.array("{sv}", [&](MsgWriter &h) {
        h.entry("urgency", "y", [](MsgWriter &v) { v.byte(1); });
        h.entry("desktop-entry", "s", [&](MsgWriter &v) { v.str(_app.appInfo().id); });
        h.entry("sender-pid", "x", [](MsgWriter &v) { v.i64(int64_t(::getpid())); });
        if (n.silent)
            h.entry("suppress-sound", "b", [](MsgWriter &v) { v.boolean(true); });
        const Image &img = n.image;
        if (!img.empty() && img.pixels.size() >= size_t(img.width) * img.height) {
            // (iiibiiay): width, height, rowstride, has_alpha, bits, channels,
            // RGBA bytes with straight alpha.
            std::vector<uint8_t> rgba(size_t(img.width) * img.height * 4);
            uint8_t             *o = rgba.data();
            for (size_t i = 0; i < size_t(img.width) * img.height; ++i) {
                const uint32_t px = core::unpremultiply(img.pixels[i]);
                *o++              = uint8_t(px >> 16);
                *o++              = uint8_t(px >> 8);
                *o++              = uint8_t(px);
                *o++              = uint8_t(px >> 24);
            }
            h.entry("image-data", "(iiibiiay)", [&](MsgWriter &v) {
                v.structure([&](MsgWriter &s) {
                    s.i32(img.width);
                    s.i32(img.height);
                    s.i32(img.width * 4);
                    s.boolean(true);
                    s.i32(8);
                    s.i32(4);
                    s.bytes(rgba.data(), rgba.size());
                });
            });
        }
    });
    w.i32(n.timeoutMs < 0 ? -1 : n.timeoutMs);

    std::weak_ptr<int> weak = _alive;
    _bus.call(m, [this, weak, id](DBusMessage *reply, const char *errName, const char *errMsg) {
        if (weak.expired())
            return;
        auto     it     = _entries.find(id);
        uint32_t server = 0;
        if (!reply || !MsgReader(reply).u32(&server)) {
            if (it != _entries.end())
                _entries.erase(it);
            std::string why = errMsg && *errMsg ? errMsg : (errName ? errName : "no reply");
            postEmit({.type = EventType::NotificationFailed, .text = why, .id = id});
            return;
        }
        if (it == _entries.end())
            return; // evicted meanwhile
        it->second.server = server;
        _byServer[server] = id;
    });
}

bool Notifier::serverId(uint64_t id, uint32_t *out) const {
    auto it = _entries.find(id);
    if (it == _entries.end() || !it->second.server)
        return false;
    *out = it->second.server;
    return true;
}

// Signals are broadcast: only the current server's count, and only ids we
// sent (the match rule already limits us to the interface and path).
bool Notifier::fromServer(DBusMessage *m) const {
    const char *sender = dbus_message_get_sender(m);
    return sender && !_owner.empty() && _owner == sender;
}

void Notifier::onActionInvoked(DBusMessage *m) {
    if (!fromServer(m))
        return;
    MsgReader   r(m);
    uint32_t    server = 0;
    std::string key;
    if (!r.u32(&server) || !r.str(&key))
        return;
    std::string token;
    if (auto t = _tokens.find(server); t != _tokens.end()) {
        token = std::move(t->second);
        _tokens.erase(t);
    }
    auto it = _byServer.find(server);
    if (it == _byServer.end())
        return;
    postEmit(
        {.type            = EventType::NotificationActivated,
         .id              = it->second,
         .action          = key == "default" ? std::string() : key,
         .activationToken = std::move(token)}
    );
}

void Notifier::onClosed(DBusMessage *m) {
    if (!fromServer(m))
        return;
    uint32_t server = 0;
    if (!MsgReader(m).u32(&server))
        return;
    auto it = _byServer.find(server);
    if (it == _byServer.end())
        return;
    const uint64_t id = it->second;
    auto           e  = _entries.find(id);
    if (e == _entries.end() || e->second.closed)
        return;
    e->second.closed = true;
    _closedOrder.push_back(id);
    while (_closedOrder.size() > kRetainClosed) {
        forget(_closedOrder.front());
        _closedOrder.pop_front();
    }
    postEmit({.type = EventType::NotificationClosed, .id = id});
}

void Notifier::forget(uint64_t id) {
    auto it = _entries.find(id);
    if (it == _entries.end())
        return;
    if (it->second.server) {
        auto s = _byServer.find(it->second.server);
        if (s != _byServer.end() && s->second == id)
            _byServer.erase(s);
    }
    _entries.erase(it);
}

void Notifier::postEmit(Event e) {
    linux_services::postEmit(_app, _alive, std::move(e));
}

} // namespace plat::linux_services

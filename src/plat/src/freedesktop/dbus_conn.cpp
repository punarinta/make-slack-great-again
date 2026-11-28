#include "freedesktop/dbus_conn.h"

#include "prim/utf8.h"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace plat::linux_services {

namespace {

bool debugEnabled() {
    static const bool on = std::getenv("PLAT_DBUS_DEBUG") != nullptr;
    return on;
}

// Outstanding pending calls per connection, so a Bus torn down with calls in
// flight can cancel them (their notify would otherwise never run and the
// callbacks, with whatever they captured, would leak).
struct PendingData {
    Bus::ReplyFn                    fn;
    std::vector<DBusPendingCall *> *owner;
};

void freePending(void *p) {
    delete static_cast<PendingData *>(p);
}

} // namespace

std::string sanitizeUtf8(std::string_view s) {
    std::string out = prim::utf8::sanitize(s);
    std::erase(out, '\0'); // D-Bus strings cannot carry NUL
    return out;
}

std::string sessionBusAddress() {
    if (const char *a = std::getenv("DBUS_SESSION_BUS_ADDRESS"); a && *a)
        return a;
    // libdbus itself only autolaunches through X11 without the variable; the
    // systemd user bus is where every modern session has it.
    if (const char *rt = std::getenv("XDG_RUNTIME_DIR"); rt && *rt) {
        const std::string path = std::string(rt) + "/bus";
        struct stat       st;
        if (::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
            char *esc = dbus_address_escape_value(path.c_str());
            if (esc) {
                std::string addr = std::string("unix:path=") + esc;
                dbus_free(esc);
                return addr;
            }
        }
    }
    return {};
}

std::string systemBusAddress() {
    if (const char *a = std::getenv("DBUS_SYSTEM_BUS_ADDRESS"); a && *a)
        return a;
    return "unix:path=/run/dbus/system_bus_socket";
}

// ── MsgWriter ───────────────────────────────────────────────────────────────

void MsgWriter::str(std::string_view s) {
    const std::string clean = sanitizeUtf8(s);
    const char       *p     = clean.c_str();
    dbus_message_iter_append_basic(&_it, DBUS_TYPE_STRING, &p);
}

void MsgWriter::bytes(const uint8_t *data, size_t n) {
    DBusMessageIter sub;
    dbus_message_iter_open_container(&_it, DBUS_TYPE_ARRAY, "y", &sub);
    if (n)
        dbus_message_iter_append_fixed_array(&sub, DBUS_TYPE_BYTE, &data, int(n));
    dbus_message_iter_close_container(&_it, &sub);
}

void MsgWriter::container(int type, const char *sig, Fill fill) {
    MsgWriter child;
    dbus_message_iter_open_container(&_it, type, sig, &child._it);
    fill(child);
    dbus_message_iter_close_container(&_it, &child._it);
}

// ── MsgReader ───────────────────────────────────────────────────────────────

template <class T>
bool MsgReader::basic(int t, T *out) {
    if (type() != t)
        return false;
    dbus_message_iter_get_basic(&_it, out);
    dbus_message_iter_next(&_it);
    return true;
}

bool MsgReader::str(std::string *out) {
    const int t = type();
    if (t != DBUS_TYPE_STRING && t != DBUS_TYPE_OBJECT_PATH && t != DBUS_TYPE_SIGNATURE)
        return false;
    const char *s = nullptr;
    dbus_message_iter_get_basic(&_it, &s);
    *out = s ? s : "";
    dbus_message_iter_next(&_it);
    return true;
}

bool MsgReader::boolean(bool *out) {
    dbus_bool_t v = FALSE;
    if (!basic(DBUS_TYPE_BOOLEAN, &v))
        return false;
    *out = v;
    return true;
}
bool MsgReader::i32(int32_t *out) {
    return basic(DBUS_TYPE_INT32, out);
}
bool MsgReader::u32(uint32_t *out) {
    return basic(DBUS_TYPE_UINT32, out);
}
bool MsgReader::f64(double *out) {
    return basic(DBUS_TYPE_DOUBLE, out);
}
bool MsgReader::unixFd(int *out) {
    return basic(DBUS_TYPE_UNIX_FD, out);
}

bool MsgReader::integer(int64_t *out) {
    switch (type()) {
    case DBUS_TYPE_BYTE: {
        uint8_t v;
        basic(DBUS_TYPE_BYTE, &v);
        *out = v;
        return true;
    }
    case DBUS_TYPE_INT16: {
        int16_t v;
        basic(DBUS_TYPE_INT16, &v);
        *out = v;
        return true;
    }
    case DBUS_TYPE_UINT16: {
        uint16_t v;
        basic(DBUS_TYPE_UINT16, &v);
        *out = v;
        return true;
    }
    case DBUS_TYPE_INT32: {
        int32_t v;
        basic(DBUS_TYPE_INT32, &v);
        *out = v;
        return true;
    }
    case DBUS_TYPE_UINT32: {
        uint32_t v;
        basic(DBUS_TYPE_UINT32, &v);
        *out = v;
        return true;
    }
    case DBUS_TYPE_INT64:
        return basic(DBUS_TYPE_INT64, out);
    case DBUS_TYPE_UINT64: {
        uint64_t v;
        basic(DBUS_TYPE_UINT64, &v);
        *out = int64_t(v);
        return true;
    }
    default:
        return false;
    }
}

bool MsgReader::strings(std::vector<std::string> *out) {
    if (type() != DBUS_TYPE_ARRAY)
        return false;
    MsgReader a = enter();
    out->clear();
    std::string s;
    while (a.str(&s))
        out->push_back(s);
    return true;
}

MsgReader MsgReader::enter() {
    MsgReader child;
    const int t = type();
    if (t == DBUS_TYPE_ARRAY || t == DBUS_TYPE_STRUCT || t == DBUS_TYPE_DICT_ENTRY ||
        t == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&_it, &child._it);
        child._ok = true;
        dbus_message_iter_next(&_it);
    }
    return child;
}

MsgReader MsgReader::value() {
    MsgReader v = *this;
    while (v.type() == DBUS_TYPE_VARIANT)
        v = v.enter();
    return v;
}

MessageRef methodCall(const char *dest, const char *path, const char *iface, const char *method) {
    return MessageRef(dbus_message_new_method_call(dest, path, iface, method));
}

MessageRef signalMessage(const char *path, const char *iface, const char *member) {
    return MessageRef(dbus_message_new_signal(path, iface, member));
}

// ── Bus ─────────────────────────────────────────────────────────────────────

Bus::Bus(BackendApp &app, std::string address)
    : _app(app), _address(std::move(address)), _alive(std::make_shared<Bus *>(this)) {}

Bus::~Bus() {
    if (_reconnectTimer)
        _app.cancelTimer(_reconnectTimer);
    // Posted dispatches check _alive; cancel what is in flight so the reply
    // callbacks (which capture their owners) are freed now, not leaked.
    _alive.reset();
    for (auto *p : std::vector<DBusPendingCall *>(_pending)) {
        dbus_pending_call_cancel(p);
        dbus_pending_call_unref(p);
    }
    _pending.clear();
    if (_conn) {
        dbus_connection_remove_filter(_conn, filterThunk, this);
        dbus_connection_close(_conn);
        dbus_connection_unref(_conn);
        _conn = nullptr;
    }
    for (const auto &w : _watches)
        if (w.id)
            _app.unwatchFd(w.id);
    for (const auto &t : _timeouts)
        if (t.id)
            _app.cancelTimer(t.id);
}

bool Bus::start() {
    if (_conn || _address.empty())
        return _conn != nullptr;
    _lostPending = false;
    DBusError err;
    dbus_error_init(&err);
    // Private: a shared libdbus connection could be touched by other code in
    // the process, which would break our single-loop assumptions.
    _conn = dbus_connection_open_private(_address.c_str(), &err);
    if (!_conn) {
        if (debugEnabled())
            std::fprintf(stderr, "plat dbus: connect %s: %s\n", _address.c_str(), err.message);
        dbus_error_free(&err);
        scheduleReconnect();
        return false;
    }
    dbus_connection_set_exit_on_disconnect(_conn, FALSE);
    dbus_connection_set_watch_functions(_conn, addWatch, removeWatch, toggleWatch, this, nullptr);
    dbus_connection_set_timeout_functions(
        _conn, addTimeout, removeTimeout, toggleTimeout, this, nullptr
    );
    dbus_connection_set_dispatch_status_function(
        _conn,
        [](DBusConnection *, DBusDispatchStatus s, void *self) {
            if (s == DBUS_DISPATCH_DATA_REMAINS)
                static_cast<Bus *>(self)->scheduleDispatch();
        },
        this,
        nullptr
    );
    dbus_connection_set_wakeup_main_function(
        _conn, [](void *self) { static_cast<Bus *>(self)->scheduleDispatch(); }, this, nullptr
    );
    dbus_connection_add_filter(_conn, filterThunk, this, nullptr);

    // Hello by hand instead of dbus_bus_register(), which blocks on the reply.
    call(
        methodCall(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "Hello"),
        [this](DBusMessage *reply, const char *errName, const char *) {
            MsgReader r(reply);
            if (!reply || !r.str(&_unique)) {
                if (debugEnabled())
                    std::fprintf(stderr, "plat dbus: Hello failed: %s\n", errName ? errName : "?");
                _lostPending = true;
                scheduleDispatch();
                return;
            }
            _ready     = true;
            _backoffMs = 1000;
            for (auto &[id, s] : _subs)
                addMatch(s.rule);
            for (auto &[name, fns] : _names)
                queryOwner(name);
            if (onReady)
                onReady();
        }
    );
    return true;
}

void Bus::call(MessageRef msg, ReplyFn fn, int timeoutMs) {
    if (!msg.get())
        return;
    auto failLater = [this, &fn] {
        if (!fn)
            return;
        std::weak_ptr<Bus *> weak = _alive;
        _app.post([weak, fn = std::move(fn)] {
            if (!weak.expired())
                fn(nullptr, DBUS_ERROR_DISCONNECTED, "not connected");
        });
    };
    if (!_conn)
        return failLater();
    if (!fn) {
        dbus_message_set_no_reply(msg.get(), TRUE);
        dbus_connection_send(_conn, msg.get(), nullptr);
        return;
    }
    DBusPendingCall *pending = nullptr;
    if (!dbus_connection_send_with_reply(_conn, msg.get(), &pending, timeoutMs) || !pending)
        return failLater();
    auto *data = new PendingData{std::move(fn), &_pending};
    _pending.push_back(pending);
    dbus_pending_call_set_notify(
        pending,
        [](DBusPendingCall *p, void *ud) {
            auto *d = static_cast<PendingData *>(ud);
            std::erase(*d->owner, p);
            DBusMessage *reply = dbus_pending_call_steal_reply(p);
            // Move the callback out: unref below may free `d`.
            ReplyFn      fn    = std::move(d->fn);
            dbus_pending_call_unref(p);
            if (!reply) {
                fn(nullptr, DBUS_ERROR_NO_REPLY, "no reply");
            } else if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
                DBusError err;
                dbus_error_init(&err);
                dbus_set_error_from_message(&err, reply);
                if (debugEnabled())
                    std::fprintf(stderr, "plat dbus: error reply %s: %s\n", err.name, err.message);
                fn(nullptr, err.name, err.message ? err.message : "");
                dbus_error_free(&err);
            } else {
                fn(reply, nullptr, nullptr);
            }
            if (reply)
                dbus_message_unref(reply);
        },
        data,
        freePending
    );
}

void Bus::send(MessageRef msg) {
    if (_conn && msg.get())
        dbus_connection_send(_conn, msg.get(), nullptr);
}

void Bus::replyEmpty(DBusMessage *call) {
    if (dbus_message_get_no_reply(call))
        return;
    send(MessageRef(dbus_message_new_method_return(call)));
}

void Bus::replyError(DBusMessage *call, const char *name, const char *msg) {
    if (dbus_message_get_no_reply(call))
        return;
    send(MessageRef(dbus_message_new_error(call, name, msg)));
}

uint64_t Bus::subscribe(
    std::string rule, std::string iface, std::string member, std::function<void(DBusMessage *)> fn
) {
    const uint64_t id = _nextSub++;
    if (_ready)
        addMatch(rule);
    _subs[id] = {std::move(rule), std::move(iface), std::move(member), std::move(fn)};
    return id;
}

void Bus::unsubscribe(uint64_t id) {
    auto it = _subs.find(id);
    if (it == _subs.end())
        return;
    if (_ready) {
        auto m = methodCall(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "RemoveMatch");
        MsgWriter(m.get()).str(it->second.rule);
        call(m);
    }
    _subs.erase(it);
}

void Bus::addMatch(const std::string &rule) {
    auto m = methodCall(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "AddMatch");
    MsgWriter(m.get()).str(rule);
    call(m);
}

void Bus::watchName(const std::string &name, std::function<void(const std::string &)> fn) {
    const bool first = !_names.count(name);
    _names[name].push_back(std::move(fn));
    if (first) {
        subscribe(
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
            "member='NameOwnerChanged',arg0='" +
                name + "'",
            DBUS_INTERFACE_DBUS,
            "NameOwnerChanged",
            [this, name](DBusMessage *m) {
                const char *sender = dbus_message_get_sender(m);
                if (!sender || std::string_view(sender) != DBUS_SERVICE_DBUS)
                    return;
                MsgReader   r(m);
                std::string n, oldOwner, newOwner;
                if (!r.str(&n) || !r.str(&oldOwner) || !r.str(&newOwner) || n != name)
                    return;
                auto fns = _names[name]; // copy: a callback may add watchers
                for (auto &f : fns)
                    f(newOwner);
            }
        );
    }
    if (_ready)
        queryOwner(name);
}

void Bus::queryOwner(const std::string &name) {
    auto m = methodCall(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetNameOwner");
    MsgWriter(m.get()).str(name);
    call(m, [this, name](DBusMessage *reply, const char *, const char *) {
        std::string owner;
        if (reply)
            MsgReader(reply).str(&owner);
        auto fns = _names[name];
        for (auto &f : fns)
            f(owner);
    });
}

DBusHandlerResult Bus::filterThunk(DBusConnection *, DBusMessage *m, void *self) {
    return static_cast<Bus *>(self)->filter(m);
}

DBusHandlerResult Bus::filter(DBusMessage *m) {
    const int type = dbus_message_get_type(m);
    if (type == DBUS_MESSAGE_TYPE_SIGNAL &&
        dbus_message_is_signal(m, DBUS_INTERFACE_LOCAL, "Disconnected")) {
        _lostPending = true;
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (type == DBUS_MESSAGE_TYPE_METHOD_CALL) {
        const char *path = dbus_message_get_path(m);
        if (!path)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        auto it = _objects.find(path);
        if (it == _objects.end())
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        auto fn = it->second; // copy: the handler may remove its own object
        return fn(m) ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (type == DBUS_MESSAGE_TYPE_SIGNAL) {
        std::vector<uint64_t> ids;
        for (auto &[id, s] : _subs)
            if (dbus_message_is_signal(m, s.iface.c_str(), s.member.c_str()))
                ids.push_back(id);
        for (auto id : ids) {
            auto it = _subs.find(id);
            if (it == _subs.end())
                continue;
            auto fn = it->second.fn;
            fn(m);
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void Bus::scheduleDispatch() {
    // Inside a watch or timeout handler the dispatch follows right after it,
    // in the same loop turn, with no closure to post.
    if (_dispatchQueued || _handling)
        return;
    _dispatchQueued           = true;
    std::weak_ptr<Bus *> weak = _alive;
    _app.post([weak] {
        if (auto p = weak.lock()) {
            (*p)->_dispatchQueued = false;
            (*p)->dispatch();
        }
    });
}

void Bus::dispatch() {
    // libdbus deadlocks on a dispatch nested inside another on one thread
    // (e.g. a test hook pumping the loop from a handler); the outer loop
    // below drains everything anyway.
    if (_dispatching)
        return;
    _dispatching = true;
    while (_conn && !_lostPending &&
           dbus_connection_dispatch(_conn) == DBUS_DISPATCH_DATA_REMAINS) {
    }
    _dispatching = false;
    if (_lostPending)
        lost();
}

void Bus::lost() {
    _lostPending = false;
    if (!_conn)
        return;
    if (debugEnabled())
        std::fprintf(stderr, "plat dbus: connection to %s lost\n", _address.c_str());
    dbus_connection_remove_filter(_conn, filterThunk, this);
    dbus_connection_close(_conn);
    // Drain the error replies libdbus queued for calls in flight, so their
    // callbacks see the failure instead of leaking.
    while (dbus_connection_dispatch(_conn) == DBUS_DISPATCH_DATA_REMAINS) {
    }
    _lostPending = false;
    dbus_connection_unref(_conn);
    _conn = nullptr;
    for (const auto &w : _watches)
        if (w.id)
            _app.unwatchFd(w.id);
    _watches.clear();
    for (const auto &t : _timeouts)
        if (t.id)
            _app.cancelTimer(t.id);
    _timeouts.clear();
    const bool wasReady = _ready;
    _ready              = false;
    _unique.clear();
    if (wasReady && onLost)
        onLost();
    scheduleReconnect();
}

void Bus::scheduleReconnect() {
    if (_reconnectTimer || _address.empty())
        return;
    _reconnectTimer = _app.addTimer(_backoffMs, false, [this] {
        _reconnectTimer = 0;
        _backoffMs      = std::min(_backoffMs * 2, 30000);
        start();
    });
}

void postEmit(BackendApp &app, const std::shared_ptr<int> &alive, Event e) {
    app.post([weak = std::weak_ptr<int>(alive), &app, e = std::move(e)] {
        if (!weak.expired())
            app.emit(e);
    });
}

// ── loop integration ────────────────────────────────────────────────────────

Bus::Slot *Bus::slot(std::vector<Slot> &v, const void *key) {
    for (auto &s : v)
        if (s.key == key)
            return &s;
    return nullptr;
}

void Bus::syncWatch(DBusWatch *w) {
    Slot *it = slot(_watches, w);
    if (!it)
        return;
    if (it->id) {
        _app.unwatchFd(it->id);
        it->id = 0;
    }
    if (!dbus_watch_get_enabled(w))
        return;
    const unsigned flags  = dbus_watch_get_flags(w);
    uint32_t       events = 0;
    if (flags & DBUS_WATCH_READABLE)
        events |= FdRead;
    if (flags & DBUS_WATCH_WRITABLE)
        events |= FdWrite;
    std::weak_ptr<Bus *> weak = _alive;
    it->id = _app.watchFd(dbus_watch_get_unix_fd(w), events, [weak, w](uint32_t ready) {
        auto p = weak.lock();
        if (!p)
            return;
        unsigned f = 0;
        if (ready & FdRead)
            f |= DBUS_WATCH_READABLE;
        if (ready & FdWrite)
            f |= DBUS_WATCH_WRITABLE;
        Bus *self       = *p;
        self->_handling = true;
        dbus_watch_handle(w, f); // may remove this very watch
        self->_handling = false;
        self->dispatch();
    });
}

dbus_bool_t Bus::addWatch(DBusWatch *w, void *self) {
    auto *bus = static_cast<Bus *>(self);
    if (!slot(bus->_watches, w))
        bus->_watches.push_back({w, 0});
    bus->syncWatch(w);
    return TRUE;
}

void Bus::removeWatch(DBusWatch *w, void *self) {
    auto *bus = static_cast<Bus *>(self);
    Slot *it  = slot(bus->_watches, w);
    if (!it)
        return;
    if (it->id)
        bus->_app.unwatchFd(it->id);
    bus->_watches.erase(bus->_watches.begin() + (it - bus->_watches.data()));
}

void Bus::toggleWatch(DBusWatch *w, void *self) {
    static_cast<Bus *>(self)->syncWatch(w);
}

void Bus::syncTimeout(DBusTimeout *t) {
    Slot *it = slot(_timeouts, t);
    if (!it)
        return;
    if (it->id) {
        _app.cancelTimer(it->id);
        it->id = 0;
    }
    if (!dbus_timeout_get_enabled(t))
        return;
    std::weak_ptr<Bus *> weak = _alive;
    it->id                    = _app.addTimer(dbus_timeout_get_interval(t), true, [weak, t] {
        auto p = weak.lock();
        if (!p)
            return;
        Bus *self       = *p;
        self->_handling = true;
        dbus_timeout_handle(t); // may remove this very timeout
        self->_handling = false;
        self->dispatch();
    });
}

dbus_bool_t Bus::addTimeout(DBusTimeout *t, void *self) {
    auto *bus = static_cast<Bus *>(self);
    if (!slot(bus->_timeouts, t))
        bus->_timeouts.push_back({t, 0});
    bus->syncTimeout(t);
    return TRUE;
}

void Bus::removeTimeout(DBusTimeout *t, void *self) {
    auto *bus = static_cast<Bus *>(self);
    Slot *it  = slot(bus->_timeouts, t);
    if (!it)
        return;
    if (it->id)
        bus->_app.cancelTimer(it->id);
    bus->_timeouts.erase(bus->_timeouts.begin() + (it - bus->_timeouts.data()));
}

void Bus::toggleTimeout(DBusTimeout *t, void *self) {
    static_cast<Bus *>(self)->syncTimeout(t);
}

} // namespace plat::linux_services

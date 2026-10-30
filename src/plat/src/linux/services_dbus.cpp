// Services on the session D-Bus: owns the shared connection (notifications,
// launcher badge, portal colour scheme); each tray brings its own (see
// sni_tray.h for why). Also, in PLAT_TEST_HOOKS builds, the TestHooks halves,
// which talk to our objects and to the fake desktop (plat/scripts/fakes) over
// a second connection.
#include "linux/file_dialog.h"
#include "linux/notifications.h"
#include "linux/services.h"
#include "linux/sni_tray.h"
#include "linux/system_events.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace plat::linux_services {
namespace {

constexpr const char *kPortal        = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath    = "/org/freedesktop/portal/desktop";
constexpr const char *kSettingsIface = "org.freedesktop.portal.Settings";
constexpr const char *kAppearance    = "org.freedesktop.appearance";
constexpr const char *kLauncherIface = "com.canonical.Unity.LauncherEntry";
constexpr const char *kLauncherPath  = "/org/nisdos/plat/LauncherEntry";
constexpr const char *kInterfaceNs   = "org.gnome.desktop.interface";
constexpr const char *kA11yNs        = "org.gnome.desktop.a11y.interface";

#ifdef PLAT_TEST_HOOKS
// The fake desktop's test-only interface (plat/scripts/fakes/plat_fake_desktop.py).
constexpr const char *kTestIface        = "org.nisdos.PlatTest";
constexpr const char *kFakeLauncher     = "org.nisdos.PlatTest.Launcher";
constexpr const char *kFakeLauncherPath = "/org/nisdos/PlatTest/Launcher";
// The fake desktop's control object: makes the fakes send their real signals.
constexpr const char *kControl          = "org.nisdos.PlatTest";
constexpr const char *kControlPath      = "/org/nisdos/PlatTest";
constexpr int         kHookTimeoutMs    = 2000;

using Clock = std::chrono::steady_clock;

// "__" is a literal underscore in dbusmenu labels, a lone "_" a mnemonic.
std::string unescapeLabel(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_') {
            if (i + 1 < s.size() && s[i + 1] == '_')
                out += '_', ++i;
            continue;
        }
        out += s[i];
    }
    return out;
}

// Depth-first labels of a dbusmenu layout node's children.
void flattenLayout(MsgReader node, std::vector<std::string> &out) {
    int32_t id = 0;
    node.i32(&id);
    node.skip(); // properties of the node itself
    MsgReader children = node.enter();
    while (children.type() == DBUS_TYPE_VARIANT) {
        MsgReader child = children.enter().value();
        if (child.type() != DBUS_TYPE_STRUCT)
            continue;
        MsgReader item = child.enter();
        MsgReader copy = item;
        int32_t   cid  = 0;
        copy.i32(&cid);
        std::string label, type;
        MsgReader   props = copy.enter();
        while (props.type() == DBUS_TYPE_DICT_ENTRY) {
            MsgReader   e = props.enter();
            std::string key, v;
            e.str(&key);
            if (e.value().str(&v)) {
                if (key == "label")
                    label = v;
                else if (key == "type")
                    type = v;
            }
        }
        out.push_back(type == "separator" ? "-" : unescapeLabel(label));
        flattenLayout(item, out);
    }
}
#endif

class DbusServices final : public Services {
public:
    explicit DbusServices(BackendApp &app)
        : Services(app), _app(app), _address(sessionBusAddress()), _bus(app, _address),
          _notifier(app, _bus), _files(app, _bus), _system(app, _bus) {
        _bus.onReady = [this] {
            _notifier.busReady();
            _files.busReady();
            readColorScheme();
            readSettings();
            if (_badgeSet)
                sendBadge(); // a restarted bus has no memory of it
        };
        _bus.onLost = [this] { _notifier.busLost(); };
        _bus.watchName(kPortal, [this](const std::string &owner) {
            // A portal that starts (or restarts) after us: read it again.
            if (!owner.empty() && owner != _portalOwner) {
                readColorScheme();
                readSettings();
            }
            _portalOwner = owner;
        });
        _bus.subscribe(
            "type='signal',interface='org.freedesktop.portal.Settings',member='SettingChanged',"
            "path='/org/freedesktop/portal/desktop'",
            kSettingsIface,
            "SettingChanged",
            [this](DBusMessage *m) {
                MsgReader   r(m);
                std::string ns, key;
                if (!r.str(&ns) || !r.str(&key))
                    return;
                if (ns == kAppearance && key == "color-scheme") {
                    applyColorScheme(r);
                    return;
                }
                const SystemSettings before = systemSettings();
                if (applySetting(ns, key, r.value()) && !sameSettings(before, systemSettings()))
                    postThemeChanged();
            }
        );
        _bus.addObject(kLauncherPath, [this](DBusMessage *m) {
            if (!dbus_message_is_method_call(m, kLauncherIface, "Query"))
                return false;
            // Unity's launcher asks when it (re)starts; answer like Update.
            MessageRef reply(dbus_message_new_method_return(m));
            writeBadge(reply.get());
            _bus.send(reply);
            return true;
        });
        _bus.start();
    }

#ifdef PLAT_TEST_HOOKS
    ~DbusServices() override {
        if (_test) {
            dbus_connection_close(_test);
            dbus_connection_unref(_test);
        }
    }
#endif

    // ── Services ────────────────────────────────────────────────────────────
    std::unique_ptr<Tray> createTray() override {
        if (_address.empty())
            return nullptr;
        return std::make_unique<SniTray>(_app, _address);
    }
    bool notificationsAvailable() const override { return _notifier.available(); }
    bool notify(uint64_t id, const Notification &n) override { return _notifier.notify(id, n); }

    void setBadgeCount(int count) override {
        _badge    = std::max(0, count);
        _badgeSet = true;
        if (_bus.ready())
            sendBadge();
    }

    std::optional<bool> darkMode() const override { return _dark; }

    void showFileDialog(
        const FileDialogDesc                 &d,
        std::string                           parentHandle,
        bool                                  tools,
        std::function<void(FileDialogResult)> cb
    ) override {
        _files.show(d, std::move(parentHandle), tools, std::move(cb));
    }

    std::optional<bool> networkOnline() const override { return _system.online(); }

    SystemSettings systemSettings() const override {
        SystemSettings s;
        if (_textScale)
            s.textScale = std::clamp(*_textScale, 0.5, 3.0);
        if (_animations)
            s.reducedMotion = !*_animations;
        // cursor-blink-time is the full on+off cycle; ours is one half.
        if (_blink && !*_blink)
            s.caretBlinkMs = 0;
        else if (_blinkTime && *_blinkTime > 0)
            s.caretBlinkMs = std::clamp(int(*_blinkTime / 2), 50, 5000);
        s.accentColor  = _accent;
        s.highContrast = _contrast == 1 || _a11yHighContrast;
        return s;
    }

#ifdef PLAT_TEST_HOOKS
    // ── TestHooks halves ────────────────────────────────────────────────────
    bool trayActivate(Tray &t) override {
        auto &sni = static_cast<SniTray &>(t);
        if (!waitFor([&] { return sni.busReady(); }))
            return false;
        auto m = methodCall(
            sni.busName().c_str(), SniTray::kItemPath, "org.kde.StatusNotifierItem", "Activate"
        );
        MsgWriter w(m.get());
        w.i32(0);
        w.i32(0);
        return testCall(m).get() != nullptr;
    }

    bool trayMenuSelect(Tray &t, uint32_t id) override {
        auto         &sni    = static_cast<SniTray &>(t);
        const int32_t menuId = sni.menuIdFor(id);
        if (menuId < 0 || !waitFor([&] { return sni.busReady(); }))
            return false;
        auto m = methodCall(
            sni.busName().c_str(), SniTray::kMenuPath, "com.canonical.dbusmenu", "Event"
        );
        MsgWriter w(m.get());
        w.i32(menuId);
        w.str("clicked");
        w.variant("i", [](MsgWriter &v) { v.i32(0); });
        w.u32(0);
        return testCall(m).get() != nullptr;
    }

    bool trayProbe(Tray &t, TestHooks::TrayProbe *out) override {
        auto &sni = static_cast<SniTray &>(t);
        if (!waitFor([&] { return sni.busReady(); }))
            return false;
        auto get = [&](const char *prop) {
            auto m = methodCall(
                sni.busName().c_str(), SniTray::kItemPath, "org.freedesktop.DBus.Properties", "Get"
            );
            MsgWriter w(m.get());
            w.str("org.kde.StatusNotifierItem");
            w.str(prop);
            return testCall(m);
        };
        MessageRef icon = get("IconPixmap"), tip = get("ToolTip");
        if (!icon.get() || !tip.get())
            return false;
        out->iconSizes.clear();
        MsgReader pixmaps = MsgReader(icon.get()).value().enter();
        while (pixmaps.type() == DBUS_TYPE_STRUCT) {
            MsgReader s = pixmaps.enter();
            int32_t   w = 0, h = 0;
            s.i32(&w);
            s.i32(&h);
            out->iconSizes.push_back({w, h});
        }
        MsgReader   tt = MsgReader(tip.get()).value().enter();
        std::string iconName;
        tt.str(&iconName);
        tt.skip();
        tt.str(&out->tooltip);

        auto layout = methodCall(
            sni.busName().c_str(), SniTray::kMenuPath, "com.canonical.dbusmenu", "GetLayout"
        );
        MsgWriter lw(layout.get());
        lw.i32(0);
        lw.i32(-1);
        lw.strings({});
        MessageRef reply = testCall(layout);
        if (!reply.get())
            return false;
        MsgReader r(reply.get());
        uint32_t  revision = 0;
        r.u32(&revision);
        out->menuLabels.clear();
        flattenLayout(r.enter(), out->menuLabels);
        return true;
    }

    bool notificationInvoke(uint64_t id, std::string_view action) override {
        uint32_t server = 0;
        if (!waitFor([&] { return _notifier.serverId(id, &server); }))
            return false;
        auto      m = methodCall(Notifier::kName, Notifier::kPath, kTestIface, "Invoke");
        MsgWriter w(m.get());
        w.u32(server);
        w.str(action.empty() ? std::string_view("default") : action);
        return testCall(m).get() != nullptr; // a real server lacks the interface
    }

    bool notificationProbe(uint64_t id, TestHooks::NotificationProbe *out) override {
        uint32_t server = 0;
        if (!waitFor([&] { return _notifier.serverId(id, &server); }))
            return false;
        auto m = methodCall(Notifier::kName, Notifier::kPath, kTestIface, "Get");
        MsgWriter(m.get()).u32(server);
        MessageRef reply = testCall(m);
        if (!reply.get())
            return false;
        MsgReader r(reply.get());
        int32_t   w = 0, h = 0;
        if (!r.str(&out->title) || !r.str(&out->body) || !r.i32(&w) || !r.i32(&h) ||
            !r.strings(&out->actionLabels))
            return false;
        out->imageSize = {w, h};
        return true;
    }

    // The next dialog's answer goes to the fake portal FileChooser (or the
    // fake zenity, which asks the same control object), so the real request
    // path runs: Request subscription, options, Response parsing.
    bool fileDialogRespond(std::vector<std::string> paths) override {
        auto m = methodCall(kControl, kControlPath, kTestIface, "SetNextFileChooserResponse");
        std::vector<std::string> uris;
        for (const auto &p : paths)
            uris.push_back(fileUri(p));
        MsgWriter(m.get()).strings(uris);
        return testCall(m).get() != nullptr;
    }

    // Asks the fakes to send the real signal (portal NetworkMonitor changed or
    // NetworkManager StateChanged, logind PrepareForSleep) and lets our
    // listeners turn it into the event.
    bool simulateSystemEvent(EventType t, bool online) override {
        constexpr int kSettleMs = 1000; // the watches are normally up long before
        MessageRef    m;
        switch (t) {
        case EventType::NetworkChanged: {
            if (!waitFor([&] { return !_system.networkSource().empty(); }, kSettleMs))
                return false;
            m = methodCall(kControl, kControlPath, kTestIface, "SetNetwork");
            MsgWriter w(m.get());
            w.str(_system.networkSource());
            w.boolean(online);
            break;
        }
        case EventType::Suspending:
        case EventType::Resumed:
            if (!waitFor([&] { return _system.sleepWatchReady(); }, kSettleMs))
                return false;
            m = methodCall(kControl, kControlPath, kTestIface, "PrepareForSleep");
            MsgWriter(m.get()).boolean(t == EventType::Suspending);
            break;
        default:
            return false;
        }
        return testCall(m).get() != nullptr;
    }

    int badgeCount() override {
        auto m = methodCall(kFakeLauncher, kFakeLauncherPath, kTestIface, "BadgeCount");
        MsgWriter(m.get()).str(appUri());
        MessageRef reply = testCall(m);
        int32_t    count = -1;
        if (!reply.get() || !MsgReader(reply.get()).i32(&count))
            return -1; // no launcher that can tell us what it shows
        return count;
    }
#endif

private:
    std::string appUri() const { return "application://" + _app.appInfo().id + ".desktop"; }

    void writeBadge(DBusMessage *m) const {
        MsgWriter w(m);
        w.str(appUri());
        w.array("{sv}", [&](MsgWriter &a) {
            a.entry("count", "x", [&](MsgWriter &v) { v.i64(_badge); });
            a.entry("count-visible", "b", [&](MsgWriter &v) { v.boolean(_badge > 0); });
        });
    }

    void sendBadge() {
        auto m = signalMessage(kLauncherPath, kLauncherIface, "Update");
        writeBadge(m.get());
        _bus.send(m);
    }

    // ReadOne (portal Settings v2) answers v(u); Read (v1) answers v(v(u)).
    void readColorScheme(bool legacy = false) {
        if (_readInFlight && !legacy)
            return;
        _readInFlight = true;
        auto      m = methodCall(kPortal, kPortalPath, kSettingsIface, legacy ? "Read" : "ReadOne");
        MsgWriter w(m.get());
        w.str(kAppearance);
        w.str("color-scheme");
        _bus.call(m, [this, legacy](DBusMessage *reply, const char *errName, const char *) {
            _readInFlight = false;
            if (!reply) {
                if (!legacy && errName &&
                    std::string_view(errName) == "org.freedesktop.DBus.Error.UnknownMethod")
                    readColorScheme(true);
                return;
            }
            MsgReader r(reply);
            applyColorScheme(r);
        });
    }

    // 0 = no preference (keep the backend default), 1 = dark, 2 = light.
    void applyColorScheme(MsgReader &r) {
        int64_t v = 0;
        if (!r.value().integer(&v))
            return;
        const std::optional<bool> dark    = v == 1   ? std::optional<bool>(true)
                                            : v == 2 ? std::optional<bool>(false)
                                                     : std::nullopt;
        const bool                changed = dark.value_or(false) != _dark.value_or(false);
        _dark                             = dark;
        if (changed)
            postThemeChanged();
    }

    void postThemeChanged() {
        std::weak_ptr<int> weak = _alive;
        _app.post([this, weak] {
            if (!weak.expired() && onThemeChanged)
                onThemeChanged();
        });
    }

    // ── portal settings beyond the colour scheme ────────────────────────────
    // ReadAll exists in every Settings version and answers a{sa{sv}}.
    void readSettings() {
        auto m = methodCall(kPortal, kPortalPath, kSettingsIface, "ReadAll");
        MsgWriter(m.get()).strings({kInterfaceNs, kA11yNs, kAppearance});
        std::weak_ptr<int> weak = _alive;
        _bus.call(m, [this, weak](DBusMessage *reply, const char *, const char *) {
            if (weak.expired() || !reply)
                return;
            const SystemSettings before = systemSettings();
            bool                 any    = false;
            MsgReader            nss    = MsgReader(reply).enter();
            while (nss.type() == DBUS_TYPE_DICT_ENTRY) {
                MsgReader   n = nss.enter();
                std::string ns;
                n.str(&ns);
                MsgReader keys = n.enter();
                while (keys.type() == DBUS_TYPE_DICT_ENTRY) {
                    MsgReader   k = keys.enter();
                    std::string key;
                    k.str(&key);
                    any |= applySetting(ns, key, k.value());
                }
            }
            // A portal that starts after us counts as a change; the first
            // read at startup mostly finds defaults and sends nothing.
            if (any && !sameSettings(before, systemSettings()))
                postThemeChanged();
        });
    }

    // Stores one value; true if it is one of ours. Wrong types are ignored.
    bool applySetting(const std::string &ns, const std::string &key, MsgReader v) {
        if (ns == kInterfaceNs) {
            if (key == "text-scaling-factor") {
                double d = 0;
                if (v.f64(&d) && std::isfinite(d) && d > 0)
                    _textScale = d;
            } else if (key == "enable-animations") {
                bool b = true;
                if (v.boolean(&b))
                    _animations = b;
            } else if (key == "cursor-blink") {
                bool b = true;
                if (v.boolean(&b))
                    _blink = b;
            } else if (key == "cursor-blink-time") {
                int64_t ms = 0;
                if (v.integer(&ms))
                    _blinkTime = ms;
            } else {
                return false;
            }
            return true;
        }
        if (ns == kA11yNs && key == "high-contrast") {
            bool b = false;
            if (v.boolean(&b))
                _a11yHighContrast = b;
            return true;
        }
        if (ns == kAppearance && key == "accent-color") {
            // (ddd) sRGB in 0..1; anything outside means "no accent" (the
            // spec's way to unset it).
            MsgReader rgb = v.enter();
            double    c[3];
            bool      ok = rgb.f64(&c[0]) && rgb.f64(&c[1]) && rgb.f64(&c[2]);
            for (double x : c)
                ok = ok && x >= 0 && x <= 1;
            _accent = 0;
            if (ok) {
                auto ch = [](double x) { return uint32_t(std::lround(x * 255)); };
                _accent = 0xff000000u | ch(c[0]) << 16 | ch(c[1]) << 8 | ch(c[2]);
            }
            return true;
        }
        if (ns == kAppearance && key == "contrast") {
            int64_t c = 0;
            if (v.integer(&c))
                _contrast = c;
            return true;
        }
        return false;
    }

    static bool sameSettings(const SystemSettings &a, const SystemSettings &b) {
        return a.reducedMotion == b.reducedMotion && a.highContrast == b.highContrast &&
               a.textScale == b.textScale && a.accentColor == b.accentColor &&
               a.caretBlinkMs == b.caretBlinkMs;
    }

#ifdef PLAT_TEST_HOOKS
    // ── test connection ─────────────────────────────────────────────────────
    // Keeps the loop running (our own objects must answer) until pred holds.
    template <class Pred>
    bool waitFor(Pred pred, int timeoutMs = kHookTimeoutMs) {
        const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!pred()) {
            if (Clock::now() > end)
                return false;
            _app.pump(10);
        }
        return true;
    }

    DBusConnection *testConnection() {
        if (_test && !dbus_connection_get_is_connected(_test)) { // the bus restarted
            dbus_connection_close(_test);
            dbus_connection_unref(_test);
            _test = nullptr;
        }
        if (_test || _address.empty())
            return _test;
        DBusError err;
        dbus_error_init(&err);
        _test = dbus_connection_open_private(_address.c_str(), &err);
        // Blocking Hello is fine here: test-only, and answered by the daemon.
        if (_test && !dbus_bus_register(_test, &err)) {
            dbus_connection_close(_test);
            dbus_connection_unref(_test);
            _test = nullptr;
        }
        dbus_error_free(&err);
        if (_test)
            dbus_connection_set_exit_on_disconnect(_test, FALSE);
        return _test;
    }

    // The reply, or empty on an error/timeout. Pumps our loop meanwhile.
    MessageRef testCall(const MessageRef &m) {
        DBusConnection *c = testConnection();
        if (!c)
            return {};
        DBusPendingCall *pending = nullptr;
        if (!dbus_connection_send_with_reply(c, m.get(), &pending, kHookTimeoutMs) || !pending)
            return {};
        dbus_connection_flush(c);
        waitFor([&] {
            dbus_connection_read_write(c, 0);
            while (dbus_connection_dispatch(c) == DBUS_DISPATCH_DATA_REMAINS) {
            }
            return bool(dbus_pending_call_get_completed(pending));
        });
        MessageRef reply;
        if (dbus_pending_call_get_completed(pending))
            reply = MessageRef(dbus_pending_call_steal_reply(pending));
        else
            dbus_pending_call_cancel(pending);
        dbus_pending_call_unref(pending);
        if (reply.get() && dbus_message_get_type(reply.get()) == DBUS_MESSAGE_TYPE_ERROR)
            return {};
        return reply;
    }
#endif

    BackendApp            &_app;
    std::string            _address;
    Bus                    _bus;
    Notifier               _notifier;
    FileChooser            _files;
    SystemMonitor          _system; // owns the system bus connection
    std::string            _portalOwner;
    // Portal settings as last read; unset = the portal never said.
    std::optional<double>  _textScale;
    std::optional<bool>    _animations, _blink;
    std::optional<int64_t> _blinkTime;
    uint32_t               _accent           = 0;
    int64_t                _contrast         = 0;
    bool                   _a11yHighContrast = false;
    bool                   _readInFlight     = false;
    std::optional<bool>    _dark;
    int                    _badge    = 0;
    bool                   _badgeSet = false;
#ifdef PLAT_TEST_HOOKS
    DBusConnection *_test = nullptr;
#endif
    std::shared_ptr<int> _alive = std::make_shared<int>(0);
};

} // namespace

std::unique_ptr<Services> Services::create(BackendApp &app) {
    return std::make_unique<DbusServices>(app);
}

} // namespace plat::linux_services

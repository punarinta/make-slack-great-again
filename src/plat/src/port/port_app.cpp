// The port backend: plat's App / Window / Tray on the C port contract
// (src/port/include/port, docs/port-contract.md). Everything OS-specific is
// behind the port's functions; this file only translates between plat's C++
// types and the contract's C structs, and keeps the C++-side state the
// contract leaves to the core (posted closures, timer closures, hit-test
// callbacks). The port owns the loop: on_wake runs the posted queue, port
// timers carry the closures.
#include "core/backends.h"
#include "port/port_internal.h"
#include "plat/testing.h"

#include "port/port_test.h"

#include <cstring>
#include <map>

namespace plat {

// The contract's enums are plat's, in the same order: a port author reads
// the C names, the core casts.
static_assert(PORT_KEY_COUNT == int(Key::Count), "port keys out of step with plat::Key");
static_assert(PORT_KEY_KP_EQUAL == int(Key::KpEqual) && PORT_KEY_SPACE == int(Key::Space));
static_assert(PORT_EV_RESUMED == int(EventType::Resumed), "port events out of step");
static_assert(PORT_EV_DRAG_FINISHED == int(EventType::DragFinished));
static_assert(PORT_CURSOR_HIDDEN == int(Cursor::Hidden), "port cursors out of step");
static_assert(PORT_HIT_CLOSE_BUTTON == int(HitArea::CloseButton), "port hit areas out of step");
static_assert(PORT_BUTTON_FORWARD == int(Button::Forward));
static_assert(PORT_SCROLL_MOMENTUM == int(ScrollPhase::Momentum));
static_assert(PORT_DROP_LINK == int(DropAction::Link) && int(PORT_DROP_ALLOW_LINK) == int(ActLink));
static_assert(
    int(PORT_MOD_SHIFT) == int(ModShift) && int(PORT_MOD_SUPER) == int(ModSuper) &&
    int(PORT_MOD_NUM) == int(ModNum)
);

namespace {

class PortApp;

// The contract's structs are filled by whoever writes them; a reader zeroes
// its copy and takes what fits, so an older port's shorter struct works.
template <class T>
T readStruct(const T *in) {
    T out{};
    if (in)
        std::memcpy(&out, in, in->size < sizeof out ? in->size : sizeof out);
    return out;
}

std::string str(const char *s, size_t n) {
    return s ? std::string(s, n) : std::string();
}

std::string str(const char *s) {
    return s ? std::string(s) : std::string();
}

// A port getter (full length returned, buffer filled as far as it fits) as
// a std::string.
template <class Get>
std::string fetch(Get get) {
    std::string out(256, '\0');
    int         n = get(out.data(), out.size());
    if (n >= 0 && size_t(n) >= out.size()) {
        out.resize(size_t(n) + 1);
        n = get(out.data(), out.size());
    }
    out.resize(n > 0 ? size_t(n) : 0);
    return out;
}

port_image portImage(const Image &i) {
    port_image p{};
    p.size   = sizeof p;
    p.width  = i.width;
    p.height = i.height;
    p.scale  = i.scale;
    p.pixels = i.pixels.data();
    return p;
}

std::vector<port_data_item> portItems(const std::vector<DataItem> &items) {
    std::vector<port_data_item> out;
    out.reserve(items.size());
    for (const DataItem &i : items)
        out.push_back({i.mime.c_str(), i.data.data(), i.data.size()});
    return out;
}

class PortWindow final : public Window {
public:
    PortWindow() = default;
    ~PortWindow() override;

    void setTitle(std::string_view t) override {
        port_window_set_title(_w, std::string(t).c_str());
    }
    Size size() const override {
        int32_t w = 0, h = 0;
        port_window_size(_w, &w, &h);
        return {w, h};
    }
    double scale() const override { return port_window_scale(_w); }
    void   setSize(Size s) override { port_window_set_size(_w, s.w, s.h); }
    void   setMinSize(Size s) override { port_window_set_min_size(_w, s.w, s.h); }
    void   show() override { port_window_show(_w); }
    void   hide() override { port_window_hide(_w); }
    void   minimize() override { port_window_minimize(_w); }
    void   setMaximized(bool on) override { port_window_set_maximized(_w, on); }
    void   setFullscreen(bool on) override { port_window_set_fullscreen(_w, on); }
    bool   isMaximized() const override { return state() & PORT_WSTATE_MAXIMIZED; }
    bool   isFullscreen() const override { return state() & PORT_WSTATE_FULLSCREEN; }
    bool   isMinimized() const override { return state() & PORT_WSTATE_MINIMIZED; }
    bool   isActive() const override { return state() & PORT_WSTATE_ACTIVE; }
    void   activate() override { port_window_activate(_w, nullptr); }
    void   activateWithToken(std::string_view token) override {
        port_window_activate(_w, std::string(token).c_str());
    }
    bool supportsAlwaysOnTop() const override { return port_caps() & PORT_CAP_ALWAYS_ON_TOP; }
    void setAlwaysOnTop(bool on) override { port_window_set_always_on_top(_w, on); }
    bool isAlwaysOnTop() const override { return state() & PORT_WSTATE_ABOVE; }
    std::optional<Point> position() const override {
        double x = 0, y = 0;
        if (port_window_position(_w, &x, &y) != PORT_OK)
            return std::nullopt;
        return Point{x, y};
    }
    bool setPosition(Point p) override { return port_window_set_position(_w, p.x, p.y) == PORT_OK; }
    uint64_t monitor() const override { return port_window_monitor(_w); }
    void     setDarkChrome(bool dark) override { port_window_set_dark_chrome(_w, dark); }
    double   titleBarHeight() const override { return port_window_title_bar_height(_w); }
    void     setCursor(Cursor c) override { port_window_set_cursor(_w, uint32_t(c)); }
    void     setHitTest(std::function<HitArea(Point)> fn) override { hitTest = std::move(fn); }
    bool     hasSystemDecorations() const override { return state() & PORT_WSTATE_SYSTEM_DECOR; }
    void     setTextInput(const TextInputState &s) override {
        port_window_set_text_input(_w, s.enabled, {s.caret.x, s.caret.y, s.caret.w, s.caret.h});
    }
    void   requestFrame() override { port_window_request_frame(_w); }
    Canvas beginPaint() override {
        port_canvas c{};
        c.size = sizeof c;
        if (port_window_begin_paint(_w, &c) != PORT_OK)
            return {};
        return {c.pixels, c.width, c.height, c.stride, c.scale};
    }
    void endPaint(const std::vector<Rect> &damage) override {
        static_assert(sizeof(Rect) == sizeof(port_rect));
        port_window_end_paint(
            _w, reinterpret_cast<const port_rect *>(damage.data()), damage.size()
        );
    }
    void  setDropAction(DropAction a) override { port_window_set_drop_action(_w, uint32_t(a)); }
    void  requestAttention() override { port_window_request_attention(_w); }
    void *nativeHandle() const override { return port_window_native(_w); }

    uint32_t     state() const { return port_window_state(_w); }
    port_window *handle() const { return _w; }
    // Set once, right after the port made the window (whose user pointer is
    // this object, so events find it from the start).
    void         attach(port_window *w) { _w = w; }

    std::function<HitArea(Point)> hitTest;

private:
    port_window *_w = nullptr;
};

class PortTray final : public Tray {
public:
    PortTray(PortApp *app, port_tray *t) : _app(app), _t(t) {}
    ~PortTray() override;

    void setIcon(const std::vector<Image> &sizes) override {
        std::vector<port_image> imgs;
        for (const Image &i : sizes)
            imgs.push_back(portImage(i));
        port_tray_set_icon(_t, imgs.data(), imgs.size(), _template);
    }
    void setTemplate(bool on) override { _template = on; }
    void setTooltip(std::string_view t) override {
        port_tray_set_tooltip(_t, std::string(t).c_str());
    }
    void setMenu(std::vector<MenuItem> items) override {
        std::vector<port_menu_item> flat;
        flatten(items, -1, flat);
        port_tray_set_menu(_t, flat.data(), flat.size());
    }
    bool isVisible() const override { return port_tray_visible(_t); }

    port_tray *handle() const { return _t; }

private:
    // Pre-order, each item naming its parent's index (the contract's flat
    // form); the labels point into `items`, which outlives the call.
    static void
    flatten(const std::vector<MenuItem> &items, int parent, std::vector<port_menu_item> &out) {
        for (const MenuItem &m : items) {
            port_menu_item p{};
            p.size    = sizeof p;
            p.kind    = uint32_t(m.kind);
            p.id      = m.id;
            p.parent  = parent;
            p.label   = m.label.c_str();
            p.enabled = m.enabled;
            p.checked = m.checked;
            out.push_back(p);
            if (m.kind == MenuItem::Kind::Submenu)
                flatten(m.children, int(out.size()) - 1, out);
        }
    }
    static_assert(PORT_MENU_SUBMENU == int(MenuItem::Kind::Submenu));

    PortApp   *_app;
    port_tray *_t;
    bool       _template = false;
};

class PortApp final : public BackendApp
#ifdef PLAT_TEST_HOOKS
    ,
                      public TestHooks
#endif
{
public:
    PortApp() { port_mutex_create(&_postMutex); }
    bool start(std::string *error) {
        port_app_desc d{};
        d.size               = sizeof d;
        d.name               = "msga";
        d.id                 = "msga";
        d.callbacks.size     = sizeof d.callbacks;
        d.callbacks.user     = this;
        d.callbacks.on_event = [](void *u, const port_event *e) {
            static_cast<PortApp *>(u)->onEvent(readStruct(e));
        };
        d.callbacks.on_wake  = [](void *u) { static_cast<PortApp *>(u)->runPosted(); };
        d.callbacks.hit_test = [](void *u, port_window *w, double x, double y) -> uint32_t {
            auto *win = static_cast<PortWindow *>(port_window_user(w));
            return win && win->hitTest ? uint32_t(win->hitTest({x, y})) : uint32_t(PORT_HIT_CLIENT);
        };
        char err[256] = "";
        if (port_app_create(&d, &_app, err, sizeof err) != PORT_OK) {
            if (error)
                *error = err[0] ? err : "the port could not start an app";
            return false;
        }
        return true;
    }
    ~PortApp() override {
        // Closures (posted or timed) are destroyed without running, first,
        // while the port still works: their destructors may post or cancel.
        std::vector<std::function<void()>> drop;
        port_mutex_lock(_postMutex);
        _closed = true;
        drop.swap(_posted);
        port_mutex_unlock(_postMutex);
        drop.clear();
        for (auto &[id, fn] : _dirWatches)
            port_unwatch_dir(_app, id);
        _dirWatches.clear();
        auto timers = std::move(_timers);
        for (auto &[id, t] : timers)
            port_timer_cancel(_app, id);
        timers.clear();
        port_app_destroy(_app);
        port_mutex_destroy(_postMutex);
    }

    const char *backendName() const override { return "port"; }
    bool        hasWindows() const override { return (port_caps() & PORT_CAP_WINDOWS) != 0; }
    port_app   *handle() const { return _app; }

    std::unique_ptr<Window> createWindow(const WindowDesc &d) override {
        std::vector<port_image> icons;
        for (const Image &i : d.icon)
            icons.push_back(portImage(i));
        auto            *win = new PortWindow;
        port_window_desc pd{};
        pd.size       = sizeof pd;
        pd.title      = d.title.c_str();
        pd.app_id     = d.appId.c_str();
        pd.wm_class   = d.wmClass.empty() ? nullptr : d.wmClass.c_str();
        pd.icons      = icons.data();
        pd.icon_count = icons.size();
        pd.width      = d.size.w;
        pd.height     = d.size.h;
        pd.min_width  = d.minSize.w;
        pd.min_height = d.minSize.h;
        pd.flags      = (d.resizable ? PORT_WINDOW_RESIZABLE : 0) |
                        (d.visible ? PORT_WINDOW_VISIBLE : 0) |
                        (d.decorations == Decorations::Custom ? PORT_WINDOW_CUSTOM_DECOR : 0) |
                        (d.position ? PORT_WINDOW_HAS_POSITION : 0);
        if (d.position) {
            pd.x = d.position->x;
            pd.y = d.position->y;
        }
        pd.user        = win;
        port_window *w = nullptr;
        if (port_window_create(_app, &pd, &w) != PORT_OK) {
            delete win;
            return nullptr;
        }
        win->attach(w);
        return std::unique_ptr<Window>(win);
    }

    void run() override { port_app_run(_app); }
    void quit() override { port_app_quit(_app); }
    void pump(int timeoutMs) override { port_app_pump(_app, timeoutMs); }

    void post(std::function<void()> fn) override {
        port_mutex_lock(_postMutex);
        if (_closed) {
            port_mutex_unlock(_postMutex);
            return; // fn goes here, outside the lock
        }
        _posted.push_back(std::move(fn));
        port_mutex_unlock(_postMutex);
        port_app_wake(_app);
    }
    void runPosted() {
        std::vector<std::function<void()>> batch;
        port_mutex_lock(_postMutex);
        batch.swap(_posted);
        port_mutex_unlock(_postMutex);
        for (auto &fn : batch) {
            fn();
            fn = nullptr; // what it held goes before the next one runs
        }
    }

    // ── Timers: the port's, carrying our closures ───────────────────────────
    struct Timer {
        PortApp              *app;
        uint64_t              id = 0;
        bool                  repeat;
        std::function<void()> fn;
    };
    TimerId addTimer(int ms, bool repeat, std::function<void()> fn) override {
        auto      *t  = new Timer{this, 0, repeat, std::move(fn)};
        const auto id = port_timer_add(_app, uint32_t(ms < 0 ? 0 : ms), repeat, &PortApp::fire, t);
        if (!id) {
            delete t;
            return 0;
        }
        t->id = id;
        _timers.emplace(id, std::unique_ptr<Timer>(t));
        return id;
    }
    void cancelTimer(TimerId id) override {
        auto it = _timers.find(id);
        if (it == _timers.end())
            return;
        port_timer_cancel(_app, id);
        if (it->second.get() == _firing)
            _firingCancelled = true; // freed once its call returns
        else
            _timers.erase(it);
    }
    static void fire(void *p) {
        auto    *t   = static_cast<Timer *>(p);
        PortApp *app = t->app;
        auto     it  = app->_timers.find(t->id);
        if (it == app->_timers.end())
            return;
        if (!t->repeat) { // the port has forgotten it: so do we, before the call
            std::unique_ptr<Timer> own = std::move(it->second);
            app->_timers.erase(it);
            own->fn();
            return;
        }
        Timer *const prevFiring = app->_firing;
        const bool   prevCancel = app->_firingCancelled;
        app->_firing            = t;
        app->_firingCancelled   = false;
        t->fn();
        if (app->_firingCancelled)
            app->_timers.erase(t->id);
        app->_firing          = prevFiring;
        app->_firingCancelled = prevCancel;
    }

    uint64_t watchFd(int, uint32_t, std::function<void(uint32_t)>) override {
        return 0; // no fds here: socket readiness lives inside the port
    }
    void unwatchFd(uint64_t) override {}

    // The contract's watch merges changes and names none: each call is a
    // DirChange without a name (anything in the folder may have changed).
    uint64_t
    watchDirectory(std::string_view path, std::function<void(const DirChange &)> fn) override {
        auto      *f  = new std::function<void(const DirChange &)>(std::move(fn));
        const auto id = port_watch_dir(
            _app,
            std::string(path).c_str(),
            [](void *u) { (*static_cast<std::function<void(const DirChange &)> *>(u))({}); },
            f
        );
        if (!id) {
            delete f;
            return 0;
        }
        _dirWatches.emplace(id, std::unique_ptr<std::function<void(const DirChange &)>>(f));
        return id;
    }
    void unwatchDirectory(uint64_t id) override {
        port_unwatch_dir(_app, id);
        _dirWatches.erase(id);
    }

    // ── Clipboard ───────────────────────────────────────────────────────────
    void setClipboard(std::vector<DataItem> items, Selection sel) override {
        const auto p = portItems(items);
        port_clipboard_set(_app, int(sel), p.data(), p.size());
    }
    void requestClipboard(
        std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
    ) override {
        auto *ctx = new std::function<void(std::optional<std::string>)>(std::move(cb));
        port_clipboard_get(
            _app,
            int(sel),
            std::string(mime).c_str(),
            [](void *u, const char *data, size_t len) {
                std::unique_ptr<std::function<void(std::optional<std::string>)>> f(
                    static_cast<std::function<void(std::optional<std::string>)> *>(u)
                );
                (*f)(data ? std::optional<std::string>(std::string(data, len)) : std::nullopt);
            },
            ctx
        );
    }
    void requestClipboardMimes(
        std::function<void(std::vector<std::string>)> cb, Selection sel
    ) override {
        auto *ctx = new std::function<void(std::vector<std::string>)>(std::move(cb));
        port_clipboard_types(
            _app,
            int(sel),
            [](void *u, const char *const *mimes, size_t n) {
                std::unique_ptr<std::function<void(std::vector<std::string>)>> f(
                    static_cast<std::function<void(std::vector<std::string>)> *>(u)
                );
                std::vector<std::string> out;
                for (size_t i = 0; i < n; ++i)
                    out.emplace_back(mimes[i]);
                (*f)(std::move(out));
            },
            ctx
        );
    }

    bool startDrag(Window &source, const DragDesc &d) override {
        const auto     items = portItems(d.items);
        port_image     img   = portImage(d.image);
        port_drag_desc pd{};
        pd.size       = sizeof pd;
        pd.items      = items.data();
        pd.item_count = items.size();
        pd.image      = d.image.empty() ? nullptr : &img;
        pd.hot_x      = d.hotspot.x;
        pd.hot_y      = d.hotspot.y;
        pd.actions    = d.actions;
        return port_drag_start(_app, static_cast<PortWindow &>(source).handle(), &pd) == PORT_OK;
    }

    // ── Tray, notifications, badge ──────────────────────────────────────────
    std::unique_ptr<Tray> createTray() override {
        port_tray *t = nullptr;
        if (!(port_caps() & PORT_CAP_TRAY) || port_tray_create(_app, &t) != PORT_OK)
            return nullptr;
        auto tray = std::make_unique<PortTray>(this, t);
        _trays.emplace(t, tray.get());
        return tray;
    }
    void forgetTray(port_tray *t) { _trays.erase(t); }

    bool notificationsAvailable() const override { return port_caps() & PORT_CAP_NOTIFY; }
    bool notify(uint64_t id, const Notification &n) override {
        port_image                      img = portImage(n.image);
        std::vector<port_notify_action> actions;
        for (const auto &a : n.actions)
            actions.push_back({a.key.c_str(), a.label.c_str()});
        port_notification pn{};
        pn.size         = sizeof pn;
        pn.title        = n.title.c_str();
        pn.body         = n.body.c_str();
        pn.image        = n.image.empty() ? nullptr : &img;
        pn.actions      = actions.data();
        pn.action_count = actions.size();
        pn.timeout_ms   = n.timeoutMs;
        pn.silent       = n.silent;
        return port_notify(_app, id, &pn) == PORT_OK;
    }
    void withdrawNotification(uint64_t id) override { port_notify_withdraw(_app, id); }
    bool launchedFromNotification() override { return port_launched_from_notification(_app); }
    void setBadgeCount(int count, bool) override { port_set_badge(_app, count); } // a count only

    std::vector<Monitor> monitors() const override {
        port_monitor buf[16];
        for (auto &m : buf)
            m = {}, m.size = sizeof m;
        const int            n = port_monitors(_app, buf, 16);
        std::vector<Monitor> out;
        for (int i = 0; i < n && i < 16; ++i) {
            const port_monitor &m = buf[i];
            out.push_back(
                {m.id,
                 m.name,
                 {m.bounds.x, m.bounds.y, m.bounds.w, m.bounds.h},
                 {m.work_area.x, m.work_area.y, m.work_area.w, m.work_area.h},
                 m.scale,
                 m.refresh_mhz,
                 m.primary != 0}
            );
        }
        return out;
    }

    bool claimSingleInstance(std::string_view key, const std::vector<std::string> &args) override {
        std::vector<const char *> argv;
        for (const auto &a : args)
            argv.push_back(a.c_str());
        return port_instance_claim(_app, std::string(key).c_str(), argv.data(), argv.size()) != 0;
    }
    bool registerUrlScheme(std::string_view scheme) override {
        return port_url_scheme_register(_app, std::string(scheme).c_str()) == PORT_OK;
    }
    std::optional<bool> networkOnline() const override {
        const int on = port_network_online(_app);
        return on < 0 ? std::nullopt : std::optional<bool>(on != 0);
    }

    void showFileDialog(
        const FileDialogDesc &d, std::function<void(std::vector<std::string>)> cb
    ) override {
        showFileDialogEx(d, [cb = std::move(cb)](FileDialogResult r) { cb(std::move(r.paths)); });
    }
    void
    showFileDialogEx(const FileDialogDesc &d, std::function<void(FileDialogResult)> cb) override {
        std::vector<std::vector<const char *>> patterns;
        std::vector<port_file_filter>          filters;
        for (const auto &f : d.filters) {
            patterns.emplace_back();
            for (const auto &p : f.patterns)
                patterns.back().push_back(p.c_str());
        }
        for (size_t i = 0; i < d.filters.size(); ++i)
            filters.push_back({d.filters[i].name.c_str(), patterns[i].data(), patterns[i].size()});
        port_file_dialog pd{};
        pd.size           = sizeof pd;
        pd.mode           = uint32_t(d.mode);
        pd.title          = d.title.c_str();
        pd.initial_dir    = d.initialDir.c_str();
        pd.suggested_name = d.suggestedName.c_str();
        pd.filters        = filters.data();
        pd.filter_count   = filters.size();
        pd.parent         = d.parent ? static_cast<PortWindow *>(d.parent)->handle() : nullptr;
        _lastDialogParent = d.parent; // compared by tests, never dereferenced
        auto *ctx         = new std::function<void(FileDialogResult)>(std::move(cb));
        port_file_dialog_show(
            _app,
            &pd,
            [](void *u, int status, const char *const *paths, size_t n) {
                std::unique_ptr<std::function<void(FileDialogResult)>> f(
                    static_cast<std::function<void(FileDialogResult)> *>(u)
                );
                FileDialogResult r;
                r.status = status == PORT_DIALOG_CHOSEN ? FileDialogResult::Status::Chosen
                           : status == PORT_DIALOG_CANCELLED
                               ? FileDialogResult::Status::Cancelled
                               : FileDialogResult::Status::Unavailable;
                for (size_t i = 0; i < n; ++i)
                    r.paths.emplace_back(paths[i]);
                (*f)(std::move(r));
            },
            ctx
        );
    }
    static_assert(PORT_DIALOG_FOLDER == int(FileDialogDesc::Mode::PickFolder));

    std::string standardDir(StandardDir d) const override {
        static constexpr int kPaths[] = {
            PORT_PATH_CONFIG,
            PORT_PATH_DATA,
            PORT_PATH_CACHE,
            PORT_PATH_STATE,
            PORT_PATH_TEMP,
            PORT_PATH_HOME,
            PORT_PATH_DESKTOP,
            PORT_PATH_DOCUMENTS,
            PORT_PATH_DOWNLOADS,
            PORT_PATH_PICTURES,
        };
        const int which = kPaths[int(d)];
        return fetch([which](char *buf, size_t cap) { return port_path_get(which, buf, cap); });
    }

    SystemSettings systemSettings() const override {
        const port_system_settings s = settings();
        SystemSettings             out;
        out.reducedMotion = s.reduced_motion;
        out.highContrast  = s.high_contrast;
        out.textScale     = s.text_scale > 0 ? s.text_scale : 1.0;
        out.accentColor   = s.accent_color;
        out.caretBlinkMs  = s.caret_blink_ms;
        return out;
    }
    std::vector<std::string> preferredLanguages() const override {
        const std::string        all = fetch(port_preferred_languages);
        std::vector<std::string> out;
        for (size_t at = 0; at < all.size();) {
            size_t nl = all.find('\n', at);
            if (nl == std::string::npos)
                nl = all.size();
            if (nl > at)
                out.push_back(all.substr(at, nl - at));
            at = nl + 1;
        }
        return out;
    }
    bool darkMode() const override { return settings().dark_mode; }
    int  doubleClickMs() const override {
        const int ms = settings().double_click_ms;
        return ms > 0 ? ms : 400;
    }
    bool openUrl(std::string_view url) override {
        return port_open_url(_app, std::string(url).c_str()) == PORT_OK;
    }

#ifdef PLAT_TEST_HOOKS
    TestHooks *testHooks() override { return (port_caps() & PORT_CAP_TEST_HOOKS) ? this : nullptr; }

    static port_window *w(Window &win) { return static_cast<PortWindow &>(win).handle(); }
    bool                injectKey(Window &win, Key k, bool down) override {
        return port_test_key(w(win), uint32_t(k), down) == PORT_OK;
    }
    bool injectPointerMove(Window &win, Point p) override {
        return port_test_pointer_move(w(win), p.x, p.y) == PORT_OK;
    }
    bool injectButton(Window &win, Button b, bool down) override {
        return port_test_button(w(win), uint32_t(b), down) == PORT_OK;
    }
    bool injectScroll(Window &win, double dx, double dy) override {
        return port_test_scroll(w(win), dx, dy) == PORT_OK;
    }
    bool injectPhasedScroll(Window &win, double dx, double dy, ScrollPhase phase) override {
        return port_test_phased_scroll(w(win), dx, dy, uint32_t(phase)) == PORT_OK;
    }
    bool injectGesture(Window &win, Gesture, int fingers, double dx, double dy) override {
        return port_test_gesture(w(win), uint32_t(fingers), dx, dy) == PORT_OK;
    }
    bool readPixel(Window &win, int x, int y, uint32_t *argb) override {
        return port_test_read_pixel(w(win), x, y, argb) == PORT_OK;
    }
    bool trayActivate(Tray &t) override {
        return port_test_tray_activate(static_cast<PortTray &>(t).handle()) == PORT_OK;
    }
    bool trayMenuSelect(Tray &t, uint32_t id) override {
        return port_test_tray_menu_select(static_cast<PortTray &>(t).handle(), id) == PORT_OK;
    }
    bool trayProbe(Tray &t, TrayProbe *out) override {
        return port_test_tray_probe(
                   static_cast<PortTray &>(t).handle(),
                   [](void *u, const port_test_tray_info *in) {
                       auto                     *o = static_cast<TrayProbe *>(u);
                       const port_test_tray_info p = readStruct(in);
                       o->iconSizes.clear();
                       for (size_t i = 0; i < p.icon_count; ++i)
                           o->iconSizes.push_back({p.icon_sizes[i].w, p.icon_sizes[i].h});
                       o->tooltip    = str(p.tooltip);
                       o->isTemplate = p.template_image;
                       o->menuLabels.clear();
                       for (size_t i = 0; i < p.label_count; ++i)
                           o->menuLabels.emplace_back(p.labels[i]);
                   },
                   out
               ) == PORT_OK;
    }
    bool notificationInvoke(uint64_t id, std::string_view action) override {
        return port_test_notification_invoke(_app, id, std::string(action).c_str()) == PORT_OK;
    }
    bool notificationProbe(uint64_t id, NotificationProbe *out) override {
        return port_test_notification_probe(
                   _app,
                   id,
                   [](void *u, const port_test_notification_info *in) {
                       auto                             *o = static_cast<NotificationProbe *>(u);
                       const port_test_notification_info p = readStruct(in);
                       o->title                            = str(p.title);
                       o->body                             = str(p.body);
                       o->imageSize                        = {p.image_width, p.image_height};
                       o->silent                           = p.silent;
                       o->actionLabels.clear();
                       for (size_t i = 0; i < p.action_count; ++i)
                           o->actionLabels.emplace_back(p.action_labels[i]);
                   },
                   out
               ) == PORT_OK;
    }
    uint64_t maxNotificationId() const override { return port_test_max_notification_id(_app); }
    size_t   notificationCount() const override { return port_test_notification_count(_app); }
    int      badgeCount() override { return port_test_badge(_app); }
    bool     wantsAttention(Window &win, bool *out) override {
        int on = 0;
        if (port_test_wants_attention(w(win), &on) != PORT_OK)
            return false;
        *out = on;
        return true;
    }
    bool fileDialogRespond(std::vector<std::string> paths) override {
        std::vector<const char *> p;
        for (const auto &s : paths)
            p.push_back(s.c_str());
        return port_test_file_dialog_respond(_app, p.data(), p.size()) == PORT_OK;
    }
    bool setFileDialogAvailable(bool on) override {
        return port_test_file_dialog_available(_app, on) == PORT_OK;
    }
    bool lastFileDialog(FileDialogDesc *out) override {
        struct Ctx {
            FileDialogDesc *out;
            Window         *parent;
        } ctx{out, _lastDialogParent};
        return port_test_last_file_dialog(
                   _app,
                   [](void *u, const port_file_dialog *in) {
                       auto                  *c = static_cast<Ctx *>(u);
                       const port_file_dialog d = readStruct(in);
                       FileDialogDesc        &o = *c->out;
                       o.mode                   = FileDialogDesc::Mode(d.mode);
                       o.title                  = str(d.title);
                       o.initialDir             = str(d.initial_dir);
                       o.suggestedName          = str(d.suggested_name);
                       o.parent                 = c->parent;
                       o.filters.clear();
                       for (size_t i = 0; i < d.filter_count; ++i) {
                           FileFilter f;
                           f.name = str(d.filters[i].name);
                           for (size_t k = 0; k < d.filters[i].pattern_count; ++k)
                               f.patterns.emplace_back(d.filters[i].patterns[k]);
                           o.filters.push_back(std::move(f));
                       }
                   },
                   &ctx
               ) == PORT_OK;
    }
    bool simulateSystemEvent(EventType type, bool online) override {
        return port_test_system_event(_app, uint32_t(type), online) == PORT_OK;
    }
    bool deliverUrl(std::string_view url) override {
        return port_test_deliver_url(_app, std::string(url).c_str()) == PORT_OK;
    }
    bool lastOpenedUrl(std::string *out) override {
        const int n = port_test_last_opened_url(_app, nullptr, 0);
        if (n < 0)
            return false;
        *out = fetch([this](char *buf, size_t cap) {
            return port_test_last_opened_url(_app, buf, cap);
        });
        return true;
    }
#endif

private:
    port_system_settings settings() const {
        port_system_settings s{};
        s.size = sizeof s;
        port_system_settings_get(_app, &s);
        return s;
    }

    void onEvent(const port_event &pe) {
        Event e;
        e.type     = EventType(pe.type);
        e.window   = pe.window ? static_cast<PortWindow *>(port_window_user(pe.window)) : nullptr;
        e.pos      = {pe.x, pe.y};
        e.button   = Button(pe.button);
        e.clicks   = pe.clicks;
        e.dx       = pe.dx;
        e.dy       = pe.dy;
        e.precise  = pe.flags & PORT_EVF_PRECISE;
        e.key      = pe.key < PORT_KEY_COUNT ? Key(pe.key) : Key::Unknown;
        e.scancode = pe.scancode;
        e.mods     = pe.mods;
        e.repeat   = pe.flags & PORT_EVF_REPEAT;
        e.text     = str(pe.text, pe.text_len);
        e.preeditCursorBegin = pe.preedit_begin;
        e.preeditCursorEnd   = pe.preedit_end;
        e.phase              = ScrollPhase(pe.phase);
        e.fingers            = int(pe.fingers);
        e.cancelled          = pe.flags & PORT_EVF_CANCELLED;
        e.online             = pe.flags & PORT_EVF_ONLINE;
        e.dropAction         = DropAction(pe.drop_action);
        e.allowedActions     = pe.allowed_actions;
        for (size_t i = 0; i < pe.item_count; ++i)
            e.items.push_back({str(pe.items[i].mime), str(pe.items[i].data, pe.items[i].len)});
        std::vector<std::string> strings;
        for (size_t i = 0; i < pe.string_count; ++i)
            strings.emplace_back(pe.strings[i]);
        // A drop's strings are its URIs; everywhere else they are the
        // event's list (argv, URLs).
        if (e.type == EventType::Drop)
            e.uris = std::move(strings);
        else
            e.strings = std::move(strings);
        switch (e.type) {
        case EventType::GestureBegin:
        case EventType::GestureUpdate:
        case EventType::GestureEnd:
            e.gesture = Gesture::Swipe; // the only multi-finger gesture plat has
            break;
        default:
            break;
        }
        if (pe.tray) {
            auto it = _trays.find(pe.tray);
            e.tray  = it == _trays.end() ? nullptr : it->second;
        }
        e.id              = pe.id;
        e.action          = str(pe.action);
        e.activationToken = str(pe.token);
        emit(e);
    }

    port_app                                  *_app       = nullptr;
    port_mutex                                *_postMutex = nullptr;
    std::vector<std::function<void()>>         _posted;
    bool                                       _closed = false;
    std::map<uint64_t, std::unique_ptr<Timer>> _timers;
    Timer                                     *_firing          = nullptr;
    bool                                       _firingCancelled = false;
    std::map<port_tray *, PortTray *>          _trays;
    std::map<uint64_t, std::unique_ptr<std::function<void(const DirChange &)>>> _dirWatches;
    Window *_lastDialogParent = nullptr;
};

PortWindow::~PortWindow() {
    port_window_destroy(_w);
}

PortTray::~PortTray() {
    _app->forgetTray(_t);
    port_tray_destroy(_t);
}

} // namespace

port_app *portApp(App &app) {
    return static_cast<PortApp &>(app).handle();
}

std::unique_ptr<App> createPortApp(std::string *error) {
    port_info info{};
    info.size = sizeof info;
    if (port_init(&info) != PORT_OK || port_check_abi(info.abi_version) != PORT_OK) {
        if (error)
            *error = "the port's ABI does not match this build";
        return nullptr;
    }
    auto app = std::make_unique<PortApp>();
    if (!app->start(error))
        return nullptr;
    return app;
}

#ifdef PLAT_TEST_HOOKS
// Unit tests poke in-memory-surface state (scale changes, frame counts);
// these names are the headless backend's, which a port build replaces.
namespace testing_internal {
void setHeadlessScale(Window &w, double s) {
    port_test_set_scale(PortApp::w(w), s);
}
int headlessFrames(Window &w) {
    return port_test_frames(PortApp::w(w));
}
Cursor headlessCursor(Window &w) {
    return Cursor(port_test_cursor(PortApp::w(w)));
}
HitArea headlessLastHit(Window &w) {
    return HitArea(port_test_last_hit(PortApp::w(w)));
}
} // namespace testing_internal
#endif

} // namespace plat

// In-memory backend: windows are plain buffers, input arrives only through
// TestHooks. It is the reference for what the real backends must emit, and
// lets toolkit tests run on any CI box with no display. Portable on purpose
// (condition variable, no poll) so the unit tests also run on Windows/macOS.
#include "core/backends.h"
#include "core/loop_core.h"
#include "core/input.h"
#include "core/transfer.h"
#include "plat/testing.h"

#include <cstdlib>
#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <set>

namespace plat {
namespace {

class HeadlessApp;

class HeadlessWindow final : public Window {
public:
    HeadlessWindow(HeadlessApp *app, const WindowDesc &d);
    ~HeadlessWindow() override;

    void   setTitle(std::string_view t) override { title = std::string(t); }
    Size   size() const override { return _size; }
    double scale() const override { return _scale; }
    void   setSize(Size s) override;
    void   setMinSize(Size s) override { _min = s; }
    void   show() override {
        visible = true;
        requestFrame();
    }
    void   hide() override { visible = false; }
    void   minimize() override;
    void   setMaximized(bool on) override;
    void   setFullscreen(bool on) override;
    bool   isMaximized() const override { return _maximized; }
    bool   isFullscreen() const override { return _fullscreen; }
    bool   isMinimized() const override { return _minimized; }
    bool   supportsAlwaysOnTop() const override { return true; }
    void   setAlwaysOnTop(bool on) override;
    bool   isAlwaysOnTop() const override { return _above; }
    bool   isActive() const override { return _active; }
    void   activate() override;
    void   setCursor(Cursor c) override { cursor = c; }
    void   setHitTest(std::function<HitArea(Point)> fn) override { hitTest = std::move(fn); }
    bool   hasSystemDecorations() const override { return _decorations == Decorations::System; }
    void   setTextInput(const TextInputState &s) override { textInput = s; }
    void   requestFrame() override;
    Canvas beginPaint() override;
    void   endPaint(const std::vector<Rect> &damage) override;
    void  *nativeHandle() const override { return (void *)this; }
    void   setDropAction(DropAction a) override { dropReply = a; }
    void   requestAttention() override {
        if (!_active)
            attention = true;
    }
    void activateWithToken(std::string_view t) override {
        lastToken = std::string(t);
        activate();
    }
    std::optional<Point> position() const override { return _pos; }
    bool                 setPosition(Point p) override;
    uint64_t             monitor() const override;

    void setScale(double s);
    void emit(Event e);

    HeadlessApp                  *app;
    std::string                   title;
    bool                          visible = false;
    Cursor                        cursor  = Cursor::Arrow;
    std::function<HitArea(Point)> hitTest;
    TextInputState                textInput;
    std::vector<uint32_t>         pixels; // last presented frame
    std::vector<uint32_t>         back;   // being painted
    int                           pw = 0, ph = 0;
    int                           framesPresented = 0;
    bool                          framePending    = false;
    HitArea                       lastHit         = HitArea::Client;
    DropAction                    dropReply       = DropAction::Copy;
    bool                          attention       = false;
    std::string                   lastToken;

private:
    Size        _size, _min;
    Point       _pos;
    double      _scale     = 1.0;
    bool        _maximized = false, _fullscreen = false, _active = false, _above = false;
    bool        _minimized = false;
    Decorations _decorations;
};

class HeadlessTray final : public Tray {
public:
    explicit HeadlessTray(HeadlessApp *a) : app(a) {}
    ~HeadlessTray() override;
    void setIcon(const std::vector<Image> &sizes) override {
        icons     = sizes;
        iconTempl = templ;
    }
    void setTemplate(bool on) override { templ = on; }
    void setTooltip(std::string_view t) override { tooltip = std::string(t); }
    void setMenu(std::vector<MenuItem> items) override { menu = std::move(items); }
    bool isVisible() const override { return true; }

    HeadlessApp          *app;
    std::vector<Image>    icons;
    std::string           tooltip;
    std::vector<MenuItem> menu;
    bool                  templ = false, iconTempl = false;
};

// Depth-first search for an enabled, selectable item.
const MenuItem *findItem(const std::vector<MenuItem> &items, uint32_t id) {
    for (const auto &m : items) {
        if (m.kind == MenuItem::Kind::Submenu) {
            if (auto *c = findItem(m.children, id))
                return c;
        } else if (m.kind != MenuItem::Kind::Separator && m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

void flattenLabels(const std::vector<MenuItem> &items, std::vector<std::string> &out) {
    for (const auto &m : items) {
        out.push_back(m.kind == MenuItem::Kind::Separator ? "-" : m.label);
        flattenLabels(m.children, out);
    }
}

class HeadlessApp final : public BackendApp, public TestHooks {
public:
    HeadlessApp() {
        _core.wake = [this] {
            std::lock_guard lock(_wakeMutex);
            _woken = true;
            _cv.notify_one();
        };
    }
    ~HeadlessApp() override { _core.shutdown(); } // before _wakeMutex goes

    const char *backendName() const override { return "headless"; }

    std::unique_ptr<Window> createWindow(const WindowDesc &d) override {
        auto w = std::make_unique<HeadlessWindow>(this, d);
        _windows.push_back(w.get());
        return w;
    }
    void forget(HeadlessWindow *w) {
        _windows.erase(std::remove(_windows.begin(), _windows.end(), w), _windows.end());
        if (_focus == w)
            _focus = nullptr;
        if (_hover == w)
            _hover = nullptr;
    }

    void run() override {
        _quit = false;
        while (!_quit)
            pump(-1);
    }
    void quit() override {
        _quit = true;
        _core.wake();
    }
    void pump(int timeoutMs) override {
        // Frames are "vsynced" at the end of every iteration: all windows that
        // asked for one get exactly one Frame, like a compositor frame callback.
        const bool frameDue = std::any_of(_windows.begin(), _windows.end(), [](auto *w) {
            return w->framePending && w->visible;
        });
        int        timeout  = _core.clampTimeout(frameDue ? 0 : timeoutMs);
        {
            std::unique_lock lock(_wakeMutex);
            auto             pred = [this] { return _woken || _quit; };
            if (timeout < 0)
                _cv.wait(lock, pred);
            else if (timeout > 0)
                _cv.wait_for(lock, std::chrono::milliseconds(timeout), pred);
            _woken = false;
        }
        _core.runPosted();
        _core.runDueTimers();
        for (auto *w : std::vector<HeadlessWindow *>(_windows)) {
            if (w->framePending && w->visible) {
                w->framePending = false;
                w->emit({.type = EventType::Frame});
            }
        }
    }

    void    post(std::function<void()> fn) override { _core.post(std::move(fn)); }
    TimerId addTimer(int ms, bool repeat, std::function<void()> fn) override {
        return _core.addTimer(ms, repeat, std::move(fn));
    }
    void     cancelTimer(TimerId id) override { _core.cancelTimer(id); }
    uint64_t watchFd(int, uint32_t, std::function<void(uint32_t)>) override { return 0; }
    void     unwatchFd(uint64_t) override {}

    void setClipboard(std::vector<DataItem> items, Selection sel) override {
        auto &board = _clipboard[size_t(sel)];
        board.clear(); // a new selection replaces every offered type
        for (auto &i : items)
            board.emplace_back(std::move(i));
    }
    void requestClipboard(
        std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
    ) override {
        std::optional<std::string> v;
        for (const auto &i : _clipboard[size_t(sel)])
            if (i.mime == mime || (core::isTextMime(i.mime) && core::isTextMime(mime)))
                v = i.data;
        post([cb = std::move(cb), v = std::move(v)]() mutable { cb(std::move(v)); });
    }
    void requestClipboardMimes(
        std::function<void(std::vector<std::string>)> cb, Selection sel
    ) override {
        std::vector<std::string> m;
        for (const auto &i : _clipboard[size_t(sel)])
            m.push_back(i.mime);
        post([cb = std::move(cb), m = std::move(m)]() mutable { cb(std::move(m)); });
    }

    // ── Drag source: moves over another window turn into Drop* there ────────
    bool startDrag(Window &source, const DragDesc &d) override {
        if (!_buttonHeld || _drag)
            return false;
        _drag       = d;
        _dragSource = &static_cast<HeadlessWindow &>(source);
        _dropTarget = nullptr;
        return true;
    }

    // ── Tray, notifications, badge ──────────────────────────────────────────
    std::unique_ptr<Tray> createTray() override {
        auto t = std::make_unique<HeadlessTray>(this);
        trays.push_back(t.get());
        return t;
    }
    void forgetTray(HeadlessTray *t) {
        trays.erase(std::remove(trays.begin(), trays.end(), t), trays.end());
    }

    bool notificationsAvailable() const override { return true; }
    bool notify(uint64_t id, const Notification &n) override {
        notifications[id] = n;
        return true;
    }
    void setBadgeCount(int n) override { badge = n; }

    // ── Round 3 ─────────────────────────────────────────────────────────────
    // Two fake monitors side by side, the second HiDPI.
    std::vector<Monitor> monitors() const override {
        return {
            {1, "HEADLESS-1", {0, 0, 1920, 1080}, {0, 0, 1920, 1040}, 1.0, 60000, true},
            {2, "HEADLESS-2", {1920, 0, 1280, 720}, {1920, 0, 1280, 720}, 1.5, 60000, false},
        };
    }
    // In-process only: a second *process* cannot find a headless app.
    bool claimSingleInstance(std::string_view key, const std::vector<std::string> &) override {
        return claimedKeys.insert(std::string(key)).second;
    }
    bool registerUrlScheme(std::string_view scheme) override {
        schemes.emplace_back(scheme);
        return true;
    }
    std::optional<bool> networkOnline() const override { return _online; }
    void                showFileDialog(
        const FileDialogDesc &d, std::function<void(std::vector<std::string>)> cb
    ) override {
        showFileDialogEx(d, [cb = std::move(cb)](FileDialogResult r) { cb(std::move(r.paths)); });
    }
    // The fake dialog: answered by fileDialogRespond() (queued or later), or
    // at once with Unavailable after setFileDialogAvailable(false).
    void
    showFileDialogEx(const FileDialogDesc &d, std::function<void(FileDialogResult)> cb) override {
        lastDialog     = d; // parent kept for tests: compare it, never dereference it
        _hasLastDialog = true;
        if (!_dialogAvailable) {
            post([cb = std::move(cb)] { cb({.status = FileDialogResult::Status::Unavailable}); });
        } else if (_dialogAnswer) {
            auto paths = std::move(*_dialogAnswer);
            _dialogAnswer.reset();
            post([cb = std::move(cb), paths = std::move(paths)] { cb(answer(paths)); });
        } else {
            _dialogPending = std::move(cb);
        }
    }
    static FileDialogResult answer(std::vector<std::string> paths) {
        FileDialogResult r;
        r.status =
            paths.empty() ? FileDialogResult::Status::Cancelled : FileDialogResult::Status::Chosen;
        r.paths = std::move(paths);
        return r;
    }
    std::string standardDir(StandardDir d) const override {
        static const char *names[] = {
            "config",
            "data",
            "cache",
            "state",
            "tmp",
            "home",
            "desktop",
            "documents",
            "downloads",
            "pictures"
        };
        namespace fs         = std::filesystem;
        // Under HOME when there is one: test mains give every run its own
        // HOME, so parallel runs never share config or cache files.
        const char     *home = std::getenv("HOME");
        const fs::path  root = home && *home ? fs::path(home) : fs::temp_directory_path();
        const fs::path  p    = root / "plat-headless" / names[int(d)];
        std::error_code ec;
        fs::create_directories(p, ec);
        return p.string();
    }
    SystemSettings systemSettings() const override { return {}; }

    bool fileDialogRespond(std::vector<std::string> paths) override {
        if (_dialogPending) {
            auto cb        = std::move(_dialogPending);
            _dialogPending = nullptr;
            post([cb = std::move(cb), paths = std::move(paths)] { cb(answer(paths)); });
        } else {
            _dialogAnswer = std::move(paths);
        }
        return true;
    }
    bool setFileDialogAvailable(bool on) override {
        _dialogAvailable = on;
        return true;
    }
    bool lastFileDialog(FileDialogDesc *out) override {
        if (_hasLastDialog)
            *out = lastDialog;
        return _hasLastDialog;
    }
    bool simulateSystemEvent(EventType t, bool online) override {
        if (t == EventType::NetworkChanged)
            _online = online;
        emit({.type = t, .online = online});
        return true;
    }
    bool deliverUrl(std::string_view url) override {
        emit({.type = EventType::OpenUrls, .strings = {std::string(url)}});
        return true;
    }
    bool lastOpenedUrl(std::string *out) override {
        if (openedUrls.empty())
            return false;
        *out = openedUrls.back();
        return true;
    }

    std::set<std::string>    claimedKeys;
    std::vector<std::string> schemes;
    FileDialogDesc           lastDialog;

    bool darkMode() const override { return false; }
    int  doubleClickMs() const override { return 400; }
    bool openUrl(std::string_view url) override {
        openedUrls.emplace_back(url);
        return true;
    }

    TestHooks *testHooks() override { return this; }

    // ── TestHooks ───────────────────────────────────────────────────────────
    bool injectKey(Window &win, Key k, bool down) override {
        auto &w = static_cast<HeadlessWindow &>(win);
        focus(&w);
        updateMods(k, down);
        Event e{.type = down ? EventType::KeyDown : EventType::KeyUp, .key = k, .mods = _mods};
        e.repeat         = down && _down[size_t(k)];
        _down[size_t(k)] = down;
        w.emit(e);
        // US layout text for the keys tests use; Ctrl/Super chords type nothing.
        if (down && !(_mods & (ModCtrl | ModSuper)) && w.textInput.enabled) {
            if (auto t = usText(k, _mods & ModShift); !t.empty())
                w.emit({.type = EventType::TextInput, .text = t});
        }
        return true;
    }
    bool injectPointerMove(Window &win, Point p) override {
        auto &w  = static_cast<HeadlessWindow &>(win);
        _pointer = p;
        if (_drag) {
            dragOver(&w, p);
            return true;
        }
        if (_hover != &w) {
            if (_hover)
                _hover->emit({.type = EventType::PointerLeave});
            _hover = &w;
            w.emit({.type = EventType::PointerEnter, .pos = p, .mods = _mods});
        }
        _pointer = p;
        w.emit({.type = EventType::PointerMove, .pos = p, .mods = _mods});
        return true;
    }
    bool injectButton(Window &win, Button b, bool down) override {
        auto &w = static_cast<HeadlessWindow &>(win);
        if (down) {
            focus(&w);
            _clicks.press(int(b), 0, 0, core::monotonicMs(), doubleClickMs(), 0, 0);
            // Mirror the real backends: a press on a non-client area becomes an
            // OS move/resize and is not delivered to the app.
            if (w.hitTest && b == Button::Left) {
                w.lastHit = w.hitTest(_pointer);
                if (isNonClient(w.lastHit))
                    return true;
            }
        }
        if (!down && _drag) {
            finishDrag();
            _buttonHeld = false;
            return true;
        }
        _buttonHeld = down;
        w.emit(
            {.type   = down ? EventType::PointerDown : EventType::PointerUp,
             .pos    = _pointer,
             .button = b,
             .clicks = down ? _clicks.clicks() : 0,
             .mods   = _mods}
        );
        return true;
    }
    bool injectScroll(Window &win, double dx, double dy) override {
        static_cast<HeadlessWindow &>(win).emit(
            {.type = EventType::Scroll, .pos = _pointer, .dx = dx, .dy = dy, .mods = _mods}
        );
        return true;
    }
    bool injectPhasedScroll(Window &win, double dx, double dy, ScrollPhase phase) override {
        static_cast<HeadlessWindow &>(win).emit(
            {.type    = EventType::Scroll,
             .pos     = _pointer,
             .dx      = dx,
             .dy      = dy,
             .precise = true,
             .mods    = _mods,
             .phase   = phase}
        );
        return true;
    }
    // One call = a whole gesture: Begin, one Update carrying the motion, End.
    bool injectGesture(Window &win, Gesture g, int fingers, double dx, double dy) override {
        auto &w = static_cast<HeadlessWindow &>(win);
        w.emit(
            {.type = EventType::GestureBegin, .pos = _pointer, .gesture = g, .fingers = fingers}
        );
        w.emit(
            {.type    = EventType::GestureUpdate,
             .pos     = _pointer,
             .dx      = dx,
             .dy      = dy,
             .gesture = g,
             .fingers = fingers}
        );
        w.emit({.type = EventType::GestureEnd, .pos = _pointer, .gesture = g, .fingers = fingers});
        return true;
    }
    bool trayActivate(Tray &t) override {
        emit({.type = EventType::TrayActivated, .tray = &t});
        return true;
    }
    bool trayMenuSelect(Tray &t, uint32_t id) override {
        auto *m = findItem(static_cast<HeadlessTray &>(t).menu, id);
        if (!m || !m->enabled)
            return false;
        emit({.type = EventType::TrayMenuItem, .tray = &t, .id = id});
        return true;
    }
    bool trayProbe(Tray &t, TrayProbe *out) override {
        auto &h = static_cast<HeadlessTray &>(t);
        out->iconSizes.clear();
        for (const auto &i : h.icons)
            out->iconSizes.push_back({i.width, i.height});
        out->tooltip    = h.tooltip;
        out->isTemplate = h.iconTempl;
        out->menuLabels.clear();
        flattenLabels(h.menu, out->menuLabels);
        return true;
    }
    bool notificationInvoke(uint64_t id, std::string_view action) override {
        auto it = notifications.find(id);
        if (it == notifications.end())
            return false;
        emit({.type = EventType::NotificationActivated, .id = id, .action = std::string(action)});
        return true;
    }
    bool notificationProbe(uint64_t id, NotificationProbe *out) override {
        auto it = notifications.find(id);
        if (it == notifications.end())
            return false;
        out->title     = it->second.title;
        out->body      = it->second.body;
        out->imageSize = {it->second.image.width, it->second.image.height};
        out->silent    = it->second.silent;
        out->actionLabels.clear();
        for (const auto &a : it->second.actions)
            out->actionLabels.push_back(a.label);
        return true;
    }
    uint64_t maxNotificationId() const override {
        uint64_t m = 0;
        for (const auto &[id, n] : notifications)
            m = std::max(m, id);
        return m;
    }
    size_t notificationCount() const override { return notifications.size(); }
    int    badgeCount() override { return badge; }
    bool   wantsAttention(Window &w, bool *out) override {
        *out = static_cast<HeadlessWindow &>(w).attention;
        return true;
    }
    bool readPixel(Window &win, int x, int y, uint32_t *argb) override {
        auto &w = static_cast<HeadlessWindow &>(win);
        if (x < 0 || y < 0 || x >= w.pw || y >= w.ph || w.pixels.empty())
            return false;
        *argb = w.pixels[size_t(y) * w.pw + x];
        return true;
    }

    std::vector<std::string>         openedUrls;
    std::vector<HeadlessTray *>      trays;
    std::map<uint64_t, Notification> notifications;
    int                              badge = 0;

private:
    Event dropEvent(EventType t, Point p, bool withData) {
        Event e{.type = t, .pos = p};
        e.dropAction     = core::preferredAction(_drag->actions);
        e.allowedActions = _drag->actions;
        for (const auto &i : _drag->items) {
            e.items.push_back({i.mime, withData ? i.data : std::string()});
            if (!withData)
                continue;
            if (i.mime == "text/uri-list") {
                size_t start = 0;
                while (start < i.data.size()) {
                    size_t end = i.data.find('\n', start);
                    if (end == std::string::npos)
                        end = i.data.size();
                    std::string line = i.data.substr(start, end - start);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (!line.empty() && line[0] != '#')
                        e.uris.push_back(line);
                    start = end + 1;
                }
            } else if (core::isTextMime(i.mime)) {
                e.text = i.data;
            }
        }
        return e;
    }
    void dragOver(HeadlessWindow *w, Point p) {
        if (w != _dropTarget) {
            if (_dropTarget)
                _dropTarget->emit({.type = EventType::DropLeave});
            _dropTarget  = w;
            w->dropReply = core::preferredAction(_drag->actions);
            w->emit(dropEvent(EventType::DropEnter, p, false));
        } else {
            w->emit(dropEvent(EventType::DropMove, p, false));
        }
    }
    void finishDrag() {
        DropAction result = DropAction::None;
        if (_dropTarget && _dropTarget->dropReply != DropAction::None) {
            result = _dropTarget->dropReply;
            _dropTarget->emit(dropEvent(EventType::Drop, _pointer, true));
        } else if (_dropTarget) {
            _dropTarget->emit({.type = EventType::DropLeave});
        }
        auto *src = _dragSource;
        _drag.reset();
        _dragSource = _dropTarget = nullptr;
        src->emit({.type = EventType::DragFinished, .dropAction = result});
    }

    void focus(HeadlessWindow *w) {
        if (_focus == w)
            return;
        if (_focus)
            _focus->emit({.type = EventType::FocusOut});
        _focus = w;
        w->emit({.type = EventType::FocusIn});
    }
    void updateMods(Key k, bool down) {
        uint32_t bit = 0;
        switch (k) {
        case Key::ShiftLeft:
        case Key::ShiftRight:
            bit = ModShift;
            break;
        case Key::ControlLeft:
        case Key::ControlRight:
            bit = ModCtrl;
            break;
        case Key::AltLeft:
        case Key::AltRight:
            bit = ModAlt;
            break;
        case Key::SuperLeft:
        case Key::SuperRight:
            bit = ModSuper;
            break;
        default:
            return;
        }
        _mods = down ? (_mods | bit) : (_mods & ~bit);
    }
    static std::string usText(Key k, bool shift) {
        if (k >= Key::A && k <= Key::Z)
            return std::string(1, char((shift ? 'A' : 'a') + (int(k) - int(Key::A))));
        if (k >= Key::Num0 && k <= Key::Num9 && !shift)
            return std::string(1, char('0' + (int(k) - int(Key::Num0))));
        if (k == Key::Space)
            return " ";
        return {};
    }

    core::LoopCore                          _core;
    std::mutex                              _wakeMutex;
    std::condition_variable                 _cv;
    bool                                    _woken = false, _quit = false;
    std::vector<HeadlessWindow *>           _windows;
    HeadlessWindow                         *_focus = nullptr, *_hover = nullptr;
    std::vector<DataItem>                   _clipboard[2]; // by Selection
    bool                                    _online = true;
    std::optional<std::vector<std::string>> _dialogAnswer;
    std::function<void(FileDialogResult)>   _dialogPending;
    bool                                    _dialogAvailable = true, _hasLastDialog = false;
    std::optional<DragDesc>                 _drag;
    HeadlessWindow                         *_dragSource = nullptr, *_dropTarget = nullptr;
    bool                                    _buttonHeld               = false;
    uint32_t                                _mods                     = 0;
    bool                                    _down[size_t(Key::Count)] = {};
    Point                                   _pointer;
    core::ClickCounter                      _clicks;

    friend class HeadlessWindow;
};

HeadlessWindow::HeadlessWindow(HeadlessApp *a, const WindowDesc &d)
    : app(a), title(d.title), _size(d.size), _min(d.minSize), _pos(d.position.value_or(Point{})),
      _decorations(d.decorations) {
    if (d.visible)
        show();
}

HeadlessWindow::~HeadlessWindow() {
    app->forget(this);
}
HeadlessTray::~HeadlessTray() {
    app->forgetTray(this);
}

void HeadlessWindow::emit(Event e) {
    e.window = this;
    app->emit(e);
}

void HeadlessWindow::setSize(Size s) {
    s.w = std::max(s.w, _min.w);
    s.h = std::max(s.h, _min.h);
    if (s.w == _size.w && s.h == _size.h)
        return;
    _size = s;
    emit({.type = EventType::Resized});
    requestFrame();
}

void HeadlessWindow::setScale(double s) {
    _scale = s;
    emit({.type = EventType::Resized});
    requestFrame();
}

bool HeadlessWindow::setPosition(Point p) {
    _pos = p;
    emit({.type = EventType::Moved});
    return true;
}

uint64_t HeadlessWindow::monitor() const {
    const double cx = _pos.x + _size.w / 2.0, cy = _pos.y + _size.h / 2.0;
    for (const auto &m : app->monitors())
        if (cx >= m.bounds.x && cx < m.bounds.x + m.bounds.w && cy >= m.bounds.y &&
            cy < m.bounds.y + m.bounds.h)
            return m.id;
    return 1;
}

void HeadlessWindow::minimize() {
    _minimized = true;
    emit({.type = EventType::StateChanged});
}
void HeadlessWindow::setMaximized(bool on) {
    _maximized = on;
    emit({.type = EventType::StateChanged});
}
void HeadlessWindow::setAlwaysOnTop(bool on) {
    _above = on;
    emit({.type = EventType::StateChanged});
}
void HeadlessWindow::setFullscreen(bool on) {
    _fullscreen = on;
    emit({.type = EventType::StateChanged});
}
void HeadlessWindow::activate() {
    _active   = true;
    attention = false;
    app->focus(this);
    if (_minimized) { // activating restores, as the OS window managers do
        _minimized = false;
        emit({.type = EventType::StateChanged});
    }
}

void HeadlessWindow::requestFrame() {
    framePending = true;
    app->_core.wake();
}

Canvas HeadlessWindow::beginPaint() {
    const int w = int(_size.w * _scale + 0.5), h = int(_size.h * _scale + 0.5);
    if (w != pw || h != ph || back.empty()) {
        pw = w;
        ph = h;
        back.assign(size_t(w) * h, 0);
    }
    return {back.data(), pw, ph, pw, _scale};
}

void HeadlessWindow::endPaint(const std::vector<Rect> &) {
    pixels = back;
    ++framesPresented;
}

} // namespace

std::unique_ptr<App> createHeadlessApp(std::string *) {
    return std::make_unique<HeadlessApp>();
}

namespace testing_internal {
// Unit tests poke headless-only state (scale changes, opened URLs).
void setHeadlessScale(Window &w, double s) {
    static_cast<HeadlessWindow &>(w).setScale(s);
}
int headlessFrames(Window &w) {
    return static_cast<HeadlessWindow &>(w).framesPresented;
}
Cursor headlessCursor(Window &w) {
    return static_cast<HeadlessWindow &>(w).cursor;
}
HitArea headlessLastHit(Window &w) {
    return static_cast<HeadlessWindow &>(w).lastHit;
}
} // namespace testing_internal

} // namespace plat

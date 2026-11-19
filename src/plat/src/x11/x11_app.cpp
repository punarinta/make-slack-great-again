// X11 backend: connection, event loop, input and the odds and ends (cursors,
// scale, XTEST hooks). Windows and present live in x11_window.cpp,
// clipboard and XDND in x11_selection.cpp.
#include "x11/x11_internal.h"
#include "core/pacing.h"
#include "freedesktop/cursor_names.h"
#include "prim/utf8.h"

#include <xcb/randr.h>
#include <xcb/shm.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>
// xkb.h names a struct member `explicit`; rename it for C++.
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit
#ifdef PLAT_TEST_HOOKS
#include <xcb/xtest.h>
#endif
#include <xkbcommon/xkbcommon-x11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>

namespace plat::x11 {

namespace {

constexpr const char *kAtomNames[AtomCount] = {
    "WM_PROTOCOLS",
    "WM_DELETE_WINDOW",
    "WM_CHANGE_STATE",
    "WM_CLIENT_MACHINE",
    "_NET_WM_PING",
    "_NET_WM_NAME",
    "_NET_WM_PID",
    "_NET_WM_STATE",
    "_NET_WM_STATE_MAXIMIZED_VERT",
    "_NET_WM_STATE_MAXIMIZED_HORZ",
    "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_HIDDEN",
    "_NET_WM_WINDOW_TYPE",
    "_NET_WM_WINDOW_TYPE_NORMAL",
    "_NET_ACTIVE_WINDOW",
    "_NET_WM_MOVERESIZE",
    "_NET_SUPPORTED",
    "_NET_SUPPORTING_WM_CHECK",
    "_MOTIF_WM_HINTS",
    "UTF8_STRING",
    "CLIPBOARD",
    "TARGETS",
    "TIMESTAMP",
    "TEXT",
    "INCR",
    core::kTextMime,
    "text/plain",
    "text/uri-list",
    "PLAT_SELECTION",
    "PLAT_DND",
    "PLAT_TIME",
    "XdndAware",
    "XdndEnter",
    "XdndPosition",
    "XdndStatus",
    "XdndLeave",
    "XdndDrop",
    "XdndFinished",
    "XdndSelection",
    "XdndTypeList",
    "XdndActionCopy",
    "XdndActionMove",
    "XdndActionLink",
    "XdndActionAsk",
    "XdndActionList",
    "XdndProxy",
    "MULTIPLE",
    "SAVE_TARGETS",
    "text/html",
    "image/png",
    "_NET_WM_STATE_DEMANDS_ATTENTION",
    "_NET_WORKAREA",
    "_NET_CURRENT_DESKTOP",
    "_NET_MOVERESIZE_WINDOW",
    "_NET_STARTUP_ID",
    "_NET_STARTUP_INFO_BEGIN",
    "_NET_STARTUP_INFO",
    "_NET_WM_STATE_ABOVE",
    "_NET_WM_ICON",
};

// _NET_WM_MOVERESIZE directions (EWMH).
int moveResizeDirection(HitArea a) {
    switch (a) {
    case HitArea::ResizeTopLeft:
        return 0;
    case HitArea::ResizeTop:
        return 1;
    case HitArea::ResizeTopRight:
        return 2;
    case HitArea::ResizeRight:
        return 3;
    case HitArea::ResizeBottomRight:
        return 4;
    case HitArea::ResizeBottom:
        return 5;
    case HitArea::ResizeBottomLeft:
        return 6;
    case HitArea::ResizeLeft:
        return 7;
    default:
        return 8; // Caption → move
    }
}

bool debugEnabled() {
    static const bool on = std::getenv("PLAT_X11_DEBUG") != nullptr;
    return on;
}

} // namespace

// ── setup ───────────────────────────────────────────────────────────────────

void X11App::emitThemeChanged() {
    std::vector<X11Window *> windows;
    for (auto &[id, w] : _windows)
        windows.push_back(w);
    for (auto *w : windows)
        w->emit({.type = EventType::ThemeChanged});
}

std::string X11App::parentHandle(Window *w) {
    // xdg-desktop-portal's parent_window format for X11.
    if (!w)
        return {};
    char buf[32];
    std::snprintf(buf, sizeof buf, "x11:%x", static_cast<X11Window *>(w)->xid());
    return buf;
}

X11App::~X11App() {
    _loop.core.shutdown(); // pending closures go while the loop still works
    resetServices();       // it watches fds on our loop
    for (auto *ev : _deferred)
        std::free(ev);
    if (_c) {
        for (auto cur : _cursors)
            if (cur)
                xcb_free_cursor(_c, cur);
        if (_cursorCtx)
            xcb_cursor_context_free(_cursorCtx);
        if (_cursorFont)
            xcb_close_font(_c, _cursorFont);
        if (_helper)
            xcb_destroy_window(_c, _helper);
        if (_colormap && _colormap != _screen->default_colormap)
            xcb_free_colormap(_c, _colormap);
        xcb_disconnect(_c);
    }
}

bool X11App::init(std::string *error) {
    auto fail = [&](const char *why) {
        if (error)
            *error = std::string("x11: ") + why;
        return false;
    };
    int screenNum = 0;
    _c            = xcb_connect(nullptr, &screenNum);
    if (!_c || xcb_connection_has_error(_c))
        return fail("cannot connect to the X server ($DISPLAY)");

    const xcb_setup_t *setup = xcb_get_setup(_c);
    auto               it    = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenNum && it.rem; ++i)
        xcb_screen_next(&it);
    if (!it.rem)
        return fail("screen not found");
    _screen = it.data;
    _root   = _screen->root;

    // Intern everything in one round trip's worth of latency.
    xcb_intern_atom_cookie_t cookies[AtomCount];
    for (int i = 0; i < AtomCount; ++i)
        cookies[i] = xcb_intern_atom(_c, 0, uint16_t(std::strlen(kAtomNames[i])), kAtomNames[i]);
    for (int i = 0; i < AtomCount; ++i) {
        Reply r(xcb_intern_atom_reply(_c, cookies[i], nullptr));
        _atoms[i]                   = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
        _internCache[kAtomNames[i]] = _atoms[i];
    }

    _maxRequestBytes = std::max<uint32_t>(xcb_get_maximum_request_length(_c) * 4u, 16384u);

    // Visual: a 32-bit ARGB one lets a compositor honour our alpha; without
    // one (plain Xvfb, no compositor) the root visual's 0x00RRGGBB layout takes
    // the same premultiplied pixels and just ignores A.
    const bool noArgb = std::getenv("PLAT_X11_NO_ARGB") != nullptr;
    auto       isRgb  = [](const xcb_visualtype_t *v) {
        return v->_class == XCB_VISUAL_CLASS_TRUE_COLOR && v->red_mask == 0xff0000 &&
               v->green_mask == 0xff00 && v->blue_mask == 0xff;
    };
    const xcb_visualtype_t *rootVisual = nullptr;
    for (auto d = xcb_screen_allowed_depths_iterator(_screen); d.rem; xcb_depth_next(&d)) {
        for (auto v = xcb_depth_visuals_iterator(d.data); v.rem; xcb_visualtype_next(&v)) {
            if (v.data->visual_id == _screen->root_visual)
                rootVisual = v.data;
            if (!noArgb && !_visual && d.data->depth == 32 && isRgb(v.data)) {
                _visual = v.data->visual_id;
                _depth  = 32;
            }
        }
    }
    if (_visual) {
        _colormap = xcb_generate_id(_c);
        xcb_create_colormap(_c, XCB_COLORMAP_ALLOC_NONE, _colormap, _root, _visual);
    } else {
        if (!rootVisual || !isRgb(rootVisual) ||
            (_screen->root_depth != 24 && _screen->root_depth != 32))
            return fail("unsupported root visual (need 24/32-bit TrueColor)");
        _visual   = _screen->root_visual;
        _depth    = _screen->root_depth;
        _colormap = _screen->default_colormap;
    }
    // Canvas pixels go to the server as-is, so its ZPixmap layout must be
    // 32 bits per pixel in our byte order.
    bool bpp32 = false;
    for (auto f = xcb_setup_pixmap_formats_iterator(setup); f.rem; xcb_format_next(&f))
        if (f.data->depth == _depth)
            bpp32 = f.data->bits_per_pixel == 32;
    const uint16_t probe   = 1;
    const bool     hostLsb = *reinterpret_cast<const uint8_t *>(&probe) == 1;
    if (!bpp32 || (setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST) != hostLsb)
        return fail("unsupported pixmap format (need 32 bpp in host byte order)");

    // Root property changes carry Xft.dpi (RESOURCE_MANAGER) and WM restarts.
    const uint32_t rootMask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(_c, _root, XCB_CW_EVENT_MASK, &rootMask);

    readScale();
    readWmSupport();
    if (!setupXkb() && debugEnabled())
        std::fprintf(stderr, "plat/x11: XKB unavailable, keys map by US position only\n");
    setupShm();
#ifdef PLAT_TEST_HOOKS
    _xtest = xcb_get_extension_data(_c, &xcb_test_id)->present;
#endif
    if (xcb_get_extension_data(_c, &xcb_xfixes_id)->present) {
        // XFixes requests fail until the client announced its version.
        Reply v(xcb_xfixes_query_version_reply(_c, xcb_xfixes_query_version(_c, 5, 0), nullptr));
        _xfixes = v && v->major_version >= 2; // window shape regions arrived in 2.0
    }
    setupXInput();
    setupRandr();
    refreshMonitors(false);
    if (xcb_cursor_context_new(_c, _screen, &_cursorCtx) < 0)
        _cursorCtx = nullptr;
    initSelection();
    xcb_flush(_c);

    _xcbWatch =
        _loop.watch(xcb_get_file_descriptor(_c), FdRead, [this](uint32_t) { dispatchAll(true); });
    _loop.beforeWait = [this] { return beforeWait(); };
    return !xcb_connection_has_error(_c) || fail("connection failed during setup");
}

void X11App::readScale() {
    double s = 1.0;
    if (const char *env = std::getenv("PLAT_SCALE"); env && *env) {
        s            = std::strtod(env, nullptr);
        _scaleForced = true;
    } else {
        // Xft.dpi is what GTK and every Xft client scale by; desktops set
        // it (and xsettingsd mirrors it) when the user picks a scale.
        auto cookie =
            xcb_get_property(_c, 0, _root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 0, 1 << 16);
        Reply r(xcb_get_property_reply(_c, cookie, nullptr));
        if (r && r->format == 8) {
            const std::string_view db(
                static_cast<const char *>(xcb_get_property_value(r.p)),
                size_t(xcb_get_property_value_length(r.p))
            );
            size_t pos = 0;
            while (pos < db.size()) {
                size_t end = db.find('\n', pos);
                if (end == std::string_view::npos)
                    end = db.size();
                std::string_view line = db.substr(pos, end - pos);
                pos                   = end + 1;
                if (line.substr(0, 8) == "Xft.dpi:") {
                    const double dpi = std::strtod(std::string(line.substr(8)).c_str(), nullptr);
                    if (dpi > 0)
                        s = dpi / 96.0;
                }
            }
        }
    }
    if (!(s >= 0.5 && s <= 8.0))
        s = 1.0;
    if (s == _scale)
        return;
    _scale = s;
    for (auto &[id, w] : std::unordered_map(_windows))
        w->onScaleChanged();
    // Logical monitor geometry follows the scale.
    if (_monitorsReady)
        scheduleMonitorRefresh(false);
}

void X11App::readWmSupport() {
    // An EWMH WM advertises itself through a check window that points at
    // itself; a stale property left by a dead WM fails the second lookup.
    auto getWindow = [this](xcb_window_t on) -> xcb_window_t {
        auto  ck = xcb_get_property(_c, 0, on, _atoms[NetSupportingWmCheck], XCB_ATOM_WINDOW, 0, 1);
        Reply r(xcb_get_property_reply(_c, ck, nullptr));
        if (!r || r->format != 32 || xcb_get_property_value_length(r.p) < 4)
            return 0;
        return *static_cast<xcb_window_t *>(xcb_get_property_value(r.p));
    };
    const xcb_window_t check = getWindow(_root);
    _wmPresent               = check && getWindow(check) == check;
    _netSupported.clear();
    if (!_wmPresent)
        return;
    auto  ck = xcb_get_property(_c, 0, _root, _atoms[NetSupported], XCB_ATOM_ATOM, 0, 4096);
    Reply r(xcb_get_property_reply(_c, ck, nullptr));
    if (r && r->format == 32) {
        auto *a = static_cast<xcb_atom_t *>(xcb_get_property_value(r.p));
        _netSupported.assign(a, a + xcb_get_property_value_length(r.p) / 4);
    }
}

bool X11App::wmSupports(AtomId a) const {
    return _wmPresent &&
           std::find(_netSupported.begin(), _netSupported.end(), _atoms[a]) != _netSupported.end();
}

bool X11App::setupXkb() {
    if (!xkb_x11_setup_xkb_extension(
            _c,
            XKB_X11_MIN_MAJOR_XKB_VERSION,
            XKB_X11_MIN_MINOR_XKB_VERSION,
            XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
            nullptr,
            nullptr,
            &_xkbEvent,
            nullptr
        ))
        return false;
    _kbdDevice = xkb_x11_get_core_keyboard_device_id(_c);
    if (_kbdDevice < 0)
        return false;
    reloadKeymap();

    // The events xkbcommon-x11 documents as needed to keep a state in sync.
    constexpr uint16_t events = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY |
                                XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_STATE_NOTIFY;
    constexpr uint16_t mapParts =
        XCB_XKB_MAP_PART_KEY_TYPES | XCB_XKB_MAP_PART_KEY_SYMS | XCB_XKB_MAP_PART_MODIFIER_MAP |
        XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS | XCB_XKB_MAP_PART_KEY_ACTIONS |
        XCB_XKB_MAP_PART_VIRTUAL_MODS | XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP;
    constexpr uint16_t stateDetails =
        XCB_XKB_STATE_PART_MODIFIER_BASE | XCB_XKB_STATE_PART_MODIFIER_LATCH |
        XCB_XKB_STATE_PART_MODIFIER_LOCK | XCB_XKB_STATE_PART_GROUP_BASE |
        XCB_XKB_STATE_PART_GROUP_LATCH | XCB_XKB_STATE_PART_GROUP_LOCK;
    xcb_xkb_select_events_details_t details{};
    details.affectNewKeyboard  = XCB_XKB_NKN_DETAIL_KEYCODES;
    details.newKeyboardDetails = XCB_XKB_NKN_DETAIL_KEYCODES;
    details.affectState        = stateDetails;
    details.stateDetails       = stateDetails;
    xcb_xkb_select_events_aux(
        _c, xcb_xkb_device_spec_t(_kbdDevice), events, 0, 0, mapParts, mapParts, &details
    );

    // Detectable auto-repeat: held keys arrive as press, press, …, release
    // instead of release/press pairs, so `repeat` is just "already down".
    auto ck = xcb_xkb_per_client_flags(
        _c,
        xcb_xkb_device_spec_t(_kbdDevice),
        XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
        XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
        0,
        0,
        0
    );
    Reply r(xcb_xkb_per_client_flags_reply(_c, ck, nullptr));
    return true;
}

void X11App::reloadKeymap() {
    xkb_keymap *km =
        xkb_x11_keymap_new_from_device(_kbd.context(), _c, _kbdDevice, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km)
        return;
    xkb_state *st = xkb_x11_state_new_from_device(km, _c, _kbdDevice);
    if (!st) {
        xkb_keymap_unref(km);
        return;
    }
    _kbd.setKeymap(km, st);
}

void X11App::setupShm() {
    if (std::getenv("PLAT_X11_NO_SHM"))
        return;
    if (!xcb_get_extension_data(_c, &xcb_shm_id)->present)
        return;
    // Shared memory only means anything to a server on this machine: over
    // TCP (ssh -X, remote displays) a SysV id names someone else's segment
    // and fd passing would kill the connection. A unix socket is the test.
    sockaddr_storage addr{};
    socklen_t        len = sizeof addr;
    if (getsockname(xcb_get_file_descriptor(_c), reinterpret_cast<sockaddr *>(&addr), &len) != 0 ||
        addr.ss_family != AF_UNIX)
        return;
    Reply v(xcb_shm_query_version_reply(_c, xcb_shm_query_version(_c), nullptr));
    if (!v)
        return;
    _shm      = true;
    _shmFd    = v->major_version > 1 || (v->major_version == 1 && v->minor_version >= 2);
    _shmEvent = xcb_get_extension_data(_c, &xcb_shm_id)->first_event;
    if (std::getenv("PLAT_X11_NO_SHM_FD"))
        _shmFd = false;
}

xcb_atom_t X11App::intern(const std::string &name) {
    if (auto it = _internCache.find(name); it != _internCache.end())
        return it->second;
    Reply            r(xcb_intern_atom_reply(
        _c, xcb_intern_atom(_c, 0, uint16_t(name.size()), name.c_str()), nullptr
    ));
    const xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
    _internCache[name] = a;
    return a;
}

std::unique_ptr<Window> X11App::createWindow(const WindowDesc &desc) {
    auto w = std::make_unique<X11Window>(this, desc);
    if (!w->ok())
        return nullptr;
    return w;
}

void X11App::registerWindow(X11Window *w) {
    _windows[w->xid()] = w;
}

void X11App::forgetWindow(X11Window *w) {
    _windows.erase(w->xid());
    if (_focus == w)
        _focus = nullptr;
    if (_pointerWin == w)
        _pointerWin = nullptr;
    if (_dnd.target == w->xid()) {
        // Mid-drop: the source still waits for an answer.
        if (_dnd.dropping)
            dndFinishTarget(false, XCB_ATOM_NONE);
        _dnd = {};
    }
    if (_drag.active && _drag.source == w) {
        // No window left to tell; end the protocol side quietly.
        _drag.source = nullptr;
        if (!_drag.released)
            dragLeaveTarget();
        dragFinish(DropAction::None);
    }
}

X11Window *X11App::findWindow(xcb_window_t id) const {
    auto it = _windows.find(id);
    return it == _windows.end() ? nullptr : it->second;
}

// ── loop ────────────────────────────────────────────────────────────────────

void X11App::run() {
    _loop.run();
}

void X11App::pump(int timeoutMs) {
    _loop.iterate(timeoutMs);
}

bool X11App::beforeWait() {
    if (_lost)
        return true;
    // Replies read by synchronous calls (property reads, get_image, …) may
    // have pulled events into xcb's queue without the fd ever becoming
    // readable again: drain them, send due Frames, and repeat a few times in
    // case painting queued more — poll() must never sleep on queued events.
    for (int i = 0; i < 4; ++i) {
        if (_drag.moved)
            dragTrack(); // once per batch of motion; its replies may queue events
        const bool any = dispatchAll(false);
        emitFrames();
        if (!any && _deferred.empty() && !_drag.moved)
            break;
    }
    xcb_flush(_c);
    if (xcb_connection_has_error(_c)) {
        connectionLost();
        return false;
    }
    // Something (a Frame handler, a deferred wait, a drag motion) left work
    // queued: go round again without sleeping.
    if (!_deferred.empty() || _drag.moved)
        return false;
    const auto now = core::Clock::now();
    for (auto &[id, w] : _windows)
        if (w->frameReady(now, nullptr))
            return false;
    return true;
}

bool X11App::dispatchAll(bool readSocket) {
    bool any = false;
    while (!_lost) {
        xcb_generic_event_t *ev = nullptr;
        if (!_deferred.empty()) {
            ev = _deferred.front();
            _deferred.pop_front();
        } else {
            ev = readSocket ? xcb_poll_for_event(_c) : xcb_poll_for_queued_event(_c);
        }
        if (!ev)
            break;
        any = true;
        handle(ev);
        std::free(ev);
    }
    flushMotion();
    if (!_lost && xcb_connection_has_error(_c))
        connectionLost();
    return any;
}

void X11App::connectionLost() {
    if (_lost)
        return;
    _lost = true;
    _loop.unwatch(_xcbWatch);
    std::fprintf(stderr, "plat/x11: lost the X server connection\n");
    // The app decides what to do; its windows are gone either way.
    for (auto &[id, w] : std::unordered_map(_windows))
        w->emit({.type = EventType::CloseRequested});
}

bool X11App::waitForEvent(const std::function<bool(xcb_generic_event_t *)> &match, int timeoutMs) {
    for (auto it = _deferred.begin(); it != _deferred.end(); ++it) {
        if (match(*it)) {
            std::free(*it);
            _deferred.erase(it);
            return true;
        }
    }
    xcb_flush(_c);
    const auto deadline = core::Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!xcb_connection_has_error(_c)) {
        while (xcb_generic_event_t *ev = xcb_poll_for_event(_c)) {
            if (match(ev)) {
                std::free(ev);
                return true;
            }
            _deferred.push_back(ev); // everything else keeps its order for dispatch
        }
        const auto left =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - core::Clock::now());
        if (left.count() <= 0)
            return false;
        pollfd p{xcb_get_file_descriptor(_c), POLLIN, 0};
        poll(&p, 1, int(left.count()));
    }
    return false;
}

void X11App::emitFrames() {
    const auto                now     = core::Clock::now();
    core::Clock::time_point   soonest = core::Clock::time_point::max();
    // Snapshot ids: a Frame handler may create or destroy windows. The
    // vector is reused across calls (a nested call gets its own).
    std::vector<xcb_window_t> ids     = std::move(_frameIds);
    ids.clear();
    for (auto &[id, w] : _windows)
        ids.push_back(id);
    for (xcb_window_t id : ids) {
        X11Window *w = findWindow(id);
        if (!w)
            continue;
        core::Clock::time_point notBefore{};
        if (w->frameReady(now, &notBefore))
            w->sendFrame(now);
        else if (notBefore != core::Clock::time_point{})
            soonest = std::min(soonest, notBefore);
    }
    _frameIds = std::move(ids);
    if (soonest != core::Clock::time_point::max() && !_frameTimer) {
        const int ms = int(std::max<long long>(
            1, std::chrono::ceil<std::chrono::milliseconds>(soonest - now).count()
        ));
        // Only wakes the loop; beforeWait sends the Frame.
        _frameTimer  = addTimer(ms, false, [this] { _frameTimer = 0; });
    }
}

// ── events ──────────────────────────────────────────────────────────────────

void X11App::handle(xcb_generic_event_t *ev) {
    const uint8_t type = ev->response_type & 0x7f;
    // Anything but more motion goes out after the held move, in order.
    if (_motionWin && type != XCB_MOTION_NOTIFY &&
        !(type == XCB_GE_GENERIC &&
          reinterpret_cast<xcb_ge_generic_event_t *>(ev)->event_type == XCB_INPUT_MOTION))
        flushMotion();
    switch (type) {
    case 0: {
        if (debugEnabled()) {
            auto *e = reinterpret_cast<xcb_generic_error_t *>(ev);
            std::fprintf(
                stderr,
                "plat/x11: X error %d (major %d minor %d, resource 0x%x)\n",
                e->error_code,
                e->major_code,
                e->minor_code,
                e->resource_id
            );
        }
        return;
    }
    case XCB_EXPOSE: {
        auto *e = reinterpret_cast<xcb_expose_event_t *>(ev);
        if (auto *w = findWindow(e->window))
            w->onExpose(e->x, e->y, e->width, e->height, e->count);
        return;
    }
    case XCB_CONFIGURE_NOTIFY: {
        auto *e = reinterpret_cast<xcb_configure_notify_event_t *>(ev);
        if (e->event == e->window)
            if (auto *w = findWindow(e->window))
                w->onConfigure(e);
        return;
    }
    case XCB_REPARENT_NOTIFY: {
        auto *e = reinterpret_cast<xcb_reparent_notify_event_t *>(ev);
        if (e->event == e->window)
            if (auto *w = findWindow(e->window))
                w->onReparent(e->parent);
        return;
    }
    case XCB_MAP_NOTIFY: {
        auto *e = reinterpret_cast<xcb_map_notify_event_t *>(ev);
        if (auto *w = findWindow(e->window))
            w->onMapped(true);
        return;
    }
    case XCB_UNMAP_NOTIFY: {
        auto *e = reinterpret_cast<xcb_unmap_notify_event_t *>(ev);
        if (auto *w = findWindow(e->window))
            w->onMapped(false);
        return;
    }
    case XCB_PROPERTY_NOTIFY: {
        auto *e   = reinterpret_cast<xcb_property_notify_event_t *>(ev);
        _lastTime = e->time;
        if (e->window == _root) {
            if (e->atom == XCB_ATOM_RESOURCE_MANAGER && !_scaleForced)
                readScale();
            else if (e->atom == _atoms[NetSupportingWmCheck] || e->atom == _atoms[NetSupported])
                readWmSupport();
            else if (isWorkareaAtom(e->atom))
                scheduleMonitorRefresh(false);
            return;
        }
        if (onSelectionProperty(e))
            return;
        if (auto *w = findWindow(e->window))
            w->onProperty(e->atom);
        return;
    }
    case XCB_FOCUS_IN:
    case XCB_FOCUS_OUT: {
        auto *e = reinterpret_cast<xcb_focus_in_event_t *>(ev);
        // Grab/ungrab focus changes (WM key grabs, menus) are transient, and
        // NotifyPointer means "keys follow the pointer", not real focus.
        if (e->mode == XCB_NOTIFY_MODE_GRAB || e->mode == XCB_NOTIFY_MODE_UNGRAB ||
            e->detail == XCB_NOTIFY_DETAIL_POINTER)
            return;
        X11Window *w = findWindow(e->event);
        if (!w)
            return;
        if (type == XCB_FOCUS_IN)
            focusWindow(w);
        else if (_focus == w && e->detail != XCB_NOTIFY_DETAIL_INFERIOR)
            focusWindow(nullptr);
        return;
    }
    case XCB_ENTER_NOTIFY:
    case XCB_LEAVE_NOTIFY: {
        auto *e      = reinterpret_cast<xcb_enter_notify_event_t *>(ev);
        _lastTime    = e->time;
        X11Window *w = findWindow(e->event);
        if (!w || e->detail == XCB_NOTIFY_DETAIL_INFERIOR)
            return;
        const Point p = w->toLogical(e->event_x, e->event_y);
        if (type == XCB_ENTER_NOTIFY)
            resetScrollBases();
        if (type == XCB_ENTER_NOTIFY && !w->hover) {
            w->hover    = true;
            _pointerWin = w;
            _pointerPos = p;
            if (!(_buttonsHeld & 0x30e))
                w->pointerAt(p);
            w->emit({.type = EventType::PointerEnter, .pos = p, .mods = pointerMods(e->state)});
        } else if (type == XCB_LEAVE_NOTIFY && w->hover) {
            w->hover = false;
            if (_pointerWin == w)
                _pointerWin = nullptr;
            w->emit({.type = EventType::PointerLeave, .pos = p, .mods = pointerMods(e->state)});
        }
        return;
    }
    case XCB_MOTION_NOTIFY: {
        auto *e   = reinterpret_cast<xcb_motion_notify_event_t *>(ev);
        _lastTime = e->time;
        if (_drag.active) {
            dragMotion(e->root_x, e->root_y, e->time);
            return;
        }
        if (X11Window *w = findWindow(e->event))
            handleMotion(w, e->event_x, e->event_y, pointerMods(e->state), e->time);
        return;
    }
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE: {
        auto      *e    = reinterpret_cast<xcb_button_press_event_t *>(ev);
        const bool down = type == XCB_BUTTON_PRESS;
        if (_drag.active) {
            // Under the drag grab: the release of the last held button drops.
            _lastTime = e->time;
            if (e->detail < 32) {
                if (down)
                    _buttonsHeld |= 1u << e->detail;
                else
                    _buttonsHeld &= ~(1u << e->detail);
            }
            if (!down && !(_buttonsHeld & 0x30e) && !_drag.released)
                dragRelease(e->time);
            return;
        }
        handleButton(e, down);
        return;
    }
    case XCB_KEY_PRESS:
    case XCB_KEY_RELEASE: {
        auto *e = reinterpret_cast<xcb_key_press_event_t *>(ev);
        if (_drag.active) {
            _lastTime = e->time;
            dragKey(e, type == XCB_KEY_PRESS);
            return;
        }
        handleKey(e, type == XCB_KEY_PRESS);
        return;
    }
    case XCB_GE_GENERIC:
        handleXInput(ev);
        return;
    case XCB_CLIENT_MESSAGE:
        handleClientMessage(reinterpret_cast<xcb_client_message_event_t *>(ev));
        return;
    case XCB_SELECTION_REQUEST:
        onSelectionRequest(reinterpret_cast<xcb_selection_request_event_t *>(ev));
        return;
    case XCB_SELECTION_CLEAR:
        onSelectionClear(reinterpret_cast<xcb_selection_clear_event_t *>(ev));
        return;
    case XCB_SELECTION_NOTIFY:
        onSelectionNotify(reinterpret_cast<xcb_selection_notify_event_t *>(ev));
        return;
    default:
        break;
    }
    if (_xkbEvent && type == _xkbEvent) {
        handleXkb(ev);
        return;
    }
    if (_randrEvent && (type == _randrEvent + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
                        type == _randrEvent + XCB_RANDR_NOTIFY)) {
        scheduleMonitorRefresh(true);
        return;
    }
    if (_shm && type == _shmEvent + XCB_SHM_COMPLETION) {
        auto *e = reinterpret_cast<xcb_shm_completion_event_t *>(ev);
        if (auto *w = findWindow(e->drawable))
            w->onShmCompletion();
    }
}

void X11App::handleMotion(X11Window *w, double px, double py, uint32_t mods, xcb_timestamp_t t) {
    _lastTime          = t;
    const Point p      = {px / _scale, py / _scale};
    // During an implicit grab motion keeps coming from outside the window;
    // that is a drag, not a re-entry.
    const bool  inside = px >= 0 && py >= 0 && px < w->physW() && py < w->physH();
    if (_motionWin && (_motionWin != w->xid() || _motionMods != mods || (!w->hover && inside)))
        flushMotion();
    if (!w->hover && inside) {
        w->hover    = true;
        _pointerWin = w;
        w->emit({.type = EventType::PointerEnter, .pos = p, .mods = mods});
    }
    _pointerPos = p;
    _motionWin  = w->xid();
    _motionPos  = p;
    _motionMods = mods;
}

void X11App::flushMotion() {
    const xcb_window_t id = _motionWin;
    _motionWin            = 0;
    if (X11Window *w = id ? findWindow(id) : nullptr) {
        // Not while a button is held: a press that started inside (a text
        // selection) keeps its cursor when it crosses the border.
        if (!(_buttonsHeld & 0x30e))
            w->pointerAt(_motionPos);
        w->emit({.type = EventType::PointerMove, .pos = _motionPos, .mods = _motionMods});
    }
}

void X11App::handleXkb(xcb_generic_event_t *ev) {
    // All XKB events share one core event code; the subtype is byte 1.
    struct Any {
        uint8_t         response_type, xkbType;
        uint16_t        sequence;
        xcb_timestamp_t time;
        uint8_t         deviceID;
    };
    const auto *any = reinterpret_cast<const Any *>(ev);
    if (any->deviceID != uint8_t(_kbdDevice))
        return;
    switch (any->xkbType) {
    case XCB_XKB_NEW_KEYBOARD_NOTIFY: {
        auto *e = reinterpret_cast<xcb_xkb_new_keyboard_notify_event_t *>(ev);
        if (e->changed & XCB_XKB_NKN_DETAIL_KEYCODES)
            reloadKeymap();
        break;
    }
    case XCB_XKB_MAP_NOTIFY:
        reloadKeymap();
        break;
    case XCB_XKB_STATE_NOTIFY: {
        auto *e = reinterpret_cast<xcb_xkb_state_notify_event_t *>(ev);
        _kbd.updateMask(
            e->baseMods,
            e->latchedMods,
            e->lockedMods,
            uint32_t(e->baseGroup),
            uint32_t(e->latchedGroup),
            uint32_t(e->lockedGroup)
        );
        // Shift/Ctrl during a drag change the requested action.
        if (_drag.active && _drag.target && !_drag.released &&
            dragRequestedAction() != _drag.sentAction) {
            if (_drag.waitingStatus)
                _drag.pending = true;
            else
                dragSendPosition();
        }
        break;
    }
    default:
        break;
    }
}

uint32_t X11App::pointerMods(uint16_t state) const {
    if (_kbd.hasKeymap())
        return _kbd.mods();
    uint32_t m = 0;
    if (state & XCB_MOD_MASK_SHIFT)
        m |= ModShift;
    if (state & XCB_MOD_MASK_CONTROL)
        m |= ModCtrl;
    if (state & XCB_MOD_MASK_1)
        m |= ModAlt;
    if (state & XCB_MOD_MASK_4)
        m |= ModSuper;
    if (state & XCB_MOD_MASK_LOCK)
        m |= ModCaps;
    if (state & XCB_MOD_MASK_2)
        m |= ModNum;
    return m;
}

void X11App::focusWindow(X11Window *w) {
    if (_focus == w)
        return;
    if (_focus)
        _focus->setActive(false);
    _focus = w;
    if (w)
        w->setActive(true);
}

void X11App::handleButton(xcb_button_press_event_t *e, bool down) {
    _lastTime    = e->time;
    X11Window *w = findWindow(e->event);
    if (!w)
        return;
    const Point    p    = w->toLogical(e->event_x, e->event_y);
    const uint32_t mods = pointerMods(e->state);
    _pointerPos         = p;

    // Core wheel: buttons 4/5 vertical, 6/7 horizontal, one notch per press.
    if (e->detail >= 4 && e->detail <= 7) {
        // A device with XI2 scroll valuators already scrolled through
        // XI_Motion; the server's emulated wheel buttons carry its timestamp.
        if (_xiScrolled && e->time == _xiScrollTime)
            return;
        if (down) {
            Event s{.type = EventType::Scroll, .pos = p, .mods = mods};
            s.dy = e->detail == 4 ? -1 : e->detail == 5 ? 1 : 0;
            s.dx = e->detail == 6 ? -1 : e->detail == 7 ? 1 : 0;
            w->emit(s);
        }
        return;
    }
    Button b;
    switch (e->detail) {
    case 1:
        b = Button::Left;
        break;
    case 2:
        b = Button::Middle;
        break;
    case 3:
        b = Button::Right;
        break;
    case 8:
        b = Button::Back;
        break;
    case 9:
        b = Button::Forward;
        break;
    default:
        return;
    }
    const uint32_t bit = 1u << e->detail;
    if (down) {
        _buttonsHeld |= bit;
        // A new press means the release we meant to swallow went elsewhere
        // (the WM took the move/resize grab).
        _swallowRelease &= ~bit;
    } else {
        _buttonsHeld &= ~bit;
    }

    if (!down) {
        if (_swallowRelease & bit) {
            _swallowRelease &= ~bit;
            return;
        }
        w->emit({.type = EventType::PointerUp, .pos = p, .button = b, .mods = mods});
        return;
    }

    // With no WM nobody else gives us focus on click (ICCCM passive input).
    if (!_wmPresent && _focus != w)
        xcb_set_input_focus(_c, XCB_INPUT_FOCUS_PARENT, w->xid(), e->time);

    if (b == Button::Left) {
        const HitArea hit = w->hitTest(p);
        if (isNonClient(hit)) {
            _swallowRelease |= bit;
            startMoveResize(w, hit, e);
            return;
        }
    }

    const int clicks = _clicks.press(int(e->detail), p.x, p.y, e->time, doubleClickMs(), 4, 4);
    w->emit(
        {.type = EventType::PointerDown, .pos = p, .button = b, .clicks = clicks, .mods = mods}
    );
}

void X11App::startMoveResize(X11Window *w, HitArea a, const xcb_button_press_event_t *e) {
    if (!wmSupports(NetWmMoveResize))
        return; // no WM to hand it to; the press is still not a click
    // The press started an implicit grab the WM cannot take over; release it
    // so the WM's own grab succeeds.
    xcb_ungrab_pointer(_c, e->time);
    const uint32_t data[5] = {
        uint32_t(e->root_x), uint32_t(e->root_y), uint32_t(moveResizeDirection(a)), e->detail, 1
    };
    sendToRoot(w->xid(), _atoms[NetWmMoveResize], data);
    xcb_flush(_c);
}

void X11App::handleKey(xcb_key_press_event_t *e, bool down) {
    _lastTime    = e->time;
    X11Window *w = findWindow(e->event);
    if (!w)
        return;
    // Keys can arrive without a FocusIn (focus-follows-pointer with no WM);
    // the app always sees focus first.
    if (_focus != w)
        focusWindow(w);
    const uint8_t code   = e->detail;
    const bool    repeat = down && _keyDown[code];
    _keyDown[code]       = down;
    // The server tracks XKB state and tells us via StateNotify; don't feed
    // keys into the local state as well.
    auto r               = _kbd.key(code, down, false);
    if (!_kbd.hasKeymap())
        r.mods = pointerMods(e->state);
    if (debugEnabled() && down)
        std::fprintf(stderr, "plat/x11: key %u -> %s\n", unsigned(code), keyName(r.key));
    Event k{
        .type     = down ? EventType::KeyDown : EventType::KeyUp,
        .key      = r.key,
        .scancode = uint32_t(code) - 8, // evdev, as on Wayland
        .mods     = r.mods,
        .repeat   = repeat
    };
    w->emit(k);
    if (down && !r.text.empty() && w->textInputEnabled() && findWindow(e->event))
        w->emit({.type = EventType::TextInput, .text = std::move(r.text)});
}

void X11App::handleClientMessage(xcb_client_message_event_t *e) {
    if (e->window == _helper) { // our drag's source window
        if (e->type == _atoms[XdndStatus])
            dragOnStatus(e);
        else if (e->type == _atoms[XdndFinished])
            dragOnFinished(e);
        return;
    }
    X11Window *w = findWindow(e->window);
    if (!w)
        return;
    if (e->type == _atoms[WmProtocols] && e->format == 32) {
        const xcb_atom_t proto = e->data.data32[0];
        if (proto == _atoms[WmDeleteWindow]) {
            w->emit({.type = EventType::CloseRequested});
        } else if (proto == _atoms[NetWmPing]) {
            // Bounce it back to the root so the WM knows we are alive.
            xcb_client_message_event_t reply = *e;
            reply.response_type              = XCB_CLIENT_MESSAGE;
            reply.window                     = _root;
            xcb_send_event(
                _c,
                0,
                _root,
                XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                reinterpret_cast<const char *>(&reply)
            );
            xcb_flush(_c);
        }
        return;
    }
    for (AtomId a : {XdndEnter, XdndPosition, XdndLeave, XdndDrop})
        if (e->type == _atoms[a]) {
            onXdnd(w, e);
            return;
        }
}

void X11App::sendToRoot(xcb_window_t win, xcb_atom_t type, const uint32_t data[5]) {
    xcb_client_message_event_t ev{};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format        = 32;
    ev.window        = win;
    ev.type          = type;
    std::memcpy(ev.data.data32, data, 5 * sizeof(uint32_t));
    xcb_send_event(
        _c,
        0,
        _root,
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
        reinterpret_cast<const char *>(&ev)
    );
}

xcb_timestamp_t X11App::serverTime() {
    // ICCCM: selection ownership needs a real timestamp, not CurrentTime. A
    // zero-length append to our own property makes the server stamp a
    // PropertyNotify for us.
    xcb_change_property(
        _c, XCB_PROP_MODE_APPEND, _helper, _atoms[PlatTime], XCB_ATOM_STRING, 8, 0, nullptr
    );
    xcb_timestamp_t t  = XCB_CURRENT_TIME;
    const bool      ok = waitForEvent(
        [&](xcb_generic_event_t *ev) {
            if ((ev->response_type & 0x7f) != XCB_PROPERTY_NOTIFY)
                return false;
            auto *p = reinterpret_cast<xcb_property_notify_event_t *>(ev);
            if (p->window != _helper || p->atom != _atoms[PlatTime])
                return false;
            t = p->time;
            return true;
        },
        500
    );
    if (ok)
        _lastTime = t;
    return t;
}

// ── cursors ─────────────────────────────────────────────────────────────────

xcb_cursor_t X11App::cursor(Cursor c) {
    const size_t i = size_t(c);
    if (i >= std::size(_cursors))
        return XCB_NONE;
    if (_cursors[i])
        return _cursors[i];

    if (c == Cursor::Hidden) {
        // A 1×1 cursor whose mask is empty.
        const xcb_pixmap_t pix = xcb_generate_id(_c);
        xcb_create_pixmap(_c, 1, pix, _root, 1, 1);
        const xcb_gcontext_t gc = xcb_generate_id(_c);
        const uint32_t       fg = 0;
        xcb_create_gc(_c, gc, pix, XCB_GC_FOREGROUND, &fg);
        const xcb_rectangle_t r{0, 0, 1, 1};
        xcb_poly_fill_rectangle(_c, pix, gc, 1, &r);
        const xcb_cursor_t cur = xcb_generate_id(_c);
        xcb_create_cursor(_c, cur, pix, pix, 0, 0, 0, 0, 0, 0, 0, 0);
        xcb_free_gc(_c, gc);
        xcb_free_pixmap(_c, pix);
        return _cursors[i] = cur;
    }

    // Theme names (freedesktop/cursor_names), then the core cursor-font glyph
    // (X11/cursorfont.h) for servers or setups with no theme at all.
    static constexpr uint16_t kGlyphs[] = {
        68, 152, 60, 150, 150, 34, 0, 52, 58, 52, 108, 116, 14, 12
    };
    static_assert(std::size(kGlyphs) == size_t(Cursor::Hidden));
    xcb_cursor_t cur = XCB_NONE;
    if (_cursorCtx)
        for (const char *const *n = linux_cursor::themeNames(c); *n; ++n)
            if ((cur = xcb_cursor_load_cursor(_cursorCtx, *n)) != XCB_NONE)
                break;
    if (cur == XCB_NONE) {
        if (!_cursorFont) {
            _cursorFont = xcb_generate_id(_c);
            xcb_open_font(_c, _cursorFont, 6, "cursor");
        }
        cur               = xcb_generate_id(_c);
        const uint16_t gl = kGlyphs[i];
        xcb_create_glyph_cursor(
            _c, cur, _cursorFont, _cursorFont, gl, gl + 1, 0, 0, 0, 0xffff, 0xffff, 0xffff
        );
    }
    return _cursors[i] = cur;
}

#ifdef PLAT_TEST_HOOKS
// ── TestHooks (XTEST) ───────────────────────────────────────────────────────

bool X11App::injectKey(Window &win, Key k, bool down) {
    if (!_xtest)
        return false;
    auto          &w    = static_cast<X11Window &>(win);
    const uint32_t code = linux_input::evdevFromKey(k);
    if (!code)
        return false;
    // Like a user clicking the window first: keys go where the test aims.
    if (_focus != &w)
        xcb_set_input_focus(_c, XCB_INPUT_FOCUS_PARENT, w.xid(), XCB_CURRENT_TIME);
    xcb_test_fake_input(
        _c,
        down ? XCB_KEY_PRESS : XCB_KEY_RELEASE,
        uint8_t(code + 8),
        XCB_CURRENT_TIME,
        _root,
        0,
        0,
        0
    );
    xcb_flush(_c);
    return true;
}

bool X11App::injectPointerMove(Window &win, Point logical) {
    if (!_xtest)
        return false;
    auto &w = static_cast<X11Window &>(win);
    Reply r(xcb_translate_coordinates_reply(
        _c, xcb_translate_coordinates(_c, w.xid(), _root, 0, 0), nullptr
    ));
    if (!r)
        return false;
    const int x = r->dst_x + int(std::lround(logical.x * _scale));
    const int y = r->dst_y + int(std::lround(logical.y * _scale));
    xcb_test_fake_input(
        _c, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, _root, int16_t(x), int16_t(y), 0
    );
    xcb_flush(_c);
    return true;
}

bool X11App::injectButton(Window &, Button b, bool down) {
    if (!_xtest)
        return false;
    static constexpr uint8_t kButton[] = {1, 3, 2, 8, 9}; // Left Right Middle Back Forward
    xcb_test_fake_input(
        _c,
        down ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE,
        kButton[size_t(b)],
        XCB_CURRENT_TIME,
        XCB_NONE,
        0,
        0,
        0
    );
    xcb_flush(_c);
    return true;
}

bool X11App::injectScroll(Window &, double dx, double dy) {
    if (!_xtest)
        return false;
    auto clicks = [&](double d, uint8_t neg, uint8_t pos) {
        const int n = d == 0 ? 0 : std::max(1, int(std::lround(std::abs(d))));
        for (int i = 0; i < n; ++i)
            for (uint8_t t : {uint8_t(XCB_BUTTON_PRESS), uint8_t(XCB_BUTTON_RELEASE)})
                xcb_test_fake_input(_c, t, d < 0 ? neg : pos, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
    };
    clicks(dy, 4, 5);
    clicks(dx, 6, 7);
    xcb_flush(_c);
    return true;
}

bool X11App::readPixel(Window &win, int x, int y, uint32_t *argb) {
    auto &w = static_cast<X11Window &>(win);
    if (x < 0 || y < 0 || x >= w.physW() || y >= w.physH())
        return false;
    // Read the root, not the window: that is what is actually on screen.
    Reply t(xcb_translate_coordinates_reply(
        _c, xcb_translate_coordinates(_c, w.xid(), _root, int16_t(x), int16_t(y)), nullptr
    ));
    if (!t)
        return false;
    Reply img(xcb_get_image_reply(
        _c,
        xcb_get_image(_c, XCB_IMAGE_FORMAT_Z_PIXMAP, _root, t->dst_x, t->dst_y, 1, 1, ~0u),
        nullptr
    ));
    if (!img || xcb_get_image_data_length(img.p) < 4)
        return false;
    uint32_t v;
    std::memcpy(&v, xcb_get_image_data(img.p), 4);
    *argb = v | 0xff000000u; // the root has no alpha
    return true;
}

bool X11App::wantsAttention(Window &win, bool *out) {
    auto &w = static_cast<X11Window &>(win);
    // Read the server's copy, not our flag: that is what a taskbar sees.
    if (wmSupports(NetWmStateDemandsAttention)) {
        Reply r(xcb_get_property_reply(
            _c, xcb_get_property(_c, 0, w.xid(), _atoms[NetWmState], XCB_ATOM_ATOM, 0, 64), nullptr
        ));
        bool  on = false;
        if (r && r->format == 32) {
            auto *a = static_cast<xcb_atom_t *>(xcb_get_property_value(r.p));
            for (int i = 0, n = xcb_get_property_value_length(r.p) / 4; i < n; ++i)
                on |= a[i] == _atoms[NetWmStateDemandsAttention];
        }
        *out = on;
        return true;
    }
    Reply r(xcb_get_property_reply(
        _c, xcb_get_property(_c, 0, w.xid(), XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 0, 9), nullptr
    ));
    if (!r || r->format != 32 || xcb_get_property_value_length(r.p) < 4)
        return false;
    *out = (*static_cast<uint32_t *>(xcb_get_property_value(r.p)) & (1u << 8)) != 0; // UrgencyHint
    return true;
}
#endif

// ── text helpers ────────────────────────────────────────────────────────────

std::string latin1ToUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x80) {
            out += char(c);
        } else {
            out += char(0xc0 | (c >> 6));
            out += char(0x80 | (c & 0x3f));
        }
    }
    return out;
}

std::string utf8ToLatin1(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        // Invalid UTF-8 decodes as U+FFFD, so it becomes '?' like any other
        // character Latin-1 has no room for.
        const uint32_t cp = prim::utf8::decode(s, i);
        out += cp <= 0xff ? char(cp) : '?';
    }
    return out;
}

} // namespace plat::x11

namespace plat {

std::unique_ptr<App> createX11App(std::string *error) {
    auto app = std::make_unique<x11::X11App>();
    if (!app->init(error))
        return nullptr;
    app->startServices();
    return app;
}

} // namespace plat

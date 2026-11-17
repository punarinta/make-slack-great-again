// X11 windows: creation, ICCCM/EWMH properties, state, and present through
// MIT-SHM (memfd or SysV) with a chunked PutImage fallback for remote servers.
#include "x11/x11_internal.h"

#include "core/image_util.h"

#include <xcb/shm.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <unistd.h>

namespace plat::x11 {

namespace {

constexpr uint32_t kEventMask =
    XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE |
    XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
    XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
    XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE;

// A put still unacknowledged after this long is assumed lost (a server that
// drops completions must not freeze painting for good).
constexpr auto kShmStall = std::chrono::milliseconds(250);

void setProp(
    xcb_connection_t *c,
    xcb_window_t      w,
    xcb_atom_t        prop,
    xcb_atom_t        type,
    uint8_t           format,
    uint32_t          count,
    const void       *data
) {
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, prop, type, format, count, data);
}

} // namespace

X11Window::X11Window(X11App *app, const WindowDesc &d)
    : _app(app), _min(d.minSize), _resizable(d.resizable), _decorations(d.decorations) {
    xcb_connection_t *c = app->conn();
    const double      s = app->scale();
    _pw                 = std::max(1, int(std::lround(std::max(d.size.w, d.minSize.w) * s)));
    _ph                 = std::max(1, int(std::lround(std::max(d.size.h, d.minSize.h) * s)));
    _parent             = app->root();
    if (d.position) {
        _rootX   = int(std::lround(d.position->x * s));
        _rootY   = int(std::lround(d.position->y * s));
        _userPos = true;
        _userX   = _rootX;
        _userY   = _rootY;
    }

    _win                    = xcb_generate_id(c);
    // Background None: the server never clears to a colour before our pixels
    // arrive, so resizes don't flash. Border pixel + colormap are mandatory
    // for a visual that differs from the parent's (the ARGB case).
    const uint32_t mask     = XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_BIT_GRAVITY |
                              XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    const uint32_t values[] = {
        XCB_BACK_PIXMAP_NONE, 0, XCB_GRAVITY_NORTH_WEST, kEventMask, app->colormap()
    };
    auto ck = xcb_create_window_checked(
        c,
        app->depth(),
        _win,
        app->root(),
        int16_t(_rootX),
        int16_t(_rootY),
        uint16_t(_pw),
        uint16_t(_ph),
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        app->visual(),
        mask,
        values
    );
    if (xcb_generic_error_t *err = xcb_request_check(c, ck)) {
        std::free(err);
        _win = 0;
        return;
    }
    const uint32_t noExposures = 0;
    _gc                        = xcb_generate_id(c);
    xcb_create_gc(c, _gc, _win, XCB_GC_GRAPHICS_EXPOSURES, &noExposures);

    setTitle(d.title);
    // WM_CLASS is "instance\0class\0": the app id, then wmClass (or the id).
    std::string cls = d.appId + '\0' + (d.wmClass.empty() ? d.appId : d.wmClass) + '\0';
    setProp(c, _win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, uint32_t(cls.size()), cls.data());
    // _NET_WM_ICON: width, height, then ARGB rows, for each size in turn.
    // Non-premultiplied; sizes past one request's limit are left out.
    std::vector<uint32_t> icon;
    for (const Image &im : d.icon) {
        const size_t n = size_t(im.width) * size_t(im.height);
        if (im.empty() || im.pixels.size() < n ||
            (icon.size() + 2 + n) * 4 + 64 > app->maxRequestBytes())
            continue;
        icon.push_back(uint32_t(im.width));
        icon.push_back(uint32_t(im.height));
        for (size_t i = 0; i < n; ++i) {
            icon.push_back(core::unpremultiply(im.pixels[i])); // _NET_WM_ICON's format
        }
    }
    if (!icon.empty())
        setProp(
            c, _win, app->atom(NetWmIcon), XCB_ATOM_CARDINAL, 32, uint32_t(icon.size()), icon.data()
        );
    const xcb_atom_t protocols[] = {app->atom(WmDeleteWindow), app->atom(NetWmPing)};
    setProp(c, _win, app->atom(WmProtocols), XCB_ATOM_ATOM, 32, 2, protocols);
    // _NET_WM_PID + WM_CLIENT_MACHINE let the WM offer "force quit" when a
    // ping goes unanswered.
    const uint32_t pid = uint32_t(getpid());
    setProp(c, _win, app->atom(NetWmPid), XCB_ATOM_CARDINAL, 32, 1, &pid);
    char host[256] = {};
    if (gethostname(host, sizeof host - 1) == 0)
        setProp(
            c,
            _win,
            XCB_ATOM_WM_CLIENT_MACHINE,
            XCB_ATOM_STRING,
            8,
            uint32_t(std::strlen(host)),
            host
        );
    writeWmHints();
    const xcb_atom_t type = app->atom(NetWmWindowTypeNormal);
    setProp(c, _win, app->atom(NetWmWindowType), XCB_ATOM_ATOM, 32, 1, &type);
    writeNormalHints();
    if (_decorations == Decorations::Custom) {
        // Motif hints, flags = MWM_HINTS_DECORATIONS, decorations = none:
        // the one "no title bar" switch every X WM honours.
        const uint32_t motif[5] = {2, 0, 0, 0, 0};
        setProp(c, _win, app->atom(MotifWmHints), app->atom(MotifWmHints), 32, 5, motif);
    }
    app->markDndAware(_win);
    app->selectXInput(_win);
    app->registerWindow(this);
    if (d.visible)
        show();
}

X11Window::~X11Window() {
    if (!_win)
        return;
    _app->forgetWindow(this);
    xcb_connection_t *c = _app->conn();
    freeBuffer(_buf);
    xcb_free_gc(c, _gc);
    xcb_destroy_window(c, _win);
    xcb_flush(c);
}

void X11Window::emit(Event e) {
    e.window = this;
    _app->emit(e);
}

Point X11Window::toLogical(int x, int y) const {
    const double s = _app->scale();
    return {x / s, y / s};
}

Size X11Window::size() const {
    const double s = _app->scale();
    return {int(std::lround(_pw / s)), int(std::lround(_ph / s))};
}

double X11Window::scale() const {
    return _app->scale();
}

void X11Window::setTitle(std::string_view utf8) {
    xcb_connection_t *c = _app->conn();
    setProp(
        c,
        _win,
        _app->atom(NetWmName),
        _app->atom(Utf8String),
        8,
        uint32_t(utf8.size()),
        utf8.data()
    );
    // Legacy WM_NAME is Latin-1 by definition.
    const std::string l1 = utf8ToLatin1(utf8);
    setProp(c, _win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, uint32_t(l1.size()), l1.data());
}

void X11Window::writeWmHints() {
    // WM_HINTS: InputHint|StateHint, input = True, NormalState. Without the
    // input hint some WMs never give keyboard focus. UrgencyHint (bit 8) is
    // the ICCCM attention flag every WM and taskbar understands.
    const uint32_t hints[9] = {1u | 2u | (_attention ? 1u << 8 : 0u), 1, 1, 0, 0, 0, 0, 0, 0};
    setProp(_app->conn(), _win, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, 9, hints);
}

void X11Window::requestAttention() {
    if (_active || _attention)
        return;
    _attention = true;
    writeWmHints();
    // EWMH WMs flash the taskbar entry for _NET_WM_STATE_DEMANDS_ATTENTION.
    sendNetWmState(true, _app->atom(NetWmStateDemandsAttention), 0);
}

void X11Window::writeNormalHints() {
    // WM_SIZE_HINTS: flags, x, y, w, h, min w/h, max w/h, inc w/h,
    // min/max aspect, base w/h, gravity — 18 CARD32s, physical pixels.
    const double s     = _app->scale();
    uint32_t     h[18] = {};
    h[0]               = 1u << 4; // PMinSize
    h[5]               = uint32_t(std::max(1, int(std::lround(_min.w * s))));
    h[6]               = uint32_t(std::max(1, int(std::lround(_min.h * s))));
    if (!_resizable) {
        h[0] |= 1u << 5; // PMaxSize = current size: not resizable
        h[5] = h[7] = uint32_t(_pw);
        h[6] = h[8] = uint32_t(_ph);
    }
    if (_userPos) {
        h[0] |= 1u << 0; // USPosition: the WM keeps an explicit position
        h[1] = uint32_t(_userX);
        h[2] = uint32_t(_userY);
    }
    // PWinGravity = StaticGravity: a position we request (ConfigureWindow or
    // the initial x/y) means where the *content* goes, not the WM frame —
    // the contract's meaning of Window::position.
    h[0] |= 1u << 9;
    h[17] = 10;
    setProp(_app->conn(), _win, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, h);
}

void X11Window::setSize(Size logical) {
    logical.w          = std::max(logical.w, _min.w);
    logical.h          = std::max(logical.h, _min.h);
    const double   s   = _app->scale();
    const uint32_t v[] = {
        uint32_t(std::max(1, int(std::lround(logical.w * s)))),
        uint32_t(std::max(1, int(std::lround(logical.h * s))))
    };
    if (!_resizable) {
        // Update the fixed-size hints first or the WM clamps us back.
        const int pw = _pw, ph = _ph;
        _pw = int(v[0]);
        _ph = int(v[1]);
        writeNormalHints();
        _pw = pw;
        _ph = ph;
    }
    xcb_configure_window(_app->conn(), _win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
    xcb_flush(_app->conn());
}

void X11Window::setMinSize(Size logical) {
    _min = logical;
    writeNormalHints();
    const Size cur = size();
    if (cur.w < logical.w || cur.h < logical.h)
        setSize({std::max(cur.w, logical.w), std::max(cur.h, logical.h)});
}

void X11Window::show() {
    xcb_map_window(_app->conn(), _win);
    requestFrame();
}

void X11Window::hide() {
    xcb_unmap_window(_app->conn(), _win);
    // ICCCM 4.1.4: a synthetic UnmapNotify tells a reparenting WM we asked
    // for Withdrawn, not Iconic.
    xcb_unmap_notify_event_t ev{};
    ev.response_type = XCB_UNMAP_NOTIFY;
    ev.event         = _app->root();
    ev.window        = _win;
    xcb_send_event(
        _app->conn(),
        0,
        _app->root(),
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
        reinterpret_cast<const char *>(&ev)
    );
    xcb_flush(_app->conn());
}

void X11Window::minimize() {
    const uint32_t data[5] = {3 /* IconicState */, 0, 0, 0, 0};
    _app->sendToRoot(_win, _app->atom(WmChangeState), data);
    xcb_flush(_app->conn());
}

void X11Window::sendNetWmState(bool add, xcb_atom_t a, xcb_atom_t b) {
    if (_mapped) {
        // Mapped: ask the WM (EWMH client message, source = application).
        const uint32_t data[5] = {add ? 1u : 0u, a, b, 1, 0};
        _app->sendToRoot(_win, _app->atom(NetWmState), data);
    } else {
        // Unmapped: the WM reads _NET_WM_STATE when it maps us.
        std::vector<xcb_atom_t> st;
        if (_maximized) {
            st.push_back(_app->atom(NetWmStateMaxVert));
            st.push_back(_app->atom(NetWmStateMaxHorz));
        }
        if (_fullscreen)
            st.push_back(_app->atom(NetWmStateFullscreen));
        if (_above)
            st.push_back(_app->atom(NetWmStateAbove));
        if (_attention)
            st.push_back(_app->atom(NetWmStateDemandsAttention));
        setProp(
            _app->conn(),
            _win,
            _app->atom(NetWmState),
            XCB_ATOM_ATOM,
            32,
            uint32_t(st.size()),
            st.data()
        );
    }
    xcb_flush(_app->conn());
}

void X11Window::setMaximized(bool on) {
    if (!_mapped)
        _maximized = on;
    sendNetWmState(on, _app->atom(NetWmStateMaxVert), _app->atom(NetWmStateMaxHorz));
}

void X11Window::setFullscreen(bool on) {
    if (!_mapped)
        _fullscreen = on;
    sendNetWmState(on, _app->atom(NetWmStateFullscreen), 0);
}

void X11Window::setAlwaysOnTop(bool on) {
    // Mapped, the WM's _NET_WM_STATE echo confirms it (onProperty); set the
    // flag now too, so a WM that ignores ABOVE still reads back what we asked.
    _above = on;
    sendNetWmState(on, _app->atom(NetWmStateAbove), 0);
}

void X11Window::activate() {
    activateAt(_app->lastTime());
}

void X11Window::activateWithToken(std::string_view token) {
    // X11's token is a startup-notification id (DESKTOP_STARTUP_ID, handed
    // over by a second instance): "<launcher>-<pid>-<host>-<app>-<n>_TIME<t>".
    // Tagging the window with it lets the WM match the launch it belongs to;
    // the _TIME part is the timestamp of the user action that launched it,
    // newer than anything we saw, so focus-stealing prevention lets the
    // activation through. Anything else (a Wayland token) means nothing here.
    const size_t at = token.rfind("_TIME");
    if (token.empty() || at == std::string_view::npos) {
        activate();
        return;
    }
    xcb_timestamp_t t = 0;
    for (size_t i = at + 5; i < token.size() && token[i] >= '0' && token[i] <= '9'; ++i)
        t = t * 10 + xcb_timestamp_t(token[i] - '0');
    setProp(
        _app->conn(),
        _win,
        _app->atom(NetStartupId),
        _app->atom(Utf8String),
        8,
        uint32_t(token.size()),
        token.data()
    );
    sendStartupRemove(token);
    // Server time wraps; take whichever of the two is later.
    const xcb_timestamp_t last = _app->lastTime();
    activateAt(t && int32_t(t - last) > 0 ? t : last);
}

void X11Window::sendStartupRemove(std::string_view id) {
    // Startup-notification spec: "remove: ID=<id>" ends the launch feedback
    // (busy cursor, taskbar placeholder) — ours to send once the launch
    // turned into an existing window. The message travels as a NUL-terminated
    // string in 20-byte ClientMessage chunks, first as _BEGIN, to the root.
    std::string msg = "remove: ID=\"";
    for (char ch : id) {
        if (ch == '"' || ch == '\\')
            msg += '\\';
        msg += ch;
    }
    msg += '"';
    msg += '\0';
    for (size_t off = 0; off < msg.size(); off += 20) {
        xcb_client_message_event_t ev{};
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format        = 8;
        ev.window        = _win;
        ev.type          = _app->atom(off == 0 ? NetStartupInfoBegin : NetStartupInfo);
        std::memcpy(ev.data.data8, msg.data() + off, std::min<size_t>(20, msg.size() - off));
        xcb_send_event(
            _app->conn(),
            0,
            _app->root(),
            XCB_EVENT_MASK_PROPERTY_CHANGE,
            reinterpret_cast<const char *>(&ev)
        );
    }
}

void X11Window::activateAt(xcb_timestamp_t t) {
    xcb_connection_t *c = _app->conn();
    if (_app->hasWm()) {
        // Source 1 = application; the WM applies focus-stealing prevention
        // with the timestamp of the user interaction behind the request.
        const uint32_t data[5] = {1, t, 0, 0, 0};
        _app->sendToRoot(_win, _app->atom(NetActiveWindow), data);
    } else if (_mapped) {
        const uint32_t above = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(c, _win, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        xcb_set_input_focus(c, XCB_INPUT_FOCUS_PARENT, _win, XCB_CURRENT_TIME);
    }
    xcb_flush(c);
}

void X11Window::setCursor(Cursor cur) {
    _cursor = cur;
    applyCursor();
}

void X11Window::pointerAt(Point p) {
    // The WM shows resize cursors over its own frame; we asked for none, so
    // over our border we have to, or the edges look inert until dragged.
    _onEdge = false;
    if (_hitTest && _resizable && !_maximized && !_fullscreen) {
        _onEdge = true;
        switch (_hitTest(p)) {
        case HitArea::ResizeTop:
        case HitArea::ResizeBottom:
            _edge = Cursor::ResizeV;
            break;
        case HitArea::ResizeLeft:
        case HitArea::ResizeRight:
            _edge = Cursor::ResizeH;
            break;
        case HitArea::ResizeTopLeft:
        case HitArea::ResizeBottomRight:
            _edge = Cursor::ResizeNWSE;
            break;
        case HitArea::ResizeTopRight:
        case HitArea::ResizeBottomLeft:
            _edge = Cursor::ResizeNESW;
            break;
        default:
            _onEdge = false;
            break;
        }
    }
    applyCursor();
}

void X11Window::applyCursor() {
    const Cursor want = _onEdge ? _edge : _cursor;
    if (want == _shown)
        return;
    _shown               = want;
    const xcb_cursor_t x = _app->cursor(want);
    xcb_change_window_attributes(_app->conn(), _win, XCB_CW_CURSOR, &x);
    xcb_flush(_app->conn());
}

void X11Window::setActive(bool on) {
    if (_active == on)
        return;
    _active = on;
    if (on && _attention) {
        // Activation ends the request (most WMs clear their state themselves;
        // the hint is ours to drop).
        _attention = false;
        writeWmHints();
        sendNetWmState(false, _app->atom(NetWmStateDemandsAttention), 0);
    }
    emit({.type = on ? EventType::FocusIn : EventType::FocusOut});
    emit({.type = EventType::StateChanged});
}

// ── server notifications ────────────────────────────────────────────────────

// ── position ────────────────────────────────────────────────────────────────

std::optional<Point> X11Window::position() const {
    // The last position the server reported (Moved fires as it changes), so
    // position() and Moved always agree.
    return toLogical(_rootX, _rootY);
}

bool X11Window::setPosition(Point logical) {
    const double      s = _app->scale();
    const int         x = int(std::lround(logical.x * s)), y = int(std::lround(logical.y * s));
    xcb_connection_t *c = _app->conn();
    // Hints first: a WM mapping us later (or re-mapping after hide()) keeps
    // a USPosition placement instead of choosing its own.
    _userPos            = true;
    _userX              = x;
    _userY              = y;
    writeNormalHints();
    if (_mapped && _app->wmSupports(NetMoveResizeWindow)) {
        // EWMH move with explicit StaticGravity (x/y = content position) and
        // source = application; clear about intent even to WMs that ignore
        // WM_NORMAL_HINTS gravity on ConfigureRequests.
        const uint32_t data[5] = {
            10u | (1u << 8) | (1u << 9) | (1u << 12), uint32_t(x), uint32_t(y), 0, 0
        };
        _app->sendToRoot(_win, _app->atom(NetMoveResizeWindow), data);
    } else {
        // No WM: this moves the window itself. With a WM: a ConfigureRequest
        // it interprets with our StaticGravity; before mapping, the initial
        // position it reads together with USPosition.
        const uint32_t v[] = {uint32_t(int32_t(x)), uint32_t(int32_t(y))};
        xcb_configure_window(c, _win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
    }
    xcb_flush(c);
    return true;
}

uint64_t X11Window::monitor() const {
    return _app->monitorAt(_rootX + _pw / 2, _rootY + _ph / 2);
}

void X11Window::setRootPos(int x, int y) {
    if (x == _rootX && y == _rootY)
        return;
    _rootX = x;
    _rootY = y;
    emit({.type = EventType::Moved});
}

void X11Window::queryRootPos() {
    xcb_connection_t *c = _app->conn();
    Reply             r(xcb_translate_coordinates_reply(
        c, xcb_translate_coordinates(c, _win, _app->root(), 0, 0), nullptr
    ));
    if (r)
        setRootPos(r->dst_x, r->dst_y);
}

void X11Window::onReparent(xcb_window_t parent) {
    _parent = parent;
    queryRootPos(); // the frame's decorations just moved the content
}

void X11Window::onConfigure(const xcb_configure_notify_event_t *e) {
    // ICCCM 4.1.5: a synthetic ConfigureNotify (the WM moved our frame)
    // carries root coordinates; a real one is relative to the parent, which
    // is only the root without a reparenting WM.
    const bool synthetic = (e->response_type & 0x80) != 0;
    if (synthetic)
        setRootPos(e->x, e->y);
    else if (_parent == _app->root())
        setRootPos(e->x + e->border_width, e->y + e->border_width);
    else
        queryRootPos();
    const int w = e->width, h = e->height;
    if (w == _pw && h == _ph)
        return;
    _pw = w;
    _ph = h;
    emit({.type = EventType::Resized});
    _framePending = true;
}

void X11Window::onExpose(int x, int y, int w, int h, int count) {
    // Our buffer still holds the last frame: repair exposures from it without
    // bothering the app. Only a missing or stale-sized buffer needs a Frame.
    // A series (count = how many follow) goes out as one batch at its end:
    // one completion and one flush.
    if (_presented && _buf.pixels && _buf.w == _pw && _buf.h == _ph && !_painting) {
        Rect r{x, y, std::min(w, _buf.w - x), std::min(h, _buf.h - y)};
        if (r.w > 0 && r.h > 0)
            _exposed.push_back(r);
        if (count > 0)
            return;
        for (size_t i = 0; i < _exposed.size(); ++i)
            putRect(_exposed[i], i + 1 == _exposed.size());
        if (!_exposed.empty())
            xcb_flush(_app->conn());
    } else {
        _framePending = true;
    }
    if (count == 0)
        _exposed.clear();
}

void X11Window::onMapped(bool mapped) {
    _mapped = mapped;
    if (mapped) {
        _framePending = true;
        queryRootPos(); // where the WM put us
    }
}

void X11Window::onProperty(xcb_atom_t atom) {
    xcb_connection_t *c = _app->conn();
    if (atom == _app->atom(NetWmState)) {
        auto  ck = xcb_get_property(c, 0, _win, atom, XCB_ATOM_ATOM, 0, 64);
        auto *r  = xcb_get_property_reply(c, ck, nullptr);
        bool  mv = false, mh = false, fs = false, hid = false, above = false;
        if (r && r->format == 32) {
            auto *a = static_cast<xcb_atom_t *>(xcb_get_property_value(r));
            for (int i = 0, n = xcb_get_property_value_length(r) / 4; i < n; ++i) {
                mv |= a[i] == _app->atom(NetWmStateMaxVert);
                mh |= a[i] == _app->atom(NetWmStateMaxHorz);
                fs |= a[i] == _app->atom(NetWmStateFullscreen);
                hid |= a[i] == _app->atom(NetWmStateHidden);
                above |= a[i] == _app->atom(NetWmStateAbove);
            }
        }
        std::free(r);
        const bool max = mv && mh;
        if (max != _maximized || fs != _fullscreen || hid != _hidden || above != _above) {
            _maximized  = max;
            _fullscreen = fs;
            _hidden     = hid;
            _above      = above;
            emit({.type = EventType::StateChanged});
        }
    }
}

void X11Window::onShmCompletion() {
    if (_shmInFlight > 0)
        --_shmInFlight;
}

void X11Window::onScaleChanged() {
    writeNormalHints();
    emit({.type = EventType::Resized});
    _framePending = true;
}

// ── frames and present ──────────────────────────────────────────────────────

void X11Window::requestFrame() {
    _framePending = true;
}

bool X11Window::frameReady(core::Clock::time_point now, core::Clock::time_point *notBefore) const {
    if (!_framePending || !_mapped || _painting)
        return false;
    // The server still reads the SHM buffer: painting now would tear. The
    // completion event re-runs the loop.
    if (_shmInFlight > 0 && now - _shmSince < kShmStall)
        return false;
    const auto due = _lastFrame + std::chrono::milliseconds(_app->frameIntervalMs());
    if (now < due) {
        if (notBefore)
            *notBefore = due;
        return false;
    }
    return true;
}

void X11Window::sendFrame(core::Clock::time_point now) {
    _framePending = false;
    _lastFrame    = now;
    emit({.type = EventType::Frame});
}

void X11Window::waitIdle() {
    while (_shmInFlight > 0) {
        const bool got = _app->waitForEvent(
            [this](xcb_generic_event_t *ev) {
                if ((ev->response_type & 0x7f) != _app->_shmEvent + XCB_SHM_COMPLETION)
                    return false;
                auto *e = reinterpret_cast<xcb_shm_completion_event_t *>(ev);
                if (e->drawable != _win)
                    return false;
                onShmCompletion();
                return true;
            },
            int(kShmStall.count())
        );
        if (!got)
            _shmInFlight = 0; // stalled: give up waiting rather than hang
    }
}

bool X11Window::allocBuffer(int w, int h) {
    // Live resize asks for a new size every few pixels: the buffer grows in
    // 128 px steps and is reused while the window fits (rows keep their
    // stride, so what overlaps stays in place), so a drag reallocates and
    // round-trips a handful of times instead of on every step. A much
    // smaller window (a quarter of the area) gives the memory back.
    if (_buf.pixels && w <= _buf.stride && h <= _buf.capH &&
        size_t(_buf.stride) * _buf.capH <= size_t(w) * h * 4) {
        _buf.w = w;
        _buf.h = h;
        return true;
    }
    xcb_connection_t *c = _app->conn();
    Buffer            nb;
    nb.w      = w;
    nb.h      = h;
    nb.stride = (w + 127) & ~127;
    nb.capH   = (h + 127) & ~127;
    nb.bytes  = size_t(nb.stride) * size_t(nb.capH) * 4;
    bool fd   = false;
    if (_app->shmMode(&fd)) {
        void *mem = nullptr;
        if (fd) {
            // memfd + fd passing (MIT-SHM 1.2): no SysV ids, works across
            // IPC namespaces (Flatpak, containers).
            const int mfd = memfd_create("plat-x11", MFD_CLOEXEC);
            if (mfd >= 0 && ftruncate(mfd, off_t(nb.bytes)) == 0) {
                mem = mmap(nullptr, nb.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
                if (mem == MAP_FAILED)
                    mem = nullptr;
            }
            if (mem) {
                nb.seg   = xcb_generate_id(c);
                nb.shmFd = true;
                // xcb closes mfd once it is sent.
                auto ck  = xcb_shm_attach_fd_checked(c, nb.seg, mfd, 1);
                if (xcb_generic_error_t *err = xcb_request_check(c, ck)) {
                    std::free(err);
                    munmap(mem, nb.bytes);
                    mem    = nullptr;
                    nb.seg = 0;
                }
            } else if (mfd >= 0) {
                close(mfd);
            }
        } else {
            const int id = shmget(IPC_PRIVATE, nb.bytes, IPC_CREAT | 0600);
            if (id >= 0) {
                mem = shmat(id, nullptr, 0);
                if (mem == (void *)-1) {
                    mem = nullptr;
                } else {
                    nb.seg  = xcb_generate_id(c);
                    auto ck = xcb_shm_attach_checked(c, nb.seg, uint32_t(id), 1);
                    if (xcb_generic_error_t *err = xcb_request_check(c, ck)) {
                        std::free(err);
                        shmdt(mem);
                        mem    = nullptr;
                        nb.seg = 0;
                    }
                }
                // Mark for removal now: it lives while attached, and cannot
                // leak if we crash.
                shmctl(id, IPC_RMID, nullptr);
            }
        }
        if (mem) {
            nb.pixels = static_cast<uint32_t *>(mem);
        } else {
            _app->disableShm(); // e.g. a server in another IPC namespace
        }
    }
    if (!nb.pixels) {
        nb.seg    = 0;
        nb.pixels = static_cast<uint32_t *>(std::calloc(nb.bytes ? nb.bytes : 4, 1));
        if (!nb.pixels)
            return false;
    }
    // Keep what overlaps, so an app repainting only its damage after a
    // resize never shows garbage in the old area.
    if (_buf.pixels) {
        const int cw = std::min(w, _buf.w), ch = std::min(h, _buf.h);
        for (int y = 0; y < ch; ++y)
            std::memcpy(
                nb.pixels + size_t(y) * nb.stride,
                _buf.pixels + size_t(y) * _buf.stride,
                size_t(cw) * 4
            );
    }
    freeBuffer(_buf);
    _buf = nb;
    return true;
}

void X11Window::freeBuffer(Buffer &b) {
    if (!b.pixels)
        return;
    if (b.seg) {
        // The server keeps its own mapping until it processes the detach.
        xcb_shm_detach(_app->conn(), b.seg);
        if (b.shmFd)
            munmap(b.pixels, b.bytes);
        else
            shmdt(b.pixels);
    } else {
        std::free(b.pixels);
    }
    b = {};
}

Canvas X11Window::beginPaint() {
    waitIdle();
    if (!_buf.pixels || _buf.w != _pw || _buf.h != _ph) {
        if (!allocBuffer(_pw, _ph))
            return {};
    }
    _painting = true;
    return {_buf.pixels, _buf.w, _buf.h, _buf.stride, _app->scale()};
}

void X11Window::endPaint(const std::vector<Rect> &damage) {
    if (!_painting)
        return;
    _painting                = false;
    std::vector<Rect> &rects = _puts;
    rects.clear();
    for (const Rect &d : damage) {
        const int x0 = std::max(0, d.x), y0 = std::max(0, d.y);
        const int x1 = std::min(_buf.w, d.x + d.w), y1 = std::min(_buf.h, d.y + d.h);
        if (x1 > x0 && y1 > y0)
            rects.push_back({x0, y0, x1 - x0, y1 - y0});
    }
    if (damage.empty())
        rects.push_back({0, 0, _buf.w, _buf.h});
    for (size_t i = 0; i < rects.size(); ++i)
        putRect(rects[i], i + 1 == rects.size());
    _presented = true;
    xcb_flush(_app->conn());
}

void X11Window::putRect(const Rect &r, bool last) {
    xcb_connection_t *c = _app->conn();
    if (_buf.seg) {
        // One completion per batch: only the last put asks for it, and the
        // server acknowledges in order.
        xcb_shm_put_image(
            c,
            _win,
            _gc,
            uint16_t(_buf.stride),
            uint16_t(_buf.capH),
            uint16_t(r.x),
            uint16_t(r.y),
            uint16_t(r.w),
            uint16_t(r.h),
            int16_t(r.x),
            int16_t(r.y),
            _app->depth(),
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            last ? 1 : 0,
            _buf.seg,
            0
        );
        if (last) {
            ++_shmInFlight;
            _shmSince = core::Clock::now();
        }
        return;
    }
    // No SHM: PutImage in strips that fit the maximum request size.
    const size_t          rowBytes = size_t(r.w) * 4;
    const size_t          budget   = _app->maxRequestBytes() - 64; // request header + slack
    const int             rows     = int(std::max<size_t>(1, budget / rowBytes));
    std::vector<uint32_t> strip;
    for (int y = r.y; y < r.y + r.h; y += rows) {
        const int       n   = std::min(rows, r.y + r.h - y);
        const uint32_t *src = _buf.pixels + size_t(y) * _buf.stride + r.x;
        if (r.w != _buf.stride) {
            strip.resize(size_t(r.w) * n);
            for (int k = 0; k < n; ++k)
                std::memcpy(
                    strip.data() + size_t(k) * r.w, src + size_t(k) * _buf.stride, rowBytes
                );
            src = strip.data();
        }
        xcb_put_image(
            c,
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            _win,
            _gc,
            uint16_t(r.w),
            uint16_t(n),
            int16_t(r.x),
            int16_t(y),
            0,
            _app->depth(),
            uint32_t(rowBytes * n),
            reinterpret_cast<const uint8_t *>(src)
        );
    }
}

} // namespace plat::x11

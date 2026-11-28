// Wayland windows: xdg-shell toplevel, decorations, scale and shm present.
#include "wayland/wl_internal.h"

#include "prim/utf8.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace plat::wl {

namespace {

// Beyond this many buffers the compositor is holding frames longer than it
// asks for new ones; extra buffers are allocated rather than blocking, and
// freed again as soon as they come back.
constexpr size_t kKeepBuffers = 3;

const wl_surface_listener kSurfaceListener = {
    .enter =
        [](void *d, wl_surface *, wl_output *o) { static_cast<WlWindow *>(d)->onSurfaceEnter(o); },
    .leave =
        [](void *d, wl_surface *, wl_output *o) { static_cast<WlWindow *>(d)->onSurfaceLeave(o); },
    .preferred_buffer_scale     = [](
                                      void *d, wl_surface *, int32_t s
                                  ) { static_cast<WlWindow *>(d)->onPreferredBufferScale(s); },
    .preferred_buffer_transform = [](void *, wl_surface *, uint32_t) {},
};

const xdg_surface_listener kXdgSurfaceListener = {
    .configure = [](void *d, xdg_surface *, uint32_t serial) {
        static_cast<WlWindow *>(d)->onConfigure(serial);
    },
};

const xdg_toplevel_listener kToplevelListener = {
    .configure        = [](
                            void *d, xdg_toplevel *, int32_t w, int32_t h, wl_array *states
                        ) { static_cast<WlWindow *>(d)->onToplevelConfigure(w, h, states); },
    .close            = [](void *d, xdg_toplevel *) { static_cast<WlWindow *>(d)->onClose(); },
    .configure_bounds = [](void *, xdg_toplevel *, int32_t, int32_t) {},
    .wm_capabilities  = [](void *, xdg_toplevel *, wl_array *) {},
};

const zxdg_toplevel_decoration_v1_listener kDecorationListener = {
    .configure = [](void *d, zxdg_toplevel_decoration_v1 *, uint32_t mode) {
        static_cast<WlWindow *>(d)->onDecorationMode(mode);
    },
};

const wp_fractional_scale_v1_listener kFractionalListener = {
    .preferred_scale = [](void *d, wp_fractional_scale_v1 *, uint32_t s) {
        static_cast<WlWindow *>(d)->onFractionalScale(s);
    },
};

const wl_callback_listener kFrameListener = {
    .done = [](void *d, wl_callback *, uint32_t) { static_cast<WlWindow *>(d)->onFrameDone(); },
};

const wl_buffer_listener kBufferListener = {
    .release = [](void *d, wl_buffer *) {
        auto *b = static_cast<ShmBuffer *>(d);
        b->owner->onBufferRelease(b);
    },
};

struct ActivationRequest {
    WlApp    *app;
    WlWindow *window;
};

const xdg_activation_token_v1_listener kActivationTokenListener = {
    .done = [](void *d, xdg_activation_token_v1 *token, const char *str) {
        auto *req = static_cast<ActivationRequest *>(d);
        if (req->app->alive(req->window))
            req->window->activateWithToken(str);
        xdg_activation_token_v1_destroy(token);
        delete req;
    },
};

const zxdg_exported_v2_listener kExportedV2Listener = {
    .handle = [](void *d, zxdg_exported_v2 *, const char *h) {
        static_cast<WlWindow *>(d)->onExportHandle(h);
    },
};

const zxdg_exported_v1_listener kExportedV1Listener = {
    .handle = [](void *d, zxdg_exported_v1 *, const char *h) {
        static_cast<WlWindow *>(d)->onExportHandle(h);
    },
};

// Wire messages are capped at 4 KiB; a title is not worth a protocol error.
std::string truncateUtf8(std::string_view s, size_t max) {
    return std::string(s.substr(0, prim::utf8::truncateAt(s, max)));
}

Rect clip(Rect r, int w, int h) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(w, r.x + r.w), y1 = std::min(h, r.y + r.h);
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

} // namespace

// WindowDesc::position is ignored: xdg-shell has no way to ask for one.
WlWindow::WlWindow(WlApp *a, const WindowDesc &d)
    : app(a), _title(d.title), _appId(d.appId), _decorationMode(d.decorations),
      _resizable(d.resizable), _size(d.size), _floatingSize(d.size), _minSize(d.minSize) {
    _size         = clampToMin({std::max(1, _size.w), std::max(1, _size.h)});
    _floatingSize = _size;
    _surface      = wl_compositor_create_surface(app->compositor);
    wl_surface_add_listener(_surface, &kSurfaceListener, this);
    if (app->viewporter)
        _viewport = wp_viewporter_get_viewport(app->viewporter, _surface);
    // Fractional scale only means something with a viewport to map the
    // physical buffer back onto the logical size.
    if (app->fractionalManager && _viewport) {
        _fractional =
            wp_fractional_scale_manager_v1_get_fractional_scale(app->fractionalManager, _surface);
        wp_fractional_scale_v1_add_listener(_fractional, &kFractionalListener, this);
    }
    // Without fractional-scale the scale is only learnt after the surface
    // enters an output, i.e. after the first frame. Seed it from the outputs
    // when they agree, so a HiDPI window does not open blurry for a frame.
    if (!_fractional && !app->outputs.empty()) {
        int  first = app->outputs.front()->scale;
        bool agree = true;
        for (auto &o : app->outputs)
            agree = agree && o->scale == first;
        if (agree)
            _scale = first;
    }
    _announcedScale = _scale; // what the app reads right after createWindow
    createRole();
}

WlWindow::~WlWindow() {
    app->forget(this);
    if (_frameCb)
        wl_callback_destroy(_frameCb);
    destroyRole();
    if (_fractional)
        wp_fractional_scale_v1_destroy(_fractional);
    if (_viewport)
        wp_viewport_destroy(_viewport);
    for (auto &b : _buffers)
        freeShmBuffer(b.get());
    _buffers.clear();
    wl_surface_destroy(_surface);
    wl_display_flush(app->display);
}

void WlWindow::createRole() {
    _xdgSurface = xdg_wm_base_get_xdg_surface(app->wmBase, _surface);
    xdg_surface_add_listener(_xdgSurface, &kXdgSurfaceListener, this);
    _toplevel = xdg_surface_get_toplevel(_xdgSurface);
    xdg_toplevel_add_listener(_toplevel, &kToplevelListener, this);
    xdg_toplevel_set_title(_toplevel, truncateUtf8(_title, 1024).c_str());
    xdg_toplevel_set_app_id(_toplevel, truncateUtf8(_appId, 256).c_str());
    if (!_resizable) {
        xdg_toplevel_set_min_size(_toplevel, _size.w, _size.h);
        xdg_toplevel_set_max_size(_toplevel, _size.w, _size.h);
    } else if (_minSize.w > 0 || _minSize.h > 0) {
        xdg_toplevel_set_min_size(_toplevel, _minSize.w, _minSize.h);
    }
    // Negotiated before the first commit, so the answer is known by the
    // first configure. No manager (GNOME) = client side = nothing drawn by us.
    if (app->decorationManager) {
        _decoration =
            zxdg_decoration_manager_v1_get_toplevel_decoration(app->decorationManager, _toplevel);
        zxdg_toplevel_decoration_v1_add_listener(_decoration, &kDecorationListener, this);
        zxdg_toplevel_decoration_v1_set_mode(
            _decoration,
            _decorationMode == Decorations::System ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                                   : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
        );
    }
}

void WlWindow::destroyRole() {
    // An exported handle names this toplevel; the next show() makes a new
    // one, and the handle is re-exported on demand.
    if (_exportedV2)
        zxdg_exported_v2_destroy(_exportedV2);
    if (_exportedV1)
        zxdg_exported_v1_destroy(_exportedV1);
    _exportedV2 = nullptr;
    _exportedV1 = nullptr;
    _exportHandle.clear();
    if (_decoration)
        zxdg_toplevel_decoration_v1_destroy(_decoration);
    if (_toplevel)
        xdg_toplevel_destroy(_toplevel);
    if (_xdgSurface)
        xdg_surface_destroy(_xdgSurface);
    _decoration = nullptr;
    _toplevel   = nullptr;
    _xdgSurface = nullptr;
}

void WlWindow::emitEvent(Event e) {
    if (app->_motionWin)
        app->flushMotion(); // a held move goes out first, in order
    e.window                = this;
    // PLAT_WAYLAND_DEBUG=1 traces what the app sees (with WAYLAND_DEBUG=1 for
    // the wire side): the quickest way to tell a compositor quirk from ours.
    static const bool trace = std::getenv("PLAT_WAYLAND_DEBUG") != nullptr;
    if (trace && e.type != EventType::Frame)
        std::fprintf(
            stderr,
            "plat/wayland: %p event %d pos %.1f,%.1f\n",
            (void *)this,
            int(e.type),
            e.pos.x,
            e.pos.y
        );
    app->emit(e);
}

Size WlWindow::clampToMin(Size s) const {
    return {std::max(s.w, _minSize.w), std::max(s.h, _minSize.h)};
}

// ── window management ───────────────────────────────────────────────────────

void WlWindow::setTitle(std::string_view t) {
    _title = std::string(t);
    if (_toplevel)
        xdg_toplevel_set_title(_toplevel, truncateUtf8(_title, 1024).c_str());
}

void WlWindow::setSize(Size s) {
    s = clampToMin({std::max(1, s.w), std::max(1, s.h)});
    if (!_resizable && _toplevel) {
        xdg_toplevel_set_min_size(_toplevel, s.w, s.h);
        xdg_toplevel_set_max_size(_toplevel, s.w, s.h);
    }
    _floatingSize = s;
    // xdg-shell has no "resize me" request: a floating window simply commits
    // a buffer of the new size. Maximised/fullscreen/tiled windows must keep
    // the configured size; the request is remembered for when they float.
    if (!floating() || (s.w == _size.w && s.h == _size.h))
        return;
    _size = s;
    if (_configured) {
        emitResized();
        _frameWanted = true;
    }
}

void WlWindow::setMinSize(Size s) {
    _minSize = s;
    // Double-buffered: takes effect with the next commit (the next paint).
    if (_resizable && _toplevel)
        xdg_toplevel_set_min_size(_toplevel, s.w, s.h);
    if (floating() && (_size.w < s.w || _size.h < s.h))
        setSize(_size);
}

void WlWindow::show() {
    if (_visible)
        return;
    _visible    = true;
    _configured = false;
    if (!_toplevel)
        createRole();
    // The initial commit carries no buffer; the compositor answers with a
    // configure, and the Frame after it maps the window.
    wl_surface_commit(_surface);
    wl_display_flush(app->display);
}

void WlWindow::hide() {
    if (!_visible)
        return;
    _visible     = false;
    _configured  = false;
    _frameWanted = false;
    if (_frameCb) {
        wl_callback_destroy(_frameCb);
        _frameCb = nullptr;
    }
    // Unmap, then drop the role objects; show() builds fresh ones. xdg-shell
    // allows re-mapping the same toplevel after a null buffer, but compositors
    // handle that path poorly (cage 0.2 segfaults), and GTK recreates too.
    wl_surface_attach(_surface, nullptr, 0, 0);
    wl_surface_commit(_surface);
    destroyRole();
    _serverDecorations = false;
    _maximized = _fullscreen = _activated = _tiled = false;
    _viewportSize                                  = {0, 0};
    wl_display_flush(app->display);
}

// While hidden there is no toplevel; like the other backends, these are then
// no-ops rather than remembered.
void WlWindow::minimize() {
    if (_toplevel)
        xdg_toplevel_set_minimized(_toplevel);
}

void WlWindow::setMaximized(bool on) {
    if (_toplevel)
        on ? xdg_toplevel_set_maximized(_toplevel) : xdg_toplevel_unset_maximized(_toplevel);
}

void WlWindow::setFullscreen(bool on) {
    if (_toplevel)
        on ? xdg_toplevel_set_fullscreen(_toplevel, nullptr)
           : xdg_toplevel_unset_fullscreen(_toplevel);
}

void WlWindow::activate() {
    // xdg-shell has set_minimized but no way back, and a compositor may have
    // minimised or hidden us without saying so; either way only a freshly
    // mapped toplevel is reliably shown again (GTK apps restoring from a
    // tray do the same). Skipped while unconfigured: show() just mapped us.
    if (_visible && _configured && !_activated) {
        const bool max = _maximized, full = _fullscreen;
        hide();
        show();
        if (max)
            setMaximized(true);
        if (full)
            setFullscreen(true);
    }
    if (!app->activation)
        return; // plain Wayland has no way to raise yourself
    auto *token = xdg_activation_v1_get_activation_token(app->activation);
    xdg_activation_token_v1_add_listener(
        token, &kActivationTokenListener, new ActivationRequest{app, this}
    );
    // Compositors grant focus only with proof of recent user input and the
    // surface that had it; without them the request may merely flash.
    if (app->seat && app->_inputSerial)
        xdg_activation_token_v1_set_serial(token, app->_inputSerial, app->seat);
    if (auto *focus = app->_kbFocus ? app->_kbFocus->surface() : nullptr)
        xdg_activation_token_v1_set_surface(token, focus);
    xdg_activation_token_v1_set_app_id(token, _appId.c_str());
    xdg_activation_token_v1_commit(token);
    wl_display_flush(app->display);
}

// A token with no input serial can never steal focus, so activating with it
// is how Wayland asks for attention: sway sets the urgent hint, KWin flags
// the window as demanding attention in the taskbar, GNOME shows a "… is
// ready" notification. Compositors without xdg-activation offer nothing.
void WlWindow::requestAttention() {
    if (_activated || !app->activation)
        return;
    auto *token = xdg_activation_v1_get_activation_token(app->activation);
    xdg_activation_token_v1_add_listener(
        token, &kActivationTokenListener, new ActivationRequest{app, this}
    );
    xdg_activation_token_v1_set_surface(token, _surface);
    xdg_activation_token_v1_set_app_id(token, _appId.c_str());
    xdg_activation_token_v1_commit(token);
    wl_display_flush(app->display);
}

void WlWindow::activateWithToken(std::string_view token) {
    if (token.empty()) {
        activate();
        return;
    }
    if (!app->activation)
        return;
    // Activating a surface with no mapped toplevel does nothing, and the
    // token is single-use: keep it for the first configure instead.
    if (!_configured) {
        _pendingToken = std::string(token);
        return;
    }
    xdg_activation_v1_activate(app->activation, std::string(token).c_str(), _surface);
    wl_display_flush(app->display);
}

std::string WlWindow::exportHandle() {
    if (!_exportHandle.empty() || !_toplevel || (!app->exporterV2 && !app->exporterV1))
        return _exportHandle;
    if (!_exportedV2 && !_exportedV1) {
        // The handle is an event. Wait for it on a private queue, so nothing
        // else of the app is dispatched under the caller's feet; the proxy
        // moves to the main queue afterwards in case the handle is late.
        wl_event_queue *q = wl_display_create_queue(app->display);
        wl_proxy       *exported;
        if (app->exporterV2) {
            auto *wrap = static_cast<zxdg_exporter_v2 *>(wl_proxy_create_wrapper(app->exporterV2));
            wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrap), q);
            _exportedV2 = zxdg_exporter_v2_export_toplevel(wrap, _surface);
            wl_proxy_wrapper_destroy(wrap);
            zxdg_exported_v2_add_listener(_exportedV2, &kExportedV2Listener, this);
            exported = reinterpret_cast<wl_proxy *>(_exportedV2);
        } else {
            auto *wrap = static_cast<zxdg_exporter_v1 *>(wl_proxy_create_wrapper(app->exporterV1));
            wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrap), q);
            _exportedV1 = zxdg_exporter_v1_export(wrap, _surface);
            wl_proxy_wrapper_destroy(wrap);
            zxdg_exported_v1_add_listener(_exportedV1, &kExportedV1Listener, this);
            exported = reinterpret_cast<wl_proxy *>(_exportedV1);
        }
        app->dispatchQueueUntil(q, [this] { return !_exportHandle.empty(); }, 500);
        wl_display_dispatch_queue_pending(app->display, q);
        wl_proxy_set_queue(exported, nullptr);
        wl_event_queue_destroy(q);
        static const bool trace = std::getenv("PLAT_WAYLAND_DEBUG") != nullptr;
        if (trace)
            std::fprintf(
                stderr,
                "plat/wayland: exported toplevel (xdg-foreign %s) as '%s'\n",
                _exportedV2 ? "v2" : "v1",
                _exportHandle.c_str()
            );
    }
    return _exportHandle;
}

void WlWindow::startMove(uint32_t serial) {
    if (app->seat && _toplevel)
        xdg_toplevel_move(_toplevel, app->seat, serial);
}

void WlWindow::startResize(uint32_t serial, HitArea a) {
    uint32_t edge = XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    switch (a) {
    case HitArea::ResizeTop:
        edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP;
        break;
    case HitArea::ResizeBottom:
        edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
        break;
    case HitArea::ResizeLeft:
        edge = XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
        break;
    case HitArea::ResizeRight:
        edge = XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
        break;
    case HitArea::ResizeTopLeft:
        edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
        break;
    case HitArea::ResizeTopRight:
        edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
        break;
    case HitArea::ResizeBottomLeft:
        edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
        break;
    case HitArea::ResizeBottomRight:
        edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
        break;
    default:
        return;
    }
    if (app->seat && _resizable && _toplevel)
        xdg_toplevel_resize(_toplevel, app->seat, serial, edge);
}

void WlWindow::showWindowMenu(uint32_t serial, Point p) {
    if (app->seat && _toplevel)
        xdg_toplevel_show_window_menu(_toplevel, app->seat, serial, int32_t(p.x), int32_t(p.y));
}

void WlWindow::setCursor(Cursor c) {
    _cursor = c;
    if (app->_pointerFocus == this)
        app->applyCursor();
}

Cursor WlWindow::cursorAt(Point p) const {
    // The compositor shows resize cursors over its own borders; over ours we
    // have to, or the edges would look inert (Win32 gets this from NCHITTEST).
    if (_hitTest && _resizable && floating()) {
        switch (_hitTest(p)) {
        case HitArea::ResizeTop:
        case HitArea::ResizeBottom:
            return Cursor::ResizeV;
        case HitArea::ResizeLeft:
        case HitArea::ResizeRight:
            return Cursor::ResizeH;
        case HitArea::ResizeTopLeft:
        case HitArea::ResizeBottomRight:
            return Cursor::ResizeNWSE;
        case HitArea::ResizeTopRight:
        case HitArea::ResizeBottomLeft:
            return Cursor::ResizeNESW;
        default:
            break;
        }
    }
    return _cursor;
}

int WlWindow::cursorScale() const {
    return std::max(1, int(std::ceil(_scale - 0.01)));
}

void WlWindow::setTextInput(const TextInputState &s) {
    const bool same = s.enabled == _textInput.enabled && s.caret.x == _textInput.caret.x &&
                      s.caret.y == _textInput.caret.y && s.caret.w == _textInput.caret.w &&
                      s.caret.h == _textInput.caret.h;
    _textInput      = s;
    if (!same && app->_tiFocus == this)
        app->syncTextInput();
}

// ── configure ───────────────────────────────────────────────────────────────

void WlWindow::onToplevelConfigure(int32_t w, int32_t h, wl_array *states) {
    _pendingW   = w;
    _pendingH   = h;
    _pendingMax = _pendingFull = _pendingActive = _pendingTiled = false;
    const auto  *s = static_cast<const uint32_t *>(states->data);
    const size_t n = states->size / sizeof(uint32_t);
    for (size_t i = 0; i < n; ++i) {
        switch (s[i]) {
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            _pendingMax = true;
            break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            _pendingFull = true;
            break;
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            _pendingActive = true;
            break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            _pendingTiled = true;
            break;
        default:
            break;
        }
    }
}

void WlWindow::onConfigure(uint32_t serial) {
    xdg_surface_ack_configure(_xdgSurface, serial);
    _uncommittedAck = true;

    const bool sized       = _pendingW > 0 && _pendingH > 0;
    const bool nowFloating = !_pendingMax && !_pendingFull && !_pendingTiled;
    Size       s           = sized ? Size{_pendingW, _pendingH} : _floatingSize;
    if (nowFloating) {
        s = clampToMin(s);
        if (sized)
            _floatingSize = s; // an interactive resize: remember it for unmaximise
    }
    const bool stateChanged =
        _pendingMax != _maximized || _pendingFull != _fullscreen || _pendingActive != _activated;
    _maximized  = _pendingMax;
    _fullscreen = _pendingFull;
    _activated  = _pendingActive;
    _tiled      = _pendingTiled;

    const bool first   = !_configured;
    _configured        = true;
    const bool resized = s.w != _size.w || s.h != _size.h;
    _size              = s;
    // A scale learnt while unconfigured was not announced yet: it goes out
    // now, or the app keeps laying out for the old one.
    if (resized || _scale != _announcedScale)
        emitResized();
    if (stateChanged)
        emitEvent({.type = EventType::StateChanged});
    _frameWanted = true; // a Frame follows every configure

    if (first) {
        // Launched from a launcher/notification: hand its activation token
        // on, so the compositor focuses us instead of flashing. A token
        // handed to activateWithToken() while unmapped goes out the same way.
        std::string tok = std::move(_pendingToken);
        _pendingToken.clear();
        if (const char *env = std::getenv("XDG_ACTIVATION_TOKEN")) {
            if (tok.empty())
                tok = env;
            unsetenv("XDG_ACTIVATION_TOKEN");
        }
        if (!tok.empty() && app->activation)
            xdg_activation_v1_activate(app->activation, tok.c_str(), _surface);
    }
}

void WlWindow::onClose() {
    emitEvent({.type = EventType::CloseRequested});
}

void WlWindow::onDecorationMode(uint32_t mode) {
    _serverDecorations = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

// ── scale ───────────────────────────────────────────────────────────────────

void WlWindow::onFractionalScale(uint32_t s120) {
    _fractional120 = s120;
    updateScale();
}

void WlWindow::onPreferredBufferScale(int32_t s) {
    _preferredScale = std::max(1, s);
    updateScale();
}

void WlWindow::onSurfaceEnter(wl_output *o) {
    if (auto *out = app->outputFor(o);
        out && std::find(_outputs.begin(), _outputs.end(), out) == _outputs.end())
        _outputs.push_back(out);
    updateScale();
}

void WlWindow::onSurfaceLeave(wl_output *o) {
    if (auto *out = app->outputFor(o))
        _outputs.erase(std::remove(_outputs.begin(), _outputs.end(), out), _outputs.end());
    updateScale();
}

void WlWindow::outputGone(Output *o) {
    _outputs.erase(std::remove(_outputs.begin(), _outputs.end(), o), _outputs.end());
    updateScale();
}

void WlWindow::updateScale() {
    double s;
    if (_viewport && _fractional120)
        s = _fractional120 / 120.0;
    else if (_preferredScale > 0)
        s = _preferredScale;
    else if (!_outputs.empty()) {
        // Pre-v6 compositors: the densest output we are on, as GTK does.
        int m = 1;
        for (auto *o : _outputs)
            m = std::max(m, o->scale);
        s = m;
    } else {
        return; // on no output (e.g. just unplugged): keep the last scale
    }
    if (s == _scale)
        return;
    _scale = s;
    // Unconfigured, the first configure announces it (onConfigure).
    if (_configured) {
        emitResized();
        _frameWanted = true;
    }
    if (app->_pointerFocus == this)
        app->applyCursor(true); // re-rasterise the theme cursor at the new scale
}

void WlWindow::emitResized() {
    _announcedScale = _scale;
    emitEvent({.type = EventType::Resized});
}

// ── present ─────────────────────────────────────────────────────────────────

void WlWindow::requestFrame() {
    // Coalesced: with a frame callback in flight the Frame waits for it;
    // otherwise the next beforeWait emits it (this runs on the loop thread,
    // so the loop cannot be asleep in poll right now).
    _frameWanted = true;
}

void WlWindow::emitFrame() {
    _frameWanted      = false;
    _callbackPaced    = false;
    _presentedInFrame = false;
    emitEvent({.type = EventType::Frame});
    // An app that had nothing to repaint still owes the compositor a commit
    // for the configure it just acked (a state-only change, e.g. focus). Only
    // safe when the attached buffer already has the right size.
    if (!_presentedInFrame && _uncommittedAck && _configured && _visible && _newest &&
        _newest->width == int(std::lround(_size.w * _scale)) &&
        _newest->height == int(std::lround(_size.h * _scale))) {
        wl_surface_commit(_surface);
        _uncommittedAck = false;
    }
}

void WlWindow::onFrameDone() {
    if (_frameCb)
        wl_callback_destroy(_frameCb);
    _frameCb       = nullptr;
    _callbackPaced = true;
    // The Frame itself goes out from WlApp::afterWait, outside dispatch.
}

// Live resize asks for a new size every few pixels: a buffer of another size
// is kept while its pool fits the window (and is not four times too big), and
// beginPaint makes it a buffer of the new size from the same memory.
static bool reusable(const ShmBuffer *b, int pw, int ph) {
    const size_t need = std::max<size_t>(1, size_t(pw)) * std::max<size_t>(1, size_t(ph)) * 4;
    return b->cap >= need && b->cap <= need * 4;
}

void WlWindow::pruneBuffers() {
    const int pw = int(std::lround(_size.w * _scale)), ph = int(std::lround(_size.h * _scale));
    for (auto it = _buffers.begin(); it != _buffers.end();) {
        ShmBuffer *b         = it->get();
        const bool wrongSize = (b->width != pw || b->height != ph) && !reusable(b, pw, ph);
        const bool surplus   = _buffers.size() > kKeepBuffers && b != _newest;
        if (!b->busy && b != _painting && (wrongSize || surplus)) {
            if (_newest == b)
                _newest = nullptr;
            freeShmBuffer(b);
            it = _buffers.erase(it);
        } else {
            ++it;
        }
    }
}

void WlWindow::onBufferRelease(ShmBuffer *b) {
    b->busy = false;
    pruneBuffers();
}

Canvas WlWindow::beginPaint() {
    const int pw = std::max(1, int(std::lround(_size.w * _scale)));
    const int ph = std::max(1, int(std::lround(_size.h * _scale)));
    if (!_painting) {
        pruneBuffers();
        // Prefer the newest buffer (nothing to refresh), else any idle one.
        ShmBuffer *pick = nullptr;
        for (auto &b : _buffers)
            if (!b->busy && b->width == pw && b->height == ph && (!pick || b.get() == _newest))
                pick = b.get();
        // Else an idle buffer of another size whose pool still fits.
        for (auto &b : _buffers) {
            if (pick)
                break;
            if (b->busy || !reusable(b.get(), pw, ph) ||
                !resizeShmBuffer(b.get(), pw, ph, WL_SHM_FORMAT_ARGB8888))
                continue;
            wl_buffer_add_listener(b->buffer, &kBufferListener, b.get());
            if (_newest == b.get())
                _newest = nullptr; // its pixels are of the old size
            b->stale.clear();
            b->staleAll = true;
            pick        = b.get();
        }
        if (!pick) {
            auto b           = std::make_unique<ShmBuffer>();
            b->owner         = this;
            // Rounded up to 128 px both ways, so the next resize steps fit.
            const size_t cap = size_t((pw + 127) & ~127) * size_t((ph + 127) & ~127) * 4;
            if (!allocShmBuffer(app->shm, pw, ph, WL_SHM_FORMAT_ARGB8888, b.get(), cap))
                return {};
            wl_buffer_add_listener(b->buffer, &kBufferListener, b.get());
            b->staleAll = true;
            pick        = b.get();
            _buffers.push_back(std::move(b));
        }
        // Bring the buffer up to date with the newest frame, so the canvas
        // always holds what is on screen and a partial repaint is correct.
        ShmBuffer *src = _newest;
        if (pick != src && src && src->width == pw && src->height == ph) {
            if (pick->staleAll) {
                std::memcpy(pick->pixels, src->pixels, pick->bytes);
            } else {
                for (Rect r : pick->stale) {
                    r = clip(r, pw, ph);
                    for (int y = r.y; y < r.y + r.h; ++y)
                        std::memcpy(
                            pick->pixels + size_t(y) * pw + r.x,
                            src->pixels + size_t(y) * pw + r.x,
                            size_t(r.w) * 4
                        );
                }
            }
        }
        pick->stale.clear();
        pick->staleAll = false;
        _painting      = pick;
    }
    return {_painting->pixels, _painting->width, _painting->height, _painting->width, _scale};
}

void WlWindow::endPaint(const std::vector<Rect> &damage) {
    ShmBuffer *b = _painting;
    if (!b)
        return;
    _painting = nullptr;

    // Other buffers now lack whatever this frame painted.
    const bool whole = damage.empty();
    for (auto &o : _buffers) {
        if (o.get() == b)
            continue;
        if (whole || o->stale.size() + damage.size() > 32) {
            o->staleAll = true;
            o->stale.clear();
        } else if (!o->staleAll) {
            o->stale.insert(o->stale.end(), damage.begin(), damage.end());
        }
    }
    _newest = b;

    // Attaching before the first configure is a protocol error; the pixels
    // are kept (as _newest) and go out with the next paint.
    if (!_configured || !_visible)
        return;

    wl_surface_attach(_surface, b->buffer, 0, 0);
    if (whole) {
        wl_surface_damage_buffer(_surface, 0, 0, INT32_MAX, INT32_MAX);
    } else {
        for (Rect r : damage) {
            r = clip(r, b->width, b->height);
            if (r.w <= 0 || r.h <= 0)
                continue;
            if (app->compositorVersion >= 4) {
                wl_surface_damage_buffer(_surface, r.x, r.y, r.w, r.h);
            } else {
                // Surface coordinates: round outwards so no edge pixel is lost.
                const int x0 = int(std::floor(r.x / _scale)), y0 = int(std::floor(r.y / _scale));
                const int x1 = int(std::ceil((r.x + r.w) / _scale));
                const int y1 = int(std::ceil((r.y + r.h) / _scale));
                wl_surface_damage(_surface, x0, y0, x1 - x0, y1 - y0);
            }
        }
    }
    if (_viewport) {
        // Buffer stays at scale 1; the viewport maps it onto the logical size.
        if (_viewportSize.w != _size.w || _viewportSize.h != _size.h) {
            wp_viewport_set_destination(_viewport, _size.w, _size.h);
            _viewportSize = _size;
        }
    } else if (app->compositorVersion >= 3 && _bufferScaleSet != int(_scale)) {
        wl_surface_set_buffer_scale(_surface, int(_scale));
        _bufferScaleSet = int(_scale);
    }
    if (_frameCb)
        wl_callback_destroy(_frameCb);
    _frameCb = wl_surface_frame(_surface);
    wl_callback_add_listener(_frameCb, &kFrameListener, this);
    wl_surface_commit(_surface);
    b->busy           = true;
    _presentedInFrame = true;
    _uncommittedAck   = false;
    wl_display_flush(app->display);
}

// ── portal parent ───────────────────────────────────────────────────────────

std::string WlApp::parentHandle(Window *w) {
    auto *ww = static_cast<WlWindow *>(w);
    if (!ww || !alive(ww))
        return {};
    const std::string h = ww->exportHandle();
    return h.empty() ? std::string() : "wayland:" + h;
}

} // namespace plat::wl

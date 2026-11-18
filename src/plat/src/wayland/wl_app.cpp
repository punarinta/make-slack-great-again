// Wayland backend: connection, registry, the loop hook and frame pacing.
#include "wayland/wl_internal.h"

#include "core/pacing.h"
#include "freedesktop/files.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

namespace plat::wl {

namespace {

const wl_registry_listener kRegistryListener = {
    .global        = [](
                         void *d, wl_registry *, uint32_t name, const char *iface, uint32_t version
                     ) { static_cast<WlApp *>(d)->onGlobal(name, iface, version); },
    .global_remove = [](void *d,
                        wl_registry *,
                        uint32_t name) { static_cast<WlApp *>(d)->onGlobalRemove(name); },
};

const xdg_wm_base_listener kWmBaseListener = {
    .ping = [](void *, xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); },
};

} // namespace

// ── shm helpers ─────────────────────────────────────────────────────────────

int createShmFd(size_t bytes) {
    return linux_files::sharedMemoryFd("plat-shm", bytes);
}

bool allocShmBuffer(wl_shm *shm, int w, int h, uint32_t format, ShmBuffer *out, size_t capacity) {
    const size_t stride = size_t(w) * 4, bytes = stride * size_t(h);
    const size_t cap = std::max(bytes, capacity);
    const int    fd  = createShmFd(cap);
    if (fd < 0)
        return false;
    void *data = mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return false;
    }
    // One pool per buffer, kept for resizes that fit: buffers are replaced
    // wholesale otherwise, so a shared growable pool would only add
    // bookkeeping.
    out->pool   = wl_shm_create_pool(shm, fd, int32_t(cap));
    out->buffer = wl_shm_pool_create_buffer(out->pool, 0, w, h, int32_t(stride), format);
    close(fd);
    out->pixels = static_cast<uint32_t *>(data);
    out->bytes  = bytes;
    out->cap    = cap;
    out->width  = w;
    out->height = h;
    return true;
}

bool resizeShmBuffer(ShmBuffer *b, int w, int h, uint32_t format) {
    const size_t stride = size_t(w) * 4, bytes = stride * size_t(h);
    if (!b->pool || bytes > b->cap)
        return false;
    if (b->buffer)
        wl_buffer_destroy(b->buffer);
    b->buffer = wl_shm_pool_create_buffer(b->pool, 0, w, h, int32_t(stride), format);
    b->bytes  = bytes;
    b->width  = w;
    b->height = h;
    return true;
}

void freeShmBuffer(ShmBuffer *b) {
    if (b->buffer)
        wl_buffer_destroy(b->buffer);
    if (b->pool)
        wl_shm_pool_destroy(b->pool);
    if (b->pixels)
        munmap(b->pixels, b->cap);
    b->buffer = nullptr;
    b->pool   = nullptr;
    b->pixels = nullptr;
}

// ── app ─────────────────────────────────────────────────────────────────────

WlApp::WlApp() = default;

void WlApp::emitThemeChanged() {
    for (auto *w : std::vector<WlWindow *>(_windows))
        w->emitEvent({.type = EventType::ThemeChanged});
}

WlApp::~WlApp() {
    _loop.core.shutdown(); // pending closures go while the loop still works
    resetServices();       // it watches fds on our loop
    // Windows outlive nothing: the app owner must destroy them first, but be
    // defensive about focus pointers into them.
    for (auto &[id, t] : _transfers) {
        if (t.watch)
            _loop.unwatch(t.watch);
        if (t.fd >= 0)
            close(t.fd);
    }
    _transfers.clear();
    stopRepeat();
#ifdef PLAT_TEST_HOOKS
    if (_vkb)
        zwp_virtual_keyboard_v1_destroy(_vkb);
    if (_vkbKeymap)
        xkb_keymap_unref(_vkbKeymap);
    if (_vptr)
        zwlr_virtual_pointer_v1_destroy(_vptr);
#endif
    if (_drag) {
        wl_data_source_destroy(_drag->source);
        if (_drag->iconViewport)
            wp_viewport_destroy(_drag->iconViewport);
        if (_drag->icon)
            wl_surface_destroy(_drag->icon);
        freeShmBuffer(&_drag->iconBuffer);
        _drag.reset();
    }
    destroyGestures();
    for (auto &[o, offer] : _offers)
        wl_data_offer_destroy(o);
    _offers.clear();
    for (auto &[o, offer] : _primaryOffers)
        zwp_primary_selection_offer_v1_destroy(o);
    _primaryOffers.clear();
    if (_source)
        wl_data_source_destroy(_source);
    if (_primarySource)
        zwp_primary_selection_source_v1_destroy(_primarySource);
    if (_primaryDevice)
        zwp_primary_selection_device_v1_destroy(_primaryDevice);
    if (_dataDevice) {
        if (dataManagerVersion >= 2)
            wl_data_device_release(_dataDevice);
        else
            wl_data_device_destroy(_dataDevice);
    }
    if (_textInput)
        zwp_text_input_v3_destroy(_textInput);
    if (_shapeDevice)
        wp_cursor_shape_device_v1_destroy(_shapeDevice);
    if (_cursorSurface)
        wl_surface_destroy(_cursorSurface);
    if (_cursorTheme)
        wl_cursor_theme_destroy(_cursorTheme);
    if (_pointer)
        seatVersion >= 3 ? wl_pointer_release(_pointer) : wl_pointer_destroy(_pointer);
    if (_keyboard)
        seatVersion >= 3 ? wl_keyboard_release(_keyboard) : wl_keyboard_destroy(_keyboard);
    for (auto &o : outputs)
        destroyOutput(o.get());
    outputs.clear();
    auto destroy = [](auto *p, auto fn) {
        if (p)
            fn(p);
    };
    destroy(xdgOutputManager, zxdg_output_manager_v1_destroy);
    destroy(exporterV2, zxdg_exporter_v2_destroy);
    destroy(exporterV1, zxdg_exporter_v1_destroy);
#ifdef PLAT_TEST_HOOKS
    destroy(screencopyManager, zwlr_screencopy_manager_v1_destroy);
    destroy(vptrManager, zwlr_virtual_pointer_manager_v1_destroy);
    destroy(vkbManager, zwp_virtual_keyboard_manager_v1_destroy);
#endif
    destroy(primaryManager, zwp_primary_selection_device_manager_v1_destroy);
    if (gesturesManager)
        wl_proxy_get_version(reinterpret_cast<wl_proxy *>(gesturesManager)) >= 2
            ? zwp_pointer_gestures_v1_release(gesturesManager)
            : zwp_pointer_gestures_v1_destroy(gesturesManager);
    destroy(textInputManager, zwp_text_input_manager_v3_destroy);
    destroy(activation, xdg_activation_v1_destroy);
    destroy(decorationManager, zxdg_decoration_manager_v1_destroy);
    destroy(cursorShapeManager, wp_cursor_shape_manager_v1_destroy);
    destroy(fractionalManager, wp_fractional_scale_manager_v1_destroy);
    destroy(viewporter, wp_viewporter_destroy);
    destroy(dataManager, wl_data_device_manager_destroy);
    if (seat)
        seatVersion >= 5 ? wl_seat_release(seat) : wl_seat_destroy(seat);
    destroy(wmBase, xdg_wm_base_destroy);
    destroy(shm, wl_shm_destroy);
    destroy(compositor, wl_compositor_destroy);
    destroy(registry, wl_registry_destroy);
    if (_displayWatch)
        _loop.unwatch(_displayWatch);
    if (display) {
        wl_display_flush(display);
        wl_display_disconnect(display);
    }
}

bool WlApp::init(std::string *error) {
    auto fail = [&](const char *why) {
        if (error)
            *error = why;
        return false;
    };
    display = wl_display_connect(nullptr);
    if (!display)
        return fail("wayland: cannot connect to the compositor ($WAYLAND_DISPLAY)");
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, this);
    if (wl_display_roundtrip(display) < 0)
        return fail("wayland: registry roundtrip failed");
    if (!compositor || !shm || !wmBase)
        return fail("wayland: compositor lacks wl_compositor, wl_shm or xdg_wm_base");
    // Second roundtrip: seat capabilities, output modes/scales, xdg-output
    // layouts and the keymap arrive in reply to the binds above.
    wl_display_roundtrip(display);
    // The outputs present now are the starting layout, not a change.
    _lastMonitors = monitors();
    _initDone     = true;

    // Writing clipboard data into a pipe the reader already closed must be an
    // EPIPE, not a process-killing signal. Only touch the default handler.
    struct sigaction sa{};
    if (sigaction(SIGPIPE, nullptr, &sa) == 0 && sa.sa_handler == SIG_DFL) {
        sa.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &sa, nullptr);
    }

    _displayWatch    = _loop.watch(wl_display_get_fd(display), FdRead, [](uint32_t) {});
    _loop.beforeWait = [this] { return beforeWait(); };
    _loop.afterWait  = [this](uint32_t ready) { afterWait(ready); };
    _loop.waitWatch  = _displayWatch;
    return true;
}

void WlApp::onGlobal(uint32_t name, const char *iface, uint32_t version) {
    auto is   = [&](const wl_interface &i) { return std::strcmp(iface, i.name) == 0; };
    auto bind = [&](const wl_interface &i, uint32_t maxVersion) {
        return wl_registry_bind(registry, name, &i, std::min(version, maxVersion));
    };
    if (is(wl_compositor_interface)) {
        compositorVersion = std::min(version, 6u);
        compositor        = static_cast<wl_compositor *>(bind(wl_compositor_interface, 6));
    } else if (is(wl_shm_interface)) {
        shm = static_cast<wl_shm *>(bind(wl_shm_interface, 1));
    } else if (is(xdg_wm_base_interface)) {
        wmBase = static_cast<xdg_wm_base *>(bind(xdg_wm_base_interface, 6));
        xdg_wm_base_add_listener(wmBase, &kWmBaseListener, this);
    } else if (is(wl_seat_interface) && !seat) {
        bindSeat(name, version);
    } else if (is(wl_output_interface)) {
        bindOutput(name, version);
    } else if (is(zxdg_output_manager_v1_interface)) {
        // v3: wl_output.done also covers the xdg-output fields.
        xdgOutputManager =
            static_cast<zxdg_output_manager_v1 *>(bind(zxdg_output_manager_v1_interface, 3));
        for (auto &o : outputs)
            bindXdgOutput(o.get());
    } else if (is(zxdg_exporter_v2_interface)) {
        exporterV2 = static_cast<zxdg_exporter_v2 *>(bind(zxdg_exporter_v2_interface, 1));
    } else if (is(zxdg_exporter_v1_interface)) {
        exporterV1 = static_cast<zxdg_exporter_v1 *>(bind(zxdg_exporter_v1_interface, 1));
    } else if (is(wl_data_device_manager_interface)) {
        dataManagerVersion = std::min(version, 3u);
        dataManager =
            static_cast<wl_data_device_manager *>(bind(wl_data_device_manager_interface, 3));
        setupDataDevice();
    } else if (is(wp_viewporter_interface)) {
        viewporter = static_cast<wp_viewporter *>(bind(wp_viewporter_interface, 1));
    } else if (is(wp_fractional_scale_manager_v1_interface)) {
        fractionalManager = static_cast<wp_fractional_scale_manager_v1 *>(
            bind(wp_fractional_scale_manager_v1_interface, 1)
        );
    } else if (is(wp_cursor_shape_manager_v1_interface)) {
        cursorShapeManager = static_cast<wp_cursor_shape_manager_v1 *>(
            bind(wp_cursor_shape_manager_v1_interface, 1)
        );
    } else if (is(zxdg_decoration_manager_v1_interface)) {
        decorationManager = static_cast<zxdg_decoration_manager_v1 *>(
            bind(zxdg_decoration_manager_v1_interface, 1)
        );
    } else if (is(xdg_activation_v1_interface)) {
        activation = static_cast<xdg_activation_v1 *>(bind(xdg_activation_v1_interface, 1));
    } else if (is(zwp_text_input_manager_v3_interface)) {
        textInputManager =
            static_cast<zwp_text_input_manager_v3 *>(bind(zwp_text_input_manager_v3_interface, 1));
        setupSeat();
    } else if (is(zwp_primary_selection_device_manager_v1_interface)) {
        primaryManager = static_cast<zwp_primary_selection_device_manager_v1 *>(
            bind(zwp_primary_selection_device_manager_v1_interface, 1)
        );
        setupDataDevice();
    } else if (is(zwp_pointer_gestures_v1_interface)) {
        gesturesManager =
            static_cast<zwp_pointer_gestures_v1 *>(bind(zwp_pointer_gestures_v1_interface, 3));
        setupGestures();
#ifdef PLAT_TEST_HOOKS
    } else if (is(zwp_virtual_keyboard_manager_v1_interface)) {
        vkbManager = static_cast<zwp_virtual_keyboard_manager_v1 *>(
            bind(zwp_virtual_keyboard_manager_v1_interface, 1)
        );
    } else if (is(zwlr_virtual_pointer_manager_v1_interface)) {
        vptrManager = static_cast<zwlr_virtual_pointer_manager_v1 *>(
            bind(zwlr_virtual_pointer_manager_v1_interface, 2)
        );
    } else if (is(zwlr_screencopy_manager_v1_interface)) {
        screencopyManager = static_cast<zwlr_screencopy_manager_v1 *>(
            bind(zwlr_screencopy_manager_v1_interface, 3)
        );
#endif
    }
    if (std::getenv("PLAT_WAYLAND_DEBUG"))
        std::fprintf(stderr, "plat/wayland: global %s v%u\n", iface, version);
}

void WlApp::onGlobalRemove(uint32_t name) {
    // Only outputs come and go in practice; other globals vanishing
    // mid-session is not worth handling.
    removeOutput(name);
}

Output *WlApp::outputFor(wl_output *o) const {
    for (auto &out : outputs)
        if (out->output == o)
            return out.get();
    return nullptr;
}

WlWindow *WlApp::windowFor(wl_surface *s) const {
    if (!s)
        return nullptr;
    for (auto *w : _windows)
        if (w->surface() == s)
            return w;
    return nullptr;
}

bool WlApp::alive(const WlWindow *w) const {
    return std::find(_windows.begin(), _windows.end(), w) != _windows.end();
}

std::unique_ptr<Window> WlApp::createWindow(const WindowDesc &desc) {
    auto w = std::make_unique<WlWindow>(this, desc);
    _windows.push_back(w.get());
    if (desc.visible)
        w->show();
    return w;
}

void WlApp::forget(WlWindow *w) {
    _windows.erase(std::remove(_windows.begin(), _windows.end(), w), _windows.end());
    if (_pointerFocus == w)
        _pointerFocus = nullptr;
    if (_kbFocus == w) {
        stopRepeat();
        _kbFocus = nullptr;
    }
    if (_tiFocus == w) {
        _tiFocus   = nullptr;
        _tiEnabled = false;
        _preedit   = {};
    }
    if (_dragWindow == w)
        _dragWindow = nullptr;
    if (_pressWindow == w) {
        _pressWindow = nullptr;
        _held        = 0;
    }
    if (_gestureWindow == w)
        _gestureWindow = nullptr;
    if (_drag && _drag->window == w)
        _drag->window = nullptr; // the drag goes on; DragFinished has nowhere to go
}

void WlApp::fatal(const char *what) {
    if (_dead)
        return;
    _dead         = true;
    const int err = wl_display_get_error(display);
    std::fprintf(
        stderr,
        "plat/wayland: %s (%s); stopping the loop\n",
        what,
        err ? std::strerror(err) : "connection lost"
    );
    // Nothing on this connection can work any more; let run() return so the
    // app can exit instead of spinning on a dead fd.
    if (_displayWatch) {
        _loop.unwatch(_displayWatch);
        _displayWatch = 0;
    }
    _loop.quit();
}

// ── loop ────────────────────────────────────────────────────────────────────

bool WlApp::dispatchQueueUntil(wl_event_queue *q, const std::function<bool()> &pred, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    const int  fd  = wl_display_get_fd(display);
    for (;;) {
        if (wl_display_dispatch_queue_pending(display, q) < 0)
            return false;
        if (pred())
            return true;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              end - std::chrono::steady_clock::now()
        )
                              .count();
        if (left <= 0)
            return false;
        while (wl_display_prepare_read_queue(display, q) != 0)
            wl_display_dispatch_queue_pending(display, q);
        wl_display_flush(display);
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, int(left)) > 0) {
            // Events for the main queue are read too; they wait there for
            // the next pump().
            if (wl_display_read_events(display) < 0)
                return false;
        } else {
            wl_display_cancel_read(display);
        }
    }
}

// Frames no frame callback paces (first frame, requestFrame while idle, a
// Frame that presented nothing) go out at most once per display refresh,
// like X11 and Win32, so an app that requests a frame from every Frame
// without presenting cannot spin the CPU. Callback-paced Frames keep a 4 ms
// floor (250 Hz) in case a compositor answers callbacks at once (a hidden
// surface).
bool WlApp::frameDue(WlWindow *w, std::chrono::steady_clock::time_point now, int *waitMs) const {
    int ms = 4;
    if (!w->callbackPaced()) {
        int hz = 0; // the window's output, else the fastest one
        if (const Output *o = w->output())
            hz = o->refreshMilliHz;
        if (hz <= 0)
            for (const auto &o : outputs)
                hz = std::max(hz, o->refreshMilliHz);
        ms = core::frameIntervalMs(hz);
    }
    const auto due = w->lastFrame + std::chrono::milliseconds(ms);
    if (now >= due)
        return true;
    *waitMs = int(std::chrono::ceil<std::chrono::milliseconds>(due - now).count());
    return false;
}

bool WlApp::emitReadyFrames() {
    int                     wait    = -1;
    const auto              now     = std::chrono::steady_clock::now();
    // A snapshot (a Frame handler may create or destroy windows), in a vector
    // reused across calls (a nested call gets its own).
    std::vector<WlWindow *> windows = std::move(_frameWindows);
    windows.assign(_windows.begin(), _windows.end());
    for (auto *w : windows) {
        if (!alive(w) || !w->frameReady())
            continue;
        int ms = 0;
        if (!frameDue(w, now, &ms)) {
            wait = wait < 0 ? ms : std::min(wait, ms);
            continue;
        }
        w->lastFrame = now;
        w->emitFrame();
    }
    _frameWindows = std::move(windows);
    if (wait >= 0 && !_frameTimer) {
        // A timer is how the loop learns when to look again.
        _frameTimer = _loop.core.addTimer(std::max(1, wait), false, [this] { _frameTimer = 0; });
    }
    return wait >= 0;
}

bool WlApp::beforeWait() {
    if (_dead)
        return false;
    emitReadyFrames();
    // The standard multi-threaded-safe read dance: drain what is already
    // queued (a callback may queue more), then announce the read.
    while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) {
            fatal("dispatch failed");
            return false;
        }
        flushMotion();
    }
    _reading = true;
    // EAGAIN means the socket is full; the rest goes out on a later flush.
    if (wl_display_flush(display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display);
        _reading = false;
        fatal("flush failed");
        return false;
    }
    // Don't sleep if a window has a frame due right now.
    const auto now = std::chrono::steady_clock::now();
    for (auto *w : _windows) {
        int ms = 0;
        if (w->frameReady() && frameDue(w, now, &ms))
            return false;
    }
    return true;
}

// displayReady: what the loop's poll found on the display fd (no second
// poll to ask again).
void WlApp::afterWait(uint32_t displayReady) {
    if (_dead)
        return;
    if (_reading) {
        _reading = false;
        if (displayReady & FdRead) {
            if (wl_display_read_events(display) < 0) {
                fatal("read failed");
                return;
            }
        } else {
            wl_display_cancel_read(display);
        }
    }
    if (wl_display_dispatch_pending(display) < 0) {
        fatal("dispatch failed");
        return;
    }
    flushMotion(); // the batch's last move, before any Frame
    // Frame callbacks that fired in this batch: paint now, outside libwayland's
    // dispatch, so the app may do anything (even block) in its Frame handler.
    emitReadyFrames();
    wl_display_flush(display);
}

void WlApp::pump(int timeoutMs) {
    if (_dead)
        return;
    _loop.iterate(timeoutMs);
}

void WlApp::run() {
    if (_dead)
        return;
    _loop.run();
}

} // namespace plat::wl

namespace plat {
std::unique_ptr<App> createWaylandApp(std::string *error) {
    auto app = std::make_unique<wl::WlApp>();
    if (!app->init(error))
        return nullptr;
    app->startServices();
    return app;
}
} // namespace plat

// TestHooks for the selftest: input goes in through the compositor with the
// wlroots virtual-keyboard / virtual-pointer protocols (the same path a real
// device takes), pixels come back with wlr-screencopy. Each hook returns
// false when the compositor lacks the protocol, so the case SKIPs (weston,
// GNOME, KDE).
//
// Limitation: Wayland never tells a client where its window is. Pointer
// positions and screen reads assume the window sits at the output's origin,
// which holds for the kiosk compositor (cage) and for the selftest's sway
// config (every window floats at 0,0).
//
// Not synthesisable: touchpad gestures (no virtual-gesture protocol in
// wlroots or anywhere else) and the attention state (a client cannot read
// the urgent hint back), so injectGesture/wantsAttention stay false.
#include "wayland/wl_internal.h"

#include "freedesktop/evdev_codes.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

namespace plat::wl {

namespace {

uint32_t nowMs() {
    using namespace std::chrono;
    return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t evdevButton(Button b) {
    switch (b) {
    case Button::Left:
        return BTN_LEFT;
    case Button::Right:
        return BTN_RIGHT;
    case Button::Middle:
        return BTN_MIDDLE;
    case Button::Back:
        return BTN_SIDE;
    case Button::Forward:
        return BTN_EXTRA;
    }
    return BTN_LEFT;
}

// Absolute motion is in units of an arbitrary extent over the output; a
// fine grid keeps fractional logical positions exact enough.
constexpr uint32_t kGrid = 16;

struct Capture {
    uint32_t format = 0, width = 0, height = 0, stride = 0, flags = 0;
    bool     haveBuffer = false, bufferDone = false, ready = false, failed = false;
};

const zwlr_screencopy_frame_v1_listener kCaptureListener = {
    .buffer =
        [](void *d,
           zwlr_screencopy_frame_v1 *,
           uint32_t fmt,
           uint32_t w,
           uint32_t h,
           uint32_t stride) {
            auto *c       = static_cast<Capture *>(d);
            c->format     = fmt;
            c->width      = w;
            c->height     = h;
            c->stride     = stride;
            c->haveBuffer = true;
        },
    .flags  = [](void *d,
                 zwlr_screencopy_frame_v1 *,
                 uint32_t f) { static_cast<Capture *>(d)->flags = f; },
    .ready  = [](
                  void *d, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t
              ) { static_cast<Capture *>(d)->ready = true; },
    .failed = [](void *d, zwlr_screencopy_frame_v1 *) { static_cast<Capture *>(d)->failed = true; },
    .damage = [](void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t, uint32_t) {},
    .linux_dmabuf = [](void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t) {},
    .buffer_done = [](void *d,
                      zwlr_screencopy_frame_v1 *) { static_cast<Capture *>(d)->bufferDone = true; },
};

} // namespace

bool WlApp::ensureVirtualPointer() {
    if (!vptrManager || !seat)
        return false;
    if (!_vptr) {
        Output *o = outputs.empty() ? nullptr : outputs.front().get();
        if (o && wl_proxy_get_version(reinterpret_cast<wl_proxy *>(vptrManager)) >= 2)
            _vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
                vptrManager, seat, o->output
            );
        else
            _vptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vptrManager, seat);
        // Without real devices the seat only now gains a pointer; wait until
        // our wl_pointer exists or the first motion would be lost.
        for (int i = 0; i < 10 && !_pointer; ++i)
            wl_display_roundtrip(display);
    }
    return _vptr && _pointer;
}

bool WlApp::injectPointerMove(Window &win, Point p) {
    if (!ensureVirtualPointer())
        return false;
    auto   &w = static_cast<WlWindow &>(win);
    Output *o = w.output() ? w.output() : (outputs.empty() ? nullptr : outputs.front().get());
    if (!o || o->modeW <= 0)
        return false;
    // The extent is the output's logical size: xdg-output's where known
    // (fractional scales), else mode / integer scale.
    const uint32_t ow = uint32_t(o->logicalW > 0 ? o->logicalW : o->modeW / o->scale);
    const uint32_t oh = uint32_t(o->logicalH > 0 ? o->logicalH : o->modeH / o->scale);
    const auto     x  = uint32_t(std::lround(std::max(0.0, p.x) * kGrid));
    const auto     y  = uint32_t(std::lround(std::max(0.0, p.y) * kGrid));
    zwlr_virtual_pointer_v1_motion_absolute(_vptr, nowMs(), x, y, ow * kGrid, oh * kGrid);
    zwlr_virtual_pointer_v1_frame(_vptr);
    wl_display_flush(display);
    return true;
}

bool WlApp::injectButton(Window &, Button b, bool down) {
    if (!ensureVirtualPointer())
        return false;
    zwlr_virtual_pointer_v1_button(
        _vptr,
        nowMs(),
        evdevButton(b),
        down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED
    );
    zwlr_virtual_pointer_v1_frame(_vptr);
    wl_display_flush(display);
    return true;
}

bool WlApp::injectScroll(Window &, double dx, double dy) {
    if (!ensureVirtualPointer())
        return false;
    const uint32_t t     = nowMs();
    // One notch = 15 px + discrete 1, what a libinput wheel reports; the
    // compositor turns the discrete steps into axis_value120. The source
    // follows each axis (see injectPhasedScroll).
    auto           notch = [&](uint32_t axis, double v) {
        if (v == 0)
            return;
        zwlr_virtual_pointer_v1_axis_discrete(
            _vptr, t, axis, wl_fixed_from_double(v * 15), int32_t(std::lround(v))
        );
        zwlr_virtual_pointer_v1_axis_source(_vptr, WL_POINTER_AXIS_SOURCE_WHEEL);
    };
    notch(WL_POINTER_AXIS_VERTICAL_SCROLL, dy);
    notch(WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx);
    zwlr_virtual_pointer_v1_frame(_vptr);
    wl_display_flush(display);
    return true;
}

// Wayland has no "fingers down" marker: the first finger axis is the Begin,
// so a zero-delta Begin sends nothing (a zero axis would read as a stop).
// End is axis_stop on both axes, as libinput reports a lift. Momentum has no
// Wayland equivalent and goes out as ordinary finger motion.
bool WlApp::injectPhasedScroll(Window &, double dx, double dy, ScrollPhase phase) {
    if (!ensureVirtualPointer())
        return false;
    if (phase == ScrollPhase::Begin && dx == 0 && dy == 0)
        return true;
    // wlroots applies axis_source to the axis the previous axis/axis_stop
    // named, so the source goes after each one, not before.
    const uint32_t t    = nowMs();
    auto           send = [&](uint32_t axis, double v) {
        if (phase == ScrollPhase::End)
            zwlr_virtual_pointer_v1_axis_stop(_vptr, t, axis);
        else if (v != 0)
            zwlr_virtual_pointer_v1_axis(_vptr, t, axis, wl_fixed_from_double(v));
        else
            return;
        zwlr_virtual_pointer_v1_axis_source(_vptr, WL_POINTER_AXIS_SOURCE_FINGER);
    };
    send(WL_POINTER_AXIS_VERTICAL_SCROLL, dy);
    send(WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx);
    zwlr_virtual_pointer_v1_frame(_vptr);
    wl_display_flush(display);
    return true;
}

bool WlApp::ensureVirtualKeyboard(WlWindow &w) {
    if (!vkbManager || !seat)
        return false;
    if (!_vkb) {
        _vkb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkbManager, seat);
        // A plain US keymap: evdevFromKey() maps plat keys by US position.
        xkb_rule_names names{};
        names.layout = "us";
        _vkbKeymap = xkb_keymap_new_from_names(_xkb.context(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!_vkbKeymap)
            return false;
        char        *text = xkb_keymap_get_as_string(_vkbKeymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        const size_t size = std::strlen(text) + 1;
        const int    fd   = createShmFd(size);
        bool         ok   = fd >= 0 && write(fd, text, size) == ssize_t(size);
        std::free(text);
        if (!ok) {
            if (fd >= 0)
                close(fd);
            return false;
        }
        zwp_virtual_keyboard_v1_keymap(_vkb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, uint32_t(size));
        close(fd);
    }
    // The seat gains a keyboard only now; wait for our wl_keyboard, its
    // keymap and the focus enter, or the first keys would go nowhere.
    for (int i = 0; i < 20 && (!_keyboard || !_xkb.hasKeymap() || _kbFocus != &w); ++i)
        wl_display_roundtrip(display);
    return _keyboard != nullptr;
}

bool WlApp::injectKey(Window &win, Key k, bool down) {
    auto &w = static_cast<WlWindow &>(win);
    if (!ensureVirtualKeyboard(w))
        return false;
    const uint32_t code = linux_input::evdevFromKey(k);
    if (!code)
        return false;
    // wlroots does not derive modifier state from virtual key events; the
    // client of the virtual keyboard owns it and must send it explicitly.
    const char *mod = nullptr;
    switch (k) {
    case Key::ShiftLeft:
    case Key::ShiftRight:
        mod = XKB_MOD_NAME_SHIFT;
        break;
    case Key::ControlLeft:
    case Key::ControlRight:
        mod = XKB_MOD_NAME_CTRL;
        break;
    case Key::AltLeft:
    case Key::AltRight:
        mod = XKB_MOD_NAME_ALT;
        break;
    case Key::SuperLeft:
    case Key::SuperRight:
        mod = XKB_MOD_NAME_LOGO;
        break;
    default:
        break;
    }
    zwp_virtual_keyboard_v1_key(
        _vkb, nowMs(), code, down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED
    );
    if (mod) {
        const xkb_mod_index_t idx = xkb_keymap_mod_get_index(_vkbKeymap, mod);
        if (idx != XKB_MOD_INVALID)
            _vkbMods = down ? (_vkbMods | (1u << idx)) : (_vkbMods & ~(1u << idx));
        zwp_virtual_keyboard_v1_modifiers(_vkb, _vkbMods, 0, 0, 0);
    }
    wl_display_flush(display);
    return true;
}

bool WlApp::readPixel(Window &win, int x, int y, uint32_t *argb) {
    auto   &w = static_cast<WlWindow &>(win);
    Output *o = w.output() ? w.output() : (outputs.empty() ? nullptr : outputs.front().get());
    if (!screencopyManager || !o)
        return false;

    // A private queue, so capturing does not dispatch app events under the
    // caller's feet.
    wl_event_queue *q = wl_display_create_queue(display);
    auto           *manager =
        static_cast<zwlr_screencopy_manager_v1 *>(wl_proxy_create_wrapper(screencopyManager));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(manager), q);
    Capture cap;
    auto   *frame = zwlr_screencopy_manager_v1_capture_output(manager, 0, o->output);
    wl_proxy_wrapper_destroy(manager);
    zwlr_screencopy_frame_v1_add_listener(frame, &kCaptureListener, &cap);
    const bool v3 = wl_proxy_get_version(reinterpret_cast<wl_proxy *>(frame)) >= 3;

    bool       ok     = dispatchQueueUntil(
                            q, [&] { return cap.failed || (cap.haveBuffer && (!v3 || cap.bufferDone)); }, 3000
                        ) &&
                        !cap.failed && cap.haveBuffer;
    wl_buffer *buffer = nullptr;
    void      *data   = MAP_FAILED;
    size_t     bytes  = size_t(cap.stride) * cap.height;
    if (ok) {
        const int fd = createShmFd(bytes);
        ok           = fd >= 0;
        if (ok) {
            data          = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            auto *shmWrap = static_cast<wl_shm *>(wl_proxy_create_wrapper(shm));
            wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(shmWrap), q);
            wl_shm_pool *pool = wl_shm_create_pool(shmWrap, fd, int32_t(bytes));
            wl_proxy_wrapper_destroy(shmWrap);
            buffer = wl_shm_pool_create_buffer(
                pool, 0, int32_t(cap.width), int32_t(cap.height), int32_t(cap.stride), cap.format
            );
            wl_shm_pool_destroy(pool);
            close(fd);
            ok = data != MAP_FAILED;
        }
    }
    if (ok) {
        zwlr_screencopy_frame_v1_copy(frame, buffer);
        ok = dispatchQueueUntil(q, [&] { return cap.ready || cap.failed; }, 3000) && cap.ready;
    }
    if (ok) {
        ok = x >= 0 && y >= 0 && uint32_t(x) < cap.width && uint32_t(y) < cap.height;
        if (ok) {
            const uint32_t row = (cap.flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT)
                                     ? cap.height - 1 - uint32_t(y)
                                     : uint32_t(y);
            uint32_t       v   = *reinterpret_cast<const uint32_t *>(
                static_cast<const char *>(data) + size_t(row) * cap.stride + size_t(x) * 4
            );
            switch (cap.format) {
            case WL_SHM_FORMAT_ARGB8888:
                break;
            case WL_SHM_FORMAT_XRGB8888:
                v |= 0xff000000u;
                break;
            case WL_SHM_FORMAT_ABGR8888:
            case WL_SHM_FORMAT_XBGR8888:
                v = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu);
                if (cap.format == WL_SHM_FORMAT_XBGR8888)
                    v |= 0xff000000u;
                break;
            default:
                ok = false;
                break;
            }
            *argb = v;
        }
    }
    if (buffer)
        wl_buffer_destroy(buffer);
    if (data != MAP_FAILED)
        munmap(data, bytes);
    zwlr_screencopy_frame_v1_destroy(frame);
    wl_display_flush(display);
    wl_event_queue_destroy(q);
    return ok;
}

} // namespace plat::wl

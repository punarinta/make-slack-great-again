// Wayland seat: pointer (buttons, hit test, scroll frames), keyboard (xkb
// keymap fd, client-side repeat), text-input-v3 and cursors.
#include "wayland/wl_internal.h"
#include "freedesktop/cursor_names.h"
#include "freedesktop/evdev_codes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace plat::wl {

namespace {

const wl_seat_listener kSeatListener = {
    .capabilities =
        [](void *d, wl_seat *, uint32_t caps) { static_cast<WlApp *>(d)->onSeatCaps(caps); },
    .name = [](void *, wl_seat *, const char *) {},
};

const wl_pointer_listener kPointerListener = {
    .enter =
        [](void *d, wl_pointer *, uint32_t serial, wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
            static_cast<WlApp *>(d)->onPointerEnter(
                serial, s, wl_fixed_to_double(x), wl_fixed_to_double(y)
            );
        },
    .leave = [](
                 void *d, wl_pointer *, uint32_t serial, wl_surface *s
             ) { static_cast<WlApp *>(d)->onPointerLeave(serial, s); },
    .motion =
        [](void *d, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
            static_cast<WlApp *>(d)->onPointerMotion(wl_fixed_to_double(x), wl_fixed_to_double(y));
        },
    .button = [](
                  void *d, wl_pointer *, uint32_t serial, uint32_t, uint32_t button, uint32_t state
              ) { static_cast<WlApp *>(d)->onPointerButton(serial, button, state); },
    .axis   = [](
                  void *d, wl_pointer *, uint32_t, uint32_t axis, wl_fixed_t v
              ) { static_cast<WlApp *>(d)->onPointerAxis(axis, wl_fixed_to_double(v)); },
    .frame  = [](void *d, wl_pointer *) { static_cast<WlApp *>(d)->onPointerFrame(); },
    .axis_source   = [](void *d,
                        wl_pointer *,
                        uint32_t src) { static_cast<WlApp *>(d)->onPointerAxisSource(src); },
    .axis_stop     = [](
                         void *d, wl_pointer *, uint32_t, uint32_t
                     ) { static_cast<WlApp *>(d)->onPointerAxisStop(); },
    .axis_discrete = [](
                         void *d, wl_pointer *, uint32_t axis, int32_t n
                     ) { static_cast<WlApp *>(d)->onPointerAxisDiscrete(axis, n); },
    .axis_value120 = [](
                         void *d, wl_pointer *, uint32_t axis, int32_t v
                     ) { static_cast<WlApp *>(d)->onPointerAxisValue120(axis, v); },
    .axis_relative_direction = [](void *, wl_pointer *, uint32_t, uint32_t) {},
};

// Swipe dx/dy are surface-local, i.e. logical pixels: the plat contract.
const zwp_pointer_gesture_swipe_v1_listener kSwipeListener = {
    .begin =
        [](void *d, zwp_pointer_gesture_swipe_v1 *, uint32_t, uint32_t, wl_surface *s, uint32_t n) {
            static_cast<WlApp *>(d)->onGestureBegin(Gesture::Swipe, s, n);
        },
    .update =
        [](void *d, zwp_pointer_gesture_swipe_v1 *, uint32_t, wl_fixed_t dx, wl_fixed_t dy) {
            static_cast<WlApp *>(d)->onGestureUpdate(
                Gesture::Swipe, wl_fixed_to_double(dx), wl_fixed_to_double(dy)
            );
        },
    .end = [](
               void *d, zwp_pointer_gesture_swipe_v1 *, uint32_t, uint32_t, int32_t cancelled
           ) { static_cast<WlApp *>(d)->onGestureEnd(Gesture::Swipe, cancelled != 0); },
};

const wl_keyboard_listener kKeyboardListener = {
    .keymap = [](
                  void *d, wl_keyboard *, uint32_t fmt, int32_t fd, uint32_t size
              ) { static_cast<WlApp *>(d)->onKeymap(fmt, fd, size); },
    .enter =
        [](void *d, wl_keyboard *, uint32_t serial, wl_surface *s, wl_array *) {
            static_cast<WlApp *>(d)->onKeyboardEnter(serial, s);
        },
    .leave = [](
                 void *d, wl_keyboard *, uint32_t serial, wl_surface *s
             ) { static_cast<WlApp *>(d)->onKeyboardLeave(serial, s); },
    .key   = [](
                 void *d, wl_keyboard *, uint32_t serial, uint32_t, uint32_t key, uint32_t state
             ) { static_cast<WlApp *>(d)->onKey(serial, key, state); },
    .modifiers =
        [](void *d,
           wl_keyboard *,
           uint32_t,
           uint32_t dep,
           uint32_t lat,
           uint32_t lock,
           uint32_t group) { static_cast<WlApp *>(d)->onModifiers(dep, lat, lock, group); },
    .repeat_info = [](
                       void *d, wl_keyboard *, int32_t rate, int32_t delay
                   ) { static_cast<WlApp *>(d)->onRepeatInfo(rate, delay); },
};

const zwp_text_input_v3_listener kTextInputListener = {
    .enter = [](void *d,
                zwp_text_input_v3 *,
                wl_surface *s) { static_cast<WlApp *>(d)->onTextInputEnter(s); },
    .leave = [](void *d,
                zwp_text_input_v3 *,
                wl_surface *s) { static_cast<WlApp *>(d)->onTextInputLeave(s); },
    .preedit_string =
        [](void *d, zwp_text_input_v3 *, const char *text, int32_t b, int32_t e) {
            auto *a            = static_cast<WlApp *>(d);
            a->_pendingPreedit = {text ? text : "", b, e};
        },
    .commit_string           = [](
                                   void *d, zwp_text_input_v3 *, const char *text
                               ) { static_cast<WlApp *>(d)->_pendingCommit = text ? text : ""; },
    // We never send surrounding text, so a conforming IME has nothing to delete.
    .delete_surrounding_text = [](void *, zwp_text_input_v3 *, uint32_t, uint32_t) {},
    .done = [](void *d,
               zwp_text_input_v3 *,
               uint32_t serial) { static_cast<WlApp *>(d)->onTextInputDone(serial); },
};

Button buttonFromEvdev(uint32_t b, bool *ok) {
    *ok = true;
    switch (b) {
    case BTN_LEFT:
        return Button::Left;
    case BTN_RIGHT:
        return Button::Right;
    case BTN_MIDDLE:
        return Button::Middle;
    case BTN_SIDE:
    case BTN_BACK:
        return Button::Back;
    case BTN_EXTRA:
    case BTN_FORWARD:
        return Button::Forward;
    default:
        *ok = false;
        return Button::Left;
    }
}

uint32_t shapeFor(Cursor c) {
    switch (c) {
    case Cursor::Arrow:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    case Cursor::IBeam:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    case Cursor::Hand:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    case Cursor::Wait:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
    case Cursor::Progress:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS;
    case Cursor::Crosshair:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
    case Cursor::NotAllowed:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
    case Cursor::Move:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
    case Cursor::Grab:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
    case Cursor::Grabbing:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
    case Cursor::ResizeH:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
    case Cursor::ResizeV:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
    case Cursor::ResizeNWSE:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
    case Cursor::ResizeNESW:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
    case Cursor::Hidden:
        break;
    }
    return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
}

} // namespace

// ── seat ────────────────────────────────────────────────────────────────────

void WlApp::bindSeat(uint32_t name, uint32_t version) {
    // One seat: multi-seat desktops are vanishingly rare for a chat app.
    seatVersion = std::min(version, 9u);
    seat =
        static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, seatVersion));
    wl_seat_add_listener(seat, &kSeatListener, this);
    setupSeat();
}

// Seat-dependent objects whose manager globals may arrive in any order.
void WlApp::setupSeat() {
    if (seat && !_textInput && textInputManager) {
        _textInput = zwp_text_input_manager_v3_get_text_input(textInputManager, seat);
        zwp_text_input_v3_add_listener(_textInput, &kTextInputListener, this);
    }
    setupDataDevice();
}

void WlApp::onSeatCaps(uint32_t caps) {
    // Capabilities come and go (virtual devices, docking); follow them.
    const bool hasPointer = caps & WL_SEAT_CAPABILITY_POINTER;
    if (hasPointer && !_pointer) {
        _pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(_pointer, &kPointerListener, this);
        if (cursorShapeManager)
            _shapeDevice = wp_cursor_shape_manager_v1_get_pointer(cursorShapeManager, _pointer);
        _cursorApplied = false;
        setupGestures();
    } else if (!hasPointer && _pointer) {
        destroyGestures();
        if (_shapeDevice)
            wp_cursor_shape_device_v1_destroy(_shapeDevice);
        _shapeDevice = nullptr;
        seatVersion >= 3 ? wl_pointer_release(_pointer) : wl_pointer_destroy(_pointer);
        _pointer = nullptr;
        if (_pointerFocus)
            _pointerFocus->emitEvent({.type = EventType::PointerLeave});
        _pointerFocus = nullptr;
    }
    const bool hasKeyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (hasKeyboard && !_keyboard) {
        _keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(_keyboard, &kKeyboardListener, this);
    } else if (!hasKeyboard && _keyboard) {
        stopRepeat();
        seatVersion >= 3 ? wl_keyboard_release(_keyboard) : wl_keyboard_destroy(_keyboard);
        _keyboard = nullptr;
        if (_kbFocus)
            _kbFocus->emitEvent({.type = EventType::FocusOut});
        _kbFocus = nullptr;
    }
}

// ── pointer ─────────────────────────────────────────────────────────────────

void WlApp::onPointerEnter(uint32_t serial, wl_surface *s, double x, double y) {
    _enterSerial  = serial;
    _pointerFocus = windowFor(s);
    _pointerPos   = {x, y};
    _swallowed    = 0;
    if (!_pointerFocus)
        return;
    applyCursor(true); // a cursor is per-enter on Wayland: always set it anew
    WlWindow *w = _pointerFocus;
    w->emitEvent({.type = EventType::PointerEnter, .pos = _pointerPos, .mods = _xkb.mods()});
    // The enter carries the position and compositors often send no motion
    // with it; the contract (and every other backend) has a Move follow.
    if (alive(w) && _pointerFocus == w)
        w->emitEvent({.type = EventType::PointerMove, .pos = _pointerPos, .mods = _xkb.mods()});
}

void WlApp::onPointerLeave(uint32_t, wl_surface *) {
    if (_pointerFocus)
        _pointerFocus->emitEvent({.type = EventType::PointerLeave, .mods = _xkb.mods()});
    _pointerFocus = nullptr;
    _swallowed    = 0;
    // No release follows a leave (a drag took the grab, or the surface went
    // away); a finger scroll cut short has no stop either.
    _held         = 0;
    _pressWindow  = nullptr;
    _scrollActive = false;
}

void WlApp::onPointerMotion(double x, double y) {
    _pointerPos = {x, y};
    if (!_pointerFocus)
        return;
    const uint32_t mods = _xkb.mods();
    if (_motionWin && (_motionWin != _pointerFocus || _motionMods != mods))
        flushMotion();
    _motionWin  = _pointerFocus;
    _motionPos  = _pointerPos;
    _motionMods = mods;
}

void WlApp::flushMotion() {
    WlWindow *w = _motionWin;
    _motionWin  = nullptr;
    if (!w || !alive(w))
        return;
    if (w == _pointerFocus)
        applyCursor();
    w->emitEvent({.type = EventType::PointerMove, .pos = _motionPos, .mods = _motionMods});
}

void WlApp::onPointerButton(uint32_t serial, uint32_t code, uint32_t state) {
    _inputSerial   = serial;
    WlWindow    *w = _pointerFocus;
    bool         known;
    const Button b    = buttonFromEvdev(code, &known);
    const bool   down = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (!w || !known)
        return;
    const uint32_t bit = 1u << uint32_t(b);
    if (!down) {
        // The release of a press that became a move/resize (compositors that
        // ignore the request, like kiosks, still send it) stays swallowed.
        if (_swallowed & bit) {
            _swallowed &= ~bit;
            return;
        }
        _held &= ~bit;
        w->emitEvent(
            {.type = EventType::PointerUp, .pos = _pointerPos, .button = b, .mods = _xkb.mods()}
        );
        return;
    }

    const int clicks = _clicks.press(
        int(code), _pointerPos.x, _pointerPos.y, core::monotonicMs(), doubleClickMs(), 4, 4
    );

    if (b == Button::Left || b == Button::Right) {
        const HitArea area = w->hitTestAt(_pointerPos);
        if (isNonClient(area)) {
            _swallowed |= bit;
            if (b == Button::Right) {
                if (area == HitArea::Caption)
                    w->showWindowMenu(serial, _pointerPos);
            } else if (area == HitArea::Caption) {
                // Client-side title bars own the double-click-to-maximise
                // gesture on Wayland (GTK does the same).
                if (clicks == 2)
                    w->setMaximized(!w->isMaximized());
                else
                    w->startMove(serial);
            } else {
                w->startResize(serial, area);
            }
            return;
        }
    }
    _held |= bit;
    _pressSerial = serial;
    _pressWindow = w;
    w->emitEvent(
        {.type   = EventType::PointerDown,
         .pos    = _pointerPos,
         .button = b,
         .clicks = clicks,
         .mods   = _xkb.mods()}
    );
}

void WlApp::onPointerAxis(uint32_t axis, double value) {
    (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? _axis.dx : _axis.dy) += value;
    _axis.any = true;
    if (seatVersion < 5)
        onPointerFrame(); // no frame event before v5: every axis stands alone
}

void WlApp::onPointerAxisDiscrete(uint32_t axis, int32_t n) {
    (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? _axis.discX : _axis.discY) += n;
    _axis.hasDiscrete = true;
}

void WlApp::onPointerAxisValue120(uint32_t axis, int32_t v) {
    (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? _axis.v120x : _axis.v120y) += v;
    _axis.has120 = true;
}

// Touchpad scrolling carries phases: the first finger axis (after a stop, or
// ever) is Begin, the rest Update, and axis_stop (fingers lifted) an End with
// no delta. There is no Momentum on Wayland: kinetic scrolling is the
// client's job (libinput stops at the lift), so a toolkit that wants it
// animates from End itself. Continuous sources (trackpoints, some mice)
// never send axis_stop, so they stay precise without phases rather than
// opening a Begin that never ends.
void WlApp::onPointerFrame() {
    const Axis a = _axis;
    _axis        = {};
    if (!_pointerFocus) {
        if (a.stop)
            _scrollActive = false;
        return;
    }
    const bool finger = a.source == WL_POINTER_AXIS_SOURCE_FINGER;
    if (a.any) {
        Event      e{.type = EventType::Scroll, .pos = _pointerPos, .mods = _xkb.mods()};
        const bool wheel = a.source == WL_POINTER_AXIS_SOURCE_WHEEL ||
                           a.source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT;
        if (a.has120) {
            e.dx = a.v120x / 120.0; // high-res wheels: fractional notches
            e.dy = a.v120y / 120.0;
        } else if (a.hasDiscrete) {
            e.dx = a.discX;
            e.dy = a.discY;
        } else if (wheel) {
            // A wheel with no discrete info (rare): libinput's 15 px per notch.
            e.dx = a.dx / 15.0;
            e.dy = a.dy / 15.0;
        } else {
            // Finger / continuous: axis values are already surface pixels.
            e.precise = true;
            e.dx      = a.dx;
            e.dy      = a.dy;
        }
        if (e.dx != 0 || e.dy != 0) {
            if (finger) {
                e.phase       = _scrollActive ? ScrollPhase::Update : ScrollPhase::Begin;
                _scrollActive = true;
            }
            _pointerFocus->emitEvent(e);
        }
    }
    if (a.stop && _scrollActive && _pointerFocus) {
        _scrollActive = false;
        _pointerFocus->emitEvent(
            {.type    = EventType::Scroll,
             .pos     = _pointerPos,
             .precise = true,
             .mods    = _xkb.mods(),
             .phase   = ScrollPhase::End}
        );
    }
}

// ── gestures ────────────────────────────────────────────────────────────────

void WlApp::setupGestures() {
    if (!gesturesManager || !_pointer || _swipe)
        return;
    _swipe = zwp_pointer_gestures_v1_get_swipe_gesture(gesturesManager, _pointer);
    zwp_pointer_gesture_swipe_v1_add_listener(_swipe, &kSwipeListener, this);
}

void WlApp::destroyGestures() {
    // The destroy requests exist since v2; a v1 object is only freed locally.
    const bool v2 =
        gesturesManager && wl_proxy_get_version(reinterpret_cast<wl_proxy *>(gesturesManager)) >= 2;
    auto gone = [v2](auto *g, auto destroy) {
        if (g)
            v2 ? destroy(g) : wl_proxy_destroy(reinterpret_cast<wl_proxy *>(g));
    };
    gone(_swipe, zwp_pointer_gesture_swipe_v1_destroy);
    _swipe         = nullptr;
    _gestureWindow = nullptr;
}

void WlApp::onGestureBegin(Gesture g, wl_surface *s, uint32_t fingers) {
    _gestureWindow  = windowFor(s);
    _gestureFingers = int(fingers);
    if (_gestureWindow)
        _gestureWindow->emitEvent(
            {.type    = EventType::GestureBegin,
             .pos     = _pointerPos,
             .mods    = _xkb.mods(),
             .gesture = g,
             .fingers = _gestureFingers}
        );
}

void WlApp::onGestureUpdate(Gesture g, double dx, double dy) {
    if (_gestureWindow)
        _gestureWindow->emitEvent(
            {.type    = EventType::GestureUpdate,
             .pos     = _pointerPos,
             .dx      = dx,
             .dy      = dy,
             .mods    = _xkb.mods(),
             .gesture = g,
             .fingers = _gestureFingers}
        );
}

void WlApp::onGestureEnd(Gesture g, bool cancelled) {
    WlWindow *w    = _gestureWindow;
    _gestureWindow = nullptr;
    if (w)
        w->emitEvent(
            {.type      = EventType::GestureEnd,
             .pos       = _pointerPos,
             .mods      = _xkb.mods(),
             .gesture   = g,
             .fingers   = _gestureFingers,
             .cancelled = cancelled}
        );
}

// ── cursors ─────────────────────────────────────────────────────────────────

void WlApp::applyCursor(bool force) {
    if (!_pointer || !_pointerFocus)
        return;
    const Cursor c     = _pointerFocus->cursorAt(_pointerPos);
    const int    scale = _pointerFocus->cursorScale();
    if (!force && _cursorApplied && c == _appliedCursor && scale == _appliedCursorScale)
        return;
    _cursorApplied      = true;
    _appliedCursor      = c;
    _appliedCursorScale = scale;

    if (c == Cursor::Hidden) {
        wl_pointer_set_cursor(_pointer, _enterSerial, nullptr, 0, 0);
        return;
    }
    if (_shapeDevice) {
        // The compositor draws it at the right size for any scale: no theme
        // loading, no buffers.
        wp_cursor_shape_device_v1_set_shape(_shapeDevice, _enterSerial, shapeFor(c));
        return;
    }
    if (!_cursorTheme || _cursorThemeScale != scale) {
        if (_cursorTheme)
            wl_cursor_theme_destroy(_cursorTheme);
        int size = 24;
        if (const char *s = std::getenv("XCURSOR_SIZE"); s && std::atoi(s) > 0)
            size = std::atoi(s);
        _cursorTheme      = wl_cursor_theme_load(std::getenv("XCURSOR_THEME"), size * scale, shm);
        _cursorThemeScale = scale;
    }
    wl_cursor *cur = nullptr;
    for (const char *const *n = linux_cursor::themeNames(c); _cursorTheme && *n; ++n)
        if ((cur = wl_cursor_theme_get_cursor(_cursorTheme, *n)))
            break;
    if (!cur || cur->image_count == 0)
        return; // no theme installed: the compositor keeps whatever it shows
    // First frame only: animated cursors (wait) would need a frame-callback
    // driven surface, which is not worth it for a chat client.
    wl_cursor_image *img = cur->images[0];
    wl_buffer       *buf = wl_cursor_image_get_buffer(img);
    if (!_cursorSurface)
        _cursorSurface = wl_compositor_create_surface(compositor);
    if (compositorVersion >= 3)
        wl_surface_set_buffer_scale(_cursorSurface, scale);
    wl_surface_attach(_cursorSurface, buf, 0, 0);
    if (compositorVersion >= 4)
        wl_surface_damage_buffer(_cursorSurface, 0, 0, int32_t(img->width), int32_t(img->height));
    else
        wl_surface_damage(_cursorSurface, 0, 0, int32_t(img->width), int32_t(img->height));
    wl_surface_commit(_cursorSurface);
    wl_pointer_set_cursor(
        _pointer,
        _enterSerial,
        _cursorSurface,
        int32_t(img->hotspot_x) / scale,
        int32_t(img->hotspot_y) / scale
    );
}

// ── keyboard ────────────────────────────────────────────────────────────────

void WlApp::onKeymap(uint32_t format, int fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
        close(fd);
        return;
    }
    // MAP_PRIVATE: since wl_seat v7 the fd may be read-only-shared.
    void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;
    const char *text = static_cast<const char *>(map);
    xkb_keymap *km   = xkb_keymap_new_from_buffer(
        _xkb.context(),
        text,
        strnlen(text, size),
        XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS
    );
    munmap(map, size);
    if (km) {
        stopRepeat();
        _xkb.setKeymap(km);
    }
}

void WlApp::onKeyboardEnter(uint32_t serial, wl_surface *s) {
    _inputSerial = serial;
    _kbFocus     = windowFor(s);
    if (_kbFocus)
        _kbFocus->emitEvent({.type = EventType::FocusIn});
}

void WlApp::onKeyboardLeave(uint32_t, wl_surface *) {
    stopRepeat();
    if (_kbFocus)
        _kbFocus->emitEvent({.type = EventType::FocusOut});
    _kbFocus = nullptr;
}

void WlApp::onKey(uint32_t serial, uint32_t key, uint32_t state) {
    _inputSerial        = serial;
    const uint32_t code = key + 8; // evdev → xkb keycode
    const bool     down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (!down && code == _repeatKey)
        stopRepeat();
    emitKey(code, down, false);
    if (down && _repeatRate > 0 && _xkb.repeats(code)) {
        // Client-side repeat, as the protocol requires: delay, then 1/rate.
        stopRepeat();
        _repeatKey   = code;
        _repeatTimer = _loop.core.addTimer(std::max(1, _repeatDelay), false, [this, code] {
            _repeatTimer = _loop.core.addTimer(std::max(1, 1000 / _repeatRate), true, [this, code] {
                emitKey(code, true, true);
            });
            emitKey(code, true, true);
        });
    }
}

void WlApp::stopRepeat() {
    if (_repeatTimer)
        _loop.core.cancelTimer(_repeatTimer);
    _repeatTimer = 0;
    _repeatKey   = 0;
}

void WlApp::onModifiers(uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    _xkb.updateMask(dep, lat, lock, group);
}

void WlApp::emitKey(uint32_t code, bool down, bool repeat) {
    WlWindow *w = _kbFocus;
    if (!w)
        return;
    // Wayland sends modifier state separately, so xkb's own state tracking
    // stays off (updateState=false).
    const auto r = _xkb.key(code, down, false);
    w->emitEvent(
        {.type     = down ? EventType::KeyDown : EventType::KeyUp,
         .key      = r.key,
         .scancode = code - 8,
         .mods     = r.mods,
         .repeat   = repeat}
    );
    // Keys an IME consumed never reach us (the compositor routes them to the
    // input method), so typed text here cannot double up with commit_string.
    if (down && !r.text.empty() && alive(w) && w->textInput().enabled)
        w->emitEvent({.type = EventType::TextInput, .text = r.text});
}

// ── text input (IME) ────────────────────────────────────────────────────────

void WlApp::syncTextInput() {
    if (!_textInput)
        return;
    WlWindow  *w    = _tiFocus;
    const bool want = w && w->textInput().enabled;
    if (want) {
        const Rect c       = w->textInput().caret;
        bool       changed = false;
        if (!_tiEnabled) {
            // enable resets all IME state, so only send it on the transition.
            zwp_text_input_v3_enable(_textInput);
            zwp_text_input_v3_set_content_type(
                _textInput,
                ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL
            );
            _tiEnabled   = true;
            _tiCaretSent = {-1, -1, -1, -1};
            changed      = true;
        }
        if (c.x != _tiCaretSent.x || c.y != _tiCaretSent.y || c.w != _tiCaretSent.w ||
            c.h != _tiCaretSent.h) {
            zwp_text_input_v3_set_cursor_rectangle(
                _textInput, c.x, c.y, std::max(1, c.w), std::max(1, c.h)
            );
            _tiCaretSent = c;
            changed      = true;
        }
        if (changed) {
            zwp_text_input_v3_commit(_textInput);
            ++_tiCommits;
        }
    } else if (_tiEnabled) {
        zwp_text_input_v3_disable(_textInput);
        zwp_text_input_v3_commit(_textInput);
        ++_tiCommits;
        _tiEnabled = false;
        endPreedit();
    }
}

void WlApp::endPreedit() {
    if (_preedit.text.empty())
        return;
    _preedit = {};
    if (_tiFocus)
        _tiFocus->emitEvent({.type = EventType::TextPreedit});
}

void WlApp::onTextInputEnter(wl_surface *s) {
    _tiFocus   = windowFor(s);
    _tiEnabled = false;
    syncTextInput();
}

void WlApp::onTextInputLeave(wl_surface *) {
    endPreedit();
    if (_tiEnabled && _textInput) {
        zwp_text_input_v3_disable(_textInput);
        zwp_text_input_v3_commit(_textInput);
        ++_tiCommits;
    }
    _tiEnabled = false;
    _tiFocus   = nullptr;
}

void WlApp::onTextInputDone(uint32_t) {
    // Applied in the order the protocol specifies: drop the old preedit,
    // insert the commit, show the new preedit. A serial that lags our commit
    // count still carries text the user typed, so it is applied too; only
    // the IME's view of our state is stale, and the next commit fixes that.
    const Preedit     next   = std::move(_pendingPreedit);
    const std::string commit = std::move(_pendingCommit);
    _pendingPreedit          = {};
    _pendingCommit.clear();
    WlWindow *w = _tiFocus;
    if (!w)
        return;
    if (!commit.empty()) {
        endPreedit();
        w->emitEvent({.type = EventType::TextInput, .text = commit});
        if (!alive(w))
            return;
    }
    if (next.text != _preedit.text || next.begin != _preedit.begin || next.end != _preedit.end) {
        _preedit = next;
        w->emitEvent(
            {.type               = EventType::TextPreedit,
             .text               = next.text,
             .preeditCursorBegin = next.text.empty() ? -1 : next.begin,
             .preeditCursorEnd   = next.text.empty() ? -1 : next.end}
        );
    }
}

} // namespace plat::wl

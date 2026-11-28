// XInput2: smooth-scroll valuators (touchpads, high-resolution wheels) and
// XI 2.4 touchpad gestures. Everything else about the pointer stays core
// protocol (x11_app.cpp) — buttons, crossing, grabs — because selecting XI2
// button events would turn every implicit grab into an XI2 grab and the core
// path is the one every WM and XTEST is proven against.
//
// The one price: selecting XI_Motion on a window stops core MotionNotify
// being delivered to it outside grabs, so XI_Motion also feeds ordinary
// pointer motion. Under a core grab (a held button, our drag) the server
// delivers core events only, and the core path takes over again.
#include "x11/x11_internal.h"

#include <xcb/xinput.h>

#include <cmath>
#include <cstdlib>

namespace plat::x11 {

namespace {

double fp1616(int32_t v) {
    return v / 65536.0;
}
double fp3232(xcb_input_fp3232_t v) {
    return double(v.integral) + double(v.frac) / 4294967296.0;
}

// One wheel notch = 3 lines × 100/3 px (the contract's line), as on Win32.
constexpr double kPxPerNotch = 100.0;

// XI 2.4 swipe gesture events (30–32) sit past the first mask word.
constexpr int kGestureSwipeBegin = XCB_INPUT_GESTURE_SWIPE_BEGIN;
constexpr int kGestureSwipeEnd   = XCB_INPUT_GESTURE_SWIPE_END;

} // namespace

void X11App::setupXInput() {
    if (std::getenv("PLAT_X11_NO_XI2"))
        return;
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(_c, &xcb_input_id);
    if (!ext || !ext->present)
        return;
    // Ask for the newest we know; the server answers with what both speak.
    Reply v(xcb_input_xi_query_version_reply(_c, xcb_input_xi_query_version(_c, 2, 4), nullptr));
    if (!v || v->major_version < 2)
        return;
    _xiOpcode = ext->major_opcode;
    _xiMinor  = v->major_version > 2 ? 4 : v->minor_version;
    // Devices appearing/disappearing re-read the scroll classes.
    struct {
        xcb_input_event_mask_t head;
        uint32_t               mask;
    } m = {{XCB_INPUT_DEVICE_ALL, 1}, XCB_INPUT_XI_EVENT_MASK_HIERARCHY};
    xcb_input_xi_select_events(_c, _root, 1, &m.head);
    queryScrollDevices();
}

void X11App::selectXInput(xcb_window_t w) {
    if (!_xiOpcode)
        return;
    struct {
        xcb_input_event_mask_t head;
        uint32_t               mask[2];
    } m{};
    m.head.deviceid = XCB_INPUT_DEVICE_ALL_MASTER;
    m.head.mask_len = 2;
    m.mask[0]       = XCB_INPUT_XI_EVENT_MASK_MOTION | XCB_INPUT_XI_EVENT_MASK_DEVICE_CHANGED;
    if (_xiMinor >= 4)
        for (int ev = kGestureSwipeBegin; ev <= kGestureSwipeEnd; ++ev)
            m.mask[ev / 32] |= 1u << (ev % 32);
    xcb_input_xi_select_events(_c, w, 1, &m.head);
}

void X11App::queryScrollDevices() {
    _scrollDevs.clear();
    Reply r(xcb_input_xi_query_device_reply(
        _c, xcb_input_xi_query_device(_c, XCB_INPUT_DEVICE_ALL), nullptr
    ));
    if (!r)
        return;
    for (auto it = xcb_input_xi_query_device_infos_iterator(r.p); it.rem;
         xcb_input_xi_device_info_next(&it)) {
        const xcb_input_xi_device_info_t *info = it.data;
        ScrollDevice                      dev;
        std::unordered_map<int, double>   values; // valuator number → current value
        for (auto c = xcb_input_xi_device_info_classes_iterator(info); c.rem;
             xcb_input_device_class_next(&c)) {
            if (c.data->type == XCB_INPUT_DEVICE_CLASS_TYPE_VALUATOR) {
                auto *vc           = reinterpret_cast<const xcb_input_valuator_class_t *>(c.data);
                values[vc->number] = fp3232(vc->value);
            } else if (c.data->type == XCB_INPUT_DEVICE_CLASS_TYPE_SCROLL) {
                auto      *sc = reinterpret_cast<const xcb_input_scroll_class_t *>(c.data);
                ScrollAxis a;
                a.number    = sc->number;
                a.increment = fp3232(sc->increment);
                if (sc->scroll_type == XCB_INPUT_SCROLL_TYPE_VERTICAL)
                    dev.v = a;
                else if (sc->scroll_type == XCB_INPUT_SCROLL_TYPE_HORIZONTAL)
                    dev.h = a;
            }
        }
        if (dev.v.number < 0 && dev.h.number < 0)
            continue;
        // Start from the device's current position so the first event after
        // start-up is a real delta, not a jump.
        for (ScrollAxis *a : {&dev.v, &dev.h})
            if (auto v = values.find(a->number); a->number >= 0 && v != values.end()) {
                a->last     = v->second;
                a->haveLast = true;
            }
        _scrollDevs[info->deviceid] = dev;
    }
}

void X11App::resetScrollBases() {
    // The valuators keep counting while the pointer is elsewhere (other
    // clients get those events): the first value after re-entering is a new
    // base, not a delta.
    for (auto &[id, dev] : _scrollDevs)
        dev.v.haveLast = dev.h.haveLast = false;
}

void X11App::handleXInput(xcb_generic_event_t *ev) {
    auto *ge = reinterpret_cast<xcb_ge_generic_event_t *>(ev);
    if (!_xiOpcode || ge->extension != _xiOpcode)
        return;
    switch (ge->event_type) {
    case XCB_INPUT_HIERARCHY:
        queryScrollDevices();
        return;
    case XCB_INPUT_DEVICE_CHANGED: {
        auto *e = reinterpret_cast<xcb_input_device_changed_event_t *>(ev);
        if (e->reason == XCB_INPUT_CHANGE_REASON_DEVICE_CHANGE) {
            queryScrollDevices();
        } else if (auto it = _scrollDevs.find(e->sourceid); it != _scrollDevs.end()) {
            // Slave switch: the master now reports this device's valuators.
            it->second.v.haveLast = it->second.h.haveLast = false;
        }
        return;
    }
    case XCB_INPUT_MOTION: {
        auto *e   = reinterpret_cast<xcb_input_motion_event_t *>(ev);
        _lastTime = e->time;
        if (_drag.active)
            return; // under our grab core events drive the drag
        X11Window *w = findWindow(e->event);
        if (!w)
            return;
        const uint32_t mods = pointerMods(uint16_t(e->mods.effective));
        double         nx = 0, ny = 0; // notches (fractional)
        bool           scrolled = false;
        if (auto it = _scrollDevs.find(e->sourceid); it != _scrollDevs.end()) {
            ScrollDevice             &dev   = it->second;
            const uint32_t           *mask  = xcb_input_button_press_valuator_mask(e);
            const int                 words = xcb_input_button_press_valuator_mask_length(e);
            const xcb_input_fp3232_t *vals  = xcb_input_button_press_axisvalues(e);
            const int                 nvals = xcb_input_button_press_axisvalues_length(e);
            // Values come packed, one per set mask bit, lowest bit first.
            for (int i = 0, k = 0; i < words * 32 && k < nvals; ++i) {
                if (!(mask[i / 32] & (1u << (i % 32))))
                    continue;
                const double v = fp3232(vals[k++]);
                for (ScrollAxis *a : {&dev.v, &dev.h}) {
                    if (a->number != i)
                        continue;
                    if (a->haveLast && a->increment != 0) {
                        (a == &dev.v ? ny : nx) += (v - a->last) / a->increment;
                        scrolled = true;
                    }
                    a->last     = v;
                    a->haveLast = true;
                }
            }
        }
        const double px = fp1616(e->event_x), py = fp1616(e->event_y);
        const Point  p{px / _scale, py / _scale};
        const bool   moved =
            std::abs(p.x - _pointerPos.x) > 1e-6 || std::abs(p.y - _pointerPos.y) > 1e-6;
        if (!scrolled || moved)
            handleMotion(w, px, py, mods, e->time);
        if (scrolled && (nx != 0 || ny != 0)) {
            flushMotion(); // the move first, then the scroll at that position
            // The server also sends emulated core wheel buttons for this
            // (same timestamp); handleButton drops those.
            _xiScrollTime = e->time;
            _xiScrolled   = true;
            Event s{.type = EventType::Scroll, .pos = p, .mods = mods};
            // Whole notches are a wheel (libinput reports mice through the
            // same valuators): notches, not precise, as on Wayland/Win32.
            // Anything fractional is a touchpad or high-res wheel: pixels.
            auto  whole = [](double n) { return std::abs(n - std::round(n)) < 1e-6; };
            if (whole(nx) && whole(ny)) {
                s.dx = std::round(nx);
                s.dy = std::round(ny);
            } else {
                s.precise = true;
                s.dx      = nx * kPxPerNotch;
                s.dy      = ny * kPxPerNotch;
            }
            w->emit(s);
        }
        return;
    }
    case XCB_INPUT_GESTURE_SWIPE_BEGIN:
    case XCB_INPUT_GESTURE_SWIPE_UPDATE:
    case XCB_INPUT_GESTURE_SWIPE_END: {
        auto     *sw    = reinterpret_cast<xcb_input_gesture_swipe_begin_event_t *>(ev);
        const int phase = ge->event_type - XCB_INPUT_GESTURE_SWIPE_BEGIN;
        _lastTime       = sw->time;
        X11Window *w    = findWindow(sw->event);
        if (!w)
            return;
        Event g{
            .type = phase == 0   ? EventType::GestureBegin
                    : phase == 1 ? EventType::GestureUpdate
                                 : EventType::GestureEnd,
            .pos  = {fp1616(sw->event_x) / _scale, fp1616(sw->event_y) / _scale},
        };
        g.gesture = Gesture::Swipe;
        g.fingers = int(sw->detail); // touch count
        g.mods    = pointerMods(uint16_t(sw->mods.effective));
        g.dx      = fp1616(sw->delta_x) / _scale;
        g.dy      = fp1616(sw->delta_y) / _scale;
        g.cancelled =
            phase == 2 && (sw->flags & XCB_INPUT_GESTURE_SWIPE_EVENT_FLAGS_GESTURE_SWIPE_CANCELLED);
        w->emit(g);
        return;
    }
    default:
        return;
    }
}

} // namespace plat::x11

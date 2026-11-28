// XDND v5 (https://freedesktop.org/wiki/Specifications/XDND/), both ends.
// Target: our top-levels are XdndAware; DropEnter/Move carry the offered
// types and actions, Window::setDropAction answers through XdndStatus, and a
// drop fetches the standard types through the XdndSelection before Drop and
// XdndFinished. Source: startDrag owns the XdndSelection, grabs the pointer
// and walks the protocol against whatever window is under it — including our
// own windows, which answer through the server like anyone else's would.
#include "x11/x11_internal.h"

#include "core/drop.h"
#include "core/image_util.h"

#include <xcb/xfixes.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>

namespace plat::x11 {

namespace {

constexpr int kStatusTimeoutMs   = 1000; // release while a status is outstanding
constexpr int kFinishedTimeoutMs = 3000; // drop sent, no XdndFinished yet
constexpr int kMaxVersion        = 5;

void sendClientMessage(
    xcb_connection_t *c, xcb_window_t to, xcb_window_t window, xcb_atom_t type, const uint32_t d[5]
) {
    xcb_client_message_event_t ev{};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format        = 32;
    ev.window        = window;
    ev.type          = type;
    std::memcpy(ev.data.data32, d, 5 * sizeof(uint32_t));
    xcb_send_event(c, 0, to, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
}

uint32_t bitOf(DropAction a) {
    switch (a) {
    case DropAction::Copy:
        return ActCopy;
    case DropAction::Move:
        return ActMove;
    case DropAction::Link:
        return ActLink;
    default:
        return 0;
    }
}

std::vector<xcb_atom_t> readAtoms(xcb_connection_t *c, xcb_window_t w, xcb_atom_t prop) {
    Reply r(
        xcb_get_property_reply(c, xcb_get_property(c, 0, w, prop, XCB_ATOM_ATOM, 0, 4096), nullptr)
    );
    if (!r || r->format != 32)
        return {};
    auto *a = static_cast<xcb_atom_t *>(xcb_get_property_value(r.p));
    return {a, a + xcb_get_property_value_length(r.p) / 4};
}

} // namespace

void X11App::markDndAware(xcb_window_t w) {
    const uint32_t version = kMaxVersion;
    xcb_change_property(
        _c, XCB_PROP_MODE_REPLACE, w, _atoms[XdndAware], XCB_ATOM_ATOM, 32, 1, &version
    );
}

DropAction X11App::actionFromAtom(xcb_atom_t a) const {
    if (a == _atoms[XdndActionCopy])
        return DropAction::Copy;
    if (a == _atoms[XdndActionMove])
        return DropAction::Move;
    if (a == _atoms[XdndActionLink])
        return DropAction::Link;
    return DropAction::None; // Ask, Private, None, unknown
}

xcb_atom_t X11App::atomFromAction(DropAction a) const {
    switch (a) {
    case DropAction::Copy:
        return _atoms[XdndActionCopy];
    case DropAction::Move:
        return _atoms[XdndActionMove];
    case DropAction::Link:
        return _atoms[XdndActionLink];
    default:
        return XCB_ATOM_NONE;
    }
}

// ── target ──────────────────────────────────────────────────────────────────

void X11App::onXdnd(X11Window *w, xcb_client_message_event_t *e) {
    const uint32_t *d = e->data.data32;
    if (e->type == _atoms[XdndEnter]) {
        // A source that lost our Leave (crashed, or moved straight to another
        // of our windows) must not leave the old window thinking it is hovered.
        if (_dnd.entered && _dnd.target != w->xid())
            if (X11Window *old = findWindow(_dnd.target))
                old->emit({.type = EventType::DropLeave});
        _dnd         = {};
        _dnd.gen     = ++_dndGen;
        _dnd.source  = d[0];
        _dnd.target  = w->xid();
        _dnd.version = int(d[1] >> 24);
        if (d[1] & 1) { // more than three types: the full list is on the source
            _dnd.types = readAtoms(_c, _dnd.source, _atoms[XdndTypeList]);
        } else {
            for (int i = 2; i < 5; ++i)
                if (d[i])
                    _dnd.types.push_back(d[i]);
        }
        for (auto &name : atomNames(_dnd.types)) {
            std::string m = normaliseMime(name);
            if (!m.empty() &&
                std::find(_dnd.mimes.begin(), _dnd.mimes.end(), m) == _dnd.mimes.end())
                _dnd.mimes.push_back(std::move(m));
        }
        // The action list is fixed for the drag (sources set it before Enter).
        for (xcb_atom_t a : readAtoms(_c, _dnd.source, _atoms[XdndActionList]))
            _dnd.listActions |= bitOf(actionFromAtom(a));
        return;
    }
    if (d[0] != _dnd.source || w->xid() != _dnd.target)
        return;
    if (e->type == _atoms[XdndPosition]) {
        if (_dnd.dropping)
            return; // positions after a drop are a source bug
        // Root coordinates: the window knows where its content is on the root
        // (it follows every move), so no round trip per position.
        const int16_t rx = int16_t(d[2] >> 16), ry = int16_t(d[2] & 0xffff);
        _dnd.pos = w->toLogical(rx - w->rootX(), ry - w->rootY());
        if (_dnd.version >= 1 && d[3])
            _lastTime = d[3];
        // v2+ carries the requested action; before that it is always Copy.
        const DropAction requested = _dnd.version >= 2 ? actionFromAtom(d[4]) : DropAction::Copy;
        _dnd.allowed               = bitOf(requested) | _dnd.listActions;
        if (!_dnd.allowed)
            _dnd.allowed = ActCopy; // Ask/Private with no list: Copy is what every target does
        _dnd.preferred =
            requested != DropAction::None ? requested : core::preferredAction(_dnd.allowed);

        Event ev{
            .type = _dnd.entered ? EventType::DropMove : EventType::DropEnter, .pos = _dnd.pos
        };
        ev.dropAction     = _dnd.preferred;
        ev.allowedActions = _dnd.allowed;
        for (auto &m : _dnd.mimes)
            ev.items.push_back({m, {}});
        if (!_dnd.entered)
            w->dropReply = _dnd.preferred; // the default answer, until the app says otherwise
        _dnd.entered       = true;
        _dnd.inPosition    = true;
        const uint64_t gen = _dnd.gen;
        w->emit(ev);
        // The handler may have destroyed the window (forgetWindow resets _dnd).
        if (_dnd.gen != gen || !findWindow(_dnd.target))
            return;
        _dnd.inPosition = false;
        sendDndStatus(w);
    } else if (e->type == _atoms[XdndLeave]) {
        if (_dnd.entered)
            w->emit({.type = EventType::DropLeave});
        _dnd = {};
    } else if (e->type == _atoms[XdndDrop]) {
        if (_dnd.dropping)
            return;
        _dnd.dropTime = _dnd.version >= 1 ? d[2] : XCB_CURRENT_TIME;
        if (_dnd.mimes.empty() || w->dropReply == DropAction::None) {
            // Rejected but dropped anyway: no Drop, and tell the source no.
            if (_dnd.entered)
                w->emit({.type = EventType::DropLeave});
            dndFinishTarget(false, XCB_ATOM_NONE);
            return;
        }
        _dnd.dropping = true;
        auto has      = [&](xcb_atom_t a) {
            return std::find(_dnd.types.begin(), _dnd.types.end(), a) != _dnd.types.end();
        };
        // The standard types, in the order the contract lists them. Anything
        // else the source offers stays unfetched: some sources offer dozens
        // of (large) representations and fetching is synchronous for them.
        if (has(_atoms[MimeUriList]))
            _dnd.wants.push_back({"text/uri-list", {_atoms[MimeUriList]}});
        if (auto tt = textTargets(_dnd.types); !tt.empty())
            _dnd.wants.push_back({core::kTextMime, std::move(tt)});
        if (has(_atoms[MimeTextHtml]))
            _dnd.wants.push_back({"text/html", {_atoms[MimeTextHtml]}});
        if (has(_atoms[MimeImagePng]))
            _dnd.wants.push_back({"image/png", {_atoms[MimeImagePng]}});
        dndFetchNext();
    }
}

void X11App::dndFetchNext() {
    if (!_dnd.wants.empty()) {
        auto want = std::move(_dnd.wants.front());
        _dnd.wants.erase(_dnd.wants.begin());
        const uint64_t gen = _dnd.gen;
        fetch(
            _atoms[XdndSelection],
            std::move(want.second),
            _dnd.dropTime,
            [this, gen, mime = std::move(want.first)](std::optional<std::string> v, xcb_atom_t) {
                if (gen != _dnd.gen || !_dnd.dropping)
                    return; // drag abandoned (window destroyed) meanwhile
                if (v)
                    _dnd.got.push_back({mime, std::move(*v)});
                dndFetchNext();
            }
        );
        return;
    }
    X11Window *w = findWindow(_dnd.target);
    if (!w)
        return;
    if (_dnd.got.empty()) {
        // Nothing we could use came back (the source refused every type).
        w->emit({.type = EventType::DropLeave});
        dndFinishTarget(false, XCB_ATOM_NONE);
        return;
    }
    Event ev{.type = EventType::Drop, .pos = _dnd.pos};
    ev.dropAction     = _dnd.preferred;
    ev.allowedActions = _dnd.allowed;
    ev.items          = std::move(_dnd.got);
    core::fillDropText(ev);
    const xcb_atom_t action = atomFromAction(w->dropReply);
    const uint64_t   gen    = _dnd.gen;
    w->emit(ev);
    if (gen == _dnd.gen)
        dndFinishTarget(true, action);
}

void X11App::sendDndStatus(X11Window *w) {
    const bool     accept    = !_dnd.mimes.empty() && w->dropReply != DropAction::None;
    // Bit 1: keep sending positions everywhere (empty rectangle) — the
    // app may answer differently per position.
    const uint32_t status[5] = {
        w->xid(), (accept ? 1u : 0u) | 2u, 0, 0, accept ? atomFromAction(w->dropReply) : 0u
    };
    sendClientMessage(_c, _dnd.source, _dnd.source, _atoms[XdndStatus], status);
    xcb_flush(_c);
}

void X11App::dndReplyChanged(X11Window *w) {
    // Inside a DropEnter/Move handler the status goes out when it returns.
    if (_dnd.entered && !_dnd.inPosition && !_dnd.dropping && _dnd.target == w->xid())
        sendDndStatus(w);
}

void X11App::dndFinishTarget(bool accepted, xcb_atom_t action) {
    if (_dnd.source) {
        // v5: data[1] bit 0 = accepted, data[2] = the action performed.
        const uint32_t fin[5] = {_dnd.target, accepted ? 1u : 0u, accepted ? action : 0u, 0, 0};
        sendClientMessage(_c, _dnd.source, _dnd.source, _atoms[XdndFinished], fin);
        xcb_flush(_c);
    }
    _dnd = {};
}

void X11Window::setDropAction(DropAction a) {
    dropReply = a;
    _app->dndReplyChanged(this);
}

// ── source ──────────────────────────────────────────────────────────────────

bool X11App::startDrag(Window &source, const DragDesc &drag) {
    auto &src = static_cast<X11Window &>(source);
    // A drag needs the button that started it: without one the grab below
    // would "drag" until the next click.
    if (_drag.active || drag.items.empty() || !findWindow(src.xid()))
        return false;
    // The server's button state, not our bookkeeping: a WM move/resize
    // swallows releases we never see.
    Reply              p(xcb_query_pointer_reply(_c, xcb_query_pointer(_c, _root), nullptr));
    constexpr uint16_t kHeld = XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3;
    if (!p || !(p->mask & kHeld))
        return false;
    if (!own(SelXdnd, drag.items))
        return false;

    const uint32_t evMask =
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION;
    // Taking over the press's implicit grab (same client) always succeeds
    // unless someone else grabbed meanwhile.
    Reply g(xcb_grab_pointer_reply(
        _c,
        xcb_grab_pointer(
            _c,
            0,
            src.xid(),
            evMask,
            XCB_GRAB_MODE_ASYNC,
            XCB_GRAB_MODE_ASYNC,
            XCB_NONE,
            cursor(Cursor::Grabbing),
            XCB_CURRENT_TIME
        ),
        nullptr
    ));
    if (!g || g->status != XCB_GRAB_STATUS_SUCCESS) {
        disown(SelXdnd);
        return false;
    }
    // Escape cancels; without the keyboard grab it would go to whatever
    // has focus. Failing it (another client holds it) only loses that.
    Reply kg(xcb_grab_keyboard_reply(
        _c,
        xcb_grab_keyboard(
            _c, 0, src.xid(), XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC
        ),
        nullptr
    ));

    _drag         = {};
    _drag.active  = true;
    _drag.source  = &src;
    _drag.actions = drag.actions ? drag.actions : uint32_t(ActCopy);
    _drag.types   = offeredTypes(drag.items);
    _drag.time    = _lastTime;
    _drag.cursor  = Cursor::Grabbing;

    // Types beyond the three an XdndEnter carries, and the allowed actions
    // when there is a choice, live on the source window (our helper).
    if (_drag.types.size() > 3)
        xcb_change_property(
            _c,
            XCB_PROP_MODE_REPLACE,
            _helper,
            _atoms[XdndTypeList],
            XCB_ATOM_ATOM,
            32,
            uint32_t(_drag.types.size()),
            _drag.types.data()
        );
    else
        xcb_delete_property(_c, _helper, _atoms[XdndTypeList]);
    std::vector<xcb_atom_t> acts;
    for (DropAction a : {DropAction::Copy, DropAction::Move, DropAction::Link})
        if (_drag.actions & bitOf(a))
            acts.push_back(atomFromAction(a));
    if (acts.size() > 1)
        xcb_change_property(
            _c,
            XCB_PROP_MODE_REPLACE,
            _helper,
            _atoms[XdndActionList],
            XCB_ATOM_ATOM,
            32,
            uint32_t(acts.size()),
            acts.data()
        );
    else
        xcb_delete_property(_c, _helper, _atoms[XdndActionList]);

    _drag.rootX  = p->root_x;
    _drag.rootY  = p->root_y;
    _buttonsHeld = 0;
    for (int b = 1; b <= 3; ++b)
        if (p->mask & (XCB_BUTTON_MASK_1 << (b - 1)))
            _buttonsHeld |= 1u << b;
    createDragIcon(drag.image, drag.hotspot);
    // Enter whatever is under the pointer right away: a drag that is
    // released without moving still gets a target.
    dragMotion(_drag.rootX, _drag.rootY, _lastTime);
    dragTrack();
    xcb_flush(_c);
    return true;
}

void X11App::createDragIcon(const Image &srcImg, Point hotspot) {
    if (srcImg.empty() || srcImg.pixels.size() < size_t(srcImg.width) * size_t(srcImg.height))
        return;
    // The image is width/scale logical px wide, so width/scale*_scale
    // physical px on this screen; resample unless that is its own size.
    const double factor = _scale / (srcImg.scale > 0 ? srcImg.scale : 1.0);
    Image        scaled;
    const Image *imgp = &srcImg;
    const int    tw   = std::max(1, int(std::lround(srcImg.width * factor)));
    const int    th   = std::max(1, int(std::lround(srcImg.height * factor)));
    if ((tw != srcImg.width || th != srcImg.height) && tw <= 4096 && th <= 4096) {
        scaled = core::scaleImage(srcImg, tw, th); // cropped to 1024² below
        imgp   = &scaled;
    }
    const Image &img = *imgp;
    const int    w = std::min(img.width, 1024), h = std::min(img.height, 1024);
    if (_xfixes) {
        _drag.hotX = int(std::lround(hotspot.x * _scale));
        _drag.hotY = int(std::lround(hotspot.y * _scale));
    } else {
        // No input shape: keep the icon off the pointer's hot spot, or the
        // target search would find the icon itself.
        _drag.hotX = -12;
        _drag.hotY = -12;
    }
    // The image becomes the window's background, so the server repaints it
    // by itself on every move and exposure.
    const xcb_pixmap_t pix = xcb_generate_id(_c);
    xcb_create_pixmap(_c, _depth, pix, _root, uint16_t(w), uint16_t(h));
    const xcb_gcontext_t gc = xcb_generate_id(_c);
    xcb_create_gc(_c, gc, pix, 0, nullptr);
    const size_t          rowBytes = size_t(w) * 4;
    const int             rows     = int(std::max<size_t>(1, (maxRequestBytes() - 64) / rowBytes));
    std::vector<uint32_t> strip;
    for (int y = 0; y < h; y += rows) {
        const int n = std::min(rows, h - y);
        strip.resize(size_t(w) * n);
        for (int k = 0; k < n; ++k)
            std::memcpy(
                strip.data() + size_t(k) * w,
                img.pixels.data() + size_t(y + k) * img.width,
                rowBytes
            );
        xcb_put_image(
            _c,
            XCB_IMAGE_FORMAT_Z_PIXMAP,
            pix,
            gc,
            uint16_t(w),
            uint16_t(n),
            0,
            int16_t(y),
            0,
            _depth,
            uint32_t(rowBytes * n),
            reinterpret_cast<const uint8_t *>(strip.data())
        );
    }
    xcb_free_gc(_c, gc);

    _drag.icon              = xcb_generate_id(_c);
    const uint32_t values[] = {pix, 0, 1, _colormap};
    xcb_create_window(
        _c,
        _depth,
        _drag.icon,
        _root,
        int16_t(_drag.rootX - _drag.hotX),
        int16_t(_drag.rootY - _drag.hotY),
        uint16_t(w),
        uint16_t(h),
        0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT,
        _visual,
        XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_COLORMAP,
        values
    );
    xcb_free_pixmap(_c, pix); // the window holds its own reference
    // Compositors give DND windows the right treatment (no shadow, no animation).
    const xcb_atom_t type = intern("_NET_WM_WINDOW_TYPE_DND");
    xcb_change_property(
        _c, XCB_PROP_MODE_REPLACE, _drag.icon, _atoms[NetWmWindowType], XCB_ATOM_ATOM, 32, 1, &type
    );
    if (_xfixes) {
        // Empty input shape: the pointer (and TranslateCoordinates, which the
        // target search uses) sees straight through the icon.
        const xcb_xfixes_region_t region = xcb_generate_id(_c);
        xcb_xfixes_create_region(_c, region, 0, nullptr);
        xcb_xfixes_set_window_shape_region(_c, _drag.icon, XCB_SHAPE_SK_INPUT, 0, 0, region);
        xcb_xfixes_destroy_region(_c, region);
    }
    xcb_map_window(_c, _drag.icon);
}

X11App::DndProbe X11App::probeDnd(xcb_window_t w) {
    if (auto it = _drag.probes.find(w); it != _drag.probes.end())
        return it->second;
    // XdndProxy and XdndAware asked together; the proxy's own pair likewise.
    auto ask = [this](xcb_window_t on) {
        return std::pair{
            xcb_get_property(_c, 0, on, _atoms[XdndProxy], XCB_ATOM_WINDOW, 0, 1),
            xcb_get_property(_c, 0, on, _atoms[XdndAware], XCB_ATOM_ATOM, 0, 1)
        };
    };
    auto value = [this](xcb_get_property_cookie_t c) -> std::optional<uint32_t> {
        Reply r(xcb_get_property_reply(_c, c, nullptr));
        if (!r || r->format != 32 || xcb_get_property_value_length(r.p) < 4)
            return std::nullopt;
        return *static_cast<uint32_t *>(xcb_get_property_value(r.p));
    };
    auto [proxyCookie, awareCookie] = ask(w);
    xcb_window_t            via     = value(proxyCookie).value_or(0);
    std::optional<uint32_t> aware   = value(awareCookie);
    if (via) {
        // The proxy must point at itself, or it is stale.
        auto [viaProxy, viaAware]     = ask(via);
        const bool              valid = value(viaProxy).value_or(0) == via;
        std::optional<uint32_t> a     = value(viaAware);
        if (valid)
            aware = a;
        else
            via = 0;
    }
    DndProbe p;
    p.aware         = aware.has_value();
    p.version       = aware ? int(*aware) : 0;
    p.proxy         = via ? via : w;
    _drag.probes[w] = p;
    return p;
}

xcb_window_t X11App::findDndTarget(int16_t x, int16_t y, xcb_window_t *proxy, int *version) {
    // Walk down from the root through the window under the point until one
    // is XdndAware (a WM frame is not; the client inside it is), honouring
    // XdndProxy. What each window says is asked once per drag (probeDnd).
    xcb_window_t w = _root;
    for (int depth = 0; depth < 32; ++depth) {
        Reply t(xcb_translate_coordinates_reply(
            _c, xcb_translate_coordinates(_c, _root, w, x, y), nullptr
        ));
        if (!t || !t->child)
            return 0;
        w = t->child;
        if (w == _drag.icon)
            return 0; // no input shape support and the icon got under the pointer anyway
        const DndProbe p = probeDnd(w);
        if (p.aware) {
            if (p.version < 3)
                return 0; // pre-v3 targets speak a different protocol
            *proxy   = p.proxy;
            *version = std::min(p.version, kMaxVersion);
            return w;
        }
    }
    return 0;
}

void X11App::sendXdnd(xcb_window_t to, xcb_window_t window, xcb_atom_t type, const uint32_t d[4]) {
    const uint32_t data[5] = {_helper, d[0], d[1], d[2], d[3]};
    sendClientMessage(_c, to, window, type, data);
}

xcb_atom_t X11App::dragRequestedAction() const {
    const uint32_t m = _kbd.hasKeymap() ? _kbd.mods() : 0;
    return atomFromAction(
        core::modifierDropAction((m & ModShift) != 0, (m & ModCtrl) != 0, _drag.actions)
    );
}

void X11App::dragMotion(int16_t rootX, int16_t rootY, xcb_timestamp_t t) {
    _drag.rootX = rootX;
    _drag.rootY = rootY;
    if (t)
        _drag.time = t;
    if (_drag.icon) {
        const uint32_t xy[] = {uint32_t(rootX - _drag.hotX), uint32_t(rootY - _drag.hotY)};
        xcb_configure_window(_c, _drag.icon, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, xy);
    }
    // The target search waits for the end of this batch of events (or for
    // a release), so a burst of motion costs one search.
    _drag.moved = true;
}

void X11App::dragTrack() {
    if (!_drag.moved)
        return;
    _drag.moved = false;
    if (_drag.released)
        return;
    const int16_t      rootX = _drag.rootX, rootY = _drag.rootY;
    xcb_window_t       proxy   = 0;
    int                version = 0;
    const xcb_window_t target  = findDndTarget(rootX, rootY, &proxy, &version);
    if (target != _drag.target) {
        dragLeaveTarget();
        if (target) {
            _drag.target            = target;
            _drag.proxy             = proxy;
            _drag.version           = version;
            const uint32_t enter[4] = {
                (uint32_t(version) << 24) | (_drag.types.size() > 3 ? 1u : 0u),
                _drag.types.size() > 0 ? _drag.types[0] : 0u,
                _drag.types.size() > 1 ? _drag.types[1] : 0u,
                _drag.types.size() > 2 ? _drag.types[2] : 0u,
            };
            sendXdnd(proxy, target, _atoms[XdndEnter], enter);
        }
    }
    if (!_drag.target) {
        dragSetCursor(Cursor::NotAllowed);
        xcb_flush(_c);
        return;
    }
    // Inside the target's "no more positions" rectangle nothing changes.
    const Rect &r = _drag.rect;
    if (_drag.haveRect && rootX >= r.x && rootY >= r.y && rootX < r.x + r.w && rootY < r.y + r.h &&
        _drag.sentAction == dragRequestedAction()) {
        xcb_flush(_c);
        return;
    }
    // One XdndPosition in flight at a time; the latest motion goes when the
    // status for the previous one arrives.
    if (_drag.waitingStatus)
        _drag.pending = true;
    else
        dragSendPosition();
    xcb_flush(_c);
}

void X11App::dragSendPosition() {
    _drag.sentAction      = dragRequestedAction();
    const uint32_t pos[4] = {
        0,
        (uint32_t(uint16_t(_drag.rootX)) << 16) | uint16_t(_drag.rootY),
        _drag.time,
        _drag.version >= 2 ? _drag.sentAction : 0u,
    };
    sendXdnd(_drag.proxy, _drag.target, _atoms[XdndPosition], pos);
    _drag.waitingStatus = true;
    _drag.pending       = false;
    xcb_flush(_c);
}

void X11App::dragLeaveTarget() {
    if (_drag.target && !_drag.dropSent) {
        const uint32_t leave[4] = {0, 0, 0, 0};
        sendXdnd(_drag.proxy, _drag.target, _atoms[XdndLeave], leave);
    }
    _drag.target = _drag.proxy = 0;
    _drag.version              = 0;
    _drag.waitingStatus = _drag.pending = _drag.accepted = _drag.haveRect = false;
    _drag.action                                                          = 0;
}

void X11App::dragSetCursor(Cursor c) {
    if (_drag.cursor == c || _drag.released)
        return;
    _drag.cursor = c;
    xcb_change_active_pointer_grab(
        _c,
        cursor(c),
        XCB_CURRENT_TIME,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION
    );
}

void X11App::dragArmTimeout(int ms) {
    if (_drag.timer)
        cancelTimer(_drag.timer);
    _drag.timer = addTimer(ms, false, [this] {
        _drag.timer = 0;
        if (!_drag.active)
            return;
        // Target went silent: an outstanding status means we never dropped.
        if (!_drag.dropSent)
            dragLeaveTarget();
        dragFinish(DropAction::None);
    });
}

void X11App::dragOnStatus(const xcb_client_message_event_t *e) {
    const uint32_t *d = e->data.data32;
    if (!_drag.active || !_drag.target || d[0] != _drag.target || _drag.dropSent)
        return;
    _drag.waitingStatus = false;
    _drag.accepted      = d[1] & 1;
    _drag.haveRect      = !(d[1] & 2);
    _drag.rect          = {
        int(int16_t(d[2] >> 16)), int(int16_t(d[2] & 0xffff)), int(d[3] >> 16), int(d[3] & 0xffff)
    };
    _drag.action = _drag.accepted ? (_drag.version >= 2 ? d[4] : _atoms[XdndActionCopy]) : 0;
    dragSetCursor(_drag.accepted ? Cursor::Grabbing : Cursor::NotAllowed);
    if (_drag.pending) {
        dragSendPosition(); // the pointer moved meanwhile: that answer is stale
        return;
    }
    if (_drag.released) {
        if (_drag.accepted) {
            dragSendDrop();
        } else {
            dragLeaveTarget();
            dragFinish(DropAction::None);
        }
    }
    xcb_flush(_c);
}

void X11App::dragRelease(xcb_timestamp_t t) {
    dragTrack(); // the drop goes where the pointer is now
    _drag.released = true;
    if (t)
        _drag.time = t;
    // Give the pointer and keyboard back at once, even if the target is
    // slow to finish.
    xcb_ungrab_pointer(_c, XCB_CURRENT_TIME);
    xcb_ungrab_keyboard(_c, XCB_CURRENT_TIME);
    if (_drag.icon) {
        xcb_destroy_window(_c, _drag.icon);
        _drag.icon = 0;
    }
    if (!_drag.target) {
        dragFinish(DropAction::None);
    } else if (_drag.waitingStatus || _drag.pending) {
        dragArmTimeout(kStatusTimeoutMs); // dragOnStatus drops (or not)
    } else if (_drag.accepted) {
        dragSendDrop();
    } else {
        dragLeaveTarget();
        dragFinish(DropAction::None);
    }
    xcb_flush(_c);
}

void X11App::dragSendDrop() {
    const uint32_t drop[4] = {0, _drag.time, 0, 0};
    sendXdnd(_drag.proxy, _drag.target, _atoms[XdndDrop], drop);
    _drag.dropSent = true;
    dragArmTimeout(kFinishedTimeoutMs);
    xcb_flush(_c);
}

void X11App::dragOnFinished(const xcb_client_message_event_t *e) {
    const uint32_t *d = e->data.data32;
    if (!_drag.active || !_drag.dropSent || d[0] != _drag.target)
        return;
    // v5 says whether it worked and what was done; before, the last status did.
    const bool       ok     = _drag.version >= 5 ? (d[1] & 1) != 0 : _drag.accepted;
    const xcb_atom_t action = _drag.version >= 5 ? d[2] : _drag.action;
    DropAction       result = ok ? actionFromAtom(action) : DropAction::None;
    if (ok && result == DropAction::None)
        result = DropAction::Copy; // accepted with Private/unknown: data was taken
    dragFinish(result);
}

void X11App::dragKey(xcb_key_press_event_t *e, bool down) {
    _keyDown[e->detail] = down;
    // keyFor, not key(): no compose/dead-key state may change under a grab.
    const Key k         = _kbd.hasKeymap() ? _kbd.keyFor(e->detail)
                                           : linux_input::keyFromEvdev(uint32_t(e->detail) - 8);
    if (down && k == Key::Escape) {
        if (!_drag.released) {
            dragLeaveTarget();
            xcb_ungrab_pointer(_c, XCB_CURRENT_TIME);
            xcb_ungrab_keyboard(_c, XCB_CURRENT_TIME);
        }
        dragFinish(DropAction::None);
    }
    // Modifier changes re-send the position (new action) via the XKB state
    // notify, which arrives after the key.
}

void X11App::dragFinish(DropAction result) {
    if (!_drag.active)
        return;
    if (_drag.timer)
        cancelTimer(_drag.timer);
    if (!_drag.released) {
        xcb_ungrab_pointer(_c, XCB_CURRENT_TIME);
        xcb_ungrab_keyboard(_c, XCB_CURRENT_TIME);
    }
    if (_drag.icon)
        xcb_destroy_window(_c, _drag.icon);
    X11Window *src = _drag.source;
    _drag          = {};
    disown(SelXdnd);
    xcb_delete_property(_c, _helper, _atoms[XdndTypeList]);
    xcb_delete_property(_c, _helper, _atoms[XdndActionList]);
    xcb_flush(_c);
    if (src && findWindow(src->xid()))
        src->emit({.type = EventType::DragFinished, .dropAction = result});
}

} // namespace plat::x11

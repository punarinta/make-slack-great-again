// X11 selections: CLIPBOARD and PRIMARY (ICCCM, multi-type, INCR both ways)
// and the XdndSelection a drag offers. A hidden InputOnly helper window owns
// every selection we hold and is the requestor of every conversion we make,
// so none of it depends on any app window existing. XDND messaging itself
// lives in x11_dnd.cpp.
#include "x11/x11_internal.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace plat::x11 {

namespace {

// Selections bigger than this go out INCR. The ICCCM threshold is the
// maximum request size, but 256 KiB keeps each request (and the time the
// requestor's event queue is blocked) small — the same choice GTK makes.
constexpr size_t kIncrChunk         = 256 * 1024;
constexpr int    kTransferTimeoutMs = 2000;
constexpr auto   kIncrSendStale     = std::chrono::seconds(10);

// Reads (and deletes) a whole property; nullopt when it is missing.
std::optional<std::string>
readProperty(xcb_connection_t *c, xcb_window_t w, xcb_atom_t prop, xcb_atom_t *type) {
    Reply r(xcb_get_property_reply(
        c, xcb_get_property(c, 1, w, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x1fffffff), nullptr
    ));
    if (!r || r->type == XCB_ATOM_NONE)
        return std::nullopt;
    if (type)
        *type = r->type;
    const int n = xcb_get_property_value_length(r.p);
    return std::string(static_cast<const char *>(xcb_get_property_value(r.p)), size_t(n));
}

} // namespace

std::string normaliseMime(std::string_view name) {
    // Selection bookkeeping targets, not data; and COMPOUND_TEXT, an ISO
    // 2022 text encoding nobody can use that GTK offers next to UTF8_STRING.
    if (name.empty() || name == "TARGETS" || name == "TIMESTAMP" || name == "MULTIPLE" ||
        name == "SAVE_TARGETS" || name == "COMPOUND_TEXT")
        return {};
    if (isTextMime(name))
        return core::kTextMime;
    return std::string(name);
}

void X11App::initSelection() {
    _helper                 = xcb_generate_id(_c);
    const uint32_t values[] = {1, XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_create_window(
        _c,
        0,
        _helper,
        _root,
        -10,
        -10,
        1,
        1,
        0,
        XCB_WINDOW_CLASS_INPUT_ONLY,
        XCB_COPY_FROM_PARENT,
        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
        values
    );
}

xcb_atom_t X11App::selAtom(int idx) const {
    switch (idx) {
    case SelClipboard:
        return _atoms[Clipboard];
    case SelPrimary:
        return XCB_ATOM_PRIMARY;
    case SelXdnd:
        return _atoms[XdndSelection];
    default:
        return XCB_ATOM_NONE;
    }
}

int X11App::selIndex(xcb_atom_t selection) const {
    for (int i = 0; i < SelCount; ++i)
        if (selAtom(i) == selection)
            return i;
    return -1;
}

bool X11App::isOurWindow(xcb_window_t w) const {
    return w == _helper || findWindow(w) != nullptr;
}

std::vector<std::string> X11App::atomNames(const std::vector<xcb_atom_t> &atoms) {
    std::vector<xcb_get_atom_name_cookie_t> cookies(atoms.size());
    for (size_t i = 0; i < atoms.size(); ++i)
        if (atoms[i] && !_atomNameCache.count(atoms[i]))
            cookies[i] = xcb_get_atom_name(_c, atoms[i]);
    std::vector<std::string> out(atoms.size());
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (!atoms[i])
            continue;
        if (auto it = _atomNameCache.find(atoms[i]); it != _atomNameCache.end()) {
            out[i] = it->second;
            continue;
        }
        Reply r(xcb_get_atom_name_reply(_c, cookies[i], nullptr));
        if (r)
            out[i].assign(xcb_get_atom_name_name(r.p), size_t(xcb_get_atom_name_name_length(r.p)));
        _atomNameCache[atoms[i]] = out[i];
    }
    return out;
}

// ── owning ──────────────────────────────────────────────────────────────────

bool X11App::own(int idx, std::vector<DataItem> items) {
    Owned &o = _owned[idx];
    o.items  = std::move(items); // a new selection replaces every offered type
    o.time   = _lastTime ? _lastTime : serverTime();
    xcb_set_selection_owner(_c, _helper, selAtom(idx), o.time);
    // Ownership can fail (an older timestamp than the current owner's).
    Reply r(xcb_get_selection_owner_reply(_c, xcb_get_selection_owner(_c, selAtom(idx)), nullptr));
    o.own = r && r->owner == _helper;
    if (!o.own)
        o.items.clear();
    return o.own;
}

void X11App::disown(int idx) {
    Owned &o = _owned[idx];
    if (o.own)
        xcb_set_selection_owner(_c, XCB_NONE, selAtom(idx), _lastTime ? _lastTime : o.time);
    o = {};
}

void X11App::setClipboard(std::vector<DataItem> items, Selection sel) {
    const int idx = sel == Selection::Primary ? SelPrimary : SelClipboard;
    if (items.empty())
        disown(idx);
    else
        own(idx, std::move(items));
    xcb_flush(_c);
}

std::vector<xcb_atom_t> X11App::offeredTypes(const std::vector<DataItem> &items) {
    std::vector<xcb_atom_t> t;
    bool                    text = false;
    for (auto &i : items) {
        text |= isTextMime(i.mime);
        t.push_back(intern(i.mime));
    }
    if (text)
        for (xcb_atom_t a :
             {_atoms[Utf8String],
              _atoms[MimeTextUtf8],
              _atoms[MimeTextPlain],
              _atoms[Text],
              xcb_atom_t(XCB_ATOM_STRING)})
            t.push_back(a);
    // Deduplicate, keeping the caller's order: the first three are what an
    // XdndEnter carries inline.
    std::vector<xcb_atom_t> out;
    for (xcb_atom_t a : t)
        if (a && std::find(out.begin(), out.end(), a) == out.end())
            out.push_back(a);
    return out;
}

std::vector<xcb_atom_t> X11App::textTargets(const std::vector<xcb_atom_t> &offered) const {
    std::vector<xcb_atom_t> out;
    for (xcb_atom_t a :
         {_atoms[Utf8String],
          _atoms[MimeTextUtf8],
          _atoms[MimeTextPlain],
          xcb_atom_t(XCB_ATOM_STRING)})
        if (std::find(offered.begin(), offered.end(), a) != offered.end())
            out.push_back(a);
    return out;
}

std::optional<std::string_view> X11App::dataFor(
    const std::vector<DataItem> &items, xcb_atom_t target, xcb_atom_t *type, std::string *latin1
) {
    for (auto &i : items)
        if (intern(i.mime) == target) {
            *type = target;
            return i.data;
        }
    const std::string *text = nullptr;
    for (auto &i : items)
        if (!text && isTextMime(i.mime))
            text = &i.data;
    if (!text)
        return std::nullopt;
    if (target == _atoms[Utf8String] || target == _atoms[MimeTextUtf8] ||
        target == _atoms[MimeTextPlain]) {
        *type = target;
        return *text;
    }
    if (target == _atoms[Text]) { // "any encoding": answer with the type we used
        *type = _atoms[Utf8String];
        return *text;
    }
    if (target == XCB_ATOM_STRING) { // ICCCM STRING is Latin-1
        *type   = XCB_ATOM_STRING;
        *latin1 = utf8ToLatin1(*text);
        return *latin1;
    }
    return std::nullopt;
}

void X11App::onSelectionRequest(xcb_selection_request_event_t *e) {
    xcb_selection_notify_event_t n{};
    n.response_type       = XCB_SELECTION_NOTIFY;
    n.time                = e->time;
    n.requestor           = e->requestor;
    n.selection           = e->selection;
    n.target              = e->target;
    n.property            = XCB_ATOM_NONE; // refusal unless we fill it below
    // Obsolete clients pass None: use the target as the property (ICCCM).
    const xcb_atom_t prop = e->property ? e->property : e->target;

    const int  idx   = selIndex(e->selection);
    const bool owned = idx >= 0 && _owned[idx].own && e->owner == _helper &&
                       (e->time == XCB_CURRENT_TIME || e->time >= _owned[idx].time);
    if (owned) {
        const auto &items = _owned[idx].items;
        xcb_atom_t  type  = 0;
        std::string latin1; // the STRING conversion, when that is the target
        if (e->target == _atoms[Targets]) {
            std::vector<xcb_atom_t> t = {_atoms[Targets], _atoms[Timestamp]};
            for (xcb_atom_t a : offeredTypes(items))
                t.push_back(a);
            xcb_change_property(
                _c,
                XCB_PROP_MODE_REPLACE,
                e->requestor,
                prop,
                XCB_ATOM_ATOM,
                32,
                uint32_t(t.size()),
                t.data()
            );
            n.property = prop;
        } else if (e->target == _atoms[Timestamp]) {
            xcb_change_property(
                _c,
                XCB_PROP_MODE_REPLACE,
                e->requestor,
                prop,
                XCB_ATOM_INTEGER,
                32,
                1,
                &_owned[idx].time
            );
            n.property = prop;
        } else if (auto data = dataFor(items, e->target, &type, &latin1)) {
            if (data->size() > kIncrChunk) {
                // INCR: announce the size, then feed a chunk every time the
                // requestor deletes the property. Watching its property
                // changes only affects our own event mask on that window —
                // unless it is one of ours (a drop or paste from ourselves),
                // which already watch property changes with a mask we must
                // not clobber.
                if (!isOurWindow(e->requestor)) {
                    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
                    xcb_change_window_attributes(_c, e->requestor, XCB_CW_EVENT_MASK, &mask);
                }
                const uint32_t size = uint32_t(data->size());
                xcb_change_property(
                    _c, XCB_PROP_MODE_REPLACE, e->requestor, prop, _atoms[Incr], 32, 1, &size
                );
                // Our own copy: the selection may change before the last chunk.
                _incrSends.push_back(
                    {e->requestor, prop, type, std::string(*data), 0, core::Clock::now()}
                );
            } else {
                xcb_change_property(
                    _c,
                    XCB_PROP_MODE_REPLACE,
                    e->requestor,
                    prop,
                    type,
                    8,
                    uint32_t(data->size()),
                    data->data()
                );
            }
            n.property = prop;
        }
        // MULTIPLE is not supported (nor advertised): refused like any unknown target.
    }
    xcb_send_event(
        _c, 0, e->requestor, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&n)
    );
    xcb_flush(_c);
}

void X11App::onSelectionClear(xcb_selection_clear_event_t *e) {
    const int idx = selIndex(e->selection);
    if (idx >= 0 && e->owner == _helper)
        _owned[idx] = {};
}

bool X11App::onSelectionProperty(xcb_property_notify_event_t *e) {
    if (e->window == _helper && e->atom == _atoms[PlatSelection] &&
        e->state == XCB_PROPERTY_NEW_VALUE && !_transfers.empty() && _transfers.front().incr) {
        // INCR receive: each new value is one chunk; a zero-length one ends it.
        auto      &req  = _transfers.front();
        xcb_atom_t type = 0;
        auto       part = readProperty(_c, _helper, _atoms[PlatSelection], &type);
        if (!part || part->empty()) {
            finishTransfer(std::move(req.data));
        } else {
            req.data += *part;
            armTransferTimeout();
        }
        return true;
    }
    // INCR send: the requestor deleted the property, it wants the next chunk.
    // The requestor may be our own helper (pasting from ourselves).
    const auto now = core::Clock::now();
    for (auto it = _incrSends.begin(); it != _incrSends.end();) {
        if (now - it->since > kIncrSendStale) { // requestor went away mid-transfer
            it = _incrSends.erase(it);
            continue;
        }
        if (it->requestor == e->window && it->property == e->atom &&
            e->state == XCB_PROPERTY_DELETE) {
            const size_t n = std::min(kIncrChunk, it->data.size() - it->offset);
            xcb_change_property(
                _c,
                XCB_PROP_MODE_REPLACE,
                it->requestor,
                it->property,
                it->type,
                8,
                uint32_t(n),
                it->data.data() + it->offset
            );
            it->offset += n;
            it->since = now;
            if (n == 0) { // the zero-length write just sent ends the transfer
                if (!isOurWindow(it->requestor)) {
                    const uint32_t none = 0;
                    xcb_change_window_attributes(_c, it->requestor, XCB_CW_EVENT_MASK, &none);
                }
                _incrSends.erase(it);
            }
            xcb_flush(_c);
            return true;
        }
        ++it;
    }
    // Our own bookkeeping (PLAT_TIME, deletes) is never a window's business.
    return e->window == _helper;
}

// ── reading ─────────────────────────────────────────────────────────────────

void X11App::requestClipboard(
    std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
) {
    // Always through the server, even when we own it: it is asynchronous
    // anyway, and it keeps one code path (and proves what other clients see).
    std::vector<xcb_atom_t> targets;
    if (isTextMime(mime))
        targets = {_atoms[Utf8String], _atoms[MimeTextUtf8], XCB_ATOM_STRING};
    else
        targets = {intern(std::string(mime))};
    fetch(
        sel == Selection::Primary ? xcb_atom_t(XCB_ATOM_PRIMARY) : _atoms[Clipboard],
        std::move(targets),
        0,
        [cb = std::move(cb)](std::optional<std::string> v, xcb_atom_t) { cb(std::move(v)); }
    );
}

void X11App::requestClipboardMimes(
    std::function<void(std::vector<std::string>)> cb, Selection sel
) {
    fetch(
        sel == Selection::Primary ? xcb_atom_t(XCB_ATOM_PRIMARY) : _atoms[Clipboard],
        {_atoms[Targets]},
        0,
        [this, cb = std::move(cb)](std::optional<std::string> v, xcb_atom_t) {
            std::vector<xcb_atom_t> atoms;
            if (v)
                for (size_t i = 0; i + 4 <= v->size(); i += 4) {
                    xcb_atom_t a;
                    std::memcpy(&a, v->data() + i, 4);
                    atoms.push_back(a);
                }
            std::vector<std::string> mimes;
            for (auto &name : atomNames(atoms)) {
                std::string m = normaliseMime(name);
                if (!m.empty() && std::find(mimes.begin(), mimes.end(), m) == mimes.end())
                    mimes.push_back(std::move(m));
            }
            cb(std::move(mimes));
        }
    );
}

void X11App::fetch(
    xcb_atom_t                                                  selection,
    std::vector<xcb_atom_t>                                     targets,
    xcb_timestamp_t                                             time,
    std::function<void(std::optional<std::string>, xcb_atom_t)> done
) {
    Transfer t;
    t.selection = selection;
    t.time      = time;
    t.targets   = std::move(targets);
    t.done      = std::move(done);
    _transfers.push_back(std::move(t));
    if (_transfers.size() == 1)
        startTransfer();
}

void X11App::startTransfer() {
    auto &t = _transfers.front();
    if (t.targets.empty()) {
        // Every target refused. Deferred so a callback never runs inside the
        // request that queued it.
        post([this] {
            if (!_transfers.empty() && _transfers.front().targets.empty() &&
                !_transfers.front().target)
                finishTransfer(std::nullopt);
        });
        t.target = 0;
        return;
    }
    t.target = t.targets.front();
    t.targets.erase(t.targets.begin());
    t.incr = false;
    t.data.clear();
    xcb_delete_property(_c, _helper, _atoms[PlatSelection]);
    xcb_convert_selection(
        _c,
        _helper,
        t.selection,
        t.target,
        _atoms[PlatSelection],
        t.time ? t.time : (_lastTime ? _lastTime : XCB_CURRENT_TIME)
    );
    xcb_flush(_c);
    armTransferTimeout();
}

void X11App::armTransferTimeout() {
    if (_transferTimer)
        cancelTimer(_transferTimer);
    _transferTimer = addTimer(kTransferTimeoutMs, false, [this] {
        _transferTimer = 0;
        if (!_transfers.empty())
            finishTransfer(std::nullopt); // owner never answered
    });
}

void X11App::finishTransfer(std::optional<std::string> v) {
    if (_transferTimer) {
        cancelTimer(_transferTimer);
        _transferTimer = 0;
    }
    Transfer t = std::move(_transfers.front());
    _transfers.pop_front();
    if (v && t.target == XCB_ATOM_STRING)
        v = latin1ToUtf8(*v);
    if (!_transfers.empty())
        startTransfer();
    t.done(std::move(v), t.target);
}

void X11App::onSelectionNotify(xcb_selection_notify_event_t *e) {
    if (e->requestor != _helper || _transfers.empty())
        return;
    auto &t = _transfers.front();
    if (e->selection != t.selection || e->target != t.target || !t.target)
        return; // a stale answer to a target we already gave up on
    if (e->property == XCB_ATOM_NONE) {
        startTransfer(); // owner refused this target: try the next one
        return;
    }
    xcb_atom_t type = 0;
    auto       v    = readProperty(_c, _helper, e->property, &type);
    if (v && type == _atoms[Incr]) {
        // Deleting the property (readProperty did) tells the owner to start.
        t.incr = true;
        armTransferTimeout();
        return;
    }
    finishTransfer(std::move(v));
}

} // namespace plat::x11

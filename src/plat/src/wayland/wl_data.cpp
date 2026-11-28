// Wayland clipboard, primary selection and drag and drop. The data lives in
// the other client: every transfer is a pipe the peer writes into (or we
// write into), serviced by the loop's fd watches so a slow or stuck peer
// never blocks us — and so a transfer between two of our own windows (the
// compositor routes our own offer back to us) works on one thread.
#include "wayland/wl_internal.h"
#include "core/drop.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace plat::wl {

namespace {

// Text MIME types other toolkits offer or ask for; UTF-8 first.
const char *const kTextMimes[] = {core::kTextMime, "UTF8_STRING", "text/plain", "TEXT", "STRING"};

constexpr const char *kText        = core::kTextMime;
constexpr const char *kUriList     = "text/uri-list";
constexpr const char *kHtml        = "text/html";
constexpr const char *kPng         = "image/png";
constexpr int         kClipTimeout = 5000;  // ms; a peer that never closes the pipe
constexpr int         kDropTimeout = 15000; // file managers can be slow to list many URIs
constexpr size_t      kMaxTransfer = size_t(256) << 20;

constexpr uint32_t kWlCopy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
constexpr uint32_t kWlMove = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;

// Every text alias other toolkits (and XWayland) use means UTF-8 text here.
std::string normalise(std::string_view m) {
    return isTextMime(m) ? std::string(kText) : std::string(m);
}

std::vector<std::string> normalised(const std::vector<std::string> &mimes) {
    std::vector<std::string> out;
    for (const auto &m : mimes) {
        std::string n = normalise(m);
        if (std::find(out.begin(), out.end(), n) == out.end())
            out.push_back(std::move(n));
    }
    return out;
}

const DataItem *itemFor(const std::vector<DataItem> &items, std::string_view mime) {
    for (const auto &i : items)
        if (i.mime == mime)
            return &i;
    if (isTextMime(mime))
        for (const auto &i : items)
            if (isTextMime(i.mime))
                return &i;
    return nullptr;
}

// The offered name to ask for when the app wants `want`: itself, or for text
// the first alias the peer offers (UTF-8 names first).
std::string pickMime(const std::vector<std::string> &offered, std::string_view want) {
    if (std::find(offered.begin(), offered.end(), want) != offered.end())
        return std::string(want);
    if (isTextMime(want)) {
        for (const char *m : kTextMimes)
            if (std::find(offered.begin(), offered.end(), m) != offered.end())
                return m;
        for (const std::string &m : offered) // e.g. "text/plain; charset=UTF-8"
            if (isTextMime(m))
                return m;
    }
    return {};
}

// Offers every item under its own name; the text item also under each alias.
template <class Fn>
void offerItems(const std::vector<DataItem> &items, Fn offer) {
    std::vector<std::string> done;
    auto                     once = [&](const std::string &m) {
        if (std::find(done.begin(), done.end(), m) == done.end()) {
            done.push_back(m);
            offer(m.c_str());
        }
    };
    for (const auto &i : items) {
        if (isTextMime(i.mime)) {
            for (const char *m : kTextMimes)
                once(m);
        } else {
            once(i.mime);
        }
    }
}

uint32_t toWlActions(uint32_t acts) {
    uint32_t wl = 0;
    if (acts & ActCopy)
        wl |= kWlCopy;
    if (acts & ActMove)
        wl |= kWlMove;
    return wl; // Wayland has no link action (ask is a different thing)
}

uint32_t fromWlActions(uint32_t wl) {
    return ((wl & kWlCopy) ? uint32_t(ActCopy) : 0u) | ((wl & kWlMove) ? uint32_t(ActMove) : 0u);
}

DropAction fromWlAction(uint32_t wl) {
    return (wl & kWlCopy) ? DropAction::Copy : (wl & kWlMove) ? DropAction::Move : DropAction::None;
}

uint32_t toWlAction(DropAction a) {
    return a == DropAction::Copy ? kWlCopy : a == DropAction::Move ? kWlMove : 0;
}

WlApp *appOf(void *d) {
    return static_cast<WlApp *>(d);
}

const wl_data_offer_listener kOfferListener = {
    .offer =
        [](void *d, wl_data_offer *o, const char *mime) {
            if (auto *offer = appOf(d)->offerFor(o))
                offer->mimes.emplace_back(mime);
        },
    .source_actions =
        [](void *d, wl_data_offer *o, uint32_t actions) {
            if (auto *offer = appOf(d)->offerFor(o))
                offer->sourceActions = actions;
        },
    .action =
        [](void *d, wl_data_offer *o, uint32_t action) {
            if (auto *offer = appOf(d)->offerFor(o))
                offer->action = action;
        },
};

const wl_data_device_listener kDataDeviceListener = {
    .data_offer = [](void *d, wl_data_device *, wl_data_offer *o) { appOf(d)->onDataOffer(o); },
    .enter =
        [](void *d,
           wl_data_device *,
           uint32_t       serial,
           wl_surface    *s,
           wl_fixed_t     x,
           wl_fixed_t     y,
           wl_data_offer *o) {
            appOf(d)->onDragEnter(serial, s, wl_fixed_to_double(x), wl_fixed_to_double(y), o);
        },
    .leave     = [](void *d, wl_data_device *) { appOf(d)->onDragLeave(); },
    .motion    = [](
                     void *d, wl_data_device *, uint32_t, wl_fixed_t x, wl_fixed_t y
                 ) { appOf(d)->onDragMotion(wl_fixed_to_double(x), wl_fixed_to_double(y)); },
    .drop      = [](void *d, wl_data_device *) { appOf(d)->onDrop(); },
    .selection = [](void *d, wl_data_device *, wl_data_offer *o) { appOf(d)->onSelection(o); },
};

// One listener for the clipboard source and the drag source; WlApp tells
// them apart by pointer.
const wl_data_source_listener kSourceListener = {
    .target             = [](void *, wl_data_source *, const char *) {},
    .send               = [](
                              void *d, wl_data_source *s, const char *mime, int32_t fd
                          ) { appOf(d)->onSourceSend(s, mime, fd); },
    .cancelled          = [](void *d, wl_data_source *s) { appOf(d)->onSourceCancelled(s); },
    .dnd_drop_performed = [](void *d, wl_data_source *s) { appOf(d)->onSourceDropPerformed(s); },
    .dnd_finished       = [](void *d, wl_data_source *s) { appOf(d)->onSourceFinished(s); },
    .action = [](void *d, wl_data_source *s, uint32_t a) { appOf(d)->onSourceAction(s, a); },
};

const zwp_primary_selection_offer_v1_listener kPrimaryOfferListener = {
    .offer = [](void *d, zwp_primary_selection_offer_v1 *o, const char *mime) {
        auto &offers = appOf(d)->_primaryOffers;
        if (auto it = offers.find(o); it != offers.end())
            it->second.mimes.emplace_back(mime);
    },
};

const zwp_primary_selection_device_v1_listener kPrimaryDeviceListener = {
    .data_offer = [](void *d,
                     zwp_primary_selection_device_v1 *,
                     zwp_primary_selection_offer_v1 *o) { appOf(d)->onPrimaryOffer(o); },
    .selection  = [](void *d,
                     zwp_primary_selection_device_v1 *,
                     zwp_primary_selection_offer_v1 *o) { appOf(d)->onPrimarySelection(o); },
};

const zwp_primary_selection_source_v1_listener kPrimarySourceListener = {
    .send      = [](
                     void *d, zwp_primary_selection_source_v1 *s, const char *mime, int32_t fd
                 ) { appOf(d)->onPrimarySourceSend(s, mime, fd); },
    .cancelled = [](void                            *d,
                    zwp_primary_selection_source_v1 *s) { appOf(d)->onPrimarySourceCancelled(s); },
};

void setNonBlocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

} // namespace

void WlApp::setupDataDevice() {
    if (!_dataDevice && seat && dataManager) {
        _dataDevice = wl_data_device_manager_get_data_device(dataManager, seat);
        wl_data_device_add_listener(_dataDevice, &kDataDeviceListener, this);
    }
    if (!_primaryDevice && seat && primaryManager) {
        _primaryDevice = zwp_primary_selection_device_manager_v1_get_device(primaryManager, seat);
        zwp_primary_selection_device_v1_add_listener(_primaryDevice, &kPrimaryDeviceListener, this);
    }
}

WlApp::Offer *WlApp::offerFor(wl_data_offer *o) {
    auto it = _offers.find(o);
    return it == _offers.end() ? nullptr : &it->second;
}

void WlApp::dropOffer(wl_data_offer *o) {
    if (!o)
        return;
    _offers.erase(o);
    wl_data_offer_destroy(o);
}

void WlApp::dropPrimaryOffer(zwp_primary_selection_offer_v1 *o) {
    if (!o)
        return;
    _primaryOffers.erase(o);
    zwp_primary_selection_offer_v1_destroy(o);
}

void WlApp::onDataOffer(wl_data_offer *o) {
    _offers[o] = {};
    wl_data_offer_add_listener(o, &kOfferListener, this);
}

void WlApp::onPrimaryOffer(zwp_primary_selection_offer_v1 *o) {
    _primaryOffers[o] = {};
    zwp_primary_selection_offer_v1_add_listener(o, &kPrimaryOfferListener, this);
}

// ── selections ──────────────────────────────────────────────────────────────

void WlApp::onSelection(wl_data_offer *o) {
    if (_selectionOffer && _selectionOffer != o)
        dropOffer(_selectionOffer);
    _selectionOffer = o;
}

void WlApp::onPrimarySelection(zwp_primary_selection_offer_v1 *o) {
    if (_primaryOffer && _primaryOffer != o)
        dropPrimaryOffer(_primaryOffer);
    _primaryOffer = o;
}

void WlApp::setClipboard(std::vector<DataItem> items, Selection sel) {
    const bool primary = sel == Selection::Primary;
    if (primary && !primaryManager)
        return; // no primary selection on this compositor: reads say nullopt
    auto shared        = std::make_shared<const std::vector<DataItem>>(std::move(items));
    _clip[size_t(sel)] = shared;
    // The serial must come from a recent input event, or the compositor
    // ignores the request (it is how it stops background clipboard theft);
    // wlroots also refuses 0. The local copy above still serves our own reads.
    if (primary) {
        if (_primarySource)
            zwp_primary_selection_source_v1_destroy(_primarySource);
        _primarySource = nullptr;
        if (!_primaryDevice)
            return;
        _primarySource = zwp_primary_selection_device_manager_v1_create_source(primaryManager);
        zwp_primary_selection_source_v1_add_listener(_primarySource, &kPrimarySourceListener, this);
        offerItems(*shared, [&](const char *m) {
            zwp_primary_selection_source_v1_offer(_primarySource, m);
        });
        zwp_primary_selection_device_v1_set_selection(_primaryDevice, _primarySource, _inputSerial);
    } else {
        if (_source)
            wl_data_source_destroy(_source);
        _source = nullptr;
        if (!_dataDevice)
            return; // no data device: the clipboard is app-local, still round-trips
        _source = wl_data_device_manager_create_data_source(dataManager);
        wl_data_source_add_listener(_source, &kSourceListener, this);
        offerItems(*shared, [&](const char *m) { wl_data_source_offer(_source, m); });
        wl_data_device_set_selection(_dataDevice, _source, _inputSerial);
    }
    wl_display_flush(display);
}

void WlApp::requestClipboardMimes(std::function<void(std::vector<std::string>)> cb, Selection sel) {
    std::vector<std::string> mimes;
    if (const Items &own = _clip[size_t(sel)]) {
        for (const auto &i : *own)
            mimes.push_back(i.mime);
        mimes = normalised(mimes);
    } else if (sel == Selection::Clipboard) {
        if (const Offer *o = _selectionOffer ? offerFor(_selectionOffer) : nullptr)
            mimes = normalised(o->mimes);
    } else if (_primaryOffer) {
        if (auto it = _primaryOffers.find(_primaryOffer); it != _primaryOffers.end())
            mimes = normalised(it->second.mimes);
    }
    post([cb = std::move(cb), mimes = std::move(mimes)]() mutable { cb(std::move(mimes)); });
}

void WlApp::requestClipboard(
    std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
) {
    if (const Items &own = _clip[size_t(sel)]) {
        // Our own selection: answered locally. Through the compositor it
        // would work too (both pipe ends are async), but costs a round trip
        // and fails whenever the compositor refused our set_selection.
        std::optional<std::string> v;
        if (const DataItem *i = itemFor(*own, mime))
            v = i->data;
        post([cb = std::move(cb), v = std::move(v)]() mutable { cb(std::move(v)); });
        return;
    }
    const std::vector<std::string> *offered = nullptr;
    if (sel == Selection::Clipboard) {
        if (const Offer *o = _selectionOffer ? offerFor(_selectionOffer) : nullptr)
            offered = &o->mimes;
    } else if (_primaryOffer) {
        if (auto it = _primaryOffers.find(_primaryOffer); it != _primaryOffers.end())
            offered = &it->second.mimes;
    }
    const std::string pick = offered ? pickMime(*offered, mime) : std::string();
    if (pick.empty()) {
        post([cb = std::move(cb)] { cb(std::nullopt); });
        return;
    }
    if (sel == Selection::Clipboard) {
        wl_data_offer *o = _selectionOffer;
        receive(
            [o, pick](int fd) { wl_data_offer_receive(o, pick.c_str(), fd); },
            kClipTimeout,
            std::move(cb)
        );
    } else {
        zwp_primary_selection_offer_v1 *o = _primaryOffer;
        receive(
            [o, pick](int fd) { zwp_primary_selection_offer_v1_receive(o, pick.c_str(), fd); },
            kClipTimeout,
            std::move(cb)
        );
    }
}

void WlApp::onSourceSend(wl_data_source *src, const char *mime, int fd) {
    if (_drag && src == _drag->source)
        serve(_drag->items, mime, fd);
    else if (src == _source && _clip[0])
        serve(_clip[0], mime, fd);
    else
        close(fd);
}

void WlApp::onPrimarySourceSend(zwp_primary_selection_source_v1 *src, const char *mime, int fd) {
    if (src == _primarySource && _clip[1])
        serve(_clip[1], mime, fd);
    else
        close(fd);
}

void WlApp::onSourceCancelled(wl_data_source *src) {
    if (_drag && src == _drag->source) {
        // Dropped nowhere, rejected, or the compositor ended the drag.
        endDrag(DropAction::None);
        return;
    }
    wl_data_source_destroy(src);
    if (src == _source) {
        // Someone else owns the clipboard now.
        _source = nullptr;
        _clip[0].reset();
    }
}

void WlApp::onPrimarySourceCancelled(zwp_primary_selection_source_v1 *src) {
    zwp_primary_selection_source_v1_destroy(src);
    if (src == _primarySource) {
        _primarySource = nullptr;
        _clip[1].reset();
    }
}

void WlApp::serve(const Items &items, const char *mime, int fd) {
    const DataItem *item = items ? itemFor(*items, mime) : nullptr;
    if (!item) {
        close(fd);
        return;
    }
    setNonBlocking(fd);
    const uint64_t id = _nextTransfer++;
    auto          &t  = _transfers[id];
    t.fd              = fd;
    // Aliases the item list: a new selection (or the drag ending) mid-transfer
    // cannot free the bytes being written.
    t.out             = std::shared_ptr<const std::string>(items, &item->data);
    t.timer = _loop.core.addTimer(kClipTimeout, false, [this, id] { finishTransfer(id, false); });
    t.watch = _loop.watch(fd, FdWrite, [this, id](uint32_t) {
        auto it = _transfers.find(id);
        if (it == _transfers.end())
            return;
        auto &t = it->second;
        while (t.off < t.out->size()) {
            const ssize_t n = write(t.fd, t.out->data() + t.off, t.out->size() - t.off);
            if (n > 0) {
                t.off += size_t(n);
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && errno == EAGAIN) {
                return; // pipe full: wait for the reader
            } else {
                finishTransfer(id, false); // EPIPE: the reader gave up
                return;
            }
        }
        finishTransfer(id, true);
    });
}

void WlApp::receive(
    const std::function<void(int)>                 &ask,
    int                                             timeoutMs,
    std::function<void(std::optional<std::string>)> done
) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        post([done = std::move(done)] { done(std::nullopt); });
        return;
    }
    ask(fds[1]);
    close(fds[1]); // the peer holds the write end now; EOF = transfer complete
    wl_display_flush(display);
    setNonBlocking(fds[0]);

    const uint64_t id = _nextTransfer++;
    auto          &t  = _transfers[id];
    t.fd              = fds[0];
    t.done            = std::move(done);
    t.timer = _loop.core.addTimer(timeoutMs, false, [this, id] { finishTransfer(id, false); });
    t.watch = _loop.watch(fds[0], FdRead, [this, id](uint32_t) {
        auto it = _transfers.find(id);
        if (it == _transfers.end())
            return;
        auto &t = it->second;
        char  buf[16384];
        for (;;) {
            const ssize_t n = read(t.fd, buf, sizeof buf);
            if (n > 0) {
                t.in.append(buf, size_t(n));
                if (t.in.size() > kMaxTransfer) {
                    finishTransfer(id, false);
                    return;
                }
            } else if (n == 0) {
                finishTransfer(id, true);
                return;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN) {
                return;
            } else {
                finishTransfer(id, false);
                return;
            }
        }
    });
}

void WlApp::finishTransfer(uint64_t id, bool ok) {
    auto it = _transfers.find(id);
    if (it == _transfers.end())
        return;
    Transfer t = std::move(it->second);
    _transfers.erase(it);
    if (t.watch)
        _loop.unwatch(t.watch);
    if (t.timer)
        _loop.core.cancelTimer(t.timer);
    if (t.fd >= 0)
        close(t.fd);
    if (t.done)
        t.done(ok ? std::optional<std::string>(std::move(t.in)) : std::nullopt);
}

// ── drop target ─────────────────────────────────────────────────────────────

Event WlApp::dropEvent(EventType t, const Offer &of) const {
    Event e{.type = t, .pos = _dragPos, .mods = _xkb.mods()};
    // Before v3 there are no actions: every drop is a copy.
    e.allowedActions = dataManagerVersion >= 3 ? fromWlActions(of.sourceActions) : ActCopy;
    e.dropAction     = core::preferredAction(e.allowedActions);
    e.items          = _dragItems;
    return e;
}

void WlApp::onDragEnter(uint32_t serial, wl_surface *s, double x, double y, wl_data_offer *o) {
    if (_dragOffer && _dragOffer != o && !_dropping)
        dropOffer(_dragOffer);
    _dropping         = false;
    _dragOffer        = o;
    _dragWindow       = windowFor(s);
    _dragSerial       = serial;
    _dragPos          = {x, y};
    _dragAnsweredOnce = false;
    _dragMime.clear();
    const Offer *of = o ? offerFor(o) : nullptr;
    if (!of)
        return;
    // Accept the richest standard type for the cursor feedback; the drop
    // itself fetches every standard type there is.
    for (const char *m : {kUriList, kText, kHtml, kPng})
        if (_dragMime = pickMime(of->mimes, m); !_dragMime.empty())
            break;
    if (_dragMime.empty() && !of->mimes.empty())
        _dragMime = of->mimes.front();
    // The offer's types are fixed once it is entered: normalise them once,
    // not on every motion.
    _dragItems.clear();
    for (auto &m : normalised(of->mimes))
        _dragItems.push_back({std::move(m), {}});
    if (_dragWindow) {
        Event e                = dropEvent(EventType::DropEnter, *of);
        _dragWindow->dropReply = e.dropAction; // "default Copy" — when the source allows it
        _dragWindow->emitEvent(std::move(e));
    }
    answerDrag(true);
}

void WlApp::onDragMotion(double x, double y) {
    _dragPos        = {x, y};
    const Offer *of = _dragOffer ? offerFor(_dragOffer) : nullptr;
    if (_dragWindow && of)
        _dragWindow->emitEvent(dropEvent(EventType::DropMove, *of));
    answerDrag(false);
}

// Tells the source what the window answered: a type + an action, or null +
// none, which gives the "no" cursor and makes the compositor cancel a drop.
void WlApp::answerDrag(bool force) {
    const Offer *of = _dragOffer ? offerFor(_dragOffer) : nullptr;
    if (!of)
        return;
    DropAction a = _dragWindow && alive(_dragWindow) ? _dragWindow->dropReply : DropAction::None;
    if (_dragMime.empty())
        a = DropAction::None;
    uint32_t wl = toWlAction(a);
    if (dataManagerVersion >= 3 && a != DropAction::None && !(wl & of->sourceActions))
        a = DropAction::None, wl = 0; // not an action the source permits
    if (!force && _dragAnsweredOnce && a == _dragAnswered)
        return;
    _dragAnswered     = a;
    _dragAnsweredOnce = true;
    wl_data_offer_accept(
        _dragOffer, _dragSerial, a == DropAction::None ? nullptr : _dragMime.c_str()
    );
    if (dataManagerVersion >= 3)
        wl_data_offer_set_actions(_dragOffer, wl, wl);
}

void WlApp::onDragLeave() {
    // After a drop the offer belongs to the receives in flight, which finish
    // and destroy it; that leave is not a cancellation.
    if (!_dropping) {
        if (_dragWindow)
            _dragWindow->emitEvent({.type = EventType::DropLeave});
        dropOffer(_dragOffer);
    }
    _dropping   = false;
    _dragOffer  = nullptr;
    _dragWindow = nullptr;
}

void WlApp::onDrop() {
    const Offer *of = _dragOffer ? offerFor(_dragOffer) : nullptr;
    if (!of || !_dragWindow || _dragAnswered == DropAction::None)
        return; // rejected: the compositor cancels, a leave follows
    _dropping = true;

    // Fetch the standard types present, all at once, then emit one Drop.
    struct Pending {
        Event                                            event;
        std::vector<std::pair<std::string, std::string>> want; // (normalised, offered name)
        std::vector<std::optional<std::string>>          got;
        size_t                                           left = 0;
    };
    auto p   = std::make_shared<Pending>();
    p->event = dropEvent(EventType::Drop, *of);
    p->event.items.clear();
    p->event.dropAction = dataManagerVersion >= 3 ? fromWlAction(of->action) : DropAction::Copy;
    if (p->event.dropAction == DropAction::None)
        p->event.dropAction = _dragAnswered;
    for (const char *m : {kUriList, kText, kHtml, kPng})
        if (std::string got = pickMime(of->mimes, m); !got.empty())
            p->want.emplace_back(m, got);
    if (p->want.empty())
        p->want.emplace_back(_dragMime, _dragMime); // only a custom type: fetch that
    p->got.resize(p->want.size());
    p->left = p->want.size();

    wl_data_offer *offer  = _dragOffer;
    WlWindow      *window = _dragWindow;
    for (size_t i = 0; i < p->want.size(); ++i) {
        const std::string name = p->want[i].second;
        receive(
            [offer, name](int fd) { wl_data_offer_receive(offer, name.c_str(), fd); },
            kDropTimeout,
            [this, p, i, offer, window](std::optional<std::string> data) {
                p->got[i] = std::move(data);
                if (--p->left)
                    return;
                Event &e = p->event;
                for (size_t k = 0; k < p->want.size(); ++k)
                    if (p->got[k])
                        e.items.push_back({p->want[k].first, std::move(*p->got[k])});
                core::fillDropText(e);
                if (alive(window))
                    window->emitEvent(e);
                // finish is a protocol error unless an action was negotiated;
                // it tells the source (dnd_finished) the transfer is over.
                const Offer *of = offerFor(offer);
                if (of && dataManagerVersion >= 3 && of->action)
                    wl_data_offer_finish(offer);
                dropOffer(offer);
                if (_dragOffer == offer)
                    _dragOffer = nullptr;
                wl_display_flush(display);
            }
        );
    }
}

// ── drag source ─────────────────────────────────────────────────────────────

bool WlApp::startDrag(Window &source, const DragDesc &d) {
    auto &w = static_cast<WlWindow &>(source);
    // start_drag needs the serial of the press whose implicit grab it takes
    // over, on the surface that got it; without a held button there is none.
    // Before data-device v3 the source hears neither dnd_finished nor a
    // cancel for a plain drop, so DragFinished could never be promised;
    // every compositor we target (wlroots, mutter, kwin, weston) has v3.
    if (!_dataDevice || dataManagerVersion < 3 || _drag || !_held || _pressWindow != &w ||
        d.items.empty())
        return false;
    auto ds    = std::make_unique<DragSource>();
    ds->window = &w;
    ds->items  = std::make_shared<const std::vector<DataItem>>(d.items);
    ds->source = wl_data_device_manager_create_data_source(dataManager);
    wl_data_source_add_listener(ds->source, &kSourceListener, this);
    offerItems(*ds->items, [&](const char *m) { wl_data_source_offer(ds->source, m); });
    wl_data_source_set_actions(
        ds->source, toWlActions(d.actions) ? toWlActions(d.actions) : kWlCopy
    );

    // The drag image, shown at width/scale logical pixels (Image::scale).
    const Image &img = d.image;
    if (!img.empty() && size_t(img.width) * img.height == img.pixels.size() &&
        allocShmBuffer(shm, img.width, img.height, WL_SHM_FORMAT_ARGB8888, &ds->iconBuffer)) {
        std::memcpy(ds->iconBuffer.pixels, img.pixels.data(), ds->iconBuffer.bytes);
        ds->icon = wl_compositor_create_surface(compositor);
    }
    if (ds->icon && std::isfinite(img.scale) && img.scale > 0 && img.scale != 1.0) {
        // A viewport takes any scale; buffer_scale only an integer that
        // divides the buffer (anything else is a protocol error). Failing
        // both, the image shows 1:1 — bigger, but never blurry or fatal.
        const int  lw = std::max(1, int(std::lround(img.width / img.scale)));
        const int  lh = std::max(1, int(std::lround(img.height / img.scale)));
        const int  is = int(std::lround(img.scale));
        const bool integral =
            std::abs(img.scale - is) < 1e-6 && img.width % is == 0 && img.height % is == 0;
        if (viewporter) {
            ds->iconViewport = wp_viewporter_get_viewport(viewporter, ds->icon);
            wp_viewport_set_destination(ds->iconViewport, lw, lh);
        } else if (integral && compositorVersion >= 3) {
            wl_surface_set_buffer_scale(ds->icon, is);
        }
    }
    wl_data_device_start_drag(_dataDevice, ds->source, w.surface(), ds->icon, _pressSerial);
    if (ds->icon) {
        // The icon's top-left starts at the pointer; shift it by the hotspot.
        const int32_t hx = -int32_t(std::lround(d.hotspot.x)),
                      hy = -int32_t(std::lround(d.hotspot.y));
        if (compositorVersion >= 5) {
            wl_surface_offset(ds->icon, hx, hy);
            wl_surface_attach(ds->icon, ds->iconBuffer.buffer, 0, 0);
        } else {
            wl_surface_attach(ds->icon, ds->iconBuffer.buffer, hx, hy);
        }
        if (compositorVersion >= 4)
            wl_surface_damage_buffer(ds->icon, 0, 0, d.image.width, d.image.height);
        else
            wl_surface_damage(ds->icon, 0, 0, INT32_MAX, INT32_MAX); // surface coords: all of it
        wl_surface_commit(ds->icon);
    }
    wl_display_flush(display);
    _drag = std::move(ds);
    return true;
}

void WlApp::onSourceDropPerformed(wl_data_source *) {
    // The user let go over a target that accepted; dnd_finished (or
    // cancelled, if it gives up) follows once it has read the data.
}

void WlApp::onSourceFinished(wl_data_source *src) {
    if (_drag && src == _drag->source)
        endDrag(fromWlAction(_drag->action));
}

void WlApp::onSourceAction(wl_data_source *src, uint32_t action) {
    if (_drag && src == _drag->source)
        _drag->action = action;
}

// Exactly one DragFinished per drag: dnd_finished (the target took it) or
// cancelled (nowhere, rejected, compositor gave up).
void WlApp::endDrag(DropAction result) {
    if (!_drag)
        return;
    std::unique_ptr<DragSource> ds = std::move(_drag);
    // Transfers still writing hold their own reference to the items.
    wl_data_source_destroy(ds->source);
    if (ds->iconViewport)
        wp_viewport_destroy(ds->iconViewport);
    if (ds->icon)
        wl_surface_destroy(ds->icon);
    freeShmBuffer(&ds->iconBuffer);
    wl_display_flush(display);
    if (alive(ds->window))
        ds->window->emitEvent({.type = EventType::DragFinished, .dropAction = result});
}

} // namespace plat::wl

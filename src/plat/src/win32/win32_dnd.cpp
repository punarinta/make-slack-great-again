// Win32 backend, OLE drag and drop: an IDropTarget per window (RegisterDragDrop)
// turning the shell's drags into DropEnter/Move/Leave/Drop, and startDrag() as
// DoDragDrop over our own IDataObject + IDropSource.
//
// Both ends speak the MIME ↔ clipboard-format mapping of win32_data.cpp, so a
// drag between two plat windows, from Explorer or to a browser all see the
// same types. DoDragDrop runs its own modal message loop until the drop; our
// hidden window's modal timer (enterModal) keeps plat timers, posted work and
// frames running inside it, exactly as for live move/resize.
#include "win32/win32.h"
#include "core/drop.h"

#include <ole2.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace plat::win32 {

namespace {

DWORD effectsFor(uint32_t actions) {
    DWORD e = DROPEFFECT_NONE;
    if (actions & ActCopy)
        e |= DROPEFFECT_COPY;
    if (actions & ActMove)
        e |= DROPEFFECT_MOVE;
    if (actions & ActLink)
        e |= DROPEFFECT_LINK;
    return e;
}

uint32_t actionsFor(DWORD effects) {
    uint32_t a = 0;
    if (effects & DROPEFFECT_COPY)
        a |= ActCopy;
    if (effects & DROPEFFECT_MOVE)
        a |= ActMove;
    if (effects & DROPEFFECT_LINK)
        a |= ActLink;
    return a;
}

DWORD effectFor(DropAction a) {
    switch (a) {
    case DropAction::Copy:
        return DROPEFFECT_COPY;
    case DropAction::Move:
        return DROPEFFECT_MOVE;
    case DropAction::Link:
        return DROPEFFECT_LINK;
    case DropAction::None:
        break;
    }
    return DROPEFFECT_NONE;
}

DropAction actionFor(DWORD effect) {
    // A target reports one effect; if it reports several, the source's order
    // of preference decides (Move is what a source must act on, so first).
    if (effect & DROPEFFECT_MOVE)
        return DropAction::Move;
    if (effect & DROPEFFECT_COPY)
        return DropAction::Copy;
    if (effect & DROPEFFECT_LINK)
        return DropAction::Link;
    return DropAction::None;
}

// What Explorer does with the modifiers held: Ctrl copies, Shift moves,
// both link; otherwise Copy when allowed. Falls back when not allowed.
DropAction proposedAction(DWORD keys, uint32_t allowed) {
    return core::modifierDropAction((keys & MK_SHIFT) != 0, (keys & MK_CONTROL) != 0, allowed);
}

uint32_t modsFor(DWORD keys) {
    uint32_t m = 0;
    if (keys & MK_SHIFT)
        m |= ModShift;
    if (keys & MK_CONTROL)
        m |= ModCtrl;
    if (keys & MK_ALT)
        m |= ModAlt;
    // No MK_ bit for the Windows key; the drag may come from another thread,
    // so the async state is the only one that is current.
    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) < 0)
        m |= ModSuper;
    return m;
}

HGLOBAL duplicate(HGLOBAL src) {
    const size_t n = GlobalSize(src);
    const void  *p = GlobalLock(src);
    if (!p)
        return nullptr;
    HGLOBAL g = globalFromBytes(p, n);
    GlobalUnlock(src);
    return g;
}

// ── IDataObject ─────────────────────────────────────────────────────────────

// Our drag payload. Also accepts SetData of anything: the shell's drag-image
// helpers store their state ("DragImageBits", "DragContext", drop
// descriptions) on the data object and read it back from the other end.
class DataObject final : public ComObject<IDataObject> {
public:
    explicit DataObject(const std::vector<NativeData> &native) {
        for (const auto &n : native)
            if (HGLOBAL g = globalFromBytes(n.bytes.data(), n.bytes.size())) {
                STGMEDIUM m{};
                m.tymed   = TYMED_HGLOBAL;
                m.hGlobal = g;
                _entries.push_back({fmt(CLIPFORMAT(n.cf)), m});
            }
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *fe, STGMEDIUM *m) override {
        if (!fe || !m)
            return E_INVALIDARG;
        Entry *e = find(fe);
        if (!e)
            return DV_E_FORMATETC;
        *m = {};
        if (e->medium.tymed == TYMED_HGLOBAL) {
            // A copy the caller owns (pUnkForRelease null → it GlobalFrees).
            m->hGlobal = duplicate(e->medium.hGlobal);
            if (!m->hGlobal)
                return E_OUTOFMEMORY;
            m->tymed = TYMED_HGLOBAL;
            return S_OK;
        }
        if (e->medium.tymed == TYMED_ISTREAM && e->medium.pstm) {
            m->tymed = TYMED_ISTREAM;
            m->pstm  = e->medium.pstm;
            m->pstm->AddRef();
            LARGE_INTEGER zero{};
            m->pstm->Seek(zero, STREAM_SEEK_SET, nullptr);
            return S_OK;
        }
        return DV_E_TYMED;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *fe) override {
        return fe && find(fe) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override {
        if (out)
            out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *fe, STGMEDIUM *m, BOOL release) override {
        if (!fe || !m)
            return E_INVALIDARG;
        STGMEDIUM own{};
        if (release) {
            own = *m; // ours now, freed with ReleaseStgMedium
        } else if (m->tymed == TYMED_HGLOBAL) {
            own.tymed   = TYMED_HGLOBAL;
            own.hGlobal = duplicate(m->hGlobal);
            if (!own.hGlobal)
                return E_OUTOFMEMORY;
        } else if (m->tymed == TYMED_ISTREAM && m->pstm) {
            own.tymed = TYMED_ISTREAM;
            own.pstm  = m->pstm;
            own.pstm->AddRef();
        } else {
            return DV_E_TYMED;
        }
        for (auto &e : _entries)
            if (e.fe.cfFormat == fe->cfFormat && e.fe.dwAspect == fe->dwAspect) {
                ReleaseStgMedium(&e.medium);
                e.medium   = own;
                e.fe.tymed = own.tymed;
                return S_OK;
            }
        FORMATETC f = *fe;
        f.ptd       = nullptr;
        f.tymed     = own.tymed;
        _entries.push_back({f, own});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC **out) override {
        if (dir != DATADIR_GET)
            return E_NOTIMPL;
        std::vector<FORMATETC> list;
        for (const auto &e : _entries)
            list.push_back(e.fe);
        return SHCreateStdEnumFmtEtc(UINT(list.size()), list.data(), out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    struct Entry {
        FORMATETC fe;
        STGMEDIUM medium;
    };
    ~DataObject() override {
        for (auto &e : _entries)
            ReleaseStgMedium(&e.medium);
    }
    static FORMATETC fmt(CLIPFORMAT cf) {
        return {cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    }
    Entry *find(const FORMATETC *fe) {
        for (auto &e : _entries)
            if (e.fe.cfFormat == fe->cfFormat && (e.fe.tymed & fe->tymed) &&
                e.fe.dwAspect == fe->dwAspect)
                return &e;
        return nullptr;
    }

    std::vector<Entry> _entries;
};

// ── IDropSource ─────────────────────────────────────────────────────────────

class DropSource final : public ComObject<IDropSource> {
public:
    explicit DropSource(DWORD button) : _button(button) {}

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        // Escape cancels; releasing the button that started the drag drops;
        // pressing another button cancels, as in Explorer.
        const DWORD buttons = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON;
        if (escape || (keys & buttons & ~_button))
            return DRAGDROP_S_CANCEL;
        if (!(keys & _button))
            return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    DWORD _button;
};

} // namespace

// ── IDropTarget ─────────────────────────────────────────────────────────────

struct DropTarget final : public ComObject<IDropTarget> {
    DropTarget(Win32Window *w, IDropTargetHelper *helper)
        : window(w), hwnd(w->hwnd()), helper(helper) {
        if (helper)
            helper->AddRef();
    }

    HRESULT STDMETHODCALLTYPE
    DragEnter(IDataObject *obj, DWORD keys, POINTL pt, DWORD *effect) override {
        Hold hold(this);
        if (helper) {
            POINT p{pt.x, pt.y};
            helper->DragEnter(hwnd, obj, &p, *effect);
        }
        setData(obj);
        allowed   = actionsFor(*effect);
        types     = offeredTypes(obj);
        explicit_ = false;
        reply     = proposedAction(keys, allowed);
        lastPt    = {pt.x, pt.y};
        lastKeys  = keys;
        emitOver(EventType::DropEnter, keys, pt);
        *effect = lastEffect = currentEffect();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD *effect) override {
        Hold hold(this);
        if (helper) {
            POINT p{pt.x, pt.y};
            helper->DragOver(&p, *effect);
        }
        // OLE calls DragOver on a timer too; only real changes are events.
        if (pt.x != lastPt.x || pt.y != lastPt.y || keys != lastKeys) {
            if (!explicit_) // the modifiers pick the action until the app answers
                reply = proposedAction(keys, allowed);
            lastPt   = {pt.x, pt.y};
            lastKeys = keys;
            emitOver(EventType::DropMove, keys, pt);
            lastEffect = currentEffect();
        }
        *effect = lastEffect;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        Hold hold(this);
        if (helper)
            helper->DragLeave();
        setData(nullptr);
        types.clear();
        if (window)
            window->sendEvent({.type = EventType::DropLeave});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    Drop(IDataObject *obj, DWORD keys, POINTL pt, DWORD *effect) override {
        Hold hold(this);
        if (helper) {
            POINT p{pt.x, pt.y};
            helper->Drop(obj, &p, *effect);
        }
        allowed            = actionsFor(*effect) ? actionsFor(*effect) : allowed;
        const DWORD chosen = effectFor(reply) & effectsFor(allowed);
        *effect            = chosen;
        if (!window) {
            setData(nullptr);
            return S_OK;
        }
        if (!chosen) {
            // Refused at the last position: the drag just leaves.
            setData(nullptr);
            window->sendEvent({.type = EventType::DropLeave});
            return S_OK;
        }
        Event       e = event(EventType::Drop, keys, pt);
        // The bytes are all copied out now, while the source is still there.
        // Converting is quick except for a bitmap offered as PNG (a dragged
        // screenshot): that encode runs on a worker, so neither this loop
        // nor the source waits for it, and the Drop follows when it is done.
        std::string slowMime;
        OsData      slowRaw;
        size_t      slowAt = 0;
        for (const auto &mime : types) {
            if (!core::readOnDrop(mime)) {
                e.items.push_back({mime, {}});
                continue;
            }
            auto raw = readFromOs(mime, [&](UINT cf) { return fetch(obj, cf); });
            if (!raw)
                continue;
            if (slowMime.empty() && decodeIsSlow(mime, *raw)) {
                slowMime = mime;
                slowRaw  = std::move(*raw);
                slowAt   = e.items.size();
                e.items.push_back({mime, {}}); // filled in when it is ready
                continue;
            }
            auto data = finishDecode(mime, std::move(*raw));
            if (!data)
                continue;
            e.items.push_back({mime, std::move(*data)});
        }
        core::fillDropText(e); // the slow item is an image: it never counts here
        e.dropAction = reply;
        setData(nullptr);
        types.clear();
        if (slowMime.empty()) {
            window->sendEvent(std::move(e));
            return S_OK;
        }
        Win32Window *w = window;
        w->app()->decodeOffThread(
            std::move(slowMime),
            std::move(slowRaw),
            [w, alive = w->aliveToken(), e = std::move(e), slowAt](auto data) mutable {
                if (alive.expired())
                    return;
                if (data)
                    e.items[slowAt].data = std::move(*data);
                else
                    e.items.erase(e.items.begin() + ptrdiff_t(slowAt));
                w->sendEvent(std::move(e));
            }
        );
        return S_OK;
    }

    // Keeps us alive across an emit that destroys the window (and so revokes us).
    struct Hold {
        DropTarget *t;
        explicit Hold(DropTarget *t) : t(t) { t->AddRef(); }
        ~Hold() { t->Release(); }
    };

    ~DropTarget() override {
        setData(nullptr);
        if (helper)
            helper->Release();
    }

    void setData(IDataObject *obj) {
        if (obj)
            obj->AddRef();
        if (data)
            data->Release();
        data = obj;
    }

    static std::optional<std::string> fetch(IDataObject *obj, UINT cf) {
        if (!obj || !cf)
            return std::nullopt;
        FORMATETC fe{CLIPFORMAT(cf), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM m{};
        if (FAILED(obj->GetData(&fe, &m)))
            return std::nullopt;
        std::optional<std::string> out;
        if (m.tymed == TYMED_HGLOBAL && m.hGlobal)
            out = bytesFromGlobal(m.hGlobal);
        ReleaseStgMedium(&m);
        return out;
    }

    static std::vector<std::string> offeredTypes(IDataObject *obj) {
        std::vector<UINT> cfs;
        IEnumFORMATETC   *en = nullptr;
        if (obj && SUCCEEDED(obj->EnumFormatEtc(DATADIR_GET, &en)) && en) {
            FORMATETC fe;
            while (en->Next(1, &fe, nullptr) == S_OK) {
                if (fe.ptd)
                    CoTaskMemFree(fe.ptd);
                // Only what we can read: shell streams (FileContents) and
                // GDI handles are out of reach of a byte-oriented DataItem.
                if ((fe.tymed & TYMED_HGLOBAL) && fe.dwAspect == DVASPECT_CONTENT)
                    cfs.push_back(fe.cfFormat);
            }
            en->Release();
        }
        return mimesForFormats(cfs);
    }

    Event event(EventType t, DWORD keys, POINTL pt) const {
        Event e{.type = t};
        e.pos            = window->screenToLogical({pt.x, pt.y});
        e.mods           = modsFor(keys);
        e.allowedActions = allowed;
        e.dropAction     = proposedAction(keys, allowed);
        return e;
    }

    void emitOver(EventType t, DWORD keys, POINTL pt) {
        if (!window)
            return;
        Event e = event(t, keys, pt);
        for (const auto &m : types)
            e.items.push_back({m, {}});
        window->sendEvent(std::move(e)); // may call setDropAction → reply
    }

    DWORD currentEffect() const { return effectFor(reply) & effectsFor(allowed); }

    Win32Window             *window; // null once the window is gone
    HWND                     hwnd;
    IDropTargetHelper       *helper;
    IDataObject             *data = nullptr;
    std::vector<std::string> types;
    uint32_t                 allowed   = 0;
    DropAction               reply     = DropAction::Copy;
    bool                     explicit_ = false; // the app answered for this drag
    POINT                    lastPt{};
    DWORD                    lastKeys = 0, lastEffect = DROPEFFECT_NONE;
};

DropTarget *registerDropTarget(Win32Window *w, IDropTargetHelper *helper) {
    auto *t = new DropTarget(w, helper);
    if (FAILED(RegisterDragDrop(w->hwnd(), t))) {
        t->Release();
        return nullptr;
    }
    return t; // RegisterDragDrop holds its own reference; this one is the window's
}

void revokeDropTarget(DropTarget *t) {
    if (!t)
        return;
    t->window = nullptr;
    RevokeDragDrop(t->hwnd);
    t->Release();
}

void setDropReply(DropTarget *t, DropAction a) {
    t->reply     = a;
    t->explicit_ = true;
}

// ── drag source ─────────────────────────────────────────────────────────────

bool Win32App::startDrag(Window &source, const DragDesc &d) {
    auto &w = static_cast<Win32Window &>(source);
    if (!_oleInit || d.items.empty())
        return false;
    // DoDragDrop ends the drag when the button that started it comes up; a
    // drag started with no button held would drop on the spot.
    DWORD button = 0;
    if (GetKeyState(VK_LBUTTON) < 0)
        button = MK_LBUTTON;
    else if (GetKeyState(VK_RBUTTON) < 0)
        button = MK_RBUTTON;
    else if (GetKeyState(VK_MBUTTON) < 0)
        button = MK_MBUTTON;
    if (!button)
        return false;

    auto *data = new DataObject(encodeForOs(d.items));
    auto *src  = new DropSource(button);
    if (!d.image.empty()) {
        IDragSourceHelper *helper = nullptr;
        if (SUCCEEDED(CoCreateInstance(
                CLSID_DragDropHelper,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_IDragSourceHelper,
                reinterpret_cast<void **>(&helper)
            ))) {
            // The shell shows the bitmap at its pixel size, and the contract
            // wants width/scale logical pixels: rescale to this window's
            // physical size unless the image was drawn for it already (a
            // 2x image on a 200% monitor goes through untouched).
            const double k  = d.image.scale > 0 ? w.scale() / d.image.scale : 1.0;
            const int    iw = std::max(1, int(std::lround(d.image.width * k)));
            const int    ih = std::max(1, int(std::lround(d.image.height * k)));
            const Image  shown =
                std::abs(iw - d.image.width) <= 1 && std::abs(ih - d.image.height) <= 1
                    ? d.image
                    : scaleImage(d.image, iw, ih);
            SHDRAGIMAGE img{};
            img.sizeDragImage = {shown.width, shown.height};
            img.ptOffset      = {LONG(d.hotspot.x * w.scale()), LONG(d.hotspot.y * w.scale())};
            img.hbmpDragImage = dibFromImage(shown);
            img.crColorKey    = CLR_NONE;
            // On success the helper owns the bitmap.
            if (img.hbmpDragImage && FAILED(helper->InitializeFromBitmap(&img, data)))
                DeleteObject(img.hbmpDragImage);
            helper->Release();
        }
    }
    DWORD allowed = effectsFor(d.actions);
    if (!allowed)
        allowed = DROPEFFECT_COPY;

    const std::weak_ptr<char> alive = w.aliveToken();
    w.forgetButtons();
    enterModal();
    DWORD         effect = DROPEFFECT_NONE;
    const HRESULT hr     = DoDragDrop(data, src, allowed, &effect);
    leaveModal();
    data->Release();
    src->Release();

    const DropAction result =
        hr == DRAGDROP_S_DROP ? actionFor(effect & allowed) : DropAction::None;
    Win32Window *win = &w;
    // Posted: startDrag runs inside the app's own PointerDown/Move handler.
    post([alive, win, result] {
        if (!alive.expired())
            win->sendEvent({.type = EventType::DragFinished, .dropAction = result});
    });
    return true;
}

} // namespace plat::win32

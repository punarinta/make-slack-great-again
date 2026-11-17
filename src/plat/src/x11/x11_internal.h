// X11 backend internals, shared by x11_app.cpp (connection, loop, input),
// x11_window.cpp (windows, present) and x11_selection.cpp (clipboard, XDND).
// Pure XCB, linked directly: the static musl build has no dlopen, and Xlib
// would drag its own locale/IM machinery in for nothing.
#pragma once

#include "core/backends.h"
#include "core/input.h"
#include "core/transfer.h"
#include "linux/posix_app.h"
#include "linux/xkb_keyboard.h"
#ifdef PLAT_TEST_HOOKS
#include "plat/testing.h"
#endif

#include <xcb/xcb.h>

#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

struct xcb_cursor_context_t;

namespace plat::x11 {

// Owning wrapper for xcb replies (malloc'd by libxcb).
template <typename T>
struct Reply {
    T *p = nullptr;
    explicit Reply(T *r) : p(r) {}
    ~Reply() { std::free(p); }
    Reply(const Reply &)              = delete;
    Reply   &operator=(const Reply &) = delete;
    T       *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Interned in one batch at startup; names in kAtomNames (x11_app.cpp).
enum AtomId : int {
    WmProtocols,
    WmDeleteWindow,
    WmChangeState,
    WmClientMachine,
    NetWmPing,
    NetWmName,
    NetWmPid,
    NetWmState,
    NetWmStateMaxVert,
    NetWmStateMaxHorz,
    NetWmStateFullscreen,
    NetWmStateHidden,
    NetWmWindowType,
    NetWmWindowTypeNormal,
    NetActiveWindow,
    NetWmMoveResize,
    NetSupported,
    NetSupportingWmCheck,
    MotifWmHints,
    Utf8String,
    Clipboard,
    Targets,
    Timestamp,
    Text,
    Incr,
    MimeTextUtf8,
    MimeTextPlain,
    MimeUriList,
    PlatSelection,
    PlatDnd,
    PlatTime,
    XdndAware,
    XdndEnter,
    XdndPosition,
    XdndStatus,
    XdndLeave,
    XdndDrop,
    XdndFinished,
    XdndSelection,
    XdndTypeList,
    XdndActionCopy,
    XdndActionMove,
    XdndActionLink,
    XdndActionAsk,
    XdndActionList,
    XdndProxy,
    Multiple,
    SaveTargets,
    MimeTextHtml,
    MimeImagePng,
    NetWmStateDemandsAttention,
    NetWorkarea,
    NetCurrentDesktop,
    NetMoveResizeWindow,
    NetStartupId,
    NetStartupInfoBegin,
    NetStartupInfo,
    NetWmStateAbove,
    NetWmIcon,
    AtomCount
};

class X11App;

class X11Window final : public Window {
public:
    X11Window(X11App *app, const WindowDesc &d);
    ~X11Window() override;
    bool ok() const { return _win != 0; }

    void                 setTitle(std::string_view utf8) override;
    Size                 size() const override;
    double               scale() const override;
    void                 setSize(Size logical) override;
    void                 setMinSize(Size logical) override;
    void                 show() override;
    void                 hide() override;
    void                 minimize() override;
    void                 setMaximized(bool on) override;
    void                 setFullscreen(bool on) override;
    bool                 isMaximized() const override { return _maximized; }
    bool                 isFullscreen() const override { return _fullscreen; }
    bool                 isMinimized() const override { return _hidden; }
    bool                 supportsAlwaysOnTop() const override { return true; }
    void                 setAlwaysOnTop(bool on) override;
    bool                 isAlwaysOnTop() const override { return _above; }
    bool                 isActive() const override { return _active; }
    void                 activate() override;
    void                 activateWithToken(std::string_view token) override;
    std::optional<Point> position() const override;
    bool                 setPosition(Point logical) override;
    uint64_t             monitor() const override;
    void                 setCursor(Cursor c) override;
    void   setHitTest(std::function<HitArea(Point)> fn) override { _hitTest = std::move(fn); }
    bool   hasSystemDecorations() const override { return _decorations == Decorations::System; }
    void   setTextInput(const TextInputState &s) override { _textInput = s; }
    void   requestFrame() override;
    Canvas beginPaint() override;
    void   endPaint(const std::vector<Rect> &damage) override;
    void   setDropAction(DropAction a) override;
    void   requestAttention() override;
    void  *nativeHandle() const override { return (void *)(uintptr_t)_win; }

    // ── backend side ────────────────────────────────────────────────────────
    xcb_window_t xid() const { return _win; }
    void         emit(Event e);
    Point        toLogical(int x, int y) const;
    int          rootX() const { return _rootX; } // content top-left on the root, physical
    int          rootY() const { return _rootY; }
    int          physW() const { return _pw; }
    int          physH() const { return _ph; }
    bool         textInputEnabled() const { return _textInput.enabled; }
    HitArea      hitTest(Point p) const { return _hitTest ? _hitTest(p) : HitArea::Client; }
    // The pointer is at p (logical, no button held): over our own resize
    // border show the matching resize cursor, elsewhere the app's.
    void         pointerAt(Point p);

    void onConfigure(const xcb_configure_notify_event_t *e);
    void onReparent(xcb_window_t parent);
    void onExpose(int x, int y, int w, int h, int count);
    void onMapped(bool mapped);
    void onProperty(xcb_atom_t atom);
    void onShmCompletion();
    void onScaleChanged();
    void setActive(bool on);

    // Frame pacing: true when a Frame is wanted and may be sent now.
    bool frameReady(core::Clock::time_point now, core::Clock::time_point *notBefore) const;
    void sendFrame(core::Clock::time_point now);

    bool       hover     = false;            // pointer inside, for Enter/Leave de-duplication
    DropAction dropReply = DropAction::Copy; // answer to the current XdndPosition

private:
    struct Buffer {
        uint32_t *pixels = nullptr;
        int       w = 0, h = 0;         // in use (the window's size)
        int       stride = 0, capH = 0; // allocated: pixels per row, rows
        size_t    bytes = 0;
        uint32_t  seg   = 0; // xcb_shm_seg_t, 0 = heap buffer
        bool      shmFd = false;
    };
    bool allocBuffer(int w, int h);
    void freeBuffer(Buffer &b);
    void waitIdle();
    void putRect(const Rect &r, bool last);
    void writeNormalHints();
    void sendNetWmState(bool add, xcb_atom_t a, xcb_atom_t b);
    void writeWmHints();
    void activateAt(xcb_timestamp_t t);
    void sendStartupRemove(std::string_view id);
    void applyCursor();
    // Where the content really is: one round trip, for the cases where the
    // event itself does not say (a real ConfigureNotify inside a WM frame).
    void queryRootPos();
    void setRootPos(int x, int y); // emits Moved on change

    X11App                       *_app;
    xcb_window_t                  _win = 0;
    xcb_gcontext_t                _gc  = 0;
    int                           _pw = 0, _ph = 0; // physical, from ConfigureNotify
    Size                          _min;
    bool                          _resizable;
    Decorations                   _decorations;
    std::function<HitArea(Point)> _hitTest;
    TextInputState                _textInput;
    Buffer                        _buf;
    std::vector<Rect>             _puts;    // endPaint's scratch
    std::vector<Rect>             _exposed; // an Expose series, put when it ends
    bool                          _mapped = false, _presented = false;
    bool                          _framePending = false, _painting = false;
    int                           _shmInFlight = 0; // puts awaiting XCB_SHM_COMPLETION
    core::Clock::time_point       _shmSince{}, _lastFrame{};
    bool                          _maximized = false, _fullscreen = false, _hidden = false;
    bool                          _above     = false;
    bool                          _active    = false;
    bool                          _attention = false;         // urgency / demands-attention set
    Cursor                        _cursor    = Cursor::Arrow; // what the app asked for
    Cursor                        _edge      = Cursor::Arrow; // resize border under the pointer…
    bool                          _onEdge    = false;         // …when it is over one
    Cursor                        _shown     = Cursor::Arrow; // what the window has
    xcb_window_t                  _parent    = 0;             // root, or the WM's frame
    int                           _rootX = 0, _rootY = 0; // content top-left, physical root coords
    bool                          _userPos = false; // an explicit position: USPosition in the hints
    int                           _userX = 0, _userY = 0; // …and that position, physical
};

// The loop, openUrl and testHooks(): linux_services::PosixServicesApp.
class X11App final : public linux_services::PosixServicesApp {
public:
    X11App() = default;
    ~X11App() override;
    bool init(std::string *error);

    // ── App ─────────────────────────────────────────────────────────────────
    const char             *backendName() const override { return "x11"; }
    std::unique_ptr<Window> createWindow(const WindowDesc &desc) override;
    void                    run() override;
    void                    pump(int timeoutMs) override;
    void                    setClipboard(std::vector<DataItem> items, Selection sel) override;
    void                    requestClipboard(
        std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
    ) override;
    void
    requestClipboardMimes(std::function<void(std::vector<std::string>)> cb, Selection sel) override;
    bool                 startDrag(Window &source, const DragDesc &drag) override;
    // darkMode(), tray, notifications, badge: linux_services::ServicesApp.
    void                 emitThemeChanged() override;
    std::string          parentHandle(Window *w) override;
    std::vector<Monitor> monitors() const override;
#ifdef PLAT_TEST_HOOKS
    // ── TestHooks (the input injectors need XTEST and say so) ───────────────
    bool injectKey(Window &w, Key k, bool down) override;
    bool injectPointerMove(Window &w, Point logical) override;
    bool injectButton(Window &w, Button b, bool down) override;
    bool injectScroll(Window &w, double dx, double dy) override;
    bool readPixel(Window &w, int x, int y, uint32_t *argb) override;
    bool wantsAttention(Window &w, bool *out) override;
#endif

    // ── shared with windows / selection code ────────────────────────────────
    xcb_connection_t        *conn() const { return _c; }
    xcb_window_t             root() const { return _root; }
    xcb_atom_t               atom(AtomId id) const { return _atoms[id]; }
    xcb_atom_t               intern(const std::string &name); // cached, synchronous
    // Names for many atoms in one round trip's worth of latency (cached).
    std::vector<std::string> atomNames(const std::vector<xcb_atom_t> &atoms);
    double                   scale() const { return _scale; }
    uint8_t                  depth() const { return _depth; }
    uint32_t                 visual() const { return _visual; }
    uint32_t                 colormap() const { return _colormap; }
    bool                     wmSupports(AtomId a) const;
    bool                     hasWm() const { return _wmPresent; }
    bool                     shmMode(bool *fdPassing) const {
        *fdPassing = _shmFd;
        return _shm;
    }
    void            disableShm() { _shm = false; }
    uint32_t        maxRequestBytes() const { return _maxRequestBytes; }
    int             frameIntervalMs() const { return _frameIntervalMs; }
    xcb_timestamp_t lastTime() const { return _lastTime; }
    xcb_timestamp_t serverTime(); // a real timestamp, via a property round trip
    xcb_cursor_t    cursor(Cursor c);
    void            sendToRoot(xcb_window_t win, xcb_atom_t type, const uint32_t data[5]);

    // Id of the monitor containing a physical root point (nearest if none).
    uint64_t monitorAt(int x, int y) const;

    // Block (bounded) until pred() holds, processing only matching events and
    // deferring everything else to the normal dispatch. For the handful of
    // spots X11 needs a synchronous answer (SHM completion before reusing the
    // buffer, a server timestamp).
    bool waitForEvent(const std::function<bool(xcb_generic_event_t *)> &match, int timeoutMs);

    void       registerWindow(X11Window *w);
    void       forgetWindow(X11Window *w);
    X11Window *findWindow(xcb_window_t id) const;

    // Selection / XDND (x11_selection.cpp).
    void initSelection();
    void onSelectionRequest(xcb_selection_request_event_t *e);
    void onSelectionClear(xcb_selection_clear_event_t *e);
    void onSelectionNotify(xcb_selection_notify_event_t *e);
    bool onSelectionProperty(xcb_property_notify_event_t *e); // true = consumed
    void onXdnd(X11Window *w, xcb_client_message_event_t *e);
    void markDndAware(xcb_window_t w);
    void dndReplyChanged(X11Window *w); // setDropAction outside a DropEnter/Move handler

    // XInput2 (x11_xinput.cpp).
    void selectXInput(xcb_window_t w);

private:
    bool     beforeWait();
    bool     dispatchAll(bool readSocket); // true if any event was handled
    void     handle(xcb_generic_event_t *ev);
    void     handleXkb(xcb_generic_event_t *ev);
    void     handleButton(xcb_button_press_event_t *e, bool down);
    void     handleKey(xcb_key_press_event_t *e, bool down);
    void     handleClientMessage(xcb_client_message_event_t *e);
    void     handleMotion(X11Window *w, double px, double py, uint32_t mods, xcb_timestamp_t t);
    void     flushMotion(); // emits the held PointerMove, if any
    void     focusWindow(X11Window *w);
    void     emitFrames();
    void     readScale();
    void     readWmSupport();
    bool     setupXkb();
    void     reloadKeymap();
    void     setupShm();
    void     setupRandr();
    // Re-read the monitor layout and work areas (x11_monitors.cpp), and the
    // frame rate; emit = send MonitorsChanged if they differ from what we had.
    void     refreshMonitors(bool emit);
    // Only the work areas (a desktop switch, a panel change).
    void     refreshWorkAreas(bool emit);
    // Coalesces bursts of RandR/property events; layout = RandR said the
    // monitors themselves changed (else only work areas or the scale did).
    void     scheduleMonitorRefresh(bool layout);
    bool     isWorkareaAtom(xcb_atom_t a) const;
    uint32_t pointerMods(uint16_t state) const;
    void     startMoveResize(X11Window *w, HitArea a, const xcb_button_press_event_t *e);
    void     connectionLost();

    xcb_connection_t                 *_c                = nullptr;
    xcb_screen_t                     *_screen           = nullptr;
    xcb_window_t                      _root             = 0;
    xcb_atom_t                        _atoms[AtomCount] = {};
    std::map<std::string, xcb_atom_t> _internCache;
    uint32_t                          _visual = 0, _colormap = 0;
    uint8_t                           _depth       = 0;
    double                            _scale       = 1.0;
    bool                              _scaleForced = false;
    bool                              _wmPresent   = false;
    std::vector<xcb_atom_t>           _netSupported;
    bool                              _shm = false, _shmFd = false;
    uint8_t                           _shmEvent = 0;
#ifdef PLAT_TEST_HOOKS
    bool _xtest = false;
#endif
    uint32_t _maxRequestBytes = 256 * 1024;
    int      _frameIntervalMs = 16;
    int      _refreshMilliHz  = 0; // fastest CRTC, 0 = unknown
    TimerId  _frameTimer      = 0;
    bool     _lost            = false;

    // RandR and the monitor cache (x11_monitors.cpp).
    uint8_t _randrEvent = 0;  // first event code, 0 = no RandR
    int     _randrMinor = -1; // 1.x; GetMonitors needs 5
    struct MonitorEntry {
        Monitor m;    // logical, as handed out
        Rect    phys; // physical root coordinates
    };
    std::vector<MonitorEntry> _monitors;
    bool                      _monitorsReady       = false;
    bool                      _monitorsPending     = false;
    bool                      _monitorsLayoutStale = false; // a pending refresh must re-read RandR
    std::vector<MonitorEntry> readLayout(); // monitors (no work areas yet) + frame rate
    void                      applyWorkAreas(std::vector<MonitorEntry> &out);
    void                      commitMonitors(std::vector<MonitorEntry> out, bool emit);
    xcb_atom_t                _gtkWorkareas = 0; // _GTK_WORKAREAS_D<current desktop>

    uint64_t                          _xcbWatch = 0;
    std::deque<xcb_generic_event_t *> _deferred;

    std::unordered_map<xcb_window_t, X11Window *> _windows;
    std::vector<xcb_window_t>                     _frameIds; // emitFrames' scratch
    X11Window                                    *_focus = nullptr, *_pointerWin = nullptr;

    // Keyboard.
    linux_input::XkbKeyboard _kbd;
    int32_t                  _kbdDevice    = -1;
    uint8_t                  _xkbEvent     = 0;
    bool                     _keyDown[256] = {};

    // Pointer.
    xcb_timestamp_t    _lastTime = 0;
    core::ClickCounter _clicks;
    uint32_t           _swallowRelease = 0; // bitmask of X buttons whose release we eat
    uint32_t           _buttonsHeld    = 0; // bitmask of X buttons down (startDrag needs one)
    Point              _pointerPos;         // logical, last seen, for scroll events
    // Motion is coalesced: the latest PointerMove waits here until another
    // event or the end of the batch, so a 1000 Hz mouse costs the app one
    // move per batch, not one per report.
    xcb_window_t       _motionWin = 0; // 0 = none held
    Point              _motionPos;
    uint32_t           _motionMods = 0;

    // Cursors.
    xcb_cursor_context_t *_cursorCtx                           = nullptr;
    uint32_t              _cursorFont                          = 0;
    xcb_cursor_t          _cursors[size_t(Cursor::Hidden) + 1] = {};

    // Selection (x11_selection.cpp). The helper owns every selection we
    // hold and is the requestor of every conversion we make.
    xcb_window_t _helper = 0;
    enum SelIndex { SelClipboard, SelPrimary, SelXdnd, SelCount };
    struct Owned {
        std::vector<DataItem> items;
        bool                  own  = false;
        xcb_timestamp_t       time = 0; // when we took ownership
    };
    Owned      _owned[SelCount];
    xcb_atom_t selAtom(int idx) const;
    int        selIndex(xcb_atom_t selection) const; // -1 for one we never own
    bool       own(int idx, std::vector<DataItem> items);
    void       disown(int idx);

    // Conversions we asked for, one in flight at a time (front) because they
    // share the helper's property.
    struct Transfer {
        xcb_atom_t              selection = 0;
        xcb_timestamp_t         time      = 0; // 0 = our last event time
        std::vector<xcb_atom_t> targets;       // still to try, in order
        xcb_atom_t              target = 0;    // in flight
        bool                    incr   = false;
        std::string             data;
        // data (nullopt = refused/timed out) and the target that produced it.
        std::function<void(std::optional<std::string>, xcb_atom_t)> done;
    };
    std::deque<Transfer> _transfers;
    TimerId              _transferTimer = 0;
    void                 fetch(
        xcb_atom_t                                                  selection,
        std::vector<xcb_atom_t>                                     targets,
        xcb_timestamp_t                                             time,
        std::function<void(std::optional<std::string>, xcb_atom_t)> done
    );
    void startTransfer();
    void finishTransfer(std::optional<std::string> v);
    void armTransferTimeout();
    struct IncrSend {
        xcb_window_t            requestor;
        xcb_atom_t              property, type;
        std::string             data;
        size_t                  offset = 0;
        core::Clock::time_point since;
    };
    std::vector<IncrSend>             _incrSends;
    std::map<xcb_atom_t, std::string> _atomNameCache;
    // What an item list offers: every mime, plus the X text aliases for text.
    std::vector<xcb_atom_t>           offeredTypes(const std::vector<DataItem> &items);
    // The bytes for `target`, viewed in `items` (or in *latin1 for STRING).
    std::optional<std::string_view>   dataFor(
        const std::vector<DataItem> &items, xcb_atom_t target, xcb_atom_t *type, std::string *latin1
    );
    // The offered text targets, best first.
    std::vector<xcb_atom_t> textTargets(const std::vector<xcb_atom_t> &offered) const;
    bool                    isOurWindow(xcb_window_t w) const;

    // XDND receiving (x11_dnd.cpp).
    struct DndIn {
        xcb_window_t             source = 0, target = 0;
        int                      version = 0;
        std::vector<xcb_atom_t>  types;           // as offered
        std::vector<std::string> mimes;           // normalised, deduplicated
        uint32_t                 listActions = 0; // from XdndActionList
        uint32_t                 allowed     = 0;
        DropAction               preferred   = DropAction::None;
        bool                     entered = false, inPosition = false, dropping = false;
        Point                    pos;
        uint64_t                 gen = 0; // stale-callback guard for drop fetches
        // Drop: what is still to fetch (mime, targets best first) and what came.
        std::vector<std::pair<std::string, std::vector<xcb_atom_t>>> wants;
        std::vector<DataItem>                                        got;
        xcb_timestamp_t                                              dropTime = 0;
    } _dnd;
    uint64_t   _dndGen = 0;
    void       sendDndStatus(X11Window *w);
    void       dndFinishTarget(bool accepted, xcb_atom_t action);
    void       dndFetchNext();
    DropAction actionFromAtom(xcb_atom_t a) const;
    xcb_atom_t atomFromAction(DropAction a) const;

    // XDND source (x11_dnd.cpp).
    struct DndProbe {
        bool         aware   = false;
        int          version = 0;
        xcb_window_t proxy   = 0; // where messages go: the window or its proxy
    };
    struct DndOut {
        bool                    active  = false;
        X11Window              *source  = nullptr;
        uint32_t                actions = 0;
        std::vector<xcb_atom_t> types;
        xcb_window_t            target = 0, proxy = 0; // proxy = where messages go
        int                     version       = 0;
        bool                    waitingStatus = false, pending = false;
        bool                    accepted = false;
        xcb_atom_t              action   = 0;     // from the last XdndStatus
        bool                    haveRect = false; // "no more positions inside" rectangle
        Rect                    rect;
        bool                    released = false, dropSent = false;
        int16_t                 rootX = 0, rootY = 0;
        xcb_timestamp_t         time       = 0;
        xcb_atom_t              sentAction = 0;
        TimerId                 timer      = 0;
        xcb_window_t            icon       = 0;
        int                     hotX = 0, hotY = 0;
        Cursor                  cursor = Cursor::Grabbing;
        bool                    moved  = false; // the target search is due (dragTrack)
        // What each window under the pointer said (XdndAware, XdndProxy),
        // asked once per drag.
        std::unordered_map<xcb_window_t, DndProbe> probes;
    } _drag;
    DndProbe     probeDnd(xcb_window_t w);
    void         dragMotion(int16_t rootX, int16_t rootY, xcb_timestamp_t t);
    void         dragTrack(); // the target search for the latest dragMotion
    void         dragRelease(xcb_timestamp_t t);
    void         dragSendPosition();
    void         dragSendDrop();
    void         dragLeaveTarget();
    void         dragFinish(DropAction result);
    void         dragOnStatus(const xcb_client_message_event_t *e);
    void         dragOnFinished(const xcb_client_message_event_t *e);
    void         dragKey(xcb_key_press_event_t *e, bool down);
    void         dragSetCursor(Cursor c);
    void         dragArmTimeout(int ms);
    xcb_atom_t   dragRequestedAction() const;
    xcb_window_t findDndTarget(int16_t x, int16_t y, xcb_window_t *proxy, int *version);
    void         createDragIcon(const Image &img, Point hotspot);
    void sendXdnd(xcb_window_t to, xcb_window_t window, xcb_atom_t type, const uint32_t d[4]);

    // XFixes (the drag icon's empty input shape).
    bool _xfixes = false;

    // XInput2 (x11_xinput.cpp).
    uint8_t _xiOpcode = 0; // 0 = no XI2
    int     _xiMinor  = 0;
    struct ScrollAxis {
        int    number    = -1; // valuator index, -1 = none
        double increment = 0;
        double last      = 0;
        bool   haveLast  = false;
    };
    struct ScrollDevice {
        ScrollAxis v, h;
    };
    std::unordered_map<uint16_t, ScrollDevice> _scrollDevs;       // by source (slave) device id
    xcb_timestamp_t                            _xiScrollTime = 0; // last smooth-scroll event
    bool                                       _xiScrolled   = false;
    void                                       setupXInput();
    void                                       queryScrollDevices();
    void                                       resetScrollBases();
    void                                       handleXInput(xcb_generic_event_t *ev);

    friend class X11Window;
};

// Helpers shared by the translation units.
std::string latin1ToUtf8(std::string_view s);
std::string utf8ToLatin1(std::string_view s);
using core::isTextMime; // any X/MIME name for UTF-8/Latin-1 text
// The contract's name for a selection/XDND target: text aliases become
// text/plain;charset=utf-8, bookkeeping targets (TARGETS, …) become "".
std::string normaliseMime(std::string_view name);

} // namespace plat::x11

// Wayland backend internals, shared by the wl_*.cpp files. Nothing here is
// visible outside src/wayland/: the only entry point is createWaylandApp().
//
// Layout:
//   wl_app.cpp       connection, registry, the poll loop hook, frame pacing
//   wl_window.cpp    xdg-shell toplevel, decorations, scale, shm present
//   wl_output.cpp    wl_output + xdg-output: monitors, MonitorsChanged
//   wl_seat.cpp      pointer / keyboard / text-input-v3 / cursors
//   wl_data.cpp      clipboard, primary selection, drag and drop (async pipes)
//   wl_testhooks.cpp wlroots virtual input + screencopy for the selftest
//                    (PLAT_TEST_HOOKS builds only)
//
// These classes are plain internal glue: members are public so the listener
// tables in each file can reach them without a wall of friend declarations.
#pragma once

#include "core/backends.h"
#include "core/input.h"
#include "core/transfer.h"
#include "freedesktop/posix_app.h"
#include "freedesktop/xkb_keyboard.h"
#ifdef PLAT_TEST_HOOKS
#include "plat/testing.h"
#endif

#include <wayland-client.h>
#include <wayland-cursor.h>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#ifdef PLAT_TEST_HOOKS
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#endif

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace plat::wl {

class WlApp;
class WlWindow;

// One wl_output global and what it (and its zxdg_output_v1) reported. The
// wl_output/xdg-output events are double-buffered until `done`; the fields
// here are simply overwritten as they arrive and read after a done.
struct Output {
    WlApp          *app       = nullptr;
    wl_output      *output    = nullptr;
    zxdg_output_v1 *xdgOutput = nullptr;
    uint32_t        name      = 0; // registry name: global_remove, and the Monitor id
    uint32_t        version   = 0;
    int             scale     = 1;        // wl_output.scale (integer)
    int             modeW = 0, modeH = 0; // current mode, physical pixels, untransformed
    int             refreshMilliHz = 0;   // current mode
    int             geomX = 0, geomY = 0; // wl_output.geometry: compositor-space position
    int             transform = 0;        // wl_output_transform; odd = rotated 90/270
    std::string     wlName, description;  // wl_output v4
    std::string     xdgName;              // zxdg_output_v1 v2+
    // xdg-output logical layout (0 size = not reported): the real
    // virtual-desktop geometry once fractional scaling is involved.
    int             logicalX = 0, logicalY = 0, logicalW = 0, logicalH = 0;
    bool            announced = false; // a first done arrived, so the fields mean something
};

// One wl_shm buffer with its own memfd-backed pool. A window keeps a small
// ring of these; `stale` tracks which parts of it are older than the newest
// presented frame, so a partial repaint can start from correct pixels. The
// pool may be larger than the buffer, so a resize that still fits makes only
// a new wl_buffer from it (resizeShmBuffer), not a new memfd and mapping.
struct ShmBuffer {
    WlWindow         *owner  = nullptr;
    wl_buffer        *buffer = nullptr;
    wl_shm_pool      *pool   = nullptr;
    uint32_t         *pixels = nullptr;
    size_t            bytes  = 0;            // width * height * 4
    size_t            cap    = 0;            // the pool's (and the mapping's) size
    int               width = 0, height = 0; // physical
    bool              busy     = false;      // attached and not yet released by the compositor
    bool              staleAll = false;
    std::vector<Rect> stale; // physical rects newer frames painted that this buffer lacks
};

// memfd (or an unlinked file in $XDG_RUNTIME_DIR) of `bytes`, or -1.
int  createShmFd(size_t bytes);
// `capacity` (bytes) reserves room for later resizeShmBuffer calls.
bool allocShmBuffer(
    wl_shm *shm, int w, int h, uint32_t format, ShmBuffer *out, size_t capacity = 0
);
// A new w x h wl_buffer from b's pool when it fits; the old one is destroyed
// (b must not be busy). False, b untouched, when it does not fit.
bool resizeShmBuffer(ShmBuffer *b, int w, int h, uint32_t format);
void freeShmBuffer(ShmBuffer *b);

class WlWindow final : public Window {
public:
    WlWindow(WlApp *app, const WindowDesc &d);
    ~WlWindow() override;

    void                 setTitle(std::string_view utf8) override;
    Size                 size() const override { return _size; }
    double               scale() const override { return _scale; }
    void                 setSize(Size logical) override;
    void                 setMinSize(Size logical) override;
    void                 show() override;
    void                 hide() override;
    void                 minimize() override;
    void                 setMaximized(bool on) override;
    void                 setFullscreen(bool on) override;
    bool                 isMaximized() const override { return _maximized; }
    bool                 isFullscreen() const override { return _fullscreen; }
    bool                 isActive() const override { return _activated; }
    void                 activate() override;
    // xdg_activation_v1.activate with a token someone handed us (a launcher,
    // a notification click, a second instance); empty = activate().
    void                 activateWithToken(std::string_view token) override;
    // Wayland never tells a client where its window is, nor lets it choose
    // (WindowDesc::position is ignored too): the compositor places windows.
    std::optional<Point> position() const override { return std::nullopt; }
    bool                 setPosition(Point) override { return false; }
    // The most recently entered output that still shows the surface
    // (wl_surface.enter/leave), else the primary monitor (see monitors()).
    uint64_t             monitor() const override;
    void                 setCursor(Cursor c) override;
    void   setHitTest(std::function<HitArea(Point)> fn) override { _hitTest = std::move(fn); }
    bool   hasSystemDecorations() const override { return _serverDecorations; }
    void   setTextInput(const TextInputState &s) override;
    void   setDropAction(DropAction a) override { dropReply = a; }
    void   requestAttention() override;
    void   requestFrame() override;
    Canvas beginPaint() override;
    void   endPaint(const std::vector<Rect> &damage) override;
    void  *nativeHandle() const override { return _surface; }

    // ── used by WlApp ───────────────────────────────────────────────────────
    void        emitEvent(Event e);
    bool        frameReady() const { return _frameWanted && _configured && _visible && !_frameCb; }
    void        emitFrame();
    // Since the last Frame, a frame callback came: the compositor paces the
    // next one. Otherwise (nothing presented, first frame) WlApp paces it.
    bool        callbackPaced() const { return _callbackPaced; }
    HitArea     hitTestAt(Point p) const { return _hitTest ? _hitTest(p) : HitArea::Client; }
    Cursor      cursorAt(Point p) const;
    int         cursorScale() const; // integer scale for wl_cursor theme images
    void        outputGone(Output *o);
    void        startMove(uint32_t serial);
    void        startResize(uint32_t serial, HitArea edge);
    void        showWindowMenu(uint32_t serial, Point p);
    void        outputChanged(Output *o); // its scale may have changed: re-derive ours
    // xdg-foreign handle of the toplevel for portal dialogs ("" = none);
    // exported on first use and kept until the toplevel goes away.
    std::string exportHandle();

    const TextInputState &textInput() const { return _textInput; }
    wl_surface           *surface() const { return _surface; }
    Output               *output() const { return _outputs.empty() ? nullptr : _outputs.front(); }
    bool                  floating() const { return !_maximized && !_fullscreen && !_tiled; }

    // Listener entry points.
    void onConfigure(uint32_t serial);
    void onToplevelConfigure(int32_t w, int32_t h, wl_array *states);
    void onClose();
    void onDecorationMode(uint32_t mode);
    void onFractionalScale(uint32_t scale120);
    void onPreferredBufferScale(int32_t s);
    void onSurfaceEnter(wl_output *o);
    void onSurfaceLeave(wl_output *o);
    void onFrameDone();
    void onBufferRelease(ShmBuffer *b);
    void onExportHandle(const char *handle) { _exportHandle = handle ? handle : ""; }

    WlApp                                *app;
    std::chrono::steady_clock::time_point lastFrame{}; // when the last Frame went out
    // The app's answer to the current DropEnter/DropMove (setDropAction).
    DropAction                            dropReply = DropAction::Copy;

private:
    void updateScale();
    void emitResized(); // tells the app the size/scale it now has
    void createRole();
    void destroyRole();
    void pruneBuffers();
    Size clampToMin(Size s) const;

    wl_surface                  *_surface    = nullptr;
    xdg_surface                 *_xdgSurface = nullptr;
    xdg_toplevel                *_toplevel   = nullptr;
    zxdg_toplevel_decoration_v1 *_decoration = nullptr;
    wp_viewport                 *_viewport   = nullptr;
    wp_fractional_scale_v1      *_fractional = nullptr;
    wl_callback                 *_frameCb    = nullptr;
    zxdg_exported_v2            *_exportedV2 = nullptr;
    zxdg_exported_v1            *_exportedV1 = nullptr;
    std::string                  _exportHandle;
    // A token that arrived while unmapped: used once the first configure is acked.
    std::string                  _pendingToken;

    std::string           _title, _appId;
    Decorations           _decorationMode;
    bool                  _resizable;
    bool                  _serverDecorations = false;
    Size                  _size, _floatingSize, _minSize;
    Size                  _viewportSize{0, 0};
    int                   _bufferScaleSet = 1;
    double                _scale          = 1.0;
    // The scale the app last learnt: at creation, then from each Resized.
    // A scale that arrives before the first configure (mutter sends it on
    // the initial commit) goes out with that configure, even when the size
    // stays the same (issue #182: text laid out at 1x on a 1.75x canvas).
    double                _announcedScale = 1.0;
    uint32_t              _fractional120  = 0;
    int                   _preferredScale = 0;
    std::vector<Output *> _outputs; // outputs the surface is on, entry order

    // Pending xdg_toplevel.configure, applied on xdg_surface.configure.
    int32_t _pendingW = 0, _pendingH = 0;
    bool _pendingMax = false, _pendingFull = false, _pendingActive = false, _pendingTiled = false;

    bool _maximized = false, _fullscreen = false, _activated = false, _tiled = false;
    bool _visible          = false; // show() called and not hidden
    bool _configured       = false; // first configure of the current mapping acked
    bool _frameWanted      = false;
    bool _uncommittedAck   = false; // configure acked, no commit carried it yet
    bool _presentedInFrame = false;
    bool _callbackPaced    = false; // a frame callback came since the last Frame

    std::vector<std::unique_ptr<ShmBuffer>> _buffers;
    ShmBuffer                              *_painting = nullptr; // between beginPaint and endPaint
    ShmBuffer                              *_newest   = nullptr; // last presented

    Cursor                        _cursor = Cursor::Arrow;
    std::function<HitArea(Point)> _hitTest;
    TextInputState                _textInput;
};

// The loop, openUrl and testHooks(): linux_services::PosixServicesApp.
class WlApp final : public linux_services::PosixServicesApp {
public:
    WlApp();
    ~WlApp() override;
    bool init(std::string *error);

    const char             *backendName() const override { return "wayland"; }
    std::unique_ptr<Window> createWindow(const WindowDesc &desc) override;
    void                    run() override;
    void                    pump(int timeoutMs) override;

    void setClipboard(std::vector<DataItem> items, Selection sel) override;
    void requestClipboard(
        std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
    ) override;
    void
    requestClipboardMimes(std::function<void(std::vector<std::string>)> cb, Selection sel) override;
    bool startDrag(Window &source, const DragDesc &drag) override;

    // darkMode(), tray, notifications, badge: linux_services::ServicesApp.
    void emitThemeChanged() override;

#ifdef PLAT_TEST_HOOKS
    bool injectKey(Window &w, Key k, bool down) override;
    bool injectPointerMove(Window &w, Point logical) override;
    bool injectButton(Window &w, Button b, bool down) override;
    bool injectScroll(Window &w, double dx, double dy) override;
    bool readPixel(Window &w, int x, int y, uint32_t *argb) override;
    bool injectPhasedScroll(Window &w, double dx, double dy, ScrollPhase phase) override;
    // injectGesture / wantsAttention: no protocol for either (TestHooks defaults).
#endif

    std::vector<Monitor> monitors() const override;
    std::string          parentHandle(Window *w) override;

    void      forget(WlWindow *w);
    bool      alive(const WlWindow *w) const;
    WlWindow *windowFor(wl_surface *s) const;
    Output   *outputFor(wl_output *o) const;

    // ── registry / loop (wl_app.cpp) ────────────────────────────────────────
    void onGlobal(uint32_t name, const char *iface, uint32_t version);
    void onGlobalRemove(uint32_t name);

    // ── outputs (wl_output.cpp) ─────────────────────────────────────────────
    void bindOutput(uint32_t name, uint32_t version);
    void bindXdgOutput(Output *o);
    void removeOutput(uint32_t name);
    void destroyOutput(Output *o);
    void onOutputDone(Output *o);
    void scheduleMonitorsChanged();
    bool beforeWait();
    void afterWait(uint32_t displayReady);
    bool emitReadyFrames(); // true if some window still wants a frame later
    // Whether w's ready Frame may go out at `now`; else *waitMs until it may.
    bool frameDue(WlWindow *w, std::chrono::steady_clock::time_point now, int *waitMs) const;
    void fatal(const char *what);

    // ── seat (wl_seat.cpp) ──────────────────────────────────────────────────
    void bindSeat(uint32_t name, uint32_t version);
    void setupSeat();
    void onSeatCaps(uint32_t caps);
    void onPointerEnter(uint32_t serial, wl_surface *s, double x, double y);
    void onPointerLeave(uint32_t serial, wl_surface *s);
    void onPointerMotion(double x, double y);
    void flushMotion(); // emits the held PointerMove, if any
    void onPointerButton(uint32_t serial, uint32_t button, uint32_t state);
    void onPointerAxis(uint32_t axis, double value);
    void onPointerAxisSource(uint32_t source) { _axis.source = int(source); }
    void onPointerAxisDiscrete(uint32_t axis, int32_t discrete);
    void onPointerAxisValue120(uint32_t axis, int32_t v120);
    void onPointerAxisStop() { _axis.stop = true; }
    void onPointerFrame();
    void setupGestures();
    void destroyGestures();
    void onGestureBegin(Gesture g, wl_surface *s, uint32_t fingers);
    void onGestureUpdate(Gesture g, double dx, double dy);
    void onGestureEnd(Gesture g, bool cancelled);
    void onKeymap(uint32_t format, int fd, uint32_t size);
    void onKeyboardEnter(uint32_t serial, wl_surface *s);
    void onKeyboardLeave(uint32_t serial, wl_surface *s);
    void onKey(uint32_t serial, uint32_t key, uint32_t state);
    void onModifiers(uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group);
    void onRepeatInfo(int32_t rate, int32_t delay) { _repeatRate = rate, _repeatDelay = delay; }
    void emitKey(uint32_t keycode, bool down, bool repeat);
    void stopRepeat();
    void applyCursor(bool force = false);
    void syncTextInput();
    void onTextInputEnter(wl_surface *s);
    void onTextInputLeave(wl_surface *s);
    void onTextInputDone(uint32_t serial);
    void endPreedit();

    // ── data (wl_data.cpp) ──────────────────────────────────────────────────
    // What a peer offers: a wl_data_offer (clipboard or drag) or a
    // zwp_primary_selection_offer_v1, keyed by the proxy.
    struct Offer {
        std::vector<std::string> mimes;
        uint32_t                 sourceActions = 0; // wl_data_offer.source_actions (v3)
        uint32_t                 action        = 0; // DnD action the compositor settled on (v3)
    };
    using Items = std::shared_ptr<const std::vector<DataItem>>;
    void   setupDataDevice();
    void   onDataOffer(wl_data_offer *o);
    void   onSelection(wl_data_offer *o);
    void   onPrimaryOffer(zwp_primary_selection_offer_v1 *o);
    void   onPrimarySelection(zwp_primary_selection_offer_v1 *o);
    void   onDragEnter(uint32_t serial, wl_surface *s, double x, double y, wl_data_offer *o);
    void   onDragLeave();
    void   onDragMotion(double x, double y);
    void   onDrop();
    void   answerDrag(bool force);
    Event  dropEvent(EventType t, const Offer &of) const;
    void   onSourceSend(wl_data_source *src, const char *mime, int fd);
    void   onSourceCancelled(wl_data_source *src);
    void   onSourceDropPerformed(wl_data_source *src);
    void   onSourceFinished(wl_data_source *src);
    void   onSourceAction(wl_data_source *src, uint32_t action);
    void   onPrimarySourceSend(zwp_primary_selection_source_v1 *src, const char *mime, int fd);
    void   onPrimarySourceCancelled(zwp_primary_selection_source_v1 *src);
    void   endDrag(DropAction result);
    Offer *offerFor(wl_data_offer *o);
    void   dropOffer(wl_data_offer *o);
    void   dropPrimaryOffer(zwp_primary_selection_offer_v1 *o);
    // Writes the item matching `mime` into fd, off the loop (fd is taken).
    void   serve(const Items &items, const char *mime, int fd);
    // Reads everything the peer writes into a pipe, off the loop; `ask` hands
    // the pipe's write end to the peer (wl_data_offer.receive and friends).
    void   receive(
        const std::function<void(int)>                 &ask,
        int                                             timeoutMs,
        std::function<void(std::optional<std::string>)> done
    );

    // Dispatches only queue q (blocking, bounded) until pred holds.
    bool dispatchQueueUntil(wl_event_queue *q, const std::function<bool()> &pred, int ms);

#ifdef PLAT_TEST_HOOKS
    // ── test hooks (wl_testhooks.cpp) ───────────────────────────────────────
    bool ensureVirtualKeyboard(WlWindow &w);
    bool ensureVirtualPointer();
#endif

    // ── state ───────────────────────────────────────────────────────────────
    bool _reading = false, _dead = false;

    wl_display                              *display            = nullptr;
    wl_registry                             *registry           = nullptr;
    wl_compositor                           *compositor         = nullptr;
    uint32_t                                 compositorVersion  = 0;
    wl_shm                                  *shm                = nullptr;
    xdg_wm_base                             *wmBase             = nullptr;
    wl_seat                                 *seat               = nullptr;
    uint32_t                                 seatVersion        = 0;
    wl_data_device_manager                  *dataManager        = nullptr;
    uint32_t                                 dataManagerVersion = 0;
    wp_viewporter                           *viewporter         = nullptr;
    wp_fractional_scale_manager_v1          *fractionalManager  = nullptr;
    wp_cursor_shape_manager_v1              *cursorShapeManager = nullptr;
    zxdg_decoration_manager_v1              *decorationManager  = nullptr;
    xdg_activation_v1                       *activation         = nullptr;
    zwp_text_input_manager_v3               *textInputManager   = nullptr;
    zwp_pointer_gestures_v1                 *gesturesManager    = nullptr;
    zwp_primary_selection_device_manager_v1 *primaryManager     = nullptr;
    zxdg_output_manager_v1                  *xdgOutputManager   = nullptr;
    zxdg_exporter_v2                        *exporterV2         = nullptr;
    zxdg_exporter_v1                        *exporterV1         = nullptr;
    std::vector<std::unique_ptr<Output>>     outputs; // bind order

    // MonitorsChanged bookkeeping: nothing is emitted for the outputs that
    // exist at startup (init() sets _initDone), and a burst of done events
    // coalesces into one event that fires only if monitors() really changed.
    bool                 _initDone        = false;
    bool                 _monitorsPending = false;
    std::vector<Monitor> _lastMonitors;

    std::vector<WlWindow *> _windows;
    std::vector<WlWindow *> _frameWindows; // emitReadyFrames' scratch
    uint64_t                _displayWatch = 0;
    TimerId                 _frameTimer   = 0; // wakes the loop for a rate-limited frame

    // Latest serial of a key/button/enter: what set_selection and
    // xdg_activation want as proof of user interaction.
    uint32_t _inputSerial = 0;

    // Pointer.
    wl_pointer                *_pointer      = nullptr;
    wp_cursor_shape_device_v1 *_shapeDevice  = nullptr;
    WlWindow                  *_pointerFocus = nullptr;
    uint32_t                   _enterSerial  = 0;
    Point                      _pointerPos;
    // Motion is coalesced: the latest PointerMove waits here until another
    // event goes out or the dispatch batch ends, so a 1000 Hz mouse costs the
    // app (and the cursor hit test) one move per batch, not one per report.
    WlWindow                  *_motionWin = nullptr;
    Point                      _motionPos;
    uint32_t                   _motionMods  = 0;
    uint32_t                   _swallowed   = 0; // buttons whose press became a move/resize
    uint32_t                   _held        = 0; // delivered presses not yet released (Button bits)
    uint32_t                   _pressSerial = 0; // of the latest delivered press: start_drag
    WlWindow                  *_pressWindow = nullptr; // where that press went
    bool                       _scrollActive = false;  // a finger scroll is between Begin and End
    struct Axis {
        double dx = 0, dy = 0;       // wl_pointer.axis values (surface px)
        int    v120x = 0, v120y = 0; // high-res wheel, 120 per notch
        int    discX = 0, discY = 0; // legacy wheel notches
        bool   any = false, has120 = false, hasDiscrete = false, stop = false;
        int    source = -1;
    } _axis;
    core::ClickCounter _clicks;
    Cursor             _appliedCursor      = Cursor::Hidden;
    int                _appliedCursorScale = 0;
    bool               _cursorApplied      = false;
    wl_cursor_theme   *_cursorTheme        = nullptr;
    int                _cursorThemeScale   = 0;
    wl_surface        *_cursorSurface      = nullptr;

    // Touchpad swipes (zwp_pointer_gestures_v1).
    zwp_pointer_gesture_swipe_v1 *_swipe          = nullptr;
    WlWindow                     *_gestureWindow  = nullptr;
    int                           _gestureFingers = 0;

    // Keyboard.
    wl_keyboard             *_keyboard = nullptr;
    WlWindow                *_kbFocus  = nullptr;
    linux_input::XkbKeyboard _xkb;
    int32_t                  _repeatRate = 25, _repeatDelay = 600;
    uint32_t                 _repeatKey   = 0;
    TimerId                  _repeatTimer = 0;

    // Text input (IME).
    zwp_text_input_v3 *_textInput = nullptr;
    WlWindow          *_tiFocus   = nullptr;
    bool               _tiEnabled = false;
    Rect               _tiCaretSent{-1, -1, -1, -1};
    uint32_t           _tiCommits = 0;
    struct Preedit {
        std::string text;
        int         begin = -1, end = -1;
    } _preedit, _pendingPreedit;
    std::string _pendingCommit;

    // Data device and primary selection.
    wl_data_device                                   *_dataDevice    = nullptr;
    zwp_primary_selection_device_v1                  *_primaryDevice = nullptr;
    std::map<wl_data_offer *, Offer>                  _offers;
    std::map<zwp_primary_selection_offer_v1 *, Offer> _primaryOffers;
    wl_data_offer                                    *_selectionOffer = nullptr;
    zwp_primary_selection_offer_v1                   *_primaryOffer   = nullptr;
    // What we own, per Selection (null = another client owns it). Kept even
    // when the compositor refused set_selection, so an in-app paste works.
    Items                                             _clip[2];
    wl_data_source                                   *_source = nullptr; // our Clipboard selection
    zwp_primary_selection_source_v1 *_primarySource           = nullptr; // our Primary selection

    // Drop target: the drag currently over one of our windows.
    wl_data_offer        *_dragOffer  = nullptr;
    WlWindow             *_dragWindow = nullptr;
    uint32_t              _dragSerial = 0; // of wl_data_device.enter, for accept
    bool                  _dropping   = false;
    std::string           _dragMime;  // what we accept; empty = nothing usable
    std::vector<DataItem> _dragItems; // the entered offer's normalised types, data empty
    Point                 _dragPos;
    DropAction            _dragAnswered     = DropAction::None;
    bool                  _dragAnsweredOnce = false;

    // Drag source: our startDrag() in flight.
    struct DragSource {
        wl_data_source *source       = nullptr;
        wl_surface     *icon         = nullptr;
        wp_viewport    *iconViewport = nullptr; // maps a HiDPI image to its logical size
        ShmBuffer       iconBuffer;
        WlWindow       *window = nullptr;
        Items           items;
        uint32_t        action = 0; // wl_data_source.action: what the target will do
    };
    std::unique_ptr<DragSource> _drag;

    uint64_t _nextTransfer = 1;
    struct Transfer {
        int                                             fd    = -1;
        uint64_t                                        watch = 0;
        TimerId                                         timer = 0;
        std::shared_ptr<const std::string>              out; // writing
        size_t                                          off = 0;
        std::string                                     in; // reading
        std::function<void(std::optional<std::string>)> done;
    };
    std::map<uint64_t, Transfer> _transfers;
    void                         finishTransfer(uint64_t id, bool ok);

#ifdef PLAT_TEST_HOOKS
    // Test injection.
    zwp_virtual_keyboard_manager_v1 *vkbManager        = nullptr;
    zwlr_virtual_pointer_manager_v1 *vptrManager       = nullptr;
    zwlr_screencopy_manager_v1      *screencopyManager = nullptr;
    zwp_virtual_keyboard_v1         *_vkb              = nullptr;
    xkb_keymap                      *_vkbKeymap        = nullptr;
    uint32_t                         _vkbMods          = 0;
    zwlr_virtual_pointer_v1         *_vptr             = nullptr;
#endif
};

using core::isTextMime;

} // namespace plat::wl

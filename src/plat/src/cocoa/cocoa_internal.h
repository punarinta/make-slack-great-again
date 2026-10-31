// Cocoa (macOS) backend internals, shared by the .mm files in this directory.
// Objective-C++ with ARC: the NSObject members of the C++ classes below are
// __strong and released by the C++ destructors, so nothing here retains by hand
// except the CoreFoundation objects, which are released explicitly.
#pragma once

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurfaceRef.h>

#include "core/backends.h"
#include "core/loop_core.h"
#include "core/input.h"
#include "core/strings.h"
#include "core/transfer.h"
#ifdef PLAT_TEST_HOOKS
#include "plat/testing.h"
#endif

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

@class PlatWindowDelegate;
@class PlatFrameTarget;
@class PlatTrayTarget;
@class PlatServicesObserver;

namespace plat::cocoa {
class CocoaApp;
class CocoaWindow;

// When the display link has not produced a requested Frame by then, the
// fallback timer does. Long enough that a healthy link always wins (it ticks
// within one refresh), short enough that a sleeping display does not stall
// an app waiting for its first Frame.
constexpr CFTimeInterval kFrameFallback = 0.1;
} // namespace plat::cocoa

// The content view of every plat window (cocoa_window.mm). Also the drag
// source of startDrag() sessions and the drop target of its window.
@interface PlatView : NSView <NSTextInputClient, NSDraggingSource>
@property(nonatomic, assign) plat::cocoa::CocoaWindow *owner;
- (void)discardComposition; // text input turned off mid-composition
- (void)button:(plat::Button)b down:(bool)down event:(NSEvent *)ev; // press/release handler
@end

namespace plat::cocoa {

// UTF-8 → NSString; @"" (never nil) for bytes that are not UTF-8 (cocoa_data.mm).
NSString *nsString(std::string_view s);

// ── Keyboard (cocoa_keys.mm) ────────────────────────────────────────────────
// The logical Key for a macOS virtual key code (kVK_*): letters and
// punctuation resolve through the active layout (Dvorak, AZERTY, …), falling
// back to the ASCII-capable layout and then the US position (plat.h's rule).
Key      keyFromKeyCode(uint16_t vk);
uint32_t modsFromFlags(NSEventModifierFlags flags);
#ifdef PLAT_TEST_HOOKS
// Reverse of keyFromKeyCode for the active layout; -1 when the key has none.
int       keyCodeForKey(Key k);
// What the key types on the active layout with these modifiers, dead keys
// off — used to fill synthesised NSEvents the way the window server would.
NSString *charactersForKeyCode(uint16_t vk, NSEventModifierFlags flags);
#endif
// Drop the cached layout table (the user switched input source).
void invalidateKeyboardLayout();

// ── Data transfer (cocoa_data.mm): clipboard, drag source, drop target ─────
// The pasteboard type a MIME type travels as: the system UTI for the
// standard ones (public.utf8-plain-text, public.html, public.png, …), the
// UTType for any MIME type macOS knows, else a dynamic UTI that encodes the
// MIME string (dyn.…), which other apps can map back to it. Never
// text/uri-list: URIs are one pasteboard item each (pasteboardItems()).
NSPasteboardType pasteboardTypeForMime(std::string_view mime);
// Reverse: the plat MIME name for a pasteboard type, "" when it has none
// (legacy NeXT/Apple types, private UTIs without a MIME mapping).
std::string      mimeForPasteboardType(NSPasteboardType type);
using core::isTextMime;
// Pasteboard items for a multi-type selection: the first carries every
// representation plus the first URI, each further URI gets its own item.
NSArray<NSPasteboardItem *> *pasteboardItems(const std::vector<DataItem> &items);
// The MIME types a pasteboard offers, normalised and deduplicated.
std::vector<std::string>     pasteboardMimes(NSPasteboard *pb);
// One representation read back; uri-list gathers every item's URL.
std::optional<std::string>   readPasteboard(NSPasteboard *pb, std::string_view mime);
// The TIFF behind a request for `mime` when that is image/png and the
// picture is on the pasteboard as TIFF only (readPasteboard converts), else nil.
NSData                      *tiffOnlyPicture(NSPasteboard *pb, std::string_view mime);
// TIFF → PNG bytes. Touches no pasteboard, so it runs on any thread.
std::optional<std::string>   pngFromTiff(NSData *tiff);
// file:// URIs etc. of every item (file references resolved to paths).
std::vector<std::string>     pasteboardUris(NSPasteboard *pb);
// plat Image (premultiplied ARGB32) → CGImage in sRGB; null when empty.
CGImageRef                   createCGImage(const Image &img);
// … → NSImage of `points` logical size, with a bitmap rep per pixel size.
NSImage                     *nsImage(const Image &img, NSSize points);

// ── Screens (cocoa_services.mm) ────────────────────────────────────────────
// plat's virtual desktop is Cocoa's global space flipped: top-left of the
// primary screen (screens[0], the one with the menu bar) is 0,0 and y grows
// down, in points. Rect conversions in both directions:
Rect     flipRect(NSRect cocoa);
NSRect   unflipRect(Rect r);
uint64_t screenId(NSScreen *s); // CGDirectDisplayID, 0 for nil

// ── Drop actions ───────────────────────────────────────────────────────────
uint32_t        dropActionsFromOperation(NSDragOperation op); // mask → DropActions
NSDragOperation operationFromActions(uint32_t actions);
DropAction      dropActionFromOperation(NSDragOperation op); // one result → action
NSDragOperation operationFromAction(DropAction a);

// ── Window (cocoa_window.mm) ────────────────────────────────────────────────

// One IOSurface the window paints into and hands to its layer. A window keeps
// a small ring of these; `stale` tracks which parts of it are older than the
// newest presented frame, so a partial repaint can start from correct pixels.
struct CocoaSurface {
    IOSurfaceRef      surface = nullptr;
    int               width = 0, height = 0; // physical
    bool              staleAll = true;
    std::vector<Rect> stale; // physical rects newer frames painted that this one lacks
};

class CocoaWindow final : public Window {
public:
    CocoaWindow(CocoaApp *app, const WindowDesc &desc);
    ~CocoaWindow() override;

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
    bool                 isMaximized() const override;
    bool                 isFullscreen() const override;
    bool                 isMinimized() const override;
    bool                 supportsAlwaysOnTop() const override { return true; }
    void                 setAlwaysOnTop(bool on) override;
    bool                 isAlwaysOnTop() const override;
    void                 setDarkChrome(bool dark) override;
    double               titleBarHeight() const override;
    bool                 isActive() const override;
    void                 activate() override;
    std::optional<Point> position() const override;
    bool                 setPosition(Point logical) override;
    uint64_t             monitor() const override;
    void                 setCursor(Cursor c) override;
    void   setHitTest(std::function<HitArea(Point)> fn) override { hitTest = std::move(fn); }
    bool   hasSystemDecorations() const override { return decorations == Decorations::System; }
    void   setTextInput(const TextInputState &s) override;
    void   requestFrame() override;
    Canvas beginPaint() override;
    void   endPaint(const std::vector<Rect> &damage) override;
    void  *nativeHandle() const override { return (__bridge void *)window; }
    void   setDropAction(DropAction a) override { dropReply = a; }
    void   requestAttention() override;

    // ── called by PlatView / PlatWindowDelegate / CocoaApp ──────────────────
    void  emit(Event e);
    void  viewResized();       // frame size changed
    void  backingChanged();    // backingScaleFactor / screen changed
    void  deliverFrameIfDue(); // display link tick or fallback timer
    void  stateMaybeChanged(); // zoom / miniaturize / fullscreen notifications
    void  applyCursor();       // the pointer is over us: make _cursor current
    void  pointerLeft();
    void  moved();                      // windowDidMove:
    Point viewPoint(NSEvent *ev) const; // logical, top-left origin

    // The surface the layer shows (nullptr before the first paint).
    IOSurfaceRef presentedSurface() const {
        return _front >= 0 ? _surfaces[_front].surface : nullptr;
    }

    CocoaApp                     *app;
    NSWindow                     *window   = nil;
    PlatView                     *view     = nil;
    PlatWindowDelegate           *delegate = nil;
    Decorations                   decorations;
    std::function<HitArea(Point)> hitTest;
    TextInputState                textInput;
    bool                          framePending     = false;
    CFAbsoluteTime                frameRequestedAt = 0; // oldest unanswered requestFrame
    bool                          pointerInside    = false;
    bool                          swallowUp[5]     = {};               // press was non-client
    DropAction                    dropReply        = DropAction::Copy; // answer to DropEnter/Move
    // The press/drag that a startDrag() from a PointerDown/Move handler
    // hands to AppKit's drag session; cleared on release.
    NSEvent                      *lastMouseEvent   = nil;
    uint32_t                      dragActions      = 0;     // of the running startDrag session
    bool                          dragging         = false; // a startDrag session is running

private:
    void startFrameClock();

    id               _displayLink  = nil; // CADisplayLink (macOS 14+), paused when idle
    PlatFrameTarget *_frameTarget  = nil;
    Cursor           _cursor       = Cursor::Arrow;
    bool             _cursorHidden = false; // we owe NSCursor one unhide
    Size             _lastSize{};
    double           _lastScale  = 0;
    bool             _lastZoomed = false, _lastMini = false, _lastFull = false;

    // Paint target ring; the canvas is the locked surface, so no separate
    // back buffer exists.
    static constexpr int kMaxSurfaces = 4;
    CocoaSurface         _surfaces[kMaxSurfaces];
    int                  _front    = -1; // on the layer: the newest presented frame
    int                  _painting = -1; // locked, between beginPaint and endPaint
};

// ── Tray (cocoa_tray.mm) ────────────────────────────────────────────────────
class CocoaTray final : public Tray {
public:
    explicit CocoaTray(CocoaApp *app);
    ~CocoaTray() override;

    void setIcon(const std::vector<Image> &sizes) override;
    void setTemplate(bool on) override { templ = on; }
    void setTooltip(std::string_view utf8) override;
    void setMenu(std::vector<MenuItem> items) override;
    bool isVisible() const override;

    void clicked();           // the status item's button fired (no menu set)
    void chosen(uint32_t id); // a menu item's action fired

    CocoaApp       *app;
    NSStatusItem   *item   = nil;
    PlatTrayTarget *target = nil;
    NSMenu         *menu   = nil; // nil = a click is TrayActivated
    bool            templ  = false;
};

// ── App (cocoa_app.mm) ──────────────────────────────────────────────────────
#ifdef PLAT_TEST_HOOKS
class CocoaApp final : public BackendApp, public TestHooks {
#else
class CocoaApp final : public BackendApp {
#endif
public:
    CocoaApp();
    ~CocoaApp() override;

    const char *backendName() const override { return "cocoa"; }

    std::unique_ptr<Window> createWindow(const WindowDesc &desc) override;
    void                    run() override;
    void                    quit() override;
    void                    pump(int timeoutMs) override;
    void                    post(std::function<void()> fn) override { _core.post(std::move(fn)); }
    TimerId  addTimer(int intervalMs, bool repeat, std::function<void()> fn) override;
    void     cancelTimer(TimerId id) override;
    uint64_t watchFd(int fd, uint32_t events, std::function<void(uint32_t)> fn) override;
    void     unwatchFd(uint64_t id) override;

    void setClipboard(std::vector<DataItem> items, Selection sel) override;
    void requestClipboard(
        std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
    ) override;
    void
    requestClipboardMimes(std::function<void(std::vector<std::string>)> cb, Selection sel) override;
    bool startDrag(Window &source, const DragDesc &drag) override;

    std::unique_ptr<Tray> createTray() override;
    bool                  notificationsAvailable() const override;
    bool                  notify(uint64_t id, const Notification &n) override;
    void                  setBadgeCount(int count) override;

    bool darkMode() const override;
    int  doubleClickMs() const override;
    bool openUrl(std::string_view url) override;

    // Round 3 (cocoa_services.mm).
    std::vector<Monitor> monitors() const override;
    bool claimSingleInstance(std::string_view key, const std::vector<std::string> &args) override;
    bool registerUrlScheme(std::string_view scheme) override;
    std::optional<bool> networkOnline() const override { return _online; }
    void                showFileDialog(
        const FileDialogDesc &d, std::function<void(std::vector<std::string>)> cb
    ) override;
    std::string              standardDir(StandardDir d) const override;
    SystemSettings           systemSettings() const override;
    std::vector<std::string> preferredLanguages() const override;

#ifdef PLAT_TEST_HOOKS
    TestHooks *testHooks() override { return this; }
    bool       injectKey(Window &w, Key k, bool down) override;
    bool       injectPointerMove(Window &w, Point logical) override;
    bool       injectButton(Window &w, Button b, bool down) override;
    bool       injectScroll(Window &w, double dx, double dy) override;
    bool       readPixel(Window &w, int x, int y, uint32_t *argb) override;
    bool       readsComposited() const override { return CGPreflightScreenCaptureAccess(); }
    bool       injectPhasedScroll(Window &w, double dx, double dy, ScrollPhase phase) override;
    // injectGesture stays the base's false: a swipe NSEvent can only be
    // built through private CGEvent types.
    bool       trayActivate(Tray &t) override;
    bool       trayMenuSelect(Tray &t, uint32_t itemId) override;
    bool       trayProbe(Tray &t, TrayProbe *out) override;
    // notificationInvoke stays false: no public API clicks a banner.
    bool       notificationProbe(uint64_t id, NotificationProbe *out) override;
    int        badgeCount() override;
    // wantsAttention stays false: AppKit has no getter for a pending
    // requestUserAttention: (the Dock bounce is not observable).
    bool       fileDialogRespond(std::vector<std::string> paths) override;
    bool       simulateSystemEvent(EventType type, bool online) override;
    bool       deliverUrl(std::string_view url) override;
#endif

    // ── backend-internal ────────────────────────────────────────────────────
    void emitEvent(const Event &e) { emit(e); }
    void forget(CocoaWindow *w);
    // Something ran from a run-loop callback (timer, post, fd, display link):
    // make a pump() blocked in -nextEventMatchingMask: return.
    void noteWork();
    void scheduleFrameFallback(CFAbsoluteTime due); // a Frame was requested
    void onTimerFired();
    void onPosted();
    void onFrameFallback();
    void onFd(uint64_t id, uint32_t events);
    void onThemeChanged();
    void requestQuit(); // Cmd+Q / Dock "Quit" / logout: QuitRequested
    // The NSApplication launch (menu bar, Dock icon, activation) happens on
    // first need — a window, tray, dialog or pump — so a second instance
    // that only calls claimSingleInstance() and exits never shows up in the
    // Dock.
    void ensureLaunched();
    // cocoa_services.mm: set up in the constructor / torn down in the destructor.
    void setUpServices();
    void tearDownServices();
    void installUrlHandler(); // kAEGetURL → onUrls; again once AppKit launched
    void onUrls(std::vector<std::string> urls);
    void onReopen(); // Dock click / LaunchServices relaunch of the running bundle
    void onSystemEvent(EventType t);
    void onNetwork(bool online);
    void onInstanceAccept();
    void onInstanceData(int fd);
#ifdef PLAT_TEST_HOOKS
    void confirmPanel(NSSavePanel *panel, bool sheet, bool accept); // fileDialogRespond driver
#endif
    // Notification delegate callbacks (cocoa_notify.mm), on the loop thread.
    void onNotificationResponse(uint64_t id, std::string action, bool dismissed);
    void onNotificationFailed(uint64_t id, std::string reason);

    std::vector<CocoaWindow *> windows;

    // UNUserNotificationCenter's blocks run on its own queues and reach the
    // loop through this, which the destructor clears so a late completion
    // finds no app to post to.
    struct Alive {
        std::mutex mutex;
        CocoaApp  *app = nullptr;
    };

    struct FdInfo { // CFFileDescriptor context: which watch fired
        CocoaApp *app;
        uint64_t  id;
    };

private:
    void rescheduleTimer();
    void dispatch(NSEvent *ev);
#ifdef PLAT_TEST_HOOKS
    bool deliverInjected(NSEvent *ev);
#endif

    struct FdWatch {
        CFFileDescriptorRef           cf        = nullptr;
        CFRunLoopSourceRef            src       = nullptr;
        CFOptionFlags                 callbacks = 0;
        std::function<void(uint32_t)> fn;
        std::unique_ptr<FdInfo>       info;
    };

    core::LoopCore              _core;
    CFRunLoopSourceRef          _postSource     = nullptr;
    CFRunLoopTimerRef           _timer          = nullptr; // LoopCore's next due timer
    CFRunLoopTimerRef           _frameFallback  = nullptr; // Frames when no display link ticks
    id                          _delegate       = nil;
    id                          _themeObserver  = nil;
    id                          _layoutObserver = nil;
    std::map<uint64_t, FdWatch> _fds;
    uint64_t                    _nextFd     = 1;
    int                         _waiting    = 0; // pump() frames blocked in nextEvent
    bool                        _wakePosted = false;
    bool                        _quit       = false;
    bool                        _launching  = false; // inside ensureLaunched()'s -run

    // Round 3 services (cocoa_services.mm).
    PlatServicesObserver    *_services    = nil;
    id                       _pathMonitor = nil; // nw_path_monitor_t
    std::optional<bool>      _online;
    std::vector<std::string> _schemes; // registerUrlScheme()'d, lower case
    // Single instance: the listening socket of the primary and the
    // connections of second instances still sending their arguments.
    int                      _instanceLock  = -1; // flock held while primary
    int                      _instanceFd    = -1;
    uint64_t                 _instanceWatch = 0;
    std::string              _instancePath, _instanceLockPath;
    std::string              _instanceKey;
    struct InstanceConn {
        uint64_t    watch = 0;
        std::string buf;
    };
    std::map<int, InstanceConn> _instanceConns;
#ifdef PLAT_TEST_HOOKS
    // fileDialogRespond(): the answer for the next showFileDialog().
    std::optional<std::vector<std::string>> _dialogAnswer;
    id                                      _dialogDriver = nil; // NSTimer driving the panel
#endif

    // Notifications (cocoa_notify.mm).
    bool                   setUpNotifications() const; // lazily, once
    std::string            notificationIdentifier(uint64_t id) const;
    void                   tearDownNotifications();
    std::shared_ptr<Alive> _alive          = std::make_shared<Alive>();
    mutable int            _notifyState    = 0; // 0 unknown, 1 usable, -1 unavailable
    mutable id             _notifyDelegate = nil;
    std::set<uint64_t>     _liveNotifications; // submitted, not yet clicked or dismissed
#ifdef PLAT_TEST_HOOKS
    // Attached picture per live id: its PNG's SHA-1 (hex) and pixel size,
    // for notificationProbe (the center's copy is not readable by us).
    std::map<uint64_t, std::pair<std::string, Size>> _notifyImages;

    // Test-injection state: what the window server would track for real input.
    NSEventModifierFlags _injFlags                    = 0;
    bool                 _injDown[size_t(Key::Count)] = {};
    Point                _injPointer;
    int                  _injButtons    = 0; // bitmask of held Buttons
    double               _injLastPress  = 0;
    Button               _injLastButton = Button::Left;
    int                  _injClicks     = 0;
#endif
};

} // namespace plat::cocoa

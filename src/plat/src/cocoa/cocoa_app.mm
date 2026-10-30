// Cocoa (macOS) backend: NSApplication, the event loop and app-wide services.
//
// Loop design. pump() blocks in -nextEventMatchingMask:untilDate: (the one
// wait that also services the window server) and dispatches with sendEvent:.
// Everything else — LoopCore timers, post()ed closures, fd watches, display
// link ticks — runs from CoreFoundation run-loop sources registered in
// kCFRunLoopCommonModes, so it keeps running inside AppKit's own modal
// tracking loops (live resize, window drag, menu tracking), which spin the
// run loop in NSEventTrackingRunLoopMode while pump() is stuck in sendEvent:.
// When one of those sources fires while pump() is waiting, it posts an
// application-defined "wake" NSEvent so -nextEventMatchingMask: returns and
// pump() keeps the "dispatch what is ready, then return" contract.
#include "cocoa/cocoa_internal.h"

#import <Carbon/Carbon.h> // kTISNotifySelectedKeyboardInputSourceChanged
#import <QuartzCore/QuartzCore.h>

#ifdef PLAT_TEST_HOOKS
#include <dlfcn.h> // readPixel
#endif

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstring>

using plat::cocoa::CocoaApp;

namespace {
constexpr short          kWakeSubtype = 0x504c; // 'PL'
constexpr NSInteger      kWakeMagic   = 0x706c6174;
constexpr CFTimeInterval kNever       = 1.0e10; // "far future" for idle CF timers

// The application-defined event that makes -nextEventMatchingMask: return.
NSEvent *wakeEvent() {
    return [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                              location:NSZeroPoint
                         modifierFlags:0
                             timestamp:0
                          windowNumber:0
                               context:nil
                               subtype:kWakeSubtype
                                 data1:kWakeMagic
                                 data2:0];
}
} // namespace

// ── NSApplication delegate ─────────────────────────────────────────────────
@interface                             PlatAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, assign) CocoaApp *owner;
@end

@implementation PlatAppDelegate
- (void)applicationWillFinishLaunching:(NSNotification *)n {
    // Apple's documented place for Apple Event handlers: after AppKit set up
    // its own, before the launch's GetURL/odoc events are dispatched.
    if (_owner)
        _owner->installUrlHandler();
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible {
    // A LaunchServices launch of the already running bundle (Finder, `open`,
    // Dock click) starts no second process: AppKit sends this instead.
    if (_owner)
        _owner->onReopen();
    return NO; // the app decides what to show
}
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    // createCocoaApp runs [NSApp run] only to get through launch (menu bar,
    // activation, Dock); stop it at once and hand the loop to pump().
    [NSApp stop:nil];
    [NSApp postEvent:wakeEvent() atStart:YES]; // -stop: only takes effect after an event
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    // Cmd+Q, Dock "Quit" and logout all land here. The app is asked to
    // quit as a whole (QuitRequested), not its windows to close: a close
    // button may only hide a window. It quits through plat's own loop, so
    // AppKit's termination is cancelled. (Follow-up: logout should get
    // NSTerminateLater and a reply.)
    if (_owner)
        _owner->requestQuit();
    return NSTerminateCancel;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return NO;
}
@end

// KVO target for NSApp.effectiveAppearance (the reliable dark-mode signal;
// the AppleInterfaceThemeChanged distributed note fires before it updates).
@interface                             PlatThemeObserver : NSObject
@property(nonatomic, assign) CocoaApp *owner;
@end
@implementation PlatThemeObserver
- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary *)change
                       context:(void *)context {
    if (_owner)
        _owner->onThemeChanged();
}
@end

namespace plat::cocoa {

namespace {

bool g_launched = false;

bool isWake(NSEvent *ev) {
    return ev.type == NSEventTypeApplicationDefined && ev.subtype == kWakeSubtype &&
           ev.data1 == kWakeMagic;
}

void buildMainMenu() {
    NSString   *name    = NSProcessInfo.processInfo.processName;
    NSMenu     *bar     = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *m = [NSMenu new];
    [m addItemWithTitle:[@"Hide " stringByAppendingString:name]
                 action:@selector(hide:)
          keyEquivalent:@"h"];
    NSMenuItem *others               = [m addItemWithTitle:@"Hide others"
                                                    action:@selector(hideOtherApplications:)
                                             keyEquivalent:@"h"];
    others.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [m addItemWithTitle:@"Show all" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    // terminate: → applicationShouldTerminate: → QuitRequested; never exit().
    [m addItemWithTitle:[@"Quit " stringByAppendingString:name]
                 action:@selector(terminate:)
          keyEquivalent:@"q"];
    appItem.submenu = m;
    NSApp.mainMenu  = bar;
}

void timerCallback(CFRunLoopTimerRef, void *info) {
    static_cast<CocoaApp *>(info)->onTimerFired();
}
void frameCallback(CFRunLoopTimerRef, void *info) {
    static_cast<CocoaApp *>(info)->onFrameFallback();
}
void postedCallback(void *info) {
    static_cast<CocoaApp *>(info)->onPosted();
}

using FdInfo = CocoaApp::FdInfo;
void fdCallback(CFFileDescriptorRef, CFOptionFlags types, void *info) {
    auto    *fi = static_cast<FdInfo *>(info);
    uint32_t ev = 0;
    if (types & kCFFileDescriptorReadCallBack)
        ev |= FdRead;
    if (types & kCFFileDescriptorWriteCallBack)
        ev |= FdWrite;
    fi->app->onFd(fi->id, ev);
}

} // namespace

CocoaApp::CocoaApp() {
    [NSApplication sharedApplication];
    PlatAppDelegate *d = [PlatAppDelegate new];
    d.owner            = this;
    _delegate          = d;
    NSApp.delegate     = d;

    _alive->app = this;

    CFRunLoopRef rl = CFRunLoopGetMain();

    CFRunLoopSourceContext sc{};
    sc.info     = this;
    sc.perform  = postedCallback;
    _postSource = CFRunLoopSourceCreate(nullptr, 0, &sc);
    CFRunLoopAddSource(rl, _postSource, kCFRunLoopCommonModes);
    _core.wake = [src = _postSource, rl] {
        // Both calls are documented thread-safe.
        CFRunLoopSourceSignal(src);
        CFRunLoopWakeUp(rl);
    };

    CFRunLoopTimerContext tc{};
    tc.info = this;
    // Repeating with a huge interval so firing never invalidates it; the
    // real schedule is set with CFRunLoopTimerSetNextFireDate.
    _timer  = CFRunLoopTimerCreate(
        nullptr, CFAbsoluteTimeGetCurrent() + kNever, kNever, 0, 0, timerCallback, &tc
    );
    CFRunLoopAddTimer(rl, _timer, kCFRunLoopCommonModes);
    _frameFallback = CFRunLoopTimerCreate(
        nullptr, CFAbsoluteTimeGetCurrent() + kNever, kNever, 0, 0, frameCallback, &tc
    );
    CFRunLoopAddTimer(rl, _frameFallback, kCFRunLoopCommonModes);

    PlatThemeObserver *obs = [PlatThemeObserver new];
    obs.owner              = this;
    _themeObserver         = obs;
    [NSApp addObserver:obs forKeyPath:@"effectiveAppearance" options:0 context:nullptr];

    _layoutObserver = [NSDistributedNotificationCenter.defaultCenter
        addObserverForName:(__bridge NSString *)kTISNotifySelectedKeyboardInputSourceChanged
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification *) {
                  invalidateKeyboardLayout();
                }];

    setUpServices();
}

CocoaApp::~CocoaApp() {
    _core.shutdown(); // pending closures go while the post source still exists
    tearDownServices();
    tearDownNotifications();
    for (auto &[id, w] : _fds) {
        CFFileDescriptorInvalidate(w.cf);
        CFRelease(w.src);
        CFRelease(w.cf);
    }
    _fds.clear();
    [NSApp removeObserver:_themeObserver forKeyPath:@"effectiveAppearance"];
    ((PlatThemeObserver *)_themeObserver).owner = nullptr;
    [NSDistributedNotificationCenter.defaultCenter removeObserver:_layoutObserver];
    ((PlatAppDelegate *)_delegate).owner = nullptr;
    _core.wake                           = nullptr;
    CFRunLoopSourceInvalidate(_postSource);
    CFRelease(_postSource);
    CFRunLoopTimerInvalidate(_timer);
    CFRelease(_timer);
    CFRunLoopTimerInvalidate(_frameFallback);
    CFRelease(_frameFallback);
}

void CocoaApp::ensureLaunched() {
    if (g_launched)
        return;
    g_launched = true;
    @autoreleasepool {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        // plat windows have no restorable state (CocoaWindow sets
        // restorable = NO). Even so, once a bundled app has crashed AppKit
        // asks on the next launch whether to "reopen windows" — an alert
        // that runs modally inside -run below and blocks the launch until
        // someone clicks it (seen on a locked Mac: the selftest hung there
        // after a crash, with no saved state on disk). Ignoring persistent
        // state skips it; the price is one NSLog line per launch.
        // Registration domain only: a user's own setting still wins.
        [NSUserDefaults.standardUserDefaults registerDefaults:@{
            @"ApplePersistenceIgnoreState" : @YES,
            @"NSQuitAlwaysKeepsWindows" : @NO,
        }];
        buildMainMenu();
        // Only -run performs the full launch dance (menu bar ownership,
        // becoming a foreground app, Dock icon); the delegate stops it again
        // as soon as launching finishes.
        if (!NSRunningApplication.currentApplication.isFinishedLaunching) {
            // -run spins the run loop, where our CF sources live: hold back
            // posted closures and timers (callers expect them from pump()
            // only, not from inside createWindow()) and re-arm them after.
            _launching = true;
            [NSApp run];
            _launching = false;
            CFRunLoopSourceSignal(_postSource);
            rescheduleTimer();
        }
        installUrlHandler(); // in case AppKit's launch replaced ours
    }
}

std::unique_ptr<Window> CocoaApp::createWindow(const WindowDesc &d) {
    ensureLaunched();
    @autoreleasepool {
        auto w = std::make_unique<CocoaWindow>(this, d);
        windows.push_back(w.get());
        // A process started from a terminal is not frontmost; the first window
        // of an app should be, like a double-clicked app bundle.
        if (d.visible && windows.size() == 1)
            w->activate();
        return w;
    }
}

void CocoaApp::forget(CocoaWindow *w) {
    windows.erase(std::remove(windows.begin(), windows.end(), w), windows.end());
}

// ── loop ────────────────────────────────────────────────────────────────────

void CocoaApp::run() {
    _quit = false;
    while (!_quit)
        pump(-1);
}

void CocoaApp::quit() {
    _quit = true;
    noteWork();
}

void CocoaApp::noteWork() {
    if (_waiting > 0 && !_wakePosted) {
        _wakePosted = true;
        [NSApp postEvent:wakeEvent() atStart:YES];
    }
}

void CocoaApp::pump(int timeoutMs) {
    ensureLaunched();
    @autoreleasepool {
        _wakePosted     = false;
        const int t     = _core.clampTimeout(timeoutMs);
        NSDate   *until = t < 0    ? NSDate.distantFuture
                          : t == 0 ? NSDate.distantPast
                                   : [NSDate dateWithTimeIntervalSinceNow:t / 1000.0];
        ++_waiting;
        NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                         untilDate:until
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:YES];
        --_waiting;
        // Drain what else is queued without blocking, bounded so a flood of
        // motion cannot keep pump() from returning.
        for (int n = 0; ev; ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:NSDate.distantPast
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES]) {
            dispatch(ev);
            if (++n == 64)
                break;
        }
        // The CF sources normally ran all of this already; calling in again
        // is idempotent and keeps pump() honest if the run loop was in
        // another mode when they fired.
        _core.runPosted();
        _core.runDueTimers();
        rescheduleTimer();
        _wakePosted = false;
    }
}

void CocoaApp::dispatch(NSEvent *ev) {
    if (isWake(ev))
        return;
    const bool key = ev.type == NSEventTypeKeyDown || ev.type == NSEventTypeKeyUp ||
                     ev.type == NSEventTypeFlagsChanged;
    if (key && ev.window && ev.window != NSApp.keyWindow) {
        // Key events aimed at one of our windows that is not key (only
        // injected ones: the window server sends real keys to the key
        // window); NSApp would drop them.
        [ev.window sendEvent:ev];
        return;
    }
    if (ev.type == NSEventTypeKeyUp && (ev.modifierFlags & NSEventModifierFlagCommand)) {
        // -[NSApplication sendEvent:] swallows key-ups while Command is held
        // (a menu-shortcut legacy); send them to the window ourselves so
        // Cmd+C gets its KeyUp like every other chord.
        [(ev.window ?: NSApp.keyWindow) sendEvent:ev];
        return;
    }
    [NSApp sendEvent:ev];
}

TimerId CocoaApp::addTimer(int ms, bool repeat, std::function<void()> fn) {
    const TimerId id = _core.addTimer(ms, repeat, std::move(fn));
    rescheduleTimer();
    return id;
}

void CocoaApp::cancelTimer(TimerId id) {
    _core.cancelTimer(id);
    rescheduleTimer();
}

void CocoaApp::rescheduleTimer() {
    const int ms = _core.msUntilNextTimer();
    CFRunLoopTimerSetNextFireDate(
        _timer, CFAbsoluteTimeGetCurrent() + (ms < 0 ? kNever : ms / 1000.0)
    );
}

void CocoaApp::onTimerFired() {
    if (_launching)
        return; // ensureLaunched() re-arms
    _core.runDueTimers();
    rescheduleTimer();
    noteWork();
}

void CocoaApp::onPosted() {
    if (_launching)
        return; // ensureLaunched() signals again
    _core.runPosted();
    rescheduleTimer(); // the closures may have added timers through LoopCore
    // Work posted by those closures signals the source again by itself.
    noteWork();
}

void CocoaApp::scheduleFrameFallback(CFAbsoluteTime due) {
    // Only ever moves the deadline earlier; one timer serves every window.
    if (CFRunLoopTimerGetNextFireDate(_frameFallback) > due)
        CFRunLoopTimerSetNextFireDate(_frameFallback, due);
}

void CocoaApp::onFrameFallback() {
    CFRunLoopTimerSetNextFireDate(_frameFallback, CFAbsoluteTimeGetCurrent() + kNever);
    const CFAbsoluteTime now  = CFAbsoluteTimeGetCurrent();
    CFAbsoluteTime       next = now + kNever;
    for (auto *w : std::vector<CocoaWindow *>(windows)) {
        if (std::find(windows.begin(), windows.end(), w) == windows.end())
            continue; // destroyed by an earlier Frame handler
        if (!w->framePending || !w->window.isVisible)
            continue; // show() asks again
        // Only requests the display link has left unanswered for a while:
        // with a live display it always wins, and the fallback adds nothing.
        const CFAbsoluteTime due = w->frameRequestedAt + kFrameFallback;
        if (due <= now + 0.001)
            w->deliverFrameIfDue();
        else
            next = std::min(next, due);
    }
    scheduleFrameFallback(next);
}

// ── fd watches ──────────────────────────────────────────────────────────────

uint64_t CocoaApp::watchFd(int fd, uint32_t events, std::function<void(uint32_t)> fn) {
    const uint64_t          id   = _nextFd++;
    auto                    info = std::make_unique<FdInfo>(FdInfo{this, id});
    CFFileDescriptorContext ctx{0, info.get(), nullptr, nullptr, nullptr};
    // closeOnInvalidate false: the fd belongs to the caller.
    CFFileDescriptorRef     cf = CFFileDescriptorCreate(nullptr, fd, false, fdCallback, &ctx);
    if (!cf)
        return 0;
    FdWatch w;
    w.info      = std::move(info);
    w.cf        = cf;
    w.callbacks = ((events & FdRead) ? kCFFileDescriptorReadCallBack : 0) |
                  ((events & FdWrite) ? kCFFileDescriptorWriteCallBack : 0);
    w.fn        = std::move(fn);
    w.src       = CFFileDescriptorCreateRunLoopSource(nullptr, cf, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), w.src, kCFRunLoopCommonModes);
    CFFileDescriptorEnableCallBacks(cf, w.callbacks);
    _fds.emplace(id, std::move(w));
    return id;
}

void CocoaApp::unwatchFd(uint64_t id) {
    auto it = _fds.find(id);
    if (it == _fds.end())
        return;
    CFFileDescriptorInvalidate(it->second.cf); // also removes the source; fd stays open
    CFRelease(it->second.src);
    CFRelease(it->second.cf);
    _fds.erase(it);
}

void CocoaApp::onFd(uint64_t id, uint32_t events) {
    auto it = _fds.find(id);
    if (it == _fds.end())
        return;
    auto fn = it->second.fn; // the callback may unwatch (destroy) itself
    fn(events);
    // CFFileDescriptor callbacks are one-shot: re-arm if still watched.
    if ((it = _fds.find(id)) != _fds.end())
        CFFileDescriptorEnableCallBacks(it->second.cf, it->second.callbacks);
    noteWork();
}

// ── clipboard ───────────────────────────────────────────────────────────────
// macOS has one clipboard (NSPasteboard.generalPasteboard) and no primary
// selection: Primary writes are dropped and reads answer nullopt/empty.
// The pasteboard server answers synchronously; only the callbacks are
// deferred, per the never-re-entrant contract (a TIFF-only picture asked for
// as PNG is converted on a worker first).

void CocoaApp::setClipboard(std::vector<DataItem> items, Selection sel) {
    if (sel != Selection::Clipboard)
        return;
    @autoreleasepool {
        NSPasteboard *pb = NSPasteboard.generalPasteboard;
        [pb clearContents]; // a new selection replaces every offered type
        if (!items.empty())
            [pb writeObjects:pasteboardItems(items)];
    }
}

void CocoaApp::requestClipboard(
    std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
) {
    std::optional<std::string> v;
    if (sel == Selection::Clipboard) {
        @autoreleasepool {
            NSPasteboard *pb = NSPasteboard.generalPasteboard;
            if (NSData *tiff = tiffOnlyPicture(pb, mime)) {
                // A screenshot's TIFF → PNG takes tens of ms or more: convert
                // the copied bytes on a worker and answer from there.
                auto alive = _alive;
                auto done  = std::make_shared<std::function<void(std::optional<std::string>)>>(
                    std::move(cb)
                );
                dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                  std::optional<std::string> png = pngFromTiff(tiff);
                  std::lock_guard            lock(alive->mutex);
                  if (CocoaApp *app = alive->app)
                      app->post([done, png = std::move(png)] { (*done)(png); });
                });
                return;
            }
            v = readPasteboard(pb, mime);
        }
    }
    post([cb = std::move(cb), v = std::move(v)] { cb(v); });
}

void CocoaApp::requestClipboardMimes(
    std::function<void(std::vector<std::string>)> cb, Selection sel
) {
    std::vector<std::string> m;
    if (sel == Selection::Clipboard) {
        @autoreleasepool {
            m = pasteboardMimes(NSPasteboard.generalPasteboard);
        }
    }
    post([cb = std::move(cb), m = std::move(m)] { cb(m); });
}

// ── misc services ───────────────────────────────────────────────────────────

bool CocoaApp::darkMode() const {
    NSAppearanceName n = [NSApp.effectiveAppearance
        bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return [n isEqualToString:NSAppearanceNameDarkAqua];
}

int CocoaApp::doubleClickMs() const {
    return int(std::lround(NSEvent.doubleClickInterval * 1000));
}

bool CocoaApp::openUrl(std::string_view url) {
    @autoreleasepool {
        // nsString() answers "" for bytes that are not UTF-8: no URL then.
        NSString *s = nsString(url);
        NSURL    *u = s.length || url.empty() ? [NSURL URLWithString:s] : nil;
        return u && [NSWorkspace.sharedWorkspace openURL:u];
    }
}

void CocoaApp::setBadgeCount(int count) {
    ensureLaunched(); // the Dock tile only exists once we are in the Dock
    // Plain AppKit, no permission needed (unlike UNUserNotificationCenter's
    // badge); the Dock hides it when the user turned "Badge app icon" off.
    // The tile outlives the process, so an app should clear it before it
    // quits or the count sticks to the Dock icon.
    NSApp.dockTile.badgeLabel = count > 0 ? [NSString stringWithFormat:@"%d", count] : nil;
}

void CocoaApp::onThemeChanged() {
    // Delivered to every window so a handler can re-theme the one it gets.
    for (auto *w : std::vector<CocoaWindow *>(windows))
        w->emit({.type = EventType::ThemeChanged});
    noteWork();
}

void CocoaApp::requestQuit() {
    emit({.type = EventType::QuitRequested});
    noteWork();
}

#ifdef PLAT_TEST_HOOKS
// ── TestHooks ───────────────────────────────────────────────────────────────
// CGEventPost would be the faithful path, but it needs the Accessibility
// permission, which a test started over ssh or from CI never has. Instead
// the hooks build the NSEvents the window server would have produced and
// hand them to the same dispatch a queued event gets (NSApp sendEvent: →
// NSWindow → PlatView), synchronously.

bool CocoaApp::deliverInjected(NSEvent *ev) {
    if (!ev)
        return false;
    // Not -postEvent:atStart: — a posted synthetic mouse event comes back out
    // of the queue with its y shifted by a varying amount (the round trip
    // through global coordinates) and can be dropped altogether. Dispatch
    // right away through the same path pump() uses for queued events.
    dispatch(ev);
    return true;
}

namespace {
NSEventModifierFlags modifierBits(Key k) {
    // Side-less flag | the device-dependent bit flagsChanged: reads.
    switch (k) {
    case Key::ShiftLeft:
        return NSEventModifierFlagShift | 0x0002;
    case Key::ShiftRight:
        return NSEventModifierFlagShift | 0x0004;
    case Key::ControlLeft:
        return NSEventModifierFlagControl | 0x0001;
    case Key::ControlRight:
        return NSEventModifierFlagControl | 0x2000;
    case Key::AltLeft:
        return NSEventModifierFlagOption | 0x0020;
    case Key::AltRight:
        return NSEventModifierFlagOption | 0x0040;
    case Key::SuperLeft:
        return NSEventModifierFlagCommand | 0x0008;
    case Key::SuperRight:
        return NSEventModifierFlagCommand | 0x0010;
    default:
        return 0;
    }
}

// Logical point in the view → window coordinates (bottom-left origin).
NSPoint windowPoint(CocoaWindow &w, Point p) {
    return [w.view convertPoint:NSMakePoint(p.x, p.y) toView:nil];
}
// … → CG global coordinates (top-left of the primary display, y down).
CGPoint globalPoint(CocoaWindow &w, Point p) {
    const NSPoint screen   = [w.window convertPointToScreen:windowPoint(w, p)];
    const CGFloat primaryH = NSScreen.screens.firstObject.frame.size.height;
    return CGPointMake(screen.x, primaryH - screen.y);
}
} // namespace

bool CocoaApp::injectKey(Window &win, Key k, bool down) {
    @autoreleasepool {
        auto     &w  = static_cast<CocoaWindow &>(win);
        const int vk = keyCodeForKey(k);
        if (vk < 0)
            return false;
        const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
        const NSInteger      num = w.window.windowNumber;
        if (modifierBits(k)) {
            // Rebuild the flags from every held modifier, so releasing one
            // Shift keeps the side-less Shift flag while the other is down.
            _injDown[size_t(k)] = down;
            _injFlags           = 0;
            for (int i = 0; i < int(Key::Count); ++i)
                if (_injDown[i])
                    _injFlags |= modifierBits(Key(i));
            // keyEventWithType: accepts FlagsChanged; characters are empty.
            return deliverInjected([NSEvent keyEventWithType:NSEventTypeFlagsChanged
                                                    location:NSZeroPoint
                                               modifierFlags:_injFlags
                                                   timestamp:now
                                                windowNumber:num
                                                     context:nil
                                                  characters:@""
                                 charactersIgnoringModifiers:@""
                                                   isARepeat:NO
                                                     keyCode:uint16_t(vk)]);
        }
        const bool repeat   = down && _injDown[size_t(k)];
        _injDown[size_t(k)] = down;
        NSString *chars     = charactersForKeyCode(uint16_t(vk), _injFlags);
        NSString *bare = charactersForKeyCode(uint16_t(vk), _injFlags & NSEventModifierFlagShift);
        return deliverInjected([NSEvent
                       keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp
                               location:NSZeroPoint
                          modifierFlags:_injFlags
                              timestamp:now
                           windowNumber:num
                                context:nil
                             characters:chars
            charactersIgnoringModifiers:bare
                              isARepeat:repeat
                                keyCode:uint16_t(vk)]);
    }
}

bool CocoaApp::injectPointerMove(Window &win, Point p) {
    @autoreleasepool {
        auto &w          = static_cast<CocoaWindow &>(win);
        _injPointer      = p;
        NSEventType type = (_injButtons & 1)   ? NSEventTypeLeftMouseDragged
                           : (_injButtons & 2) ? NSEventTypeRightMouseDragged
                           : _injButtons       ? NSEventTypeOtherMouseDragged
                                               : NSEventTypeMouseMoved;
        NSEvent    *ev   = [NSEvent mouseEventWithType:type
                                              location:windowPoint(w, p)
                                         modifierFlags:_injFlags
                                             timestamp:NSProcessInfo.processInfo.systemUptime
                                          windowNumber:w.window.windowNumber
                                               context:nil
                                           eventNumber:0
                                            clickCount:0
                                              pressure:0];
        if (type == NSEventTypeMouseMoved) {
            // Hover motion reaches views through tracking areas, which the
            // window server drives from the real cursor position; a posted
            // move cannot trigger them (and warping the user's cursor is not
            // on), so hand it to the view's handler directly.
            [w.view mouseMoved:ev];
            return true;
        }
        return deliverInjected(ev);
    }
}

bool CocoaApp::injectButton(Window &win, Button b, bool down) {
    @autoreleasepool {
        auto                &w   = static_cast<CocoaWindow &>(win);
        const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
        if (down) {
            // Click counting is the window server's job for real input.
            const bool again =
                b == _injLastButton && now - _injLastPress < NSEvent.doubleClickInterval;
            _injClicks     = again ? _injClicks + 1 : 1;
            _injLastPress  = now;
            _injLastButton = b;
            _injButtons |= 1 << int(b);
        } else {
            _injButtons &= ~(1 << int(b));
        }
        NSEventType type;
        int         number = 0;
        switch (b) {
        case Button::Left:
            type = down ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp;
            break;
        case Button::Right:
            type   = down ? NSEventTypeRightMouseDown : NSEventTypeRightMouseUp;
            number = 1;
            break;
        default:
            type   = down ? NSEventTypeOtherMouseDown : NSEventTypeOtherMouseUp;
            number = b == Button::Middle ? 2 : b == Button::Back ? 3 : 4;
            break;
        }
        NSEvent *proto = [NSEvent mouseEventWithType:type
                                            location:windowPoint(w, _injPointer)
                                       modifierFlags:_injFlags
                                           timestamp:now
                                        windowNumber:w.window.windowNumber
                                             context:nil
                                         eventNumber:0
                                          clickCount:_injClicks
                                            pressure:down ? 1.0 : 0.0];
        if (number >= 2) {
            // An NSEvent cannot be told which "other" button it is except by
            // going through a CGEvent, and AppKit's CG→window conversion puts
            // such an event tens of points off (as it does for posted ones).
            // Give the view's handler the button explicitly instead.
            [w.view button:b down:down event:proto];
            return true;
        }
        return deliverInjected(proto);
    }
}

bool CocoaApp::injectScroll(Window &win, double dx, double dy) {
    @autoreleasepool {
        auto      &w  = static_cast<CocoaWindow &>(win);
        // Line units, like a notched mouse wheel. CG's wheel axes are
        // "content moves" (+1 = up/left), plat's are "scroll down/right".
        CGEventRef cg = CGEventCreateScrollWheelEvent(
            nullptr, kCGScrollEventUnitLine, 2, int32_t(std::lround(-dy)), int32_t(std::lround(-dx))
        );
        if (!cg)
            return false;
        CGEventSetLocation(cg, globalPoint(w, _injPointer));
        NSEvent *ev = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
        if (!ev)
            return false;
        // Scroll events are routed by the window server to the window under
        // the real cursor, which a synthesised event does not have; deliver
        // it to the view the injected pointer is over.
        [w.view scrollWheel:ev];
        return true;
    }
}

bool CocoaApp::injectPhasedScroll(Window &win, double dx, double dy, ScrollPhase phase) {
    @autoreleasepool {
        auto      &w  = static_cast<CocoaWindow &>(win);
        // A touchpad scroll as the window server reports it: pixel units,
        // "continuous" (precise), with the gesture-phase fields -[NSEvent
        // phase]/-momentumPhase are decoded from. All public CGEvent fields;
        // only posting it would need Accessibility, so it is converted and
        // delivered like injectScroll's.
        CGEventRef cg = CGEventCreateScrollWheelEvent2(
            nullptr,
            kCGScrollEventUnitPixel,
            2,
            int32_t(std::lround(-dy)),
            int32_t(std::lround(-dx)),
            0
        );
        if (!cg)
            return false;
        CGEventSetIntegerValueField(cg, kCGScrollWheelEventIsContinuous, 1);
        CGEventSetIntegerValueField(cg, kCGScrollWheelEventPointDeltaAxis1, std::lround(-dy));
        CGEventSetIntegerValueField(cg, kCGScrollWheelEventPointDeltaAxis2, std::lround(-dx));
        CGEventSetDoubleValueField(cg, kCGScrollWheelEventFixedPtDeltaAxis1, -dy);
        CGEventSetDoubleValueField(cg, kCGScrollWheelEventFixedPtDeltaAxis2, -dx);
        int64_t scroll = 0, momentum = kCGMomentumScrollPhaseNone;
        switch (phase) {
        case ScrollPhase::Begin:
            scroll = kCGScrollPhaseBegan;
            break;
        case ScrollPhase::Update:
            scroll = kCGScrollPhaseChanged;
            break;
        case ScrollPhase::End:
            scroll = kCGScrollPhaseEnded;
            break;
        case ScrollPhase::Momentum:
            momentum = kCGMomentumScrollPhaseContinue;
            break;
        case ScrollPhase::None:
            break;
        }
        CGEventSetIntegerValueField(cg, kCGScrollWheelEventScrollPhase, scroll);
        CGEventSetIntegerValueField(cg, kCGScrollWheelEventMomentumPhase, momentum);
        CGEventSetLocation(cg, globalPoint(w, _injPointer));
        NSEvent *ev = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
        if (!ev)
            return false;
        [w.view scrollWheel:ev]; // routed like injectScroll's, see there
        return true;
    }
}

int CocoaApp::badgeCount() {
    NSString *label = NSApp.dockTile.badgeLabel;
    return label.length ? int(label.integerValue) : 0;
}

bool CocoaApp::readPixel(Window &win, int x, int y, uint32_t *argb) {
    @autoreleasepool {
        auto &w           = static_cast<CocoaWindow &>(win);
        // Preferred: what the window server composited for our window.
        // CGWindowListCreateImage is gone from the macOS 15 SDK headers but
        // still exported, and capturing the caller's own windows needs no
        // Screen Recording permission. Looked up at run time so the build
        // does not depend on the SDK still declaring it.
        using CreateImage = CGImageRef (*)(CGRect, uint32_t, uint32_t, uint32_t);
        static auto create =
            reinterpret_cast<CreateImage>(dlsym(RTLD_DEFAULT, "CGWindowListCreateImage"));
        CGImageRef img = nullptr;
        int        ox = 0, oy = 0; // our view's origin inside the image, physical px
        bool       owned = false;
        // Without the Screen Recording permission the capture "succeeds" with
        // a blank image even for our own window (macOS 15+), so ask first.
        if (create && CGPreflightScreenCaptureAccess() &&
            !std::getenv("PLAT_COCOA_READBACK_LAYER")) {
            img = create(
                CGRectNull,
                1 << 3 /* kCGWindowListOptionIncludingWindow */,
                uint32_t(w.window.windowNumber),
                (1 << 0) | (1 << 3) /* BoundsIgnoreFraming | BestResolution */
            );
            owned = img != nullptr;
            if (img) {
                const double k  = double(CGImageGetWidth(img)) / w.window.frame.size.width;
                const NSRect vr = [w.view convertRect:w.view.bounds toView:nil];
                ox              = int(std::lround(vr.origin.x * k));
                oy              = int(std::lround((w.window.frame.size.height - NSMaxY(vr)) * k));
            }
        }
        if (IOSurfaceRef s = w.presentedSurface(); !img && s) {
            // Fallback: the surface we last handed to the compositor. Proves
            // the surface/format path, not that it reached the screen.
            static bool told = false;
            if (!told) {
                told = true;
                std::fprintf(
                    stderr,
                    "plat/cocoa: readPixel reads the layer's contents, not the screen "
                    "(grant Screen Recording to read composited pixels)\n"
                );
            }
            const bool ok = x >= 0 && y >= 0 && size_t(x) < IOSurfaceGetWidth(s) &&
                            size_t(y) < IOSurfaceGetHeight(s);
            if (ok) {
                IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
                const auto *base = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(s));
                std::memcpy(argb, base + size_t(y) * IOSurfaceGetBytesPerRow(s) + size_t(x) * 4, 4);
                IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
            }
            return ok;
        }
        if (!img)
            return false;
        const int ix = x + ox, iy = y + oy;
        bool      ok = ix >= 0 && iy >= 0 && size_t(ix) < CGImageGetWidth(img) &&
                       size_t(iy) < CGImageGetHeight(img);
        if (ok) {
            // Draw the one pixel into a known format; drawing converts from the
            // capture's colour space (the display's) back to sRGB.
            uint32_t        px = 0;
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef    cx = CGBitmapContextCreate(
                &px,
                1,
                1,
                8,
                4,
                cs,
                CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little
            );
            CGColorSpaceRelease(cs);
            const CGFloat iw = CGImageGetWidth(img), ih = CGImageGetHeight(img);
            // CG contexts are y-up: image row iy sits at 1 - (ih - iy) from the bottom.
            CGContextSetBlendMode(cx, kCGBlendModeCopy);
            CGContextDrawImage(cx, CGRectMake(-ix, -(ih - iy - 1), iw, ih), img);
            CGContextRelease(cx);
            *argb = px;
        }
        if (owned)
            CGImageRelease(img);
        return ok;
    }
}

#endif // PLAT_TEST_HOOKS

} // namespace plat::cocoa

namespace plat {

std::unique_ptr<App> createCocoaApp(std::string *error) {
    if (!NSThread.isMainThread) {
        if (error)
            *error = "the Cocoa backend must be created on the main thread";
        return nullptr;
    }
    // Without a window-server session (plain ssh, launchd daemon) AppKit
    // aborts inside NSApplication instead of failing; check first.
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (!session) {
        if (error)
            *error = "no window server session (not logged in at the console?)";
        return nullptr;
    }
    CFRelease(session);
    return std::make_unique<cocoa::CocoaApp>();
}

} // namespace plat

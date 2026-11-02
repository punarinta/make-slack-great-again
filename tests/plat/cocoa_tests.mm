// Cocoa-only checks: data transfer against a private, uniquely named
// NSPasteboard (the general pasteboard, the user's clipboard, is never
// touched) and the IOSurface present path. Needs a login session (pasteboard
// and window server), so like plat_win32_tests it is not a ctest test — run
// it directly on a Mac.
#include "cocoa/cocoa_internal.h"
#include "core/backends.h"
#include "test_util.h"

#include <algorithm>
#include <cstring>
#include <set>

using namespace plat;
using namespace plat_test;

// The parts of NSDraggingInfo the drop target reads, over a private pasteboard.
@interface                                 FakeDragInfo : NSObject
@property(nonatomic, strong) NSPasteboard *draggingPasteboard;
@property(nonatomic) NSPoint               draggingLocation;
@property(nonatomic) NSDragOperation       draggingSourceOperationMask;
@property(nonatomic) NSInteger             draggingSequenceNumber;
@end
@implementation FakeDragInfo
@end

namespace {

bool has(const std::vector<std::string> &v, const char *s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// A 2×2 picture with distinct opaque pixels, as many apps copy one: TIFF only.
NSData *tiffPicture() {
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                    pixelsWide:2
                                                                    pixelsHigh:2
                                                                 bitsPerSample:8
                                                               samplesPerPixel:4
                                                                      hasAlpha:YES
                                                                      isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:8
                                                                  bitsPerPixel:32];
    const unsigned char px[16] = {
        255,
        0,
        0,
        255,
        /**/ 0,
        255,
        0,
        255,
        /**/ 0,
        0,
        255,
        255,
        /**/ 255,
        255,
        255,
        255,
    };
    std::memcpy(rep.bitmapData, px, sizeof px);
    return [rep TIFFRepresentation];
}

void caseTiffOnlyPictureReadsAsPng() {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard pasteboardWithUniqueName];
        [pb clearContents];
        NSData *tiff = tiffPicture();
        CHECK(tiff.length > 0);
        CHECK([pb setData:tiff forType:NSPasteboardTypeTIFF]);

        // Offered as image/png too, though only TIFF is on the pasteboard.
        const std::vector<std::string> mimes = cocoa::pasteboardMimes(pb);
        CHECK(has(mimes, "image/tiff"));
        CHECK(has(mimes, "image/png"));
        CHECK([pb dataForType:NSPasteboardTypePNG] == nil);

        // Read as PNG: converted, same pixels.
        const std::optional<std::string> png = cocoa::readPasteboard(pb, "image/png");
        CHECK(png && png->size() > 8 && png->compare(0, 4, "\x89PNG") == 0);
        if (png) {
            NSData           *d   = [NSData dataWithBytes:png->data() length:png->size()];
            NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:d];
            CHECK(rep && rep.pixelsWide == 2 && rep.pixelsHigh == 2);
            if (rep) {
                NSUInteger p[4] = {};
                [rep getPixel:p atX:0 y:0];
                CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0);
                [rep getPixel:p atX:1 y:1];
                CHECK(p[0] == 255 && p[1] == 255 && p[2] == 255);
            }
        }
        // The TIFF itself is still there as it was.
        const std::optional<std::string> raw = cocoa::readPasteboard(pb, "image/tiff");
        CHECK(raw && raw->size() == tiff.length);

        // A real PNG on the pasteboard is returned as is, no extra mime.
        [pb clearContents];
        [pb setData:[NSData dataWithBytes:png->data() length:png->size()]
            forType:NSPasteboardTypePNG];
        const std::vector<std::string> pngOnly = cocoa::pasteboardMimes(pb);
        CHECK(has(pngOnly, "image/png") && !has(pngOnly, "image/tiff"));
        const std::optional<std::string> same = cocoa::readPasteboard(pb, "image/png");
        CHECK(same && *same == *png);

        // Nothing usable: no PNG.
        [pb clearContents];
        [pb setString:@"text" forType:NSPasteboardTypeString];
        CHECK(!cocoa::readPasteboard(pb, "image/png"));
        CHECK(!has(cocoa::pasteboardMimes(pb), "image/png"));

        [pb releaseGlobally];
    }
}

// Every pixel of the surface the layer shows equals `want` (w×h, tight rows).
bool presentedEquals(cocoa::CocoaWindow &win, const std::vector<uint32_t> &want, int w, int h) {
    IOSurfaceRef s = win.presentedSurface();
    if (!s || int(IOSurfaceGetWidth(s)) != w || int(IOSurfaceGetHeight(s)) != h)
        return false;
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
    const auto *base = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(s));
    bool        same = true;
    for (int y = 0; y < h && same; ++y)
        same =
            std::memcmp(
                base + size_t(y) * IOSurfaceGetBytesPerRow(s), &want[size_t(y) * w], size_t(w) * 4
            ) == 0;
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
    return same;
}

bool canvasEquals(const Canvas &c, const std::vector<uint32_t> &want) {
    for (int y = 0; y < c.height; ++y)
        if (std::memcmp(
                c.pixels + size_t(y) * c.stride, &want[size_t(y) * c.width], size_t(c.width) * 4
            ))
            return false;
    return true;
}

void pumpRunLoop(double seconds) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

// Partial repaints go into a surface other than the one on screen, which must
// first catch up with the frames it missed: the canvas always starts as the
// last presented frame, and the presented frame is exactly what was painted.
void casePartialPresentsKeepEveryFrame() {
    @autoreleasepool {
        std::string          err;
        std::unique_ptr<App> app = createCocoaApp(&err);
        if (!app) {
            skip("no window-server session: " + err);
            return;
        }
        std::unique_ptr<Window> win =
            app->createWindow({.title = "plat present", .size = {160, 120}});
        auto &cw = static_cast<cocoa::CocoaWindow &>(*win);
        pumpRunLoop(0.2);

        Canvas c = win->beginPaint();
        CHECK(c.pixels && c.width > 0 && c.height > 0 && c.stride >= c.width);
        if (!c.pixels)
            return;
        const int             w = c.width, h = c.height;
        std::vector<uint32_t> model(size_t(w) * h, 0xff102030);
        for (int y = 0; y < h; ++y)
            std::memcpy(c.pixels + size_t(y) * c.stride, &model[size_t(y) * w], size_t(w) * 4);
        win->endPaint({});
        CHECK(presentedEquals(cw, model, w, h));

        std::set<IOSurfaceRef> seen{cw.presentedSurface()};
        IOSurfaceRef           prev = cw.presentedSurface();
        uint32_t               seed = 1;
        auto                   next = [&](int n) {
            seed = seed * 1103515245u + 12345u;
            return int((seed >> 8) % uint32_t(n));
        };
        for (int f = 0; f < 40; ++f) {
            c = win->beginPaint();
            CHECK(c.pixels && c.width == w && c.height == h);
            if (!c.pixels)
                return;
            CHECK(canvasEquals(c, model));
            // One to three small rects, some partly outside the canvas.
            std::vector<plat::Rect> damage;
            for (int k = 1 + next(3); k > 0; --k) {
                const plat::Rect r{
                    next(w + 10) - 5, next(h + 10) - 5, 1 + next(w / 3), 1 + next(h / 3)
                };
                damage.push_back(r);
                const uint32_t col = 0xff000000u | (uint32_t(f * 40503 + k * 977) & 0xffffff);
                for (int y = std::max(0, r.y); y < std::min(h, r.y + r.h); ++y)
                    for (int x = std::max(0, r.x); x < std::min(w, r.x + r.w); ++x) {
                        c.pixels[size_t(y) * c.stride + x] = col;
                        model[size_t(y) * w + x]           = col;
                    }
            }
            win->endPaint(damage);
            CHECK(presentedEquals(cw, model, w, h));
            CHECK(cw.presentedSurface() != prev); // never repaints the one on screen
            prev = cw.presentedSurface();
            seen.insert(prev);
            if (f % 4 == 0)
                pumpRunLoop(0.03); // let the window server take frames
        }
        CHECK(seen.size() >= 2 && seen.size() <= 4);

        // A resize keeps what fits of the old frame; the rest starts black.
        win->setSize({200, 100});
        pumpRunLoop(0.1);
        c = win->beginPaint();
        CHECK(c.pixels && c.width != w);
        if (c.pixels) {
            bool kept = true;
            for (int y = 0; y < c.height; ++y)
                for (int x = 0; x < c.width; ++x) {
                    const uint32_t want = x < w && y < h ? model[size_t(y) * w + x] : 0xff000000u;
                    kept                = kept && c.pixels[size_t(y) * c.stride + x] == want;
                }
            CHECK(kept);
            win->endPaint({});
        }
        win.reset();
        pumpRunLoop(0.05);
    }
}

const DataItem *itemOf(const Event &e, std::string_view mime) {
    for (const auto &i : e.items)
        if (i.mime == mime)
            return &i;
    return nullptr;
}

// The drop target's events: types only while hovering (the type list kept
// across moves of one drag until the pasteboard changes), data on Drop with
// the text and the URIs each read once into both of their fields.
void caseDropTargetEvents() {
    @autoreleasepool {
        std::string          err;
        std::unique_ptr<App> app = createCocoaApp(&err);
        if (!app) {
            skip("no window-server session: " + err);
            return;
        }
        std::vector<Event> got;
        app->setEventHandler([&](const Event &e) { got.push_back(e); });
        std::unique_ptr<Window> win = app->createWindow({.title = "plat drop", .size = {160, 120}});
        PlatView               *view = static_cast<cocoa::CocoaWindow &>(*win).view;
        pumpRunLoop(0.1);

        NSPasteboard *pb = [NSPasteboard pasteboardWithUniqueName];
        [pb clearContents];
        const std::string text = "h\xc3\xa9llo";
        [pb writeObjects:cocoa::pasteboardItems(
                             {{core::kTextMime, text},
                              {"text/uri-list", "file:///tmp/a%20b.txt\r\n"}}
                         )];
        FakeDragInfo *fake               = [FakeDragInfo new];
        fake.draggingPasteboard          = pb;
        fake.draggingLocation            = NSMakePoint(10, 10);
        fake.draggingSourceOperationMask = NSDragOperationCopy;
        fake.draggingSequenceNumber      = 7;
        id<NSDraggingInfo> info          = (id<NSDraggingInfo>)(id)fake;

        auto last = [&](EventType t) -> const Event * {
            for (auto it = got.rbegin(); it != got.rend(); ++it)
                if (it->type == t)
                    return &*it;
            return nullptr;
        };
        CHECK([view draggingEntered:info] == NSDragOperationCopy);
        const Event *enter = last(EventType::DropEnter);
        CHECK(enter && itemOf(*enter, core::kTextMime) && itemOf(*enter, "text/uri-list"));
        CHECK(enter && itemOf(*enter, core::kTextMime)->data.empty());
        CHECK(enter && enter->dropAction == DropAction::Copy);

        [view draggingUpdated:info];
        const Event *move = last(EventType::DropMove);
        CHECK(move && move->items.size() == enter->items.size());

        CHECK([view performDragOperation:info]);
        const Event *drop = last(EventType::Drop);
        CHECK(drop != nullptr);
        if (drop) {
            const DataItem *t = itemOf(*drop, core::kTextMime);
            const DataItem *u = itemOf(*drop, "text/uri-list");
            CHECK(t && t->data == text);
            CHECK(u && u->data == "file:///tmp/a%20b.txt\r\n");
            CHECK(drop->text == text);
            CHECK(drop->uris == std::vector<std::string>{"file:///tmp/a%20b.txt"});
        }

        // Same drag, new contents: the type list is read again.
        [pb clearContents];
        [pb setData:[NSData dataWithBytes:"<b>x</b>" length:8] forType:NSPasteboardTypeHTML];
        [view draggingUpdated:info];
        move = last(EventType::DropMove);
        CHECK(move && itemOf(*move, "text/html") && !itemOf(*move, core::kTextMime));
        CHECK([view performDragOperation:info]);
        drop = last(EventType::Drop);
        CHECK(drop && drop->text.empty() && drop->uris.empty());
        CHECK(drop && itemOf(*drop, "text/html") && itemOf(*drop, "text/html")->data == "<b>x</b>");

        [pb releaseGlobally];
        win.reset();
        pumpRunLoop(0.05);
    }
}

bool waitUntil(const std::function<bool()> &done, double seconds) {
    const CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + seconds;
    while (!done() && CFAbsoluteTimeGetCurrent() < end)
        pumpRunLoop(0.02);
    return done();
}

// Full screen goes without the empty title-bar toolbar, and every way out of
// a transition (done, failed or cancelled) leaves a windowed window with it
// back: the same object, the same title-bar height (the traffic lights'
// placement).
void caseFullScreenParksTheToolbar() {
    @autoreleasepool {
        std::string          err;
        std::unique_ptr<App> app = createCocoaApp(&err);
        if (!app) {
            skip("no window-server session: " + err);
            return;
        }
        std::unique_ptr<Window> win = app->createWindow(
            {.title = "plat full screen", .size = {480, 320}, .decorations = Decorations::Custom}
        );
        auto                   &cw  = static_cast<cocoa::CocoaWindow &>(*win);
        NSWindow               *w   = cw.window;
        id<NSWindowDelegate>    d   = (id<NSWindowDelegate>)cw.delegate;
        NSToolbar              *bar = w.toolbar;
        const double            h0  = win->titleBarHeight();
        CHECK(bar != nil);
        CHECK(h0 > 0);
        auto note = [&](NSNotificationName name) {
            return [NSNotification notificationWithName:name object:w];
        };
        auto windowed = [&] { return w.toolbar == bar && win->titleBarHeight() == h0; };
        // The optional ones: a delegate without them fails the case, not the run.
        auto failedToEnter = [&] {
            const bool has = [d respondsToSelector:@selector(windowDidFailToEnterFullScreen:)];
            CHECK(has);
            if (has)
                [d windowDidFailToEnterFullScreen:w];
        };
        auto failedToExit = [&] {
            const bool has = [d respondsToSelector:@selector(windowDidFailToExitFullScreen:)];
            CHECK(has);
            if (has)
                [d windowDidFailToExitFullScreen:w];
        };

        // Entered, then left.
        [d windowWillEnterFullScreen:note(NSWindowWillEnterFullScreenNotification)];
        CHECK(w.toolbar == nil);
        [d windowDidEnterFullScreen:note(NSWindowDidEnterFullScreenNotification)];
        CHECK(w.toolbar == nil);
        [d windowWillExitFullScreen:note(NSWindowWillExitFullScreenNotification)];
        CHECK(windowed());
        [d windowDidExitFullScreen:note(NSWindowDidExitFullScreenNotification)];
        CHECK(windowed());

        // AppKit refuses the entry: windowed again, with the toolbar.
        [d windowWillEnterFullScreen:note(NSWindowWillEnterFullScreenNotification)];
        failedToEnter();
        CHECK(windowed());

        // The exit fails: still full screen, so without it; a later exit
        // brings it back.
        [d windowWillEnterFullScreen:note(NSWindowWillEnterFullScreenNotification)];
        [d windowDidEnterFullScreen:note(NSWindowDidEnterFullScreenNotification)];
        [d windowWillExitFullScreen:note(NSWindowWillExitFullScreenNotification)];
        failedToExit();
        CHECK(w.toolbar == nil);
        [d windowWillExitFullScreen:note(NSWindowWillExitFullScreenNotification)];
        [d windowDidExitFullScreen:note(NSWindowDidExitFullScreenNotification)];
        CHECK(windowed());

        // The real transition, however AppKit ends it (it refuses while the
        // screen is locked): full screen without the toolbar, windowed with it.
        win->show();
        pumpRunLoop(0.2);
        win->setFullscreen(true);
        waitUntil([&] { return win->isFullscreen() && w.toolbar == nil; }, 5);
        pumpRunLoop(1.5); // a refused entry unwinds after the first resize
        if (win->isFullscreen()) {
            CHECK(w.toolbar == nil);
            CHECK(!win->isMaximized());
            win->setFullscreen(false);
            waitUntil([&] { return !win->isFullscreen(); }, 5);
            pumpRunLoop(1.0);
        }
        CHECK(!win->isFullscreen());
        CHECK(windowed());

        win.reset();
        pumpRunLoop(0.05);
    }
}

// A window with the system title bar has no toolbar to park.
void caseFullScreenLeavesSystemDecorationsAlone() {
    @autoreleasepool {
        std::string          err;
        std::unique_ptr<App> app = createCocoaApp(&err);
        if (!app) {
            skip("no window-server session: " + err);
            return;
        }
        std::unique_ptr<Window> win = app->createWindow({.title = "plat system", .size = {320, 240}});
        auto                   &cw  = static_cast<cocoa::CocoaWindow &>(*win);
        NSWindow               *w   = cw.window;
        id<NSWindowDelegate>    d   = (id<NSWindowDelegate>)cw.delegate;
        CHECK(w.toolbar == nil);
        [d windowWillEnterFullScreen:[NSNotification
                                         notificationWithName:NSWindowWillEnterFullScreenNotification
                                                       object:w]];
        if ([d respondsToSelector:@selector(windowDidFailToEnterFullScreen:)])
            [d windowDidFailToEnterFullScreen:w];
        CHECK(w.toolbar == nil);
        win.reset();
        pumpRunLoop(0.05);
    }
}

// The Window menu: one full-screen entry. AppKit puts its own (Globe+F) in
// place of ours and hides ours, which keeps answering the classic ⌃⌘F.
void caseWindowMenuHasOneFullScreenEntry() {
    @autoreleasepool {
        std::string          err;
        std::unique_ptr<App> app = createCocoaApp(&err);
        if (!app) {
            skip("no window-server session: " + err);
            return;
        }
        std::unique_ptr<Window> win = app->createWindow(
            {.title = "plat menu", .size = {320, 240}, .decorations = Decorations::Custom}
        );
        NSWindow *w = static_cast<cocoa::CocoaWindow &>(*win).window;
        win->show();
        pumpRunLoop(0.3);
        NSMenu *menu = NSApp.windowsMenu;
        CHECK(menu != nil);
        if (!menu)
            return;
        [menu update];
        int shown = 0;
        for (NSMenuItem *i in menu.itemArray)
            if (i.action == @selector(toggleFullScreen:) && !i.hidden)
                ++shown;
        CHECK(shown == 1);

        auto press = [&](NSEventModifierFlags mods) {
            NSEvent *e = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                          location:NSZeroPoint
                                     modifierFlags:mods
                                         timestamp:0
                                      windowNumber:w.windowNumber
                                           context:nil
                                        characters:@"f"
                       charactersIgnoringModifiers:@"f"
                                         isARepeat:NO
                                           keyCode:3];
            const bool handled = [NSApp.mainMenu performKeyEquivalent:e];
            // However the transition ends, end up windowed for the next one.
            pumpRunLoop(1.5);
            if (win->isFullscreen()) {
                win->setFullscreen(false);
                waitUntil([&] { return !win->isFullscreen(); }, 5);
                pumpRunLoop(1.0);
            }
            return handled;
        };
        CHECK(press(NSEventModifierFlagControl | NSEventModifierFlagCommand));
        CHECK(press(NSEventModifierFlagFunction));
        win.reset();
        pumpRunLoop(0.05);
    }
}

} // namespace

int main() {
    runCase("a TIFF-only picture is offered and read as PNG", caseTiffOnlyPictureReadsAsPng);
    runCase("partial presents keep every frame", casePartialPresentsKeepEveryFrame);
    runCase("drop target: types while hovering, data once on drop", caseDropTargetEvents);
    runCase("full screen parks the title-bar toolbar and always puts it back",
            caseFullScreenParksTheToolbar);
    runCase("full screen leaves a system title bar alone", caseFullScreenLeavesSystemDecorationsAlone);
    runCase("the Window menu has one full-screen entry and both shortcuts",
            caseWindowMenuHasOneFullScreenEntry);
    return summary();
}

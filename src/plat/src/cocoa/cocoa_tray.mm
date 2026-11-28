// Cocoa tray: an NSStatusItem in the menu bar. Without a menu a click is
// TrayActivated; with one, a click (either button) opens the menu, as with
// every status item — plat.h's macOS exception.
#include "cocoa/cocoa_internal.h"

#include "core/image_util.h"

#include <algorithm>
#include <cmath>

using plat::cocoa::CocoaTray;

// Action target of the status item's button and of every menu item. Holds
// no strong reference back, so the tray can go away while a menu is open.
@interface                              PlatTrayTarget : NSObject
@property(nonatomic, assign) CocoaTray *owner;
@end

@implementation PlatTrayTarget
- (void)buttonClicked:(id)sender {
    if (_owner)
        _owner->clicked();
}
- (void)menuItemChosen:(NSMenuItem *)item {
    if (_owner)
        _owner->chosen(uint32_t(item.tag));
}
@end

namespace plat::cocoa {

namespace {

// Status-item icons are 18 pt square in a 22–24 pt menu bar (the HIG size).
constexpr int kIconPoints = 18;

// src drawn into an exactly px-sized sRGB bitmap: a rep of the menu bar's
// real size, so AppKit never resamples it at draw time (and the probe sees
// what is drawn).
NSBitmapImageRep *repAt(const Image &src, int px) {
    CGImageRef cg = createCGImage(src);
    if (!cg)
        return nil;
    const double    k  = double(px) / std::max(src.width, src.height);
    const int       w  = std::max(1, int(std::lround(src.width * k)));
    const int       h  = std::max(1, int(std::lround(src.height * k)));
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef    cx = CGBitmapContextCreate(
        nullptr,
        size_t(w),
        size_t(h),
        8,
        0,
        cs,
        CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little
    );
    CGColorSpaceRelease(cs);
    if (!cx) {
        CGImageRelease(cg);
        return nil;
    }
    CGContextSetInterpolationQuality(cx, kCGInterpolationHigh);
    CGContextDrawImage(cx, CGRectMake(0, 0, w, h), cg);
    CGImageRelease(cg);
    CGImageRef scaled = CGBitmapContextCreateImage(cx);
    CGContextRelease(cx);
    if (!scaled)
        return nil;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:scaled];
    CGImageRelease(scaled);
    rep.size = NSMakeSize(w * double(kIconPoints) / px, h * double(kIconPoints) / px);
    return rep;
}

NSMenu *buildMenu(const std::vector<MenuItem> &items, PlatTrayTarget *target) {
    NSMenu *m          = [[NSMenu alloc] initWithTitle:@""];
    m.autoenablesItems = NO; // enabled is ours to decide, not validateMenuItem:'s
    for (const auto &it : items) {
        if (it.kind == MenuItem::Kind::Separator) {
            [m addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:nsString(it.label)
                                                    action:nil
                                             keyEquivalent:@""];
        mi.enabled     = it.enabled;
        if (it.kind == MenuItem::Kind::Submenu) {
            mi.submenu = buildMenu(it.children, target);
        } else {
            mi.target = target;
            mi.action = @selector(menuItemChosen:);
            mi.tag    = NSInteger(it.id);
            if (it.kind == MenuItem::Kind::Checkbox)
                mi.state = it.checked ? NSControlStateValueOn : NSControlStateValueOff;
        }
        [m addItem:mi];
    }
    return m;
}

#ifdef PLAT_TEST_HOOKS
// Depth-first: the (sub)menu and index holding the item that sends `id`.
bool findItem(NSMenu *m, uint32_t id, NSMenu **inMenu, NSInteger *index) {
    for (NSInteger i = 0; i < m.numberOfItems; ++i) {
        NSMenuItem *mi = [m itemAtIndex:i];
        if (mi.submenu) {
            if (findItem(mi.submenu, id, inMenu, index))
                return true;
        } else if (!mi.isSeparatorItem && mi.action && NSInteger(id) == mi.tag) {
            *inMenu = m;
            *index  = i;
            return true;
        }
    }
    return false;
}

void flattenTitles(NSMenu *m, std::vector<std::string> &out) {
    for (NSMenuItem *mi in m.itemArray) {
        out.emplace_back(mi.isSeparatorItem ? "-" : mi.title.UTF8String);
        if (mi.submenu)
            flattenTitles(mi.submenu, out);
    }
}
#endif

} // namespace

CocoaTray::CocoaTray(CocoaApp *a) : app(a) {
    item         = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    target       = [PlatTrayTarget new];
    target.owner = this;
    item.button.target = target;
    item.button.action = @selector(buttonClicked:);
    // Right clicks count as clicks too, so with no menu set either button
    // activates (macOS users do not expect a right-click-only behaviour).
    [item.button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
}

CocoaTray::~CocoaTray() {
    target.owner = nullptr;
    if (item)
        [NSStatusBar.systemStatusBar removeStatusItem:item];
}

void CocoaTray::setIcon(const std::vector<Image> &sizes) {
    @autoreleasepool {
        // A template (setTemplate) is tinted by the menu bar from its alpha;
        // otherwise the pixels show as given (a colour picture, its badge).
        NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(kIconPoints, kIconPoints)];
        // Per rep: the smallest source at least px big, else the largest;
        // images drawn for the rep's scale win, so an app that hands over an
        // 18 px @1x and a 36 px @2x icon gets exactly those two.
        for (int scale : {1, 2}) {
            const int px = kIconPoints * scale;
            if (const Image *src = core::pickImage(sizes, px, scale, true))
                if (NSBitmapImageRep *rep = repAt(*src, px))
                    [image addRepresentation:rep];
        }
        [image setTemplate:templ ? YES : NO];
        item.button.image         = image.representations.count ? image : nil;
        item.button.imagePosition = NSImageOnly;
    }
}

void CocoaTray::setTooltip(std::string_view utf8) {
    item.button.toolTip = nsString(utf8);
}

void CocoaTray::setMenu(std::vector<MenuItem> items) {
    @autoreleasepool {
        menu      = items.empty() ? nil : buildMenu(items, target);
        item.menu = menu; // a set menu takes over the click (plat.h)
    }
}

bool CocoaTray::isVisible() const {
    // The menu bar shows every status item that has a window; a user can
    // still hide it (Cmd-drag, menu-bar managers, the notch overflowing),
    // which item.visible then reports.
    return item && item.isVisible && item.button.window != nil;
}

void CocoaTray::clicked() {
    app->emitEvent({.type = EventType::TrayActivated, .tray = this});
    app->noteWork();
}

void CocoaTray::chosen(uint32_t id) {
    app->emitEvent({.type = EventType::TrayMenuItem, .tray = this, .id = id});
    app->noteWork();
}

// ── CocoaApp: creation and test hooks ───────────────────────────────────────

std::unique_ptr<Tray> CocoaApp::createTray() {
    ensureLaunched();
    @autoreleasepool {
        return std::make_unique<CocoaTray>(this);
    }
}

#ifdef PLAT_TEST_HOOKS
bool CocoaApp::trayActivate(Tray &t) {
    auto &c = static_cast<CocoaTray &>(t);
    // With a menu a click opens it (and blocks in menu tracking): that is
    // not an activation, so there is nothing honest to synthesise.
    if (c.menu || !c.item.button)
        return false;
    // The button's own click path: highlight, target/action, as a real click.
    [c.item.button performClick:nil];
    return true;
}

bool CocoaApp::trayMenuSelect(Tray &t, uint32_t itemId) {
    auto     &c = static_cast<CocoaTray &>(t);
    NSMenu   *m = nil;
    NSInteger i = 0;
    if (!c.menu || !findItem(c.menu, itemId, &m, &i) || ![m itemAtIndex:i].isEnabled)
        return false; // a user cannot choose a disabled item either
    // What NSMenu does when the user picks the item: validation, the
    // action sent to its target, and the NSMenuDidSendAction notification.
    [m performActionForItemAtIndex:i];
    return true;
}

bool CocoaApp::trayProbe(Tray &t, TrayProbe *out) {
    auto &c = static_cast<CocoaTray &>(t);
    if (!c.item.button)
        return false;
    out->iconSizes.clear();
    for (NSImageRep *rep in c.item.button.image.representations)
        out->iconSizes.push_back({int(rep.pixelsWide), int(rep.pixelsHigh)});
    out->tooltip    = c.item.button.toolTip ? c.item.button.toolTip.UTF8String : "";
    out->isTemplate = c.item.button.image && c.item.button.image.isTemplate;
    out->menuLabels.clear();
    if (c.item.menu)
        flattenTitles(c.item.menu, out->menuLabels);
    return true;
}
#endif

} // namespace plat::cocoa

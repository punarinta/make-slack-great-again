#include "util/desktop_notifier.h"
#include "util/mac_app_badge.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <atomic>

// Notifications go through UNUserNotificationCenter (UserNotifications.framework).
// NSUserNotification, which this file used to call, has been deprecated since
// macOS 11, renders at most ONE action button, and is on its way out. Note that
// Qt still uses it for QSystemTrayIcon::showMessage, so on macOS the tray
// fallback is not an independent second path — it is the same old API.
//
// Both APIs need the same thing before anything is visible: a properly
// identified, code-signed .app bundle. Without a CFBundleIdentifier the
// authorization prompt never appears and the app never gets an entry under
// System Settings ▸ Notifications (the symptom that led here), and
// currentNotificationCenter asserts outright for an unbundled process. Our CMake
// target is a MACOSX_BUNDLE with CFBundleIdentifier com.nisdos.msga, ad-hoc
// signed after every build, so this holds. The @try below is a best-effort guard
// for anything else — it wraps a framework assertion, which is not reliably
// catchable — degrading to isAvailable()==false.
//
// This file is compiled under manual reference counting (MRC, the project
// default for Obj-C++), so retained objects are released explicitly.

namespace {
// Whether the OS will actually show what we post. Starts true so the very first
// notification (possibly still racing the authorization prompt) is attempted
// rather than dropped, then converges on the real setting via the authorization
// callback and a re-read after every notify(). Written from the framework's
// callback queues and read on the GUI thread, hence atomic.
std::atomic<bool> g_authorized{true};

// Fire-and-forget re-read of the OS switch, so denying at the prompt — or
// revoking (or granting) later in System Settings — is picked up without a
// restart. UNAuthorizationStatusNotDetermined is not a denial: the prompt is
// still unanswered.
void refreshAuthorized(UNUserNotificationCenter *center) {
    [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {
        g_authorized.store(
            settings.authorizationStatus != UNAuthorizationStatusDenied, std::memory_order_relaxed
        );
    }];
}

// Categories accumulate for the process lifetime: setNotificationCategories:
// REPLACES the whole set, so we keep every category we've built and re-set the
// union each time a new one appears. Keyed by category id so a repeat action
// set is registered once. Never released (lives as long as the app).
NSMutableDictionary<NSString *, UNNotificationCategory *> *g_categories = nil;

// Stable category id for an ordered set of action keys, e.g. "msga.cat.join".
NSString *categoryIdForActions(const QList<NotifAction> &actions) {
    NSMutableArray<NSString *> *keys = [NSMutableArray array];
    for (const auto &a : actions)
        [keys addObject:a.key.toNSString()];
    return [@"msga.cat." stringByAppendingString:[keys componentsJoinedByString:@"."]];
}
} // namespace

// Delegate: presents banners while the app is frontmost and routes a click
// (body or action button) back to the C++ owner as activated().
@interface MsgaNotifDelegate : NSObject <UNUserNotificationCenterDelegate> {
    DesktopNotifier *_owner;
}
- (instancetype)initWithOwner:(DesktopNotifier *)owner;
@end

@implementation MsgaNotifDelegate
- (instancetype)initWithOwner:(DesktopNotifier *)owner {
    if ((self = [super init]))
        _owner = owner;
    return self;
}

// Show the banner even when MSGA is the active app (so the avatar always
// appears). No sound here — the app plays its own configurable notification
// sound (Sound::Player), and attaching one to the notification would double it.
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))completionHandler {
    NSDictionary *info = response.notification.request.content.userInfo;
    NSString     *aid  = response.actionIdentifier;
    NSString     *token = nil;
    if ([aid isEqualToString:UNNotificationDismissActionIdentifier]) {
        // User swiped the banner away — nothing to open.
    } else if ([aid isEqualToString:UNNotificationDefaultActionIdentifier]) {
        token = info[@"token"]; // body click
    } else {
        // An action button — its per-button token was stashed under "action.<id>".
        token = info[[@"action." stringByAppendingString:aid]];
    }
    if (token.length > 0 && _owner)
        _owner->emitActivated(QString::fromNSString(token));
    completionHandler();
}
@end

DesktopNotifier::DesktopNotifier(QObject *parent) : QObject(parent) {
    UNUserNotificationCenter *center = nil;
    @try {
        center = [UNUserNotificationCenter currentNotificationCenter];
    } @catch (NSException *e) {
        // Raised for an unbundled/unsigned process — fall back to the tray.
        NSLog(@"msga: UNUserNotificationCenter unavailable (%@) — using tray fallback", e.reason);
        return;
    }
    if (!center)
        return;

    MsgaNotifDelegate *delegate = [[MsgaNotifDelegate alloc] initWithOwner:this];
    center.delegate             = delegate; // assign (not retained) by the center
    _delegate                   = delegate; // we hold the strong reference

    // Registers the app under System Settings ▸ Notifications and prompts once.
    // Badge is included so the app may set the Dock/notification badge; we never
    // attach a sound (the app plays its own), but request it so the user's OS
    // toggle for sound is meaningful should that change.
    //
    // A denial is recorded so notify() can report failure instead of posting into
    // a void: _available stays true (the backend itself works), while
    // g_authorized tracks whether the user lets anything through.
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert |
                                            UNAuthorizationOptionSound | UNAuthorizationOptionBadge
                          completionHandler:^(BOOL granted, NSError *error) {
                              if (error)
                                  NSLog(@"msga: notification authorization error: %@", error);
                              g_authorized.store(granted, std::memory_order_relaxed);
                          }];
    _available = true;
}

DesktopNotifier::~DesktopNotifier() {
    if (_delegate) {
        UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
        if (center && center.delegate == (id)_delegate)
            center.delegate = nil;
        [(MsgaNotifDelegate *)_delegate release];
        _delegate = nullptr;
    }
}

bool DesktopNotifier::notify(const QString &title, const QString &body, const QImage &image,
                             const QString &token, const QList<NotifAction> &actions,
                             int /*timeoutMs*/) {
    if (!_available)
        return false;
    // Notifications are switched off for us: report failure so the caller takes
    // its tray fallback (and, on macOS, at least keeps the Dock badge and the
    // in-app unread marks meaningful) rather than dropping the message silently.
    if (!g_authorized.load(std::memory_order_relaxed))
        return false;

    UNMutableNotificationContent *content = [[[UNMutableNotificationContent alloc] init] autorelease];
    content.title                         = title.toNSString();
    content.body                          = body.toNSString();

    // userInfo carries the click tokens: the body token plus one per action
    // button (keyed "action.<id>"), recovered by the delegate on click.
    NSMutableDictionary *info = [NSMutableDictionary dictionary];
    if (!token.isEmpty())
        info[@"token"] = token.toNSString();

    UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
    bool newCategoryPosted           = false;
    if (!actions.isEmpty()) {
        NSString *catId = categoryIdForActions(actions);
        if (!g_categories)
            g_categories = [[NSMutableDictionary alloc] init];
        if (!g_categories[catId]) {
            // OptionNone, not OptionForeground: an action means "do this thing",
            // not "come to the front" — the huddle Join button deliberately opens
            // the browser without raising the window (see handleNotifToken). The
            // app is running anyway, since it just posted this notification.
            NSMutableArray<UNNotificationAction *> *acts = [NSMutableArray array];
            for (const auto &a : actions)
                [acts addObject:[UNNotificationAction actionWithIdentifier:a.key.toNSString()
                                                                    title:a.label.toNSString()
                                                                  options:UNNotificationActionOptionNone]];
            UNNotificationCategory *cat =
                [UNNotificationCategory categoryWithIdentifier:catId
                                                       actions:acts
                                             intentIdentifiers:@[]
                                                       options:UNNotificationCategoryOptionNone];
            g_categories[catId] = cat;
            [center setNotificationCategories:[NSSet setWithArray:g_categories.allValues]];
            newCategoryPosted = true;
        }
        content.categoryIdentifier = catId;
        for (const auto &a : actions)
            if (!a.token.isEmpty())
                info[[@"action." stringByAppendingString:a.key.toNSString()]] = a.token.toNSString();
    }
    content.userInfo = info;

    // Picture: UNNotificationAttachment needs a file URL, so spool the PNG to a
    // temp file. An accepted attachment is MOVED into the system's own store when
    // the request is scheduled, so there is nothing to clean up afterwards — only
    // a rejected one leaves the spool file behind.
    if (!image.isNull()) {
        QByteArray png;
        QBuffer    buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (image.save(&buf, "PNG")) {
            NSString *name =
                [[[NSProcessInfo processInfo] globallyUniqueString] stringByAppendingString:@".png"];
            NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:name];
            NSData   *data =
                [NSData dataWithBytes:png.constData() length:static_cast<NSUInteger>(png.size())];
            if ([data writeToFile:path atomically:YES]) {
                NSError *err = nil;
                UNNotificationAttachment *att =
                    [UNNotificationAttachment attachmentWithIdentifier:@"image"
                                                                   URL:[NSURL fileURLWithPath:path]
                                                               options:nil
                                                                 error:&err];
                if (att) {
                    content.attachments = @[att];
                } else {
                    NSLog(@"msga: notification attachment rejected (%@)", err);
                    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
                }
            }
        }
    }

    NSString *reqId = [[NSProcessInfo processInfo] globallyUniqueString];
    UNNotificationRequest *req =
        [UNNotificationRequest requestWithIdentifier:reqId content:content trigger:nil];
    if (newCategoryPosted) {
        // setNotificationCategories: is an async round-trip to the notification
        // daemon; a request that overtakes it is shown WITHOUT its buttons. Calls
        // on that connection are ordered, so a getNotificationCategories… reply
        // proves the set… landed — only the first notification of a given action
        // set pays for the extra hop. (The block retains center/req under MRC.)
        auto post = ^(NSSet<UNNotificationCategory *> *) {
            [center addNotificationRequest:req withCompletionHandler:nil];
        };
        [center getNotificationCategoriesWithCompletionHandler:post];
    } else {
        [center addNotificationRequest:req withCompletionHandler:nil];
    }
    refreshAuthorized(center);
    return true;
}

// ── Dock tile badge (independent of notification authorization) ───────────────
// badgeLabel is plain AppKit and needs no permission, so the Dock count still
// works when the user has denied notifications; the system just ignores it when
// "Badge app icon" is off. nil clears the badge. The badge is drawn on the Dock
// tile, which outlives the process — so it has to be cleared on the way out
// (~MainWindow) or a stale count sticks to the icon after quitting.
void macSetDockBadge(int count) {
    NSString *label = count > 0 ? [NSString stringWithFormat:@"%d", count] : nil;
    [NSApp dockTile].badgeLabel = label;
}

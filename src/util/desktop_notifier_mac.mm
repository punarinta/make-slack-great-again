#include "util/desktop_notifier.h"
#include "util/mac_app_badge.h"

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QImage>

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

// Notifications go through UNUserNotificationCenter (UserNotifications.framework).
// The older NSUserNotification API this file used to call is not merely
// deprecated: on modern macOS it delivers nothing and — the symptom that led
// here — never registers the app under System Settings ▸ Notifications, so the
// user has no permission toggle and never sees a banner. UNUserNotificationCenter
// is the supported path; requesting authorization at startup is what registers
// the app (creating the Settings entry) and prompts the user once.
//
// UNUserNotificationCenter requires a real, signed .app bundle with a bundle id
// (CFBundleIdentifier) — currentNotificationCenter raises if the process is an
// unbundled/unsigned binary. Our CMake bundle sets app.msga.msga and the build
// ad-hoc signs the .app, so this is satisfied; the @try guard degrades to the
// tray fallback (isAvailable()==false) anywhere it isn't.
//
// This file is compiled under manual reference counting (MRC, the project
// default for Obj-C++), so retained objects are released explicitly.

namespace {
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
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert |
                                            UNAuthorizationOptionSound | UNAuthorizationOptionBadge
                          completionHandler:^(BOOL granted, NSError *error) {
                              if (error)
                                  NSLog(@"msga: notification authorization error: %@", error);
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

    UNMutableNotificationContent *content = [[[UNMutableNotificationContent alloc] init] autorelease];
    content.title                         = title.toNSString();
    content.body                          = body.toNSString();

    // userInfo carries the click tokens: the body token plus one per action
    // button (keyed "action.<id>"), recovered by the delegate on click.
    NSMutableDictionary *info = [NSMutableDictionary dictionary];
    if (!token.isEmpty())
        info[@"token"] = token.toNSString();

    UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
    if (!actions.isEmpty()) {
        NSString *catId = categoryIdForActions(actions);
        if (!g_categories)
            g_categories = [[NSMutableDictionary alloc] init];
        if (!g_categories[catId]) {
            NSMutableArray<UNNotificationAction *> *acts = [NSMutableArray array];
            for (const auto &a : actions)
                [acts addObject:[UNNotificationAction actionWithIdentifier:a.key.toNSString()
                                                                    title:a.label.toNSString()
                                                                  options:UNNotificationActionOptionForeground]];
            UNNotificationCategory *cat =
                [UNNotificationCategory categoryWithIdentifier:catId
                                                       actions:acts
                                             intentIdentifiers:@[]
                                                       options:UNNotificationCategoryOptionNone];
            g_categories[catId] = cat;
            [center setNotificationCategories:[NSSet setWithArray:g_categories.allValues]];
        }
        content.categoryIdentifier = catId;
        for (const auto &a : actions)
            if (!a.token.isEmpty())
                info[[@"action." stringByAppendingString:a.key.toNSString()]] = a.token.toNSString();
    }
    content.userInfo = info;

    // Picture: UNNotificationAttachment needs a file URL, so spool the PNG to a
    // temp file (the system copies it into its own store on schedule).
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
                if (att)
                    content.attachments = @[att];
            }
        }
    }

    NSString *reqId = [[NSProcessInfo processInfo] globallyUniqueString];
    UNNotificationRequest *req =
        [UNNotificationRequest requestWithIdentifier:reqId content:content trigger:nil];
    [center addNotificationRequest:req withCompletionHandler:nil];
    return true;
}

// ── Dock tile badge (independent of notification authorization) ───────────────
// setBadgeLabel needs no permission, so the Dock count works even when the user
// has notifications turned off. Empty label clears the badge.
void macSetDockBadge(int count) {
    NSString *label = count > 0 ? [NSString stringWithFormat:@"%d", count] : nil;
    [NSApp dockTile].badgeLabel = label;
}

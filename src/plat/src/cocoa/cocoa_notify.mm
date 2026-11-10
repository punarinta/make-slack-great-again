// Cocoa notifications: UNUserNotificationCenter.
//
// It only works for a code-signed .app bundle with a CFBundleIdentifier (the
// identity macOS keeps the user's permission and Notification Center entry
// under): for a bare executable currentNotificationCenter raises instead of
// failing, so notificationsAvailable() checks the bundle first and answers
// false. AppInfo::id does not rename anything here — the bundle's identifier
// is what the OS shows; keep the two equal. Everything the center reports
// arrives on its own queues and is posted to the loop thread.
#include "cocoa/cocoa_internal.h"

#ifdef PLAT_TEST_HOOKS
#import <CommonCrypto/CommonDigest.h>
#endif
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UserNotifications/UserNotifications.h>

#include <unistd.h> // getppid

#include <cstdio>
#include <cstdlib>

using plat::cocoa::CocoaApp;
using plat::cocoa::nsString;

namespace {

using Alive = CocoaApp::Alive;

// Run fn on the loop thread if the app still exists.
void toLoop(const std::shared_ptr<Alive> &alive, std::function<void(CocoaApp *)> fn) {
    std::lock_guard lock(alive->mutex);
    if (CocoaApp *app = alive->app)
        app->post([app, fn = std::move(fn)] { fn(app); });
}

// Categories accumulate for the process lifetime: setNotificationCategories:
// replaces the whole set, so every category built so far is re-set each time
// a new one appears. Keyed by the ordered action keys and labels.
NSMutableDictionary<NSString *, UNNotificationCategory *> *g_categories;

// The category for an action set, registering it when new (*added = true).
// Every notification gets one, even without actions: CustomDismissAction is
// what makes the center report a user's dismissal (→ NotificationClosed).
NSString *categoryFor(const std::vector<plat::NotificationAction> &actions, bool *added) {
    NSMutableString *key = [NSMutableString stringWithString:@"plat"];
    for (const auto &a : actions)
        [key appendFormat:@".%@=%@", nsString(a.key), nsString(a.label)];
    if (!g_categories)
        g_categories = [NSMutableDictionary dictionary];
    *added = false;
    if (!g_categories[key]) {
        NSMutableArray<UNNotificationAction *> *acts = [NSMutableArray array];
        // OptionNone, not Foreground: an action means "do this", not "come to
        // the front"; the app decides whether to activate a window.
        for (const auto &a : actions)
            [acts addObject:[UNNotificationAction
                                actionWithIdentifier:nsString(a.key)
                                               title:nsString(a.label)
                                             options:UNNotificationActionOptionNone]];
        g_categories[key] = [UNNotificationCategory
            categoryWithIdentifier:key
                           actions:acts
                 intentIdentifiers:@[]
                           options:UNNotificationCategoryOptionCustomDismissAction];
        [UNUserNotificationCenter.currentNotificationCenter
            setNotificationCategories:[NSSet setWithArray:g_categories.allValues]];
        *added = true;
    }
    return key;
}

// UNNotificationAttachment takes a file: the picture as PNG in the temp
// directory. The center moves an accepted attachment into its own store, so
// only a rejected one is left to delete. In test builds *sha1 gets the file's
// SHA-1 (hex): the store names its copy by it (see notificationProbe).
NSURL *writePng(const plat::Image &img, [[maybe_unused]] std::string *sha1) {
    CGImageRef cg = plat::cocoa::createCGImage(img);
    if (!cg)
        return nil;
    NSMutableData        *png = [NSMutableData data];
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(
        (__bridge CFMutableDataRef)png, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr
    );
    bool ok = false;
    if (dst) {
        CGImageDestinationAddImage(dst, cg, nullptr);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CGImageRelease(cg);
    if (!ok)
        return nil;
#ifdef PLAT_TEST_HOOKS
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(png.bytes, CC_LONG(png.length), digest);
    sha1->clear();
    for (unsigned char c : digest) {
        char hex[3];
        std::snprintf(hex, sizeof hex, "%02x", c);
        *sha1 += hex;
    }
#endif
    NSString *name =
        [NSProcessInfo.processInfo.globallyUniqueString stringByAppendingString:@".png"];
    NSURL *url =
        [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:name]];
    return [png writeToURL:url atomically:YES] ? url : nil;
}

#ifdef PLAT_TEST_HOOKS
// Wait for a completion handler the center runs on its own queue (never the
// main one, so blocking the loop thread for it cannot deadlock). Test hooks only.
bool waitFor(dispatch_semaphore_t s) {
    return dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) == 0;
}
#endif

} // namespace

// Presents banners while the app is frontmost and routes clicks and
// dismissals back to the app.
@interface PlatNotifyDelegate : NSObject <UNUserNotificationCenterDelegate> {
@public
    std::shared_ptr<Alive> alive;
    std::string            prefix; // CocoaApp::_notifyPrefix: only this instance's requests
}
@end

@implementation PlatNotifyDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))done {
    // Without this a notification posted while we are the active app only
    // lands in Notification Center, silently.
    UNNotificationPresentationOptions o =
        UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList;
    if (notification.request.content.sound)
        o |= UNNotificationPresentationOptionSound;
    done(o);
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))done {
    const std::string ident = response.notification.request.identifier.UTF8String;
    if (ident.rfind(prefix, 0) == 0) {
        const uint64_t id        = std::strtoull(ident.c_str() + prefix.size(), nullptr, 10);
        NSString      *aid       = response.actionIdentifier;
        const bool     dismissed = [aid isEqualToString:UNNotificationDismissActionIdentifier];
        std::string    action;
        if (!dismissed && ![aid isEqualToString:UNNotificationDefaultActionIdentifier])
            action = aid.UTF8String;
        toLoop(alive, [id, action, dismissed](CocoaApp *app) {
            app->onNotificationResponse(id, action, dismissed);
        });
    }
    done();
}
@end

namespace plat::cocoa {

bool CocoaApp::notificationsAvailable() const {
    return setUpNotifications();
}

bool CocoaApp::setUpNotifications() const {
    if (_notifyState)
        return _notifyState > 0;
    _notifyState = -1;
    NSBundle *b  = NSBundle.mainBundle;
    if (!b.bundleIdentifier.length || ![b.bundlePath.pathExtension isEqualToString:@"app"])
        return false;
    UNUserNotificationCenter *center = nil;
    @try {
        center = UNUserNotificationCenter.currentNotificationCenter;
    } @catch (NSException *e) {
        // The bundle check above should make this unreachable; a framework
        // assertion is not a crash we want to hand the app.
        std::fprintf(stderr, "plat/cocoa: no notification center (%s)\n", e.reason.UTF8String);
        return false;
    }
    if (!center)
        return false;
    // Request identifiers are "<prefix><id>". The caller owns the ids; with
    // a scope it keeps them across restarts, so a notification clicked after
    // one still names this instance. Without one they are per process.
    const std::string &scope = appInfo().notificationScope;
    _notifyPrefix =
        (scope.empty() ? std::string(NSProcessInfo.processInfo.globallyUniqueString.UTF8String)
                       : scope) +
        ".";
    PlatNotifyDelegate *d = [PlatNotifyDelegate new];
    d->alive              = _alive;
    d->prefix             = _notifyPrefix;
    center.delegate       = d; // weak in the center; _notifyDelegate keeps it
    _notifyDelegate       = d;
    _notifyState          = 1;
    return true;
}

std::string CocoaApp::notificationIdentifier(uint64_t id) const {
    return _notifyPrefix + std::to_string(id);
}

bool CocoaApp::launchedFromNotification() {
    // Only LaunchServices starts us with launchd as the parent: a `msga`
    // run from a shell never came from a click, and finishing its launch
    // would put up a Dock icon for a process that may only hand off and exit.
    if (getppid() != 1 || NSRunningApplication.currentApplication.isFinishedLaunching)
        return false;
    // The delegate first: the launch below is what delivers the click.
    if (!setUpNotifications())
        return false;
    ensureLaunched();
    return !_defaultLaunch;
}

bool CocoaApp::notify(uint64_t id, const Notification &n) {
    ensureLaunched();
    if (!setUpNotifications())
        return false;
    @autoreleasepool {
        UNMutableNotificationContent *content = [UNMutableNotificationContent new];
        content.title                         = nsString(n.title);
        content.body                          = nsString(n.body);
        if (!n.silent)
            content.sound = UNNotificationSound.defaultSound;
        // timeoutMs has no macOS equivalent: banners go away on the user's
        // schedule (System Settings: banners vs. alerts).
        bool newCategory           = false;
        content.categoryIdentifier = categoryFor(n.actions, &newCategory);
        std::string sha1;
        if (NSURL *png = n.image.empty() ? nil : writePng(n.image, &sha1)) {
            NSError                  *err = nil;
            UNNotificationAttachment *att =
                [UNNotificationAttachment attachmentWithIdentifier:@"image"
                                                               URL:png
                                                           options:nil
                                                             error:&err];
            if (att) {
                content.attachments = @[ att ];
#ifdef PLAT_TEST_HOOKS
                _notifyImages[id] = {sha1, {n.image.width, n.image.height}};
#endif
            } else
                [NSFileManager.defaultManager removeItemAtURL:png error:nil];
        }
        UNNotificationRequest *req =
            [UNNotificationRequest requestWithIdentifier:nsString(notificationIdentifier(id))
                                                 content:content
                                                 trigger:nil];

        UNUserNotificationCenter *center = UNUserNotificationCenter.currentNotificationCenter;
        std::shared_ptr<Alive>    alive  = _alive;
        auto                      fail   = [alive, id](std::string why) {
            toLoop(alive, [id, why](CocoaApp *app) { app->onNotificationFailed(id, why); });
        };
        auto added = [alive, id] {
            toLoop(alive, [id](CocoaApp *app) { app->onNotificationAdded(id); });
        };
        auto submit = ^{
          [center addNotificationRequest:req
                   withCompletionHandler:^(NSError *error) {
                     if (error)
                         fail(error.localizedDescription.UTF8String);
                     else
                         added();
                   }];
        };
        // A new category must be registered before a request names it, or
        // the first notification shows without buttons; asking for the set
        // back is the center's way of waiting for that.
        auto afterCategories = ^{
          if (newCategory)
              [center getNotificationCategoriesWithCompletionHandler:^(NSSet *) {
                submit();
              }];
          else
              submit();
        };
        // Settings are read for every request: a cached denial would keep
        // the app silent after the user turned notifications on.
        [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *s) {
          if (s.authorizationStatus == UNAuthorizationStatusNotDetermined) {
              // First run: ask (the OS shows its prompt once), then submit.
              [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert |
                                                      UNAuthorizationOptionSound |
                                                      UNAuthorizationOptionBadge
                                    completionHandler:^(BOOL granted, NSError *error) {
                                      if (granted)
                                          afterCategories();
                                      else
                                          fail(
                                              error ? error.localizedDescription.UTF8String
                                                    : "the user did not allow notifications"
                                          );
                                    }];
          } else if (s.authorizationStatus == UNAuthorizationStatusDenied) {
              fail("notifications are turned off for this app in System Settings");
          } else {
              afterCategories();
          }
        }];
        _liveNotifications.insert(id);
        _submitting.insert(id);
        return true;
    }
}

// By its request identifier: with a scope that names one from an earlier run
// too (still in Notification Center). The delegate hears nothing of it. One
// still on its way to the center (notify() is asynchronous) is removed again
// once it is added.
void CocoaApp::withdrawNotification(uint64_t id) {
    if (!setUpNotifications())
        return;
    if (_submitting.count(id))
        _withdrawOnAdd.insert(id);
    _liveNotifications.erase(id);
#ifdef PLAT_TEST_HOOKS
    _notifyImages.erase(id);
#endif
    @autoreleasepool {
        UNUserNotificationCenter *center = UNUserNotificationCenter.currentNotificationCenter;
        NSArray                  *ids    = @[ nsString(notificationIdentifier(id)) ];
        [center removeDeliveredNotificationsWithIdentifiers:ids];
        [center removePendingNotificationRequestsWithIdentifiers:ids];
    }
}

void CocoaApp::onNotificationResponse(uint64_t id, std::string action, bool dismissed) {
    // A click removes the notification from Notification Center, so either
    // way it is no longer ours to close.
    const bool live = _liveNotifications.erase(id) > 0;
#ifdef PLAT_TEST_HOOKS
    _notifyImages.erase(id);
#endif
    if (dismissed) {
        if (live)
            emit({.type = EventType::NotificationClosed, .id = id});
    } else {
        emit({.type = EventType::NotificationActivated, .id = id, .action = std::move(action)});
    }
    noteWork();
}

void CocoaApp::onNotificationAdded(uint64_t id) {
    _submitting.erase(id);
    if (_withdrawOnAdd.erase(id))
        withdrawNotification(id);
}

void CocoaApp::onNotificationFailed(uint64_t id, std::string reason) {
    _liveNotifications.erase(id);
    _submitting.erase(id);
    if (_withdrawOnAdd.erase(id))
        return; // taken back already: nobody waits for news of it
#ifdef PLAT_TEST_HOOKS
    _notifyImages.erase(id);
#endif
    emit({.type = EventType::NotificationFailed, .text = std::move(reason), .id = id});
    noteWork();
}

void CocoaApp::tearDownNotifications() {
    {
        std::lock_guard lock(_alive->mutex);
        _alive->app = nullptr; // late completions now find nobody to post to
    }
    if (_notifyState <= 0)
        return;
    @autoreleasepool {
        // With a scope the delivered ones stay in Notification Center: the
        // caller keeps its ids, so a click after we are gone, even after a
        // clean quit, still resolves in the next run. Without one the ids
        // die with the process: a click could not be reported to anyone, so
        // they leave Notification Center with us.
        UNUserNotificationCenter *center = UNUserNotificationCenter.currentNotificationCenter;
        if (appInfo().notificationScope.empty()) {
            NSMutableArray *ids = [NSMutableArray array];
            for (uint64_t id : _liveNotifications)
                [ids addObject:nsString(notificationIdentifier(id))];
            if (ids.count) {
                [center removeDeliveredNotificationsWithIdentifiers:ids];
                [center removePendingNotificationRequestsWithIdentifiers:ids];
            }
        }
        if (center.delegate == _notifyDelegate)
            center.delegate = nil;
    }
    _notifyDelegate = nil;
    _notifyState    = 0;
}

#ifdef PLAT_TEST_HOOKS
// ── test hook ───────────────────────────────────────────────────────────────

bool CocoaApp::notificationProbe(uint64_t id, NotificationProbe *out) {
    if (_notifyState <= 0)
        return false;
    @autoreleasepool {
        UNUserNotificationCenter *center = UNUserNotificationCenter.currentNotificationCenter;
        NSString                 *ident  = nsString(notificationIdentifier(id));
        // What Notification Center holds for us, not what we sent.
        __block UNNotification   *found  = nil;
        dispatch_semaphore_t      sem    = dispatch_semaphore_create(0);
        [center getDeliveredNotificationsWithCompletionHandler:^(NSArray<UNNotification *> *list) {
          for (UNNotification *n in list)
              if ([n.request.identifier isEqualToString:ident])
                  found = n;
          dispatch_semaphore_signal(sem);
        }];
        if (!waitFor(sem) || !found)
            return false;
        UNNotificationContent *c = found.request.content;
        out->title               = c.title.UTF8String;
        out->body                = c.body.UTF8String;
        out->imageSize           = {};
        if (NSURL *url = c.attachments.firstObject.URL) {
            // The center's own copy of the attachment. Reading it is the
            // direct proof, but it lives in usernoted's group container, which
            // macOS's app-data protection keeps from other apps: open() fails
            // with EPERM even though the URL is reachable and
            // startAccessingSecurityScopedResource says yes (measured on
            // macOS 26). The store names each file by the SHA-1 of its bytes,
            // so a name equal to the digest of the PNG we attached proves it
            // holds exactly that image, whose size we know.
            const bool scoped = [url startAccessingSecurityScopedResource];
            if (CGImageSourceRef src =
                    CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr)) {
                if (CGImageSourceGetStatus(src) == kCGImageStatusComplete) {
                    NSDictionary *p =
                        CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr));
                    out->imageSize = {
                        [p[(__bridge NSString *)kCGImagePropertyPixelWidth] intValue],
                        [p[(__bridge NSString *)kCGImagePropertyPixelHeight] intValue]
                    };
                }
                CFRelease(src);
            }
            if (scoped)
                [url stopAccessingSecurityScopedResource];
            auto sent = _notifyImages.find(id);
            if (out->imageSize.w == 0 && sent != _notifyImages.end() &&
                sent->second.first == url.URLByDeletingPathExtension.lastPathComponent.UTF8String)
                out->imageSize = sent->second.second;
        }
        out->actionLabels.clear();
        __block NSArray<UNNotificationAction *> *actions = nil;
        NSString                                *cat     = c.categoryIdentifier;
        dispatch_semaphore_t                     sem2    = dispatch_semaphore_create(0);
        [center
            getNotificationCategoriesWithCompletionHandler:^(NSSet<UNNotificationCategory *> *set) {
              for (UNNotificationCategory *k in set)
                  if ([k.identifier isEqualToString:cat])
                      actions = k.actions;
              dispatch_semaphore_signal(sem2);
            }];
        if (waitFor(sem2))
            for (UNNotificationAction *a in actions)
                out->actionLabels.emplace_back(a.title.UTF8String);
        return true;
    }
}
#endif

} // namespace plat::cocoa

#include "util/desktop_notifier.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

// NSUserNotification is deprecated (10.14) in favour of UserNotificationsUI, but
// it still works, needs no extra entitlement/framework, and crucially tolerates
// the unbundled dev binary (UNUserNotificationCenter throws without a bundle id).
// It is also the only AppKit API that takes a per-notification picture
// (contentImage, shown on the right) without the heavier UserNotifications setup.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// Obj-C delegate that routes a notification click back to the C++ owner.
@interface MsgaNotifDelegate : NSObject <NSUserNotificationCenterDelegate> {
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
// Show banners even when MSGA is frontmost (so the avatar always appears).
- (BOOL)userNotificationCenter:(NSUserNotificationCenter *)center
     shouldPresentNotification:(NSUserNotification *)notification {
    return YES;
}
- (void)userNotificationCenter:(NSUserNotificationCenter *)center
       didActivateNotification:(NSUserNotification *)notification {
    NSString *token = notification.userInfo[@"token"];
    if (token && _owner)
        _owner->emitActivated(QString::fromNSString(token));
}
@end

DesktopNotifier::DesktopNotifier(QObject *parent) : QObject(parent) {
    NSUserNotificationCenter *center = [NSUserNotificationCenter defaultUserNotificationCenter];
    if (!center)
        return; // no Notification Center (e.g. bare binary) → caller uses the tray
    MsgaNotifDelegate *delegate = [[MsgaNotifDelegate alloc] initWithOwner:this];
    center.delegate              = delegate; // assign (not retained) by the center
    _delegate                    = delegate; // we hold the strong reference
    _available                   = true;
}

DesktopNotifier::~DesktopNotifier() {
    if (_delegate) {
        NSUserNotificationCenter *center = [NSUserNotificationCenter defaultUserNotificationCenter];
        if (center && center.delegate == (id)_delegate)
            center.delegate = nil;
        [(MsgaNotifDelegate *)_delegate release];
        _delegate = nullptr;
    }
}

bool DesktopNotifier::notify(const QString &title, const QString &body, const QImage &image,
                             const QString &token, int /*timeoutMs*/) {
    if (!_available)
        return false;

    NSUserNotification *note = [[[NSUserNotification alloc] init] autorelease];
    note.title               = title.toNSString();
    note.informativeText     = body.toNSString();
    if (!token.isEmpty())
        note.userInfo = @{@"token" : token.toNSString()};

    if (!image.isNull()) {
        QByteArray png;
        QBuffer    buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (image.save(&buf, "PNG")) {
            NSData *data =
                [NSData dataWithBytes:png.constData() length:static_cast<NSUInteger>(png.size())];
            NSImage *img = [[[NSImage alloc] initWithData:data] autorelease];
            if (img)
                note.contentImage = img; // right-side thumbnail
        }
    }

    [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:note];
    return true;
}

#pragma clang diagnostic pop

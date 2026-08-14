#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <QList>

class QImage;

// One optional action button on a notification. `key` is a stable id, `label`
// is the button text, and `token` is echoed back via activated() when THIS
// button (rather than the body) is clicked. Not every backend can render
// buttons (the freedesktop daemon may not support actions, the mingw build
// lacks WinRT toasts, NSUserNotification allows a single button) — where they
// can't, the notification still shows and the body click still works.
struct NotifAction {
    QString key;
    QString label;
    QString token;
};

// Per-platform desktop-notification client that can put a per-message picture
// (the sender's avatar) into the OS notification — something Qt's
// QSystemTrayIcon::showMessage cannot do on any platform.
//
//   • Linux (freedesktop): calls org.freedesktop.Notifications.Notify with the
//     avatar as the `image-data` hint (the large notification image). Qt only
//     ever sends the QIcon as `app_icon`, which GNOME/Ubuntu ignore.
//   • Windows: shows a WinRT toast with the avatar as a circular
//     `appLogoOverride` (same placement as the Linux image). Qt's path only
//     yields the small Shell_NotifyIcon balloon icon.
//   • macOS: shows an NSUserNotification with the avatar as the right-side
//     `contentImage`. The large left icon is hard-locked to the app icon by the
//     OS — no app can replace it per-notification — so the avatar sits on the
//     right.
//
// Cross-platform contract (.rules): each backend lives in its own TU
// (desktop_notifier_{linux,win,mac}.{cpp,mm}); the header is shared. When a
// backend can't run (no QtDBus, no notification daemon, missing WinRT headers,
// unbundled mac binary, …) isAvailable() returns false and the caller falls
// back to QSystemTrayIcon, so behaviour never regresses.
class DesktopNotifier : public QObject {
    Q_OBJECT
public:
    explicit DesktopNotifier(QObject *parent = nullptr);
    ~DesktopNotifier() override;

    // True when the platform backend is usable.
    bool isAvailable() const { return _available; }

    // Show a notification. `image` is rendered as the notification picture when
    // non-null (any QImage format; converted internally). `token` is an opaque
    // string echoed back via activated() if the user clicks the notification
    // body. `actions` add optional buttons, each echoing its own token on click
    // (see NotifAction); ignored by backends that can't render buttons.
    // Returns false when not delivered, so the caller can fall back to the tray.
    bool notify(
        const QString            &title,
        const QString            &body,
        const QImage             &image,
        const QString            &token,
        const QList<NotifAction> &actions   = {},
        int                       timeoutMs = 5000
    );

    // Implementation hook: the platform click handler calls this to surface a
    // click as the activated() signal. Public only so the Obj-C delegate and the
    // protocol-activation path can reach it; not part of the public contract.
    void emitActivated(const QString &token) { emit activated(token); }

signals:
    void activated(const QString &token); // user clicked a notification

private:
    bool _available = false;

#if defined(Q_OS_LINUX) && defined(MSGA_HAS_DBUS)
private slots:
    void onActionInvoked(uint id, const QString &actionKey);
    void onNotificationClosed(uint id, uint reason);

private:
    // notification id → (action key → caller token). The body click is stored
    // under the "default" key. Entries survive NotificationClosed on purpose:
    // Plasma keeps expired popups in its history and re-emits ActionInvoked
    // when one is clicked there, so dropping the token on close would make
    // history entries dead (issue #40). Bounded FIFO eviction instead.
    QHash<uint, QHash<QString, QString>> _tokens;
    QList<uint>                          _tokenOrder; // insertion order, oldest first
#endif

#if defined(Q_OS_MACOS)
    void *_delegate = nullptr; // MsgaNotifDelegate* (Obj-C), retained
#endif

#if defined(Q_OS_WIN)
    QString _aumid; // AppUserModelID this process publishes toasts under
#endif
};

#include "util/desktop_notifier.h"

#include <QImage>

#if defined(MSGA_HAS_DBUS)

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QStringList>
#include <QVariantMap>

namespace {
constexpr auto kService = "org.freedesktop.Notifications";
constexpr auto kPath    = "/org/freedesktop/Notifications";
constexpr auto kIface   = "org.freedesktop.Notifications";
} // namespace

// The `image-data` hint payload — spec signature (iiibiiay): width, height,
// rowstride, has-alpha, bits-per-sample, channels, raw pixels.
struct FdImage {
    int        width = 0, height = 0, rowStride = 0;
    bool       hasAlpha      = true;
    int        bitsPerSample = 8, channels = 4;
    QByteArray data;
};
Q_DECLARE_METATYPE(FdImage)

static QDBusArgument &operator<<(QDBusArgument &arg, const FdImage &i) {
    arg.beginStructure();
    arg << i.width << i.height << i.rowStride << i.hasAlpha << i.bitsPerSample << i.channels
        << i.data;
    arg.endStructure();
    return arg;
}
static const QDBusArgument &operator>>(const QDBusArgument &arg, FdImage &i) {
    arg.beginStructure();
    arg >> i.width >> i.height >> i.rowStride >> i.hasAlpha >> i.bitsPerSample >> i.channels >>
        i.data;
    arg.endStructure();
    return arg;
}

DesktopNotifier::DesktopNotifier(QObject *parent) : QObject(parent) {
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    auto *iface = bus.interface();
    if (!iface || !iface->isServiceRegistered(kService))
        return;

    qDBusRegisterMetaType<FdImage>();

    // ActionInvoked/NotificationClosed are broadcast for every app's
    // notifications; we filter by ids we actually created (in _tokens).
    bus.connect(
        kService,
        kPath,
        kIface,
        QStringLiteral("ActionInvoked"),
        this,
        SLOT(onActionInvoked(uint, QString))
    );
    bus.connect(
        kService,
        kPath,
        kIface,
        QStringLiteral("NotificationClosed"),
        this,
        SLOT(onNotificationClosed(uint, uint))
    );
    _available = true;
}

DesktopNotifier::~DesktopNotifier() = default;

bool DesktopNotifier::notify(
    const QString &title,
    const QString &body,
    const QImage  &image,
    const QString &token,
    int            timeoutMs
) {
    if (!_available)
        return false;

    QVariantMap hints;
    // Tie the notification to our app entry (grouping, icon fallback, focus).
    hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("msga"));
    hints.insert(QStringLiteral("urgency"), QVariant::fromValue<uchar>(1)); // normal
    if (!image.isNull()) {
        const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
        FdImage      fd;
        fd.width         = rgba.width();
        fd.height        = rgba.height();
        fd.rowStride     = int(rgba.bytesPerLine());
        fd.hasAlpha      = true;
        fd.bitsPerSample = 8;
        fd.channels      = 4;
        fd.data =
            QByteArray(reinterpret_cast<const char *>(rgba.constBits()), int(rgba.sizeInBytes()));
        hints.insert(QStringLiteral("image-data"), QVariant::fromValue(fd));
    }

    // "default" is the implicit action GNOME fires when the body is clicked.
    const QStringList actions{QStringLiteral("default"), QString()};

    QDBusInterface notifier(kService, kPath, kIface, QDBusConnection::sessionBus());
    if (!notifier.isValid())
        return false;
    QDBusReply<uint> reply = notifier.call(
        QStringLiteral("Notify"),
        QStringLiteral("MSGA"),
        uint(0),
        QStringLiteral("msga"),
        title,
        body,
        actions,
        hints,
        int(timeoutMs)
    );
    if (!reply.isValid())
        return false;
    if (!token.isEmpty())
        _tokens.insert(reply.value(), token);
    return true;
}

void DesktopNotifier::onActionInvoked(uint id, const QString &) {
    const auto it = _tokens.constFind(id);
    if (it != _tokens.constEnd())
        emitActivated(it.value());
}

void DesktopNotifier::onNotificationClosed(uint id, uint) {
    _tokens.remove(id);
}

#else // !MSGA_HAS_DBUS — no freedesktop backend; caller falls back to the tray.

DesktopNotifier::DesktopNotifier(QObject *parent) : QObject(parent) {}
DesktopNotifier::~DesktopNotifier() = default;

bool DesktopNotifier::notify(
    const QString &, const QString &, const QImage &, const QString &, int
) {
    return false;
}

#endif

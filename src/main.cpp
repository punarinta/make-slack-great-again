// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/main_window.h"
#include "app/crash_handler.h"
#include "app/single_instance.h"
#include "util/desktop_integration.h"
#include "util/time_format.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileOpenEvent>
#include <QFont>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QSettings>
#include <QTranslator>
#include <QUrlQuery>

#if defined(Q_OS_LINUX)
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <QProcess>
#endif

#if defined(Q_OS_LINUX)
static QFont detectSystemFont() {
    // KDE: ~/.config/kdeglobals [Fonts] General=Family,size,...
    {
        QSettings kde(QDir::homePath() + "/.config/kdeglobals", QSettings::IniFormat);
        kde.beginGroup("Fonts");
        const QString val = kde.value("General").toString();
        if (!val.isEmpty()) {
            const QStringList parts = val.split(',');
            if (parts.size() >= 2) {
                bool      ok;
                const int sz = parts[1].trimmed().toInt(&ok);
                QFont     f(parts[0].trimmed());
                if (ok && sz > 0)
                    f.setPointSize(sz);
                return f;
            }
        }
    }
    // GNOME/GTK: ~/.config/gtk-{4,3}.0/settings.ini  gtk-font-name=Family Size
    for (const char *dir : {"gtk-4.0", "gtk-3.0"}) {
        QFile f(QDir::homePath() + "/.config/" + dir + "/settings.ini");
        if (!f.open(QIODevice::ReadOnly))
            continue;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.startsWith("gtk-font-name")) {
                const QString val = line.section('=', 1).trimmed();
                const int     sp  = val.lastIndexOf(' ');
                if (sp > 0) {
                    bool      ok;
                    const int sz = val.mid(sp + 1).toInt(&ok);
                    QFont     font(val.left(sp).trimmed());
                    if (ok && sz > 0)
                        font.setPointSize(sz);
                    return font;
                }
            }
        }
    }
    // GNOME (dconf): gsettings get org.gnome.desktop.interface font-name → 'Family Size'
    {
        QProcess gs;
        gs.start("gsettings", {"get", "org.gnome.desktop.interface", "font-name"});
        if (gs.waitForFinished(500)) {
            QString val = QString::fromUtf8(gs.readAllStandardOutput()).trimmed();
            val.remove('\'').remove('"');
            const int sp = val.lastIndexOf(' ');
            if (sp > 0) {
                bool      ok;
                const int sz = val.mid(sp + 1).toInt(&ok);
                QFont     font(val.left(sp).trimmed());
                if (ok && sz > 0)
                    font.setPointSize(sz);
                return font;
            }
        }
    }
    return QFont();
}
#endif

int main(int argc, char *argv[]) {
#if defined(Q_OS_LINUX)
    // Wayland fractional scaling (wp_fractional_scale_v1) gives widgets a
    // fractional devicePixelRatio (e.g. 1.3333 at 133%), and Qt's per-widget
    // region rounding then leaves 1-device-pixel artifacts: hairline seams
    // between adjacent widgets and stale border pixels after popups hide
    // (painted vs flushed region mismatch, QTBUG-82601 family). Opting out of
    // the protocol falls back to the integer wl_output scale: Qt renders at
    // 2x and the compositor downscales — the same path GTK apps use, with no
    // fractional rounding anywhere in widget painting. Respect an explicit
    // user override if the variable is already set.
    if (!qEnvironmentVariableIsSet("QT_WAYLAND_DISABLED_INTERFACES"))
        qputenv("QT_WAYLAND_DISABLED_INTERFACES", "wp_fractional_scale_manager_v1");
#endif
    QApplication app(argc, argv);
    app.setApplicationName("MSGA");
    app.setOrganizationName("msga");
    // One-time migration: the default per-conversation notification level changed
    // from "Just mentions" (1) to "All new posts" (0). Existing installs have the
    // old default persisted, which would otherwise mask the new one; reset that
    // leftover value once. A "Just mentions" chosen *after* this still sticks,
    // because the migration flag stops us from touching it again.
    // Safe to remove from July 2026 onward (by then ~all installs have migrated).
    {
        QSettings s("msga", "msga");
        if (!s.value("notifications/defaultMigrated", false).toBool()) {
            if (s.value("notifications/level", 0).toInt() == 1)
                s.setValue("notifications/level", 0);
            s.setValue("notifications/defaultMigrated", true);
        }
    }
    // A crash now prints a stack trace (stderr + crash.log in AppDataLocation)
    // instead of a bare "Segmentation fault", then still core-dumps as before.
    CrashHandler::install();
#if defined(Q_OS_LINUX)
    app.setFont(detectSystemFont());
#endif

    // Collect any msga:// URL argument (delivered by the OS after OAuth redirect).
    QString urlArg;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith("msga://"))
            urlArg = arg;
    }

    // Single-instance guard: forward the URL to a running instance and exit,
    // or become the primary and continue.
    SingleInstance singleInstance;
    if (!singleInstance.init(urlArg))
        return 0;

    // Start TLS handshake to slack.com before any UI is built so it can
    // complete in the background during window construction.
    QNetworkAccessManager preWarmNam;
    preWarmNam.connectToHostEncrypted("slack.com", 443);

    // The language preference from Settings → Appearance overrides the system
    // locale ("system" resolves to it). Changing the preference at runtime
    // requires a restart — the translator is installed only here. Setting the
    // default locale keeps any locale-aware formatting in sync.
    const QLocale uiLocale = TimeFmt::locale();
    QLocale::setDefault(uiLocale);

    QTranslator   translator;
    const QString locale = uiLocale.name(); // e.g. "ja_JP"
    const bool    loaded = translator.load(":/translations/msga_" + locale) ||
                        translator.load(":/translations/msga_" + locale.section('_', 0, 0));
    if (loaded)
        app.installTranslator(&translator);

    MainWindow window;
    // Dispatch incoming msga:// URLs: notification-click activation (Windows
    // toasts use launch="msga://notif?token=…") opens the conversation; anything
    // else is an OAuth redirect.
    auto       routeUri = [&window](const QUrl &uri) {
        if (uri.host() == "notif") {
            const QString token = QUrlQuery(uri).queryItemValue("token", QUrl::FullyDecoded);
            window.handleNotifToken(token);
        } else {
            window.handleOAuthUri(uri);
        }
    };
    QObject::connect(&singleInstance, &SingleInstance::uriReceived, &window, routeUri);
    // On Linux, clicking the dock icon when the window is hidden causes the DE to
    // launch a new process. SingleInstance blocks the second process and emits
    // activateRequested so we can show the window instead of doing nothing.
    QObject::connect(&singleInstance, &SingleInstance::activateRequested, &window, [&window] {
        window.show();
        window.raise();
        window.activateWindow();
    });

    // On macOS, already-running apps receive URLs via QFileOpenEvent (Apple Events),
    // not as command-line arguments. Install a filter on QApplication to catch them.
    class UrlEventFilter : public QObject {
    public:
        UrlEventFilter(MainWindow *w, QObject *parent) : QObject(parent), _window(w) {}
        bool eventFilter(QObject *, QEvent *ev) override {
            if (ev->type() == QEvent::FileOpen) {
                const QUrl url = static_cast<QFileOpenEvent *>(ev)->url();
                if (url.scheme() == "msga") {
                    if (url.host() == "notif")
                        QMetaObject::invokeMethod(
                            _window,
                            "handleNotifToken",
                            Qt::QueuedConnection,
                            Q_ARG(
                                QString, QUrlQuery(url).queryItemValue("token", QUrl::FullyDecoded)
                            )
                        );
                    else
                        QMetaObject::invokeMethod(
                            _window, "handleOAuthUri", Qt::QueuedConnection, Q_ARG(QUrl, url)
                        );
                }
            }
            return false;
        }

    private:
        MainWindow *_window;
    };
    app.installEventFilter(new UrlEventFilter(&window, &app));

    // On macOS, clicking the dock icon when the window is hidden sends
    // ApplicationActivate. Re-show the window in that case.
    QObject::connect(
        &app,
        &QApplication::applicationStateChanged,
        &window,
        [&window](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && !window.isVisible()) {
                window.show();
                window.raise();
                window.activateWindow();
            }
        }
    );

    window.show();
    window.raise();
    window.activateWindow();

    // On freedesktop systems, (re)install the .desktop launcher + icon and the
    // msga:// scheme handler in the background. No-op on macOS/Windows.
    DesktopIntegration::installIfSupported();

    const int ret = app.exec();

    if (ret == MainWindow::kRestartExitCode) {
        // Release the SingleInstance server so the new process can become primary,
        // then re-exec the (already-updated) binary cleanly.
        singleInstance.release();
#if defined(Q_OS_LINUX)
        const QByteArray exe = QCoreApplication::applicationFilePath().toLocal8Bit();
        ::execv(exe.constData(), argv);
        // execv only returns on error; fall through to normal exit.
#elif defined(Q_OS_WIN)
        QProcess::startDetached(
            QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1)
        );
#endif
    }

    return ret;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/main_window.h"

#include "backend/imap/imap_auth.h"          // dev IMAP env-seed (Phase 1)
#include "backend/imap/imap_auth_strategy.h" // IMAP add-account prompt hook
#include "ui/imap_add_account/imap_add_account_dialog.h"
#include "app/crash_handler.h"
#include "app/single_instance.h"
#include "util/desktop_integration.h"
#include "util/time_format.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileOpenEvent>
#include <QFont>
#include <QHostInfo>
#include <QLocale>
#include <QProcess>
#include <QSettings>
#include <QTimer>
#include <QTranslator>
#include <QUrlQuery>

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
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
    // We always set PassThrough so nothing silently rounds the scale factor in a
    // way that up-sizes the UI; the actual scale we render at is decided on Wayland
    // by whether the fractional-scale protocol is enabled (below).
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough
    );
#if defined(Q_OS_LINUX)
    // Scaling policy on Wayland.
    //
    // Native fractional rendering (wp_fractional_scale enabled) gives the crispest
    // text — Qt renders directly at the real fractional devicePixelRatio (e.g. 1.5
    // at 150%). But it has a cost: Qt rounds each widget's painted device-pixel
    // region independently, leaving 1-device-pixel gaps at sibling boundaries
    // (QTBUG-82601). The window backdrop is painted as a colour-matched mirror to
    // hide the *static* seams (see BackdropFrame in main_window.cpp), but a
    // translucent transient overlay (the tooltip) composites onto those gap pixels
    // and they aren't cleanly restored on hide — no in-window repaint reaches a
    // gap that lies between widgets — so a faint seam lingers until something else
    // repaints. We could not find an in-app fix for that transient case.
    //
    // So we DISABLE the fractional-scale protocol by default. Qt then renders at
    // the integer wl_output scale and the compositor downscales as needed. Crucial
    // property: this is a no-op on integer-scaled outputs (100%/200% render 1:1 and
    // stay crisp); the only cost — a bilinear downscale that softens glyphs — is
    // incurred *exactly* on fractionally-scaled outputs, which is precisely where
    // the seam would otherwise appear. Net: the seam is gone everywhere, and only
    // fractional displays pay for it (and there, no-seam beats crisp-with-seam).
    //
    // We cannot decide this per-display at startup: before a surface exists, the
    // screen reports only the integer wl_output scale (a 150% output looks like 2),
    // so the fractional scale is unknowable until it is too late to set this env.
    // Disabling the protocol unconditionally sidesteps that — it self-targets
    // fractional outputs via the downscale described above.
    //
    // Escape hatch: MSGA_FRACTIONAL_SCALE=1 keeps native fractional rendering
    // (crisper text, at the price of the tooltip seam on fractional displays).
    if (qEnvironmentVariableIntValue("MSGA_FRACTIONAL_SCALE") != 1 &&
        !qEnvironmentVariableIsSet("QT_WAYLAND_DISABLED_INTERFACES"))
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
#if defined(MSGA_HANG_WATCHDOG)
    // Dev builds only: arm the main-thread hang watchdog. If the GUI event loop
    // stalls past the threshold, it appends the stuck main thread's backtrace to
    // crash.log (the heartbeat QTimer that proves the loop is alive is started
    // below). AddressSanitizer makes everything ~5-10x slower, so a 5 s threshold
    // false-trips on heavy-but-finite work (e.g. laying out a big conversation);
    // give ASan builds a roomier window so only genuine hangs fire.
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    CrashHandler::startWatchdog(20000);
#else
    CrashHandler::startWatchdog(5000);
#endif
#endif
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

    // Pre-resolve slack.com (the API host — WebApiClient::kBaseUrl is
    // https://slack.com/api/) as early as possible, in parallel with translator
    // load and window construction. Only DNS is warmed, not a socket: the
    // keep-alive connection that serves API calls lives on the per-workspace API
    // client's own QNetworkAccessManager, so a socket opened here could never be
    // reused — it would just sit idle until reaped. The one thing that does
    // carry over is the resolved address: Qt's process-global QHostInfo cache is
    // consulted when the backend later connects (and when PublicBackend fires its
    // own pool-correct preWarm for its API host during ensureSession), so that
    // connect skips the DNS round-trip. A session workspace addresses its own
    // <team>.slack.com host instead (see apiBaseFor) and warms that one itself;
    // slack.com is still the host for OAuth workspaces and for sign-in.
    // Fire-and-forget; qApp scopes the result.
    QHostInfo::lookupHost(QStringLiteral("slack.com"), qApp, [](const QHostInfo &) {});

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

    // IMAP "Add email account" dialog: injected into the (Widgets-free) auth
    // strategy as the credential prompt (imap-backend-plan §5).
    imap::AuthStrategy::setPrompt(&ImapAddAccountDialog::prompt);

    // Dev bridge (Phase 1): also seed an IMAP workspace from IMAP_* env vars when
    // set, so email can be exercised without the dialog. No-op when unset.
    imap::seedDevWorkspaceFromEnv();

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

#if defined(MSGA_HANG_WATCHDOG)
    // Pet the hang watchdog from the event loop. While the main thread keeps
    // pumping, this fires every second and re-arms the watchdog's deadline so it
    // never expires; if the loop wedges, the timer stops and the watchdog dumps
    // the stuck stack. One timer_settime syscall per second — negligible.
    QTimer watchdogHeartbeat;
    QObject::connect(&watchdogHeartbeat, &QTimer::timeout, [] { CrashHandler::heartbeat(); });
    watchdogHeartbeat.start(1000);
#endif

    const int ret = app.exec();

    if (ret == MainWindow::kRestartExitCode) {
        // Release the SingleInstance server so the new process can become primary,
        // then re-exec the (already-updated) binary cleanly.
        singleInstance.release();
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
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

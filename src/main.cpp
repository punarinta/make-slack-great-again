// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/main_window.h"
#include "app/single_instance.h"

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
    QApplication app(argc, argv);
    app.setApplicationName("MSGA");
    app.setOrganizationName("msga");
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

    QTranslator   translator;
    const QString locale = QLocale::system().name(); // e.g. "fr_FR"
    const bool    loaded = translator.load(":/translations/msga_" + locale) ||
                        translator.load(":/translations/msga_" + locale.section('_', 0, 0));
    if (loaded)
        app.installTranslator(&translator);

    MainWindow window;
    QObject::connect(
        &singleInstance, &SingleInstance::uriReceived, &window, &MainWindow::handleOAuthUri
    );
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
                if (url.scheme() == "msga")
                    QMetaObject::invokeMethod(
                        _window, "handleOAuthUri", Qt::QueuedConnection, Q_ARG(QUrl, url)
                    );
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

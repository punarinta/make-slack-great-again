// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/main_window.h"
#include "app/single_instance.h"

#include <QApplication>
#include <QFile>
#include <QLocale>
#include <QTextStream>
#include <QTranslator>

static void loadStyleSheet(QApplication &app) {
    QFile f(":/style.qss");
    if (f.open(QFile::ReadOnly | QFile::Text)) {
        QTextStream ts(&f);
        app.setStyleSheet(ts.readAll());
    }
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("MSGA");
    app.setOrganizationName("msga");

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

    QTranslator translator;
    const QString locale = QLocale::system().name(); // e.g. "fr_FR"
    const bool loaded = translator.load(":/translations/msga_" + locale)
                     || translator.load(":/translations/msga_" + locale.section('_', 0, 0));
    if (loaded)
        app.installTranslator(&translator);

    loadStyleSheet(app);

    MainWindow window;
    QObject::connect(&singleInstance, &SingleInstance::uriReceived,
                     &window, &MainWindow::handleOAuthUri);
    window.show();
    window.raise();
    window.activateWindow();

    return app.exec();
}

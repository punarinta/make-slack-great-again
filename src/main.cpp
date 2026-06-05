// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/main_window.h"
#include "app/single_instance.h"

#include <QApplication>
#include <QFile>
#include <QTextStream>

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

    loadStyleSheet(app);

    MainWindow window;
    QObject::connect(&singleInstance, &SingleInstance::uriReceived,
                     &window, &MainWindow::handleOAuthUri);
    window.show();

    return app.exec();
}

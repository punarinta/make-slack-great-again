// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Smoke + screenshot harness for SessionImportDialog. Renders the dialog (in the
// state a machine with no automatic path shows: guided manual fields revealed) to
// a PNG so the layout can be eyeballed, and asserts the card grew to fit its
// content (regression guard for the clipped/overlapping-steps bug). Browser
// sign-in is pinned off so the rendered state doesn't depend on whether the
// machine running the tests happens to have Chrome installed.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QPixmap>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QWidget>

#include "ui/session_import_dialog/session_import_dialog.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

int main(int argc, char **argv) {
    // Pin browser sign-in off before any widget asks whether it's available, so the
    // rendered state doesn't depend on the test machine having Chrome. Set the var
    // yourself (MSGA_BROWSER_LOGIN=1) to eyeball the browser-enabled layout instead.
    if (!qEnvironmentVariableIsSet("MSGA_BROWSER_LOGIN"))
        qputenv("MSGA_BROWSER_LOGIN", "0");
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-session-import");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();
    static QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());
    return Catch::Session().run(argc, argv);
}

static void pump(int ms) {
    for (int i = 0; i < ms / 5; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

TEST_CASE("session import dialog renders its content without clipping", "[session_import_dialog]") {
    QWidget host;
    host.resize(1000, 840);
    host.show();
    pump(50);

    auto *dlg = new SessionImportDialog(&host);
    dlg->open();
    pump(100);

    // The card must be tall enough to contain all its stacked content. If the
    // guided-steps label's wrapped height weren't counted (the nested-layout bug),
    // the card would clamp far shorter and the steps would overlap the fields.
    QWidget *card = dlg->findChild<QWidget *>(); // the AppDialog card frame
    REQUIRE(card != nullptr);
    CHECK(card->height() > 380);

    const QString out = qEnvironmentVariableIsSet("MSGA_SHOT")
                            ? qEnvironmentVariable("MSGA_SHOT")
                            : QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                  .filePath("session_import_dialog.png");
    // Grab the host so the backdrop + centred card are both captured.
    host.grab().save(out);
    INFO("screenshot: " << out.toStdString());
    CHECK(QFileInfo::exists(out));
}

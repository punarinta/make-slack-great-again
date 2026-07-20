// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "shared_realtime.h"

#include "slack_auth.h"
#include "socket_mode_realtime.h"

#include <QDebug>
#include <QtGlobal>

namespace slack {

namespace {
// Process-global, single-threaded (GUI thread): the one socket plus its
// refcount. Guarded only by the Qt main-thread affinity that all backend
// construction/teardown already obeys.
int                 gRefcount = 0;
SocketModeRealtime *gSocket   = nullptr;
} // namespace

SharedRealtime::SharedRealtime() {
    if (gRefcount++ == 0) {
        // Precedence, highest first: (1) the personal xapp the user saved in
        // Settings → System, (2) the SLACK_XAPP_TOKEN env override (dev), (3) the
        // token compiled in via credentials.cmake. Settings MUST win over the env
        // var so a stale exported SLACK_XAPP_TOKEN can't silently shadow the key a
        // prebuilt-app user just entered (that was a "my settings key is ignored"
        // trap). appConfig().xapp already folds personal-over-compiled, so we only
        // need to know whether it came from settings to place the env var correctly.
        const QString personalXapp = personalAppCredentials().xapp.trimmed();
        const QString envXapp      = qEnvironmentVariable("SLACK_XAPP_TOKEN").trimmed();
        QString       xapp;
        const char   *source = "none";
        if (!personalXapp.isEmpty()) {
            xapp   = personalXapp;
            source = "settings";
        } else if (!envXapp.isEmpty()) {
            xapp   = envXapp;
            source = "env SLACK_XAPP_TOKEN";
        } else if (!appConfig().xapp.isEmpty()) {
            xapp   = appConfig().xapp;
            source = "compiled-in (credentials.cmake)";
        }
        // Mask the token: enough to eyeball WHICH key is live, never the whole
        // secret in a log the user may paste.
        qInfo().noquote() << "Socket Mode: xapp token source =" << source << "prefix ="
                          << (xapp.isEmpty() ? QStringLiteral("(empty)")
                                             : xapp.left(12) + QStringLiteral("…"));
        if (!xapp.isEmpty())
            gSocket = new SocketModeRealtime(xapp);
    }
    _socket = gSocket;
}

SharedRealtime::~SharedRealtime() {
    _socket = nullptr;
    if (--gRefcount == 0) {
        delete gSocket;
        gSocket = nullptr;
    }
}

} // namespace slack

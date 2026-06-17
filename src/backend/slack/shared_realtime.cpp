// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "shared_realtime.h"

#include "slack_auth.h"
#include "socket_mode_realtime.h"

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
        // Env override mirrors the historical ensureSession() behavior.
        const QString xapp = qEnvironmentVariable("SLACK_XAPP_TOKEN", appConfig().xapp);
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

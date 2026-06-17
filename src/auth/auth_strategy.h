// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Neutral, service-agnostic contract for acquiring a workspace's credentials.
#pragma once

#include "auth/token_store.h"

#include <QObject>
#include <QString>
#include <QUrl>

namespace auth {

// The per-service auth provider. A strategy runs whatever flow its service
// needs — Slack's OAuth v2 + PKCE, Telegram's phone+code, Teams' MSAL/Graph
// consent — and on success emits a fully-populated neutral WorkspaceRecord with
// the per-service secrets already encoded into its opaque `auth` blob. The UI
// (MainWindow) drives every service through this contract and never sees a
// service-specific credential type. The factory (auth_strategy_factory.h) is the
// single point that switches *into* a service's strategy, mirroring makeBackend.
class AuthStrategy : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~AuthStrategy() override = default;

    // Begin the flow. Emits succeeded() or failed() exactly once. May open a
    // browser, prompt for a phone number, etc. — that is the strategy's concern.
    virtual void start() = 0;

    // Deliver an OS-routed msga://oauth/callback URI (from SingleInstance).
    // Strategies that don't use a browser-redirect callback (phone+code, device
    // flow) ignore it — hence the no-op default, so the interface can express a
    // non-OAuth flow without forcing a stub override.
    virtual void handleCallbackUri(const QUrl &) {}

signals:
    // The neutral result: ready to hand to TokenStore::saveWorkspace().
    void succeeded(TokenStore::WorkspaceRecord record);
    void failed(QString reason);
};

} // namespace auth

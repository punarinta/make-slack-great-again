// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QCoreApplication>
#include <QNetworkAccessManager>

namespace net {

// Process-wide QNetworkAccessManager for client-side fetches that carry their
// auth per-request (or none): avatar/image downloads, the update checker, the
// LLM providers, and the startup TLS pre-warm.
//
// Each QNAM owns a dedicated network/socket thread for its lifetime, so one
// manager per component meant a fresh thread (and its stack + glibc arena) for
// every cache, checker and provider. Funnelling the auth-agnostic consumers
// through a single shared manager collapses those threads into one and lets
// them reuse the same keep-alive connection pool (fewer TLS handshakes).
//
// NOT used by HttpQueue — the per-workspace API client keeps its own manager so
// that clearConnectionCache() on a transport hiccup stays scoped to one
// workspace, and so per-workspace request queues never share a connection pool.
//
// Main-thread only: the manager is parented to qApp and created lazily on first
// use; every consumer (GUI widgets, providers) lives on the main thread.
inline QNetworkAccessManager *sharedNam() {
    static QNetworkAccessManager *nam = new QNetworkAccessManager(qApp);
    return nam;
}

} // namespace net

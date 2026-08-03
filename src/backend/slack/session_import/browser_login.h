// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// "Sign in with your browser" for Slack session auth: launches a Chromium-family
// browser in a throwaway profile pointed at Slack's sign-in page, then reads the
// resulting `d` session cookie (and, when available, the signed-in workspaces plus
// their xoxc- tokens) out of it over the DevTools protocol. The temp profile is the
// sandbox — nothing touches the user's real browser profile, and the profile is
// wiped when the flow ends.
//
// Cross-platform and dependency-free: QProcess + QWebSocket only, so unlike
// local_importer this is NOT gated behind MSGA_SLACK_SESSION_IMPORT. Every failure
// path emits finished() with a short machine `error` so the UI can fall back to the
// guided manual-paste flow. See docs/BROWSER_LOGIN_PLAN.md.
#pragma once

#include "backend/slack/session_import/session_types.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QProcess;
class QTemporaryDir;
class QTimer;
class QWebSocket;

namespace slack::session {

// True when a drivable browser was found, i.e. the UI should offer the button.
// Set MSGA_BROWSER_LOGIN=0 to force this off (support escape hatch).
[[nodiscard]] bool browserLoginSupported();

// Display name of the browser that would be driven ("Google Chrome", "Brave", …),
// for the button label. Empty when unsupported.
[[nodiscard]] QString browserLoginName();

class BrowserLogin : public QObject {
    Q_OBJECT
public:
    explicit BrowserLogin(QObject *parent = nullptr);
    ~BrowserLogin() override;

    // Launches the browser and starts watching for the session. Emits finished()
    // exactly once. Deleting the object (or the parent dialog) aborts everything
    // and kills the browser.
    void start();

signals:
    // Human-readable step, for the dialog's status line.
    void progress(const QString &message);
    // `cookie` is the `d` value (xoxd-…, verbatim as the browser stored it) and
    // `teams` the discovered workspaces (possibly empty — then the UI asks for the
    // workspace address). On failure `cookie` is empty and `error` holds a short
    // reason: no_browser, launch_failed, no_devtools, cdp_failed, cancelled, timeout.
    void finished(const QString &cookie, const QList<TeamSession> &teams, const QString &error);

private:
    using Handler = std::function<void(const QJsonObject &result, bool ok)>;

    void probeDevTools();
    void openCdp(const QString &wsUrl);
    void poll();
    void
    send(const QString &method, const QJsonObject &params, const QString &sessionId, Handler h);
    void onMessage(const QString &text);
    void onEvent(const QString &method, const QJsonObject &params, const QString &sessionId);
    void attachPage(const QJsonValue &targetId);
    void interceptHandoff(const QString &sessionId);
    void handleHandoff(const QString &url, const QString &requestId, const QString &sessionId);
    void collectCookies(const QJsonObject &result);
    void discoverTeams();
    void readLocalConfig(const QString &sessionId);
    void finish(const QString &cookie, const QList<TeamSession> &teams, const QString &error);
    void shutdown();

    QProcess                      *_proc = nullptr;
    QPointer<QProcess>             _browserProc; // survives _proc being handed off
    std::shared_ptr<QTemporaryDir> _profile;
    QString                        _profilePath; // outlives _profile, for the final sweep
    QNetworkAccessManager         *_nam        = nullptr;
    QWebSocket                    *_ws         = nullptr;
    QTimer                        *_probe      = nullptr;
    QTimer                        *_poll       = nullptr;
    int                            _port       = 0;
    bool                           _probing    = false;
    bool                           _done       = false;
    bool                           _evaluating = false;
    QElapsedTimer                  _since;    // whole flow, for the overall timeout
    QElapsedTimer                  _cookieAt; // cookie found, for the team grace period
    QString                        _cookie;
    QSet<QString>                  _hosts;        // harvested *.slack.com hosts (fallback)
    QString                        _sessionId;    // flat CDP session of the app.slack.com page
    QHash<QString, QString>        _pageSessions; // page target id → flat CDP session
    QHash<int, Handler>            _pending;      // CDP request id → continuation
    int                            _nextId = 1;
};

// ── Pure helpers, exposed for tests ─────────────────────────────────────────────

// Pulls "webSocketDebuggerUrl" out of a /json/version response body.
[[nodiscard]] QString parseDebuggerUrl(const QByteArray &jsonVersion);

// Parses the web client's localStorage `localConfig_v2` blob into workspaces,
// including their xoxc- tokens (so the deriver only has to validate them).
[[nodiscard]] QList<TeamSession> parseLocalConfig(const QString &json);

// Fallback discovery: extracts team workspaces from arbitrary URLs/cookie domains,
// skipping Slack's own infrastructure subdomains. Tokens are left empty for the
// deriver to scrape.
[[nodiscard]] QList<TeamSession> teamsFromHosts(const QStringList &values);

} // namespace slack::session

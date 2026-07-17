// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"
#include "rpl/event_stream.h"

#include <QObject>
#include <QWebSocket>
#include <QNetworkAccessManager>
#include <QUrl>
#include <deque>
#include <vector>

class QNetworkReply;

class QTimer;

// Connects to Slack's Socket Mode over WebSocket.
// Requires an xapp- app-level token with the connections:write scope.
// Calls apps.connections.open to get a one-use wss URL, then maintains
// a persistent WebSocket connection, acking envelopes and normalizing
// incoming Events for the Backend event stream.
//
// The connection is app-level: Slack delivers events for EVERY workspace the
// app is installed in over this one socket (and round-robins events between
// sockets if several are open for the same app). One instance is therefore
// shared by all workspace backends; each backend registers its event stream
// as a sink and every normalized event is broadcast to all sinks. Sinks
// (Sessions) ignore events for conversations/users they don't know.
namespace slack {

class SocketModeRealtime : public QObject {
    Q_OBJECT
public:
    explicit SocketModeRealtime(QString xappToken, QObject *parent = nullptr);
    ~SocketModeRealtime() override;

    // Idempotent — the first call opens the connection.
    void start();
    void stop();

    // Safety-net health checks driven by the Session's periodic timer (the
    // watchdog only catches a socket that goes silent; it can't see a socket
    // that still answers pings but to which Slack has quietly stopped routing
    // events). ensureConnected() reconnects only if the socket has dropped and
    // no connect cycle is already underway — a cheap no-op while healthy, it
    // covers a reconnect that stalled past the watchdog. reconnectNow() recovers
    // a silently-stalled-but-still-connected socket via a gapless overlapping
    // replacement (the old socket keeps being read until the new one connects),
    // used when the app detected the stream had silently missed events; it would
    // otherwise drop yet more events during the swap and feed a reconnect storm.
    void ensureConnected();
    void reconnectNow();

    // Register/unregister a backend's event stream. Events are broadcast to
    // all registered sinks.
    void addSink(rpl::event_stream<Event> *events);
    void removeSink(rpl::event_stream<Event> *events);

    // Subscribe to presence events for the given user IDs on behalf of `sink`.
    // The union of all sinks' IDs is sent immediately if connected, or queued
    // for the next connect.
    void subscribePresence(rpl::event_stream<Event> *sink, QStringList userIds);

    // ── Test seams ──────────────────────────────────────────────────────────
    // Point the apps.connections.open handshake at a local server.
    void setConnectionsOpenUrlForTest(const QUrl &url) { _openUrl = url; }
    // Speed up the liveness watchdog so tests don't wait tens of seconds.
    void setWatchdogTimingForTest(int watchdogMs, int staleMs);
    // Trigger an extra connect attempt, as the watchdog/reachability watcher
    // would. The single-flight guard must coalesce it while one is already
    // underway (regression test for overlapping reconnects opening competing
    // sockets — Slack delivers each event to only one connection).
    void connectNowForTest() { openAndConnect(); }
    // True once the live socket has completed its handshake — lets a test wait
    // until reconnectNow() will take the gapless overlapping-replacement path
    // rather than the not-connected fallback.
    bool isConnectedForTest() const {
        return _ws && _ws->state() == QAbstractSocket::ConnectedState;
    }

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &text);

private:
    void                 openAndConnect();
    void                 connectWs(const QUrl &url);
    void                 ack(const QString &envelopeId);
    void                 scheduleReconnect();
    // Abort any in-flight handshake and the current socket (signals first, so
    // their teardown can't re-enter our slots) and clear the single-flight
    // guard. Shared by stop() and forceReconnect().
    void                 teardownConnection();
    // Tear down the current socket and reconnect immediately (no backoff). Used
    // by the liveness watchdog when the connection has gone silently dead.
    void                 forceReconnect();
    // Watchdog: pings the socket and forces a reconnect if it has gone silent.
    void                 checkLiveness();
    // Record that the socket is alive right now (wall clock).
    void                 touchActivity();
    // Wire up QNetworkInformation so we reconnect the instant the OS reports the
    // network is reachable again (e.g. right after wake), instead of waiting for
    // the watchdog deadline. Best-effort: no-op if no backend is available.
    void                 setupReachabilityWatch();
    void                 sendPresenceSub();
    // Broadcast an event to every registered sink (workspace backend).
    void                 broadcast(const Event &e);
    // Contention detection: distinguish Slack evicting our socket from the app's
    // full connection pool (another instance on the shared xapp token) from a
    // routine drop. noteBareClose() records a bare code-1000 close and, once
    // enough cluster in the window, raises the notice; maybeNotifyContention()
    // broadcasts EvRealtimeContended, throttled so a sustained storm warns once
    // per window. See EvRealtimeContended.
    void                 noteBareClose();
    void                 maybeNotifyContention();
    std::optional<Event> normalizeSlackEvent(const QJsonObject &event);
    // Extra huddle-state event for a huddle_thread message (start) or its edit
    // (end); additive — does not replace the message's normal event.
    std::optional<Event> huddleEventFor(const QJsonObject &event);

    QString                                 _xappToken;
    std::vector<rpl::event_stream<Event> *> _sinks; // non-owning
    QNetworkAccessManager                  *_nam;
    QWebSocket                             *_ws             = nullptr;
    // A replacement socket being established while _ws is still live. Used for
    // gapless recycling: when Slack warns it will soon recycle the current
    // socket, we open this one and keep reading events off _ws until it connects
    // (onConnected), then promote it to _ws and drop the old one — so the handover
    // leaves no window without a live socket (Slack does not replay events missed
    // while disconnected). nullptr when no replacement is in flight; _ws itself is
    // only ever a fully-connected socket (a not-yet-connected one lives here).
    QWebSocket                             *_pendingWs      = nullptr;
    // The in-flight apps.connections.open reply, or nullptr. Tracked so a
    // teardown can abort it and the late `finished` handler can detect that it
    // was superseded (a stale handshake must never establish a competing socket).
    QNetworkReply                          *_openReply      = nullptr;
    // Single-flight guard: true from the moment a connect cycle begins (the
    // apps.connections.open POST is sent) until the socket is established
    // (onConnected) or the attempt fails. While set, further connect triggers
    // are ignored — Slack load-balances each event to exactly ONE of an app's
    // open sockets, so a second, app-unread socket would silently steal events.
    bool                                    _connecting     = false;
    bool                                    _started        = false;
    bool                                    _stopped        = false;
    // Set once the first Socket Mode session is live ("hello"). A later "hello"
    // means the session was re-established after a gap → fire
    // EvRealtimeReconnected so backends/UI backfill the events Slack didn't
    // replay. Not fired on the first hello (the initial load already covers it).
    bool                                    _hadHello       = false;
    int                                     _reconnectMs    = 1000; // exponential backoff, max 30s
    // Liveness watchdog. A sleeping laptop severs the TCP connection silently —
    // QWebSocket can be left half-open ("connected" but dead) so `disconnected`
    // never fires and we'd never reconnect. The watchdog pings the socket and,
    // if no frame/pong has arrived within a deadline, forces a reconnect.
    // `_lastActivityMs` is WALL-CLOCK (QDateTime), not monotonic: monotonic time
    // pauses during system suspend, so a suspend gap would be invisible to it;
    // wall clock makes the post-wake staleness obvious.
    QTimer                                 *_watchdog       = nullptr;
    qint64                                  _lastActivityMs = 0;
    int                                     _watchdogMs     = 20000;
    int                                     _staleMs        = 50000;
    bool                                    _reachabilityWatched = false;
    // apps.connections.open endpoint; overridable for tests.
    QUrl                                    _openUrl{"https://slack.com/api/apps.connections.open"};
    // users to track, per sink; the union is re-sent on every connect
    QHash<rpl::event_stream<Event> *, QStringList> _presenceIds;

    // ── Connection-pool contention detection ─────────────────────────────────
    // Slack cleanly closing our LIVE socket with code 1000 and NO preceding
    // "disconnect" envelope is an eviction from the app's ≤10-connection pool —
    // the signature of another msga instance sharing the compiled-in app-level
    // xapp token (see EvRealtimeContended). One such close can be a routine server
    // recycle; several within _contentionWindowMs is contention. Wall-clock ms of
    // recent bare closes, pruned to the window.
    std::deque<qint64>   _bareCloseTimes;
    // True for the brief span between handling a server "disconnect" envelope and
    // the close it triggers, so onDisconnected knows that close was expected (we
    // asked for it) and must NOT be counted as a bare eviction. Our own close()
    // reports the same code 1000, so this flag — not the code — is what tells the
    // two apart.
    bool                 _serverRequestedClose   = false;
    // Wall-clock ms of the last contention notice, throttling it so a sustained
    // storm raises EvRealtimeContended at most once per _contentionNoticeGapMs.
    qint64               _lastContentionNoticeMs = 0;
    // Other clients' share of the app's connection pool: the last hello's
    // num_connections minus the sockets we hold. >0 means another client is on
    // the shared xapp token (another device/account — never a second local copy,
    // which SingleInstance forbids). Carried into EvRealtimeContended.
    int                  _lastOtherConnections   = 0;
    // Wall-clock ms of our last overlapping-replacement promotion (a recycle in
    // which we briefly held two of our OWN sockets). For a short grace window
    // after it, Slack's num_connections may still count the socket we just
    // aborted, so we allow one extra before calling it contention — otherwise a
    // routine recycle would masquerade as a competitor.
    qint64               _recentOverlapPromoteMs = 0;
    static constexpr int _contentionWindowMs     = 60'000;  // 1-min sliding window
    static constexpr int _contentionThreshold    = 3;       // bare closes ⇒ contention
    static constexpr int _contentionNoticeGapMs  = 300'000; // ≥5 min between notices
    static constexpr int _overlapGraceMs         = 5'000;   // num_connections settle window
};

} // namespace slack

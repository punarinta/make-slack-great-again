// Slack Socket Mode: the
// app-level WebSocket an xapp- token opens (apps.connections.open → a
// one-use wss URL), its envelope acks, and the recovery machinery around it.
//
// The connection belongs to the APP, not to a workspace: Slack delivers the
// events of every workspace the app is installed in over it, and spreads
// each event over ONE of the app's open sockets (at most 10, fleet-wide).
// A second socket of ours would silently swallow a share of the events, so
// there is exactly one per app token: whoever creates SlackBackends hands
// each the same instance (shared_ptr; the socket closes with the last
// holder) and every backend registers as a sink. Every event goes to every
// sink; a sink drops what is not its workspace's.
//
// Recovery:
//   - one connect cycle at a time (a single-flight guard), exponential
//     backoff (1 s … 30 s) reset only after a connection proved durable
//     (60 s) — an evicted socket must not reconnect once a second;
//   - Slack's "disconnect"/"warning" and reconnectNow() bring up an
//     OVERLAPPING replacement: the old socket is read until the new one is
//     open, so the swap loses nothing (Slack never replays missed events);
//   - a watchdog whose own tick cadence reveals a suspend gap (the laptop
//     slept: TCP is gone but nothing said so) forces a fresh connect; mere
//     silence is NOT death (Slack never answers client pings). Every other
//     tick sends a WebSocket ping: a keepalive for NAT/middlebox mappings,
//     not a probe;
//   - the OS saying the network is back (plat NetworkChanged, online) on a
//     socket that is down reconnects at once instead of waiting out the
//     backoff; on a live one it just pings;
//   - a later "hello" after the first is a reconnect after a gap: sinks
//     backfill (reconnected);
//   - contention: a hello counting more connections than ours means another
//     device runs the same app keys and steals events. Sinks hear it at most
//     once per 5 minutes (contended). A too_many_websockets disconnect or 3
//     bare code-1000 closes within a minute raise it too, but only while a
//     hello has counted others: with none, the closes are a network that
//     drops WebSocket keepalives, and that is only logged.
#pragma once

#include "base/json.h"
#include "net/net.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace plat {
class App;
}

namespace slack {

class SocketMode {
public:
    SocketMode(plat::App &app, net::Client &client, std::string appToken);
    ~SocketMode(); // closes; no sink callback runs afterwards
    SocketMode(const SocketMode &)            = delete;
    SocketMode &operator=(const SocketMode &) = delete;

    // Everything runs on the UI thread. A sink may remove itself (or another)
    // from inside a callback.
    struct Sink {
        // An events_api envelope's payload: team_id, authorizations, event…
        std::function<void(const json::Value &payload)> event;
        std::function<void()>                           reconnected;
        std::function<void(int otherConnections)>       contended;
    };
    using SinkId = uint32_t;
    SinkId addSink(Sink sink);
    void   removeSink(SinkId id);

    // Idempotent: the first call opens the connection.
    void start();
    void stop();
    // The safety net's calls (the backends' poll tick). ensureConnected: a
    // started socket that is down with nothing in flight reconnects (a no-op
    // while healthy). reconnectNow: a socket that still looks fine but
    // missed events — the overlapping replacement.
    void ensureConnected();
    void reconnectNow();
    // plat's NetworkChanged: the network's reachability changed.
    void networkChanged(bool online);
    bool connected() const;

    // ── Test seams ──────────────────────────────────────────────────────────
    struct Timing {
        int watchdogMs = 20'000, staleMs = 50'000; // the suspend detector
        int stableMs  = 60'000;                    // a durable connection resets the backoff
        int backoffMs = 1'000, backoffMaxMs = 30'000;
    };
    void setTimingForTest(const Timing &t);
    int  socketsForTest() const { return (_ws ? 1 : 0) + (_pending ? 1 : 0); }

private:
    void openAndConnect();
    void connectWs(const std::string &url);
    void teardown();
    void forceReconnect();
    void scheduleReconnect();
    void checkLiveness();
    void onOpen(net::WebSocket *sock);
    void onClosed(net::WebSocket *sock, int code);
    void onText(std::string text);
    enum Notice : uint8_t { kEvent, kReconnected, kContended };
    void notifySinks(Notice what, const json::Value &payload, int others);
    void noteBareClose();
    void maybeNotifyContention();

    plat::App   &_app;
    net::Client &_client;
    std::string  _token;
    Timing       _t;

    struct Slot {
        SinkId id;
        Sink   sink;
    };
    std::vector<Slot> _sinks;
    SinkId            _nextSink = 1;

    // _ws is only ever an open socket; a not-yet-open one (the first, or an
    // overlapping replacement) lives in _pending.
    std::unique_ptr<net::WebSocket> _ws, _pending;
    net::RequestId                  _openReq    = 0;
    bool                            _connecting = false; // single-flight guard
    bool                            _started = false, _stopped = false;
    bool                            _hadHello = false;
    int                             _reconnectMs;
    uint64_t                        _reconnectTimer = 0, _watchdog = 0;
    int64_t                         _connectedSince = 0; // wall ms; 0 = not connected
    int64_t                         _lastCheck      = 0; // wall ms of the last watchdog tick
    std::shared_ptr<bool>           _alive;

    // Contention detection (see the header comment).
    std::vector<int64_t> _bareCloses; // wall ms, pruned to the window
    bool                 _serverRequestedClose = false;
    int64_t              _lastNotice           = 0;
    int                  _otherConnections     = 0;
    int64_t              _overlapPromoted      = 0;
    static constexpr int kContentionWindowMs = 60'000, kContentionThreshold = 3;
    static constexpr int kNoticeGapMs = 300'000, kOverlapGraceMs = 5'000;
};

} // namespace slack

// SocketMode (see socket_mode.h), over net::WebSocket: pings are
// net::WebSocket::ping (WinHTTP sends its own keepalives instead), and
// reachability is plat's NetworkChanged, which the app hands to
// networkChanged().
#include "app/slack/socket_mode.h"

#include "app/slack/web_api.h"
#include "base/log.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>

namespace slack {

namespace {

int64_t wallMs() {
    return base::nowMicros() / 1000;
}

} // namespace

SocketMode::SocketMode(plat::App &app, net::Client &client, std::string appToken)
    : _app(app), _client(client), _token(std::move(appToken)), _reconnectMs(_t.backoffMs),
      _alive(std::make_shared<bool>(true)) {}

SocketMode::~SocketMode() {
    *_alive = false;
    _sinks.clear();
    stop();
}

SocketMode::SinkId SocketMode::addSink(Sink sink) {
    const SinkId id = _nextSink++;
    _sinks.push_back({id, std::move(sink)});
    return id;
}

void SocketMode::removeSink(SinkId id) {
    std::erase_if(_sinks, [id](const Slot &s) { return s.id == id; });
}

void SocketMode::setTimingForTest(const Timing &t) {
    _t           = t;
    _reconnectMs = t.backoffMs;
    if (_watchdog) {
        _app.cancelTimer(_watchdog);
        _watchdog = _app.addTimer(_t.watchdogMs, true, [this] { checkLiveness(); });
    }
}

void SocketMode::start() {
    if (_started)
        return;
    _started  = true;
    // Force-reconnects ONLY when its own cadence shows the process was
    // frozen (checkLiveness), never on idle silence.
    _watchdog = _app.addTimer(_t.watchdogMs, true, [this] { checkLiveness(); });
    openAndConnect();
}

void SocketMode::stop() {
    _stopped = true;
    teardown();
    if (_watchdog)
        _app.cancelTimer(_watchdog);
    if (_reconnectTimer)
        _app.cancelTimer(_reconnectTimer);
    _watchdog = _reconnectTimer = 0;
}

bool SocketMode::connected() const {
    return _ws && _ws->isOpen();
}

void SocketMode::teardown() {
    // The handshake in flight is cancelled (its answer must never open a
    // competing socket), and both sockets go without a word.
    if (_openReq)
        _client.cancel(_openReq);
    _openReq = 0;
    retireSocket(_app, _pending);
    retireSocket(_app, _ws);
    _connecting = false;
}

// ── Connecting ──────────────────────────────────────────────────────────────

void SocketMode::openAndConnect() {
    // One cycle at a time: triggers from a close, the watchdog, a warning and
    // the safety poll would otherwise race into several live sockets, and
    // each one we don't read swallows a share of the events.
    if (_stopped || _connecting)
        return;
    _connecting = true;
    Auth auth;
    auth.token = _token;
    _openReq   = apiCall(
        _client,
        auth,
        "apps.connections.open",
        {},
        [this, alive = _alive](const json::Document &doc, const std::string &err) {
            if (!*alive)
                return;
            _openReq = 0;
            if (_stopped) {
                _connecting = false;
                return;
            }
            const std::string_view url = doc.root()["url"].str();
            if (!err.empty() || url.empty()) {
                LOG_WARN(
                    "slack",
                    "Socket Mode: apps.connections.open error: %s",
                    err.empty() ? "no url" : err.c_str()
                );
                _connecting = false;
                scheduleReconnect();
                return;
            }
            // The backoff is NOT reset here: a handshake proves nothing about
            // the socket surviving (an evicted one would retry once a second).
            connectWs(std::string(url));
        }
    );
}

void SocketMode::connectWs(const std::string &url) {
    // Into _pending; _ws (if any) stays live until onOpen promotes this one.
    retireSocket(_app, _pending);
    _pending             = std::make_unique<net::WebSocket>(_app);
    net::WebSocket *sock = _pending.get();
    sock->onOpen         = [this, sock] { onOpen(sock); };
    sock->onText         = [this](std::string text) { onText(std::move(text)); };
    sock->onClosed       = [this, sock](int code, std::string reason) {
        if (code)
            LOG_INFO("slack", "Socket Mode: closed — code %d %s", code, reason.c_str());
        onClosed(sock, code);
    };
    sock->open(url);
}

void SocketMode::scheduleReconnect() {
    if (_reconnectTimer)
        return; // one pending at a time
    LOG_INFO("slack", "Socket Mode: reconnecting in %d ms", _reconnectMs);
    _reconnectTimer = _app.addTimer(_reconnectMs, false, [this] {
        _reconnectTimer = 0;
        if (!_stopped)
            openAndConnect();
    });
    _reconnectMs    = std::min(_reconnectMs * 2, _t.backoffMaxMs);
}

void SocketMode::ensureConnected() {
    if (!_started || _stopped || _connecting || connected())
        return;
    // Started, down, nothing in flight: a reconnect stalled. Now.
    LOG_INFO("slack", "Socket Mode: ensureConnected — not connected, reconnecting");
    if (_reconnectTimer) {
        _app.cancelTimer(_reconnectTimer);
        _reconnectTimer = 0;
    }
    openAndConnect();
}

void SocketMode::reconnectNow() {
    if (!_started || _stopped)
        return;
    // The socket still looks fine but Slack stopped routing events to it:
    // replace it the gapless way a "warning" does. A teardown first would
    // drop more events (for every workspace) and feed a reconnect storm.
    if (connected()) {
        LOG_WARN("slack", "Socket Mode: realtime stalled — bringing up overlapping replacement");
        _reconnectMs = _t.backoffMs;
        openAndConnect();
        return;
    }
    forceReconnect();
}

void SocketMode::forceReconnect() {
    if (_stopped)
        return;
    LOG_WARN("slack", "Socket Mode: forcing reconnect");
    teardown();
    if (_reconnectTimer) {
        _app.cancelTimer(_reconnectTimer);
        _reconnectTimer = 0;
    }
    _reconnectMs = _t.backoffMs;
    openAndConnect();
}

void SocketMode::checkLiveness() {
    // The gap is measured on every tick, connected or not, so a reconnect in
    // progress can't leave a baseline that later reads as a suspend. Wall
    // clock on purpose: a monotonic one pauses during suspend.
    const int64_t now = wallMs(), gap = _lastCheck ? now - _lastCheck : 0;
    _lastCheck = now;
    if (_stopped || !connected())
        return; // onClosed / scheduleReconnect own recovery
    if (gap > _t.staleMs) {
        LOG_WARN(
            "slack", "Socket Mode: watchdog gap %lld ms — process was suspended", (long long)gap
        );
        forceReconnect();
        return;
    }
    // Keepalive only, NOT a liveness probe: Slack never pongs it, so its
    // absence means nothing. It keeps middlebox NAT mappings from dropping
    // an idle connection.
    _ws->ping();
}

void SocketMode::networkChanged(bool online) {
    if (!online || !_started || _stopped)
        return; // only the network coming back matters
    if (!connected()) {
        // Down, maybe waiting out the backoff: recover now.
        LOG_INFO("slack", "Socket Mode: network reachable — reconnecting now");
        forceReconnect();
    } else {
        // It may have died silently across the switch: a ping makes a dead
        // TCP connection fail (and close) sooner.
        _ws->ping();
    }
}

void SocketMode::onOpen(net::WebSocket *sock) {
    if (sock != _pending.get())
        return; // superseded
    // Promote; an old socket still live (a recycle overlap) goes only now.
    if (_ws) {
        retireSocket(_app, _ws);
        // Slack's next num_connections may still count the one just dropped.
        _overlapPromoted = wallMs();
    }
    _ws             = std::move(_pending);
    _connecting     = false;
    _connectedSince = wallMs();
    LOG_INFO("slack", "Socket Mode: connected");
}

void SocketMode::onClosed(net::WebSocket *sock, int code) {
    if (sock == _pending.get()) {
        // Never opened (code 0) or dropped before promotion: the attempt
        // failed. A live _ws (an overlap) keeps serving meanwhile.
        retireSocket(_app, _pending);
        _connecting = false;
        if (!_stopped)
            scheduleReconnect();
        return;
    }
    if (sock != _ws.get())
        return;
    const bool expected   = _serverRequestedClose;
    _serverRequestedClose = false;
    // Slack closing our live socket normally with no "disconnect" envelope
    // first: an eviction from the app's pool, or a network that drops our
    // keepalives — told apart by the hello counts (maybeNotifyContention).
    const bool bareClose  = !expected && code == 1000;
    retireSocket(_app, _ws);
    // A replacement already in flight finishes instead of racing a second.
    if (_connecting || _pending)
        return;
    const int64_t now   = wallMs();
    const int64_t alive = _connectedSince ? now - _connectedSince : 0;
    _connectedSince     = 0;
    if (bareClose) {
        LOG_INFO("slack", "Socket Mode: bare 1000 close after %lld ms alive", (long long)alive);
        noteBareClose();
    }
    if (alive >= _t.stableMs)
        _reconnectMs = _t.backoffMs;
    if (!_stopped)
        scheduleReconnect();
}

// ── Envelopes ───────────────────────────────────────────────────────────────

void SocketMode::onText(std::string text) {
    json::Document doc;
    if (!doc.parse(std::move(text)))
        return;
    const json::Value      env  = doc.root();
    const std::string_view type = env["type"].str();

    if (type == "hello") {
        // num_connections counts the app's sockets fleet-wide; beyond the
        // ones we hold, another client uses the same app keys (never a second
        // local copy: the app is single-instance).
        const int     total  = int(env["num_connections"].integer(1));
        const int     ours   = socketsForTest();
        const int64_t now    = wallMs();
        const bool    recent = _overlapPromoted && now - _overlapPromoted < kOverlapGraceMs;
        _otherConnections    = std::max(0, total - ours - (recent ? 1 : 0));
        LOG_INFO(
            "slack",
            "Socket Mode: hello — num_connections %d (we hold %d) host %.*s",
            total,
            ours,
            int(env["debug_info"]["host"].str().size()),
            env["debug_info"]["host"].str().data()
        );
        if (_otherConnections > 0)
            maybeNotifyContention();
        // A later hello: re-established after a gap Slack won't replay.
        if (_hadHello)
            notifySinks(kReconnected, env, 0);
        _hadHello = true;
        return;
    }

    if (type == "disconnect") {
        const std::string_view reason = env["reason"].str();
        // "warning": this socket will be recycled soon and Slack keeps
        // delivering on it meanwhile — bring up the replacement now and read
        // both until it is open.
        if (reason == "warning") {
            LOG_INFO("slack", "Socket Mode: disconnect warning — opening overlapping replacement");
            openAndConnect();
            return;
        }
        LOG_INFO(
            "slack",
            "Socket Mode: server requested disconnect — %.*s",
            int(reason.size()),
            reason.data()
        );
        if (reason == "too_many_connections" || reason == "too_many_websockets")
            maybeNotifyContention();
        // A close we asked for: not an eviction (our close reports 1000 too).
        _serverRequestedClose = true;
        if (_ws)
            _ws->close();
        return;
    }

    if (type == "events_api") {
        // Ack first, always: an unacked envelope is redelivered.
        if (_ws) {
            std::string ack = "{\"envelope_id\":";
            json::escapeString(ack, env["envelope_id"].str());
            ack += '}';
            _ws->sendText(ack);
        }
        notifySinks(kEvent, env["payload"], 0);
    }
}

// One handler of every sink, over a copy of the ids and a copy of each
// handler: a sink may remove itself (or another) meanwhile.
void SocketMode::notifySinks(Notice what, const json::Value &payload, int others) {
    std::vector<SinkId> ids;
    for (const Slot &s : _sinks)
        ids.push_back(s.id);
    for (SinkId id : ids)
        for (const Slot &s : _sinks) {
            if (s.id != id)
                continue;
            if (what == kEvent && s.sink.event) {
                auto fn = s.sink.event;
                fn(payload);
            } else if (what == kReconnected && s.sink.reconnected) {
                auto fn = s.sink.reconnected;
                fn();
            } else if (what == kContended && s.sink.contended) {
                auto fn = s.sink.contended;
                fn(others);
            }
            break;
        }
}

void SocketMode::noteBareClose() {
    const int64_t now = wallMs();
    _bareCloses.push_back(now);
    while (!_bareCloses.empty() && now - _bareCloses.front() > kContentionWindowMs)
        _bareCloses.erase(_bareCloses.begin()); // a handful: a vector will do
    if (int(_bareCloses.size()) >= kContentionThreshold)
        maybeNotifyContention();
}

void SocketMode::maybeNotifyContention() {
    const int64_t now = wallMs();
    if (_lastNotice && now - _lastNotice < kNoticeGapMs)
        return; // one notice per window, however long the storm
    _lastNotice = now;
    if (_otherConnections <= 0) {
        // Repeated closes with nobody else counted: not contention but a
        // proxy/VPN/firewall dropping WebSocket control frames.
        LOG_WARN(
            "slack",
            "Socket Mode: socket repeatedly closed by the server with no other connections — "
            "realtime keepalives may be blocked by a proxy/VPN/firewall"
        );
        return;
    }
    LOG_WARN(
        "slack",
        "Socket Mode: connection-pool contention — %d other connection(s) on this app's keys",
        _otherConnections
    );
    notifySinks(kContended, json::Value(), _otherConnections);
}

} // namespace slack

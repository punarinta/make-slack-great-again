// RtmPresence (see rtm_presence.h), over net::WebSocket.
#include "app/slack/rtm_presence.h"

#include "app/slack/web_api.h"

#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

#include <algorithm>

namespace slack {

namespace {

// Pings unanswered before a silent socket counts as dead (a minute at the
// default cadence; also what a sleep gap looks like).
constexpr int kMaxMissedPongs = 2;

// rtm.connect answers meaning "this token never gets a socket" (dead
// credentials, or the method refused for it): retrying would only churn the
// rate limit. Anything else backs off and retries.
bool fatalConnectError(const std::string &e) {
    return isAuthError(e) || isMethodUnavailable(e);
}

} // namespace

RtmPresence::RtmPresence(plat::App &app, Connect connect, std::string cookie)
    : _app(app), _connect(std::move(connect)), _cookie(std::move(cookie)),
      _alive(std::make_shared<bool>(true)) {}

RtmPresence::~RtmPresence() {
    *_alive        = false;
    onStateChanged = nullptr;
    teardown();
    stopTimer(_idleTimer);
    for (Waiter &w : _waiters) // never answered: their owner goes too
        stopTimer(w.timer);
    _waiters.clear();
}

void RtmPresence::hold() {
    if (++_holds > 1 || (_mode != Mode::Native && _state != Link::Idle))
        return; // the mode keeps it up already (or will)
    if (_unavailable)
        return;
    if (_state == Link::Idle || _state == Link::Off)
        setState(Link::Connecting);
    ensureHolding();
}

void RtmPresence::release() {
    if (_holds == 0 || --_holds > 0)
        return;
    if (_mode == Mode::Native) {
        teardown();
        setState(Link::Off);
        settleWaiters(false);
    } else if (_mode == Mode::WhileUsing && !_idleTimer) {
        onIdle(); // the idle time ran out while it was held
    }
}

void RtmPresence::whenActive(int timeoutMs, std::function<void(bool)> done) {
    if (!done)
        return;
    if (_state == Link::Active && connected()) {
        done(true);
        return;
    }
    if (!holding() || _unavailable) {
        done(false);
        return;
    }
    Waiter w;
    w.key   = ++_nextWaiter;
    w.done  = std::move(done);
    w.timer = _app.addTimer(timeoutMs, false, [this, key = w.key] {
        for (size_t i = 0; i < _waiters.size(); ++i)
            if (_waiters[i].key == key) {
                auto fn = std::move(_waiters[i].done);
                _waiters.erase(_waiters.begin() + long(i));
                fn(false);
                return;
            }
    });
    _waiters.push_back(std::move(w));
}

void RtmPresence::settleWaiters(bool up) {
    std::vector<Waiter> ws = std::move(_waiters);
    _waiters.clear();
    for (Waiter &w : ws) {
        stopTimer(w.timer);
        if (w.done)
            w.done(up);
    }
}

void RtmPresence::stopTimer(uint64_t &id) {
    if (id)
        _app.cancelTimer(id);
    id = 0;
}

bool RtmPresence::connected() const {
    return _ws && _ws->isOpen();
}

void RtmPresence::setMode(Mode mode) {
    if (mode == _mode)
        return;
    _mode        = mode;
    // A deliberate change is a fresh start: forget a refusal (the session may
    // have been re-imported) and any inherited backoff.
    _unavailable = false;
    _reconnectMs = _t.reconnectMinMs;
    if (!holding()) {
        teardown();
        stopTimer(_idleTimer);
        setState(Link::Off);
        return;
    }
    stopTimer(_idleTimer);
    if (_mode == Mode::WhileUsing) // the click that chose it counts as activity
        _idleTimer = _app.addTimer(_t.idleMs, false, [this] {
            _idleTimer = 0;
            onIdle();
        });
    // Idle → WhileRunning brings the link straight back.
    if (_state == Link::Idle || _state == Link::Off)
        setState(Link::Connecting);
    if (connected()) {
        // Already up: only the tickle cadence differs between the modes.
        stopTimer(_tickleTimer);
        if (_mode == Mode::WhileRunning)
            _tickleTimer = _app.addTimer(_t.alwaysTickleMs, true, [this] { sendTickle(true); });
        return;
    }
    ensureHolding();
}

void RtmPresence::noteActivity() {
    if (!holding())
        return;
    if (_mode == Mode::WhileUsing) {
        stopTimer(_idleTimer);
        _idleTimer = _app.addTimer(_t.idleMs, false, [this] {
            _idleTimer = 0;
            onIdle();
        });
        if (_state == Link::Idle) {
            _reconnectMs = _t.reconnectMinMs; // the user is back: no inherited backoff
            setState(Link::Connecting);
            ensureHolding();
            return;
        }
    }
    sendTickle(false);
}

void RtmPresence::setPresenceSubs(std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids == _subs)
        return;
    _subs = std::move(ids);
    sendPresenceSub();
}

void RtmPresence::sendPresenceSub() {
    // Before hello Slack drops it; hello sends it then.
    if (_state != Link::Active || !connected() || _subs.empty())
        return;
    std::string frame = "{\"type\":\"presence_sub\",\"ids\":[";
    for (size_t i = 0; i < _subs.size(); ++i) {
        if (i)
            frame += ',';
        json::escapeString(frame, _subs[i]);
    }
    frame += "]}";
    _ws->sendText(frame);
}

void RtmPresence::ensureHolding() {
    if (!holding() || _unavailable || _connecting || _reconnectTimer)
        return;
    if (_state == Link::Idle || connected())
        return; // dropped on purpose (only noteActivity brings it back), or up
    openAndConnect();
}

void RtmPresence::openAndConnect() {
    if (_connecting)
        return;
    _connecting = true;
    setState(Link::Connecting);
    // presence_sub keeps Slack from streaming the whole roster's
    // presence_change (only the ids of setPresenceSubs); batch_presence_aware
    // is its prerequisite.
    const int gen = _generation;
    _connect(
        "batch_presence_aware=1&presence_sub=true",
        [this, gen, alive = _alive](const json::Document &doc, const std::string &err) {
            if (!*alive || gen != _generation)
                return; // torn down meanwhile: never a competing socket
            if (err.empty()) {
                const std::string_view url = doc.root()["url"].str();
                if (!url.empty()) {
                    connectWs(std::string(url));
                    return;
                }
            }
            _connecting = false;
            if (fatalConnectError(err)) {
                LOG_WARN(
                    "slack",
                    "RtmPresence: rtm.connect refused (%s) — cannot keep this workspace active",
                    err.c_str()
                );
                _unavailable = true;
                setState(Link::Unavailable);
                settleWaiters(false);
                return;
            }
            LOG_INFO("slack", "RtmPresence: rtm.connect failed — %s", err.c_str());
            scheduleReconnect();
        }
    );
}

void RtmPresence::connectWs(const std::string &url) {
    // Never two sockets: each one counts as a client.
    retireSocket(_app, _ws);
    _ws           = std::make_unique<net::WebSocket>(_app);
    _ws->onOpen   = [this] { onOpen(); };
    _ws->onText   = [this](std::string text) { onText(text); };
    _ws->onClosed = [this](int code, std::string reason) { onClosed(code, reason); };
    // The URL carries no auth: the `d` cookie on the handshake IS the auth.
    _ws->open(url, {{"Cookie", "d=" + _cookie}});
}

void RtmPresence::teardown() {
    ++_generation;
    _connecting     = false;
    _connectedSince = 0;
    _awaitingPongs  = 0;
    stopTimer(_reconnectTimer);
    stopTimer(_pingTimer);
    stopTimer(_tickleTimer);
    retireSocket(_app, _ws); // not inside its own callback
}

void RtmPresence::scheduleReconnect() {
    if (!holding() || _unavailable || _reconnectTimer)
        return; // one pending reconnect at a time
    setState(Link::Connecting);
    const int delay = std::max(_reconnectMs, _t.reconnectMinMs);
    _reconnectMs    = std::min(delay * 2, _t.reconnectMaxMs);
    _reconnectTimer = _app.addTimer(delay, false, [this] {
        _reconnectTimer = 0;
        ensureHolding();
    });
}

void RtmPresence::onOpen() {
    _connecting     = false;
    _connectedSince = base::monotonicMs();
    _awaitingPongs  = 0;
    stopTimer(_pingTimer);
    _pingTimer = _app.addTimer(_t.pingMs, true, [this] { sendPing(); });
    // Active comes with `hello`: an unauthenticated handshake opens too.
}

void RtmPresence::onClosed(int code, const std::string &reason) {
    const int64_t now = base::monotonicMs();
    LOG_INFO(
        "slack",
        "RtmPresence: socket closed — code %d %s (up %lld ms)",
        code,
        reason.c_str(),
        (long long)(_connectedSince ? now - _connectedSince : 0)
    );
    _connecting = false;
    stopTimer(_pingTimer);
    stopTimer(_tickleTimer);
    retireSocket(_app, _ws);
    // Only a durable connection resets the backoff (rtm.connect is Tier 1).
    if (_connectedSince && now - _connectedSince >= _t.stableMs)
        _reconnectMs = _t.reconnectMinMs;
    _connectedSince = 0;
    _awaitingPongs  = 0;
    if (holding() && _state != Link::Idle)
        scheduleReconnect();
}

void RtmPresence::onText(const std::string &text) {
    // The socket carries every event of the workspace; only hello, pong,
    // error, presence_change, view_opened and workflow_execution_outcome_changed
    // are ours, and a frame without one of the words is none of them (no
    // parse for the rest).
    const std::string_view t(text);
    if (t.find("hello") == t.npos && t.find("pong") == t.npos && t.find("error") == t.npos &&
        t.find("presence_change") == t.npos && t.find("\"view_opened\"") == t.npos &&
        t.find("\"workflow_execution_outcome_changed\"") == t.npos)
        return;
    json::Document doc;
    if (!doc.parse(std::string_view(text), nullptr))
        return;
    const std::string_view type = doc.root()["type"].str();
    if (type == "hello") {
        _reconnectMs = _t.reconnectMinMs;
        setState(Link::Active);
        if (_mode == Mode::WhileRunning) {
            stopTimer(_tickleTimer);
            _tickleTimer = _app.addTimer(_t.alwaysTickleMs, true, [this] { sendTickle(true); });
        }
        if (_mode != Mode::Native)
            sendTickle(true); // active at once, not at the first input
        sendPresenceSub();    // a new socket subscribes to nobody
        settleWaiters(true);
        return;
    }
    if (type == "view_opened" || type == "workflow_execution_outcome_changed") {
        if (auto fn = onWorkflow) // a handler may replace it
            fn(doc.root());
        return;
    }
    if (type == "presence_change") {
        onPresenceChange(doc.root());
        return;
    }
    if (type == "pong") {
        _awaitingPongs = 0;
        return;
    }
    if (type == "error") {
        // e.g. invalid_auth without the cookie: Slack drops the socket soon
        // anyway; don't wait for it.
        LOG_WARN(
            "slack",
            "RtmPresence: server error frame — %.*s",
            int(std::min<size_t>(text.size(), 200)),
            text.data()
        );
        teardown();
        scheduleReconnect();
    }
    // Everything else is the event stream: not ours.
}

void RtmPresence::sendPing() {
    if (!connected())
        return;
    if (_awaitingPongs >= kMaxMissedPongs) {
        LOG_WARN("slack", "RtmPresence: no pong for %d pings — reconnecting", _awaitingPongs);
        teardown();
        scheduleReconnect();
        return;
    }
    ++_awaitingPongs;
    _ws->sendText(str::concat({"{\"type\":\"ping\",\"id\":", str::number(++_pingId), "}"}));
}

void RtmPresence::sendTickle(bool force) {
    if (_state != Link::Active || !connected() || _mode == Mode::Native)
        return; // Native: up only for a hold, never "active" on purpose
    const int64_t now = base::monotonicMs();
    if (!force && _lastTickle && now - _lastTickle < _t.tickleGapMs)
        return;
    _lastTickle = now;
    _ws->sendText("{\"type\":\"tickle\"}");
}

// {"type":"presence_change","user":"U1","presence":"active"}, or batched
// (batch_presence_aware) with "users":["U1","U2"].
void RtmPresence::onPresenceChange(const json::Value &frame) {
    const std::string_view presence = frame["presence"].str();
    if (presence.empty() || !onPresence)
        return;
    const bool               active = presence == "active";
    std::vector<std::string> ids;
    if (const std::string_view one = frame["user"].str(); !one.empty())
        ids.emplace_back(one);
    for (const json::Value u : frame["users"])
        if (!u.str().empty())
            ids.emplace_back(u.str());
    auto fn = onPresence; // a handler may replace it
    for (const std::string &id : ids)
        fn(id, active);
}

void RtmPresence::onIdle() {
    if (_mode != Mode::WhileUsing || _holds > 0)
        return; // held: release() drops it once the hold ends
    LOG_INFO("slack", "RtmPresence: no input for %d s — dropping the link", _t.idleMs / 1000);
    teardown();
    setState(Link::Idle);
}

void RtmPresence::setState(Link s) {
    if (s == _state)
        return;
    _state = s;
    if (onStateChanged) {
        auto fn = onStateChanged;
        fn(s);
    }
}

} // namespace slack

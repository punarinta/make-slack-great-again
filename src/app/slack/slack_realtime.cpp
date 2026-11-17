// SlackBackend's realtime half (see slack_backend.h): Socket Mode events
// into the Store, what a reconnect backfills, the presence link and the
// OAuth token refresh.
//
// Typing (user_typing is RTM-only, dead over Socket Mode) is not here.
#include "app/slack/rtm_presence.h"
#include "app/model/timers.h"
#include "app/slack/slack_backend.h"
#include "app/slack/slack_json.h"
#include "app/slack/socket_mode.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "net/net.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace slack {

using model::ConvRef;
using model::kNoConv;
using model::kNoUser;
using model::Ts;
using model::UserRef;

namespace {

// The reconnect throttles and the token refresh window.
constexpr int64_t kReconnectReloadGapMs  = 2 * 60'000;
constexpr int64_t kUnreadResyncGapMs     = 2 * 60'000;
// Every DM and group DM at most this often; a reconnect in between sweeps
// only the kUnreadResyncTop most recently active (a flapping socket would
// otherwise keep the paced lane sweeping without end).
constexpr int64_t kUnreadFullResyncGapMs = 10 * 60'000;
constexpr size_t  kUnreadResyncTop       = 24;
constexpr int64_t kReestablishGapMs      = 60'000;
constexpr int64_t kContentionNoticeGap   = 5 * 60'000;
constexpr int     kRefreshCheckMs        = 60'000;
constexpr int64_t kRefreshAheadSecs      = 3600;
constexpr int     kPresenceRefreshMs     = 2'000; // Slack registers the socket a beat after hello
constexpr int     kUsergroupsDebounceMs  = 2'000;
constexpr int     kUnreadPatchMs         = 300;

// oauth.v2.access answers worth trying again later.
bool transientRefreshError(const json::Document &doc, const std::string &e) {
    if (isTransientSlackError(e) || e == "ratelimited")
        return true;
    // No Slack answer at all (offline, a 5xx page): never proof of dead keys.
    return !doc.root().has("ok");
}

} // namespace

struct SlackBackend::Live {
    explicit Live(SlackBackend &b);
    ~Live();

    SlackBackend        &b;
    model::Store        &s;
    int64_t              speed = 1; // MSGA_SLACK_TEST_SPEEDUP, as the read half
    model::OneShotTimers timers{b._app};

    int64_t now() const { return base::monotonicMs() * speed; }
    void    later(int64_t ms, std::function<void()> fn);
    // A throttle stamp: -1 = never.
    bool    due(int64_t &last, int64_t gap) {
        const int64_t t = now();
        if (last >= 0 && t - last < gap)
            return false;
        last = t;
        return true;
    }

    // ── Socket Mode ─────────────────────────────────────────────────────────
    std::shared_ptr<SocketMode> socket;
    uint32_t                    sink       = 0;
    int64_t                     lastReload = -1, lastResync = -1, lastReestablish = -1;
    int64_t                     lastFullResync = -1;
    int64_t                     lastContention = -1;
    plat::TimerId               groupsTimer    = 0;
    struct Pending {
        model::Message m;
        bool           parentIsMe;
    };
    std::unordered_map<std::string, std::vector<Pending>> unknown; // conversations.info in flight

    void attach(std::shared_ptr<SocketMode> sock);
    void onPayload(const json::Value &payload);
    void apply(const json::Value &ev, bool ours);
    void onMessage(const json::Value &ev, bool ours);
    bool patchUsergroup(std::string_view type, const json::Value &ev);
    void fetchUnknown(const std::string &id, model::Message m, bool parentIsMe);
    void fetchJoined(const std::string &id);
    // conversations.info (with the member count the header shows), mapped:
    // `then` gets the conversation, or one with an empty id and the error.
    void fetchInfo(
        const std::string                                                    &id,
        std::function<void(model::Conversation info, const std::string &err)> then
    );
    void onReconnected();
    void onContended();

    // ── resyncUnreads ───────────────────────────────────────────────────────
    int                                                      resyncInFlight = 0;
    std::vector<std::pair<std::string, model::Conversation>> patches;
    plat::TimerId                                            patchTimer = 0;
    void                                                     resyncUnreads();
    void                                                     applyPatches();

    // ── The presence link ───────────────────────────────────────────────────
    std::unique_ptr<RtmPresence> rtm;
    plat::TimerId                selfTimer = 0;

    // ── Token refresh ───────────────────────────────────────────────────────
    AppConfig                              cfg;
    bool                                   refreshing = false;
    std::vector<std::function<void(bool)>> waiters;
    net::RequestId                         refreshReq   = 0;
    plat::TimerId                          refreshTimer = 0;
    void                                   refresh(std::function<void(bool)> then);
    void                                   finishRefresh(int result, const std::string &err);
    void                                   maybeProactiveRefresh();
};

SlackBackend::Live::Live(SlackBackend &b) : b(b), s(b.store()) {
    speed = testSpeedup();
    // The presence link exists only for a session token (rtm.connect refuses
    // granular OAuth ones); idle until a mode is set.
    if (b._creds.sessionAuth()) {
        rtm = std::make_unique<RtmPresence>(
            b._app,
            [this](std::string form, ApiDone done) {
                this->b.api("rtm.connect", std::move(form), std::move(done));
            },
            b._creds.cookie
        );
        rtm->onStateChanged = [this](RtmPresence::Link) {
            // The footer's tooltip names the link's state (no user changed).
            s.usersChanged(kNoUser);
            // Slack flips `online` as the socket comes and goes, but the
            // self snapshot is polled once a minute: look again shortly.
            if (selfTimer)
                this->b._app.cancelTimer(selfTimer);
            selfTimer = this->b._app.addTimer(int(kPresenceRefreshMs / speed), false, [this] {
                selfTimer = 0;
                this->b.refreshSelfPresence(nullptr);
            });
        };
    }
    // Proactive refresh: a periodic wall-clock check (a timer that long would
    // sleep through a suspend), retrying transient failures by itself. The
    // first check comes once the backend is fully built.
    if (!b._creds.refreshToken.empty()) {
        refreshTimer = b._app.addTimer(int(kRefreshCheckMs / speed), true, [this] {
            maybeProactiveRefresh();
        });
        later(0, [this] { maybeProactiveRefresh(); });
    }
}

SlackBackend::Live::~Live() {
    rtm.reset(); // before anything it calls into
    if (socket && sink)
        socket->removeSink(sink);
    timers.cancelAll(); // before the rest goes: they capture this
    for (plat::TimerId id : {groupsTimer, patchTimer, selfTimer, refreshTimer})
        if (id)
            b._app.cancelTimer(id);
    if (refreshReq)
        b._client.cancel(refreshReq);
}

void SlackBackend::Live::later(int64_t ms, std::function<void()> fn) {
    // Clamped to what the timer takes: a due time weeks away (a scheduled
    // message) would otherwise wrap and fire at once. It re-arms then.
    timers.after(int(std::clamp<int64_t>(ms / speed, 0, INT32_MAX)), std::move(fn));
}

// ── Socket Mode ─────────────────────────────────────────────────────────────

void SlackBackend::setRealtime(std::shared_ptr<SocketMode> socket) {
    _live->attach(std::move(socket));
}

bool SlackBackend::hasRealtimePush() const {
    return _live->socket != nullptr;
}

void SlackBackend::Live::attach(std::shared_ptr<SocketMode> sock) {
    if (socket && sink)
        socket->removeSink(sink);
    socket.reset();
    sink = 0;
    // Session mode is a hard off switch for Socket Mode: a session workspace never takes the app's
    // socket.
    if (!sock || b._creds.sessionAuth())
        return;
    socket = std::move(sock);
    SocketMode::Sink k;
    k.event       = [this](const json::Value &payload) { onPayload(payload); };
    k.reconnected = [this] { onReconnected(); };
    k.contended   = [this](int) { onContended(); };
    sink          = socket->addSink(std::move(k));
    socket->start();
}

void SlackBackend::realtimeTick() {
    // (1) Subscription intact: reconnect a dropped socket (no-op if healthy).
    if (_live->socket)
        _live->socket->ensureConnected();
}

void SlackBackend::realtimeMissed() {
    Live &l = *_live;
    // Throttled: a persistently sick socket would otherwise reconnect on
    // every poll, each one a conversations.list reload (the 429 storm).
    if (!l.socket || !l.due(l.lastReestablish, kReestablishGapMs))
        return;
    LOG_WARN(
        "slack", "%s: realtime missed messages — re-establishing socket", _creds.teamId.c_str()
    );
    l.socket->reconnectNow();
}

// The socket is the app's, shared by every workspace: an event is ours when
// its payload names this workspace, or (Slack Connect, Grid) it is about a
// conversation or user we know. What we don't know is someone else's.
void SlackBackend::Live::onPayload(const json::Value &payload) {
    if (b._authLost)
        return;
    const std::string &team = s.workspaceId.empty() ? b._creds.teamId : s.workspaceId;
    bool               ours = !team.empty() && payload["team_id"].str() == team;
    for (const json::Value a : payload["authorizations"])
        ours = ours || (!team.empty() && a["team_id"].str() == team);
    apply(payload["event"], ours);
}

void SlackBackend::Live::apply(const json::Value &ev, bool ours) {
    const std::string_view type = ev["type"].str();
    if (type == "message") {
        onMessage(ev, ours);
        return;
    }
    if (type == "reaction_added" || type == "reaction_removed") {
        const json::Value item = ev["item"];
        const ConvRef     c    = s.findConversation(item["channel"].str());
        if (c == kNoConv)
            return;
        // My own echo is a no-op: the optimistic toggle counted it already.
        s.setReaction(
            c,
            model::parseTs(item["ts"].str()),
            ev["reaction"].str(),
            s.internUser(ev["user"].str()),
            type == "reaction_added"
        );
        return;
    }
    if (type == "channel_marked" || type == "group_marked" || type == "im_marked" ||
        type == "mpim_marked") {
        const ConvRef c = s.findConversation(ev["channel"].str());
        if (c == kNoConv)
            return;
        const Ts       ts       = model::parseTs(ev["ts"].str());
        const uint32_t unread   = uint32_t(ev["unread_count_display"].integer());
        const uint32_t mentions = uint32_t(ev["mention_count_display"].integer());
        s.updateConversation(c, [&](model::Conversation &x) {
            x.lastRead = ts;
            x.unread   = unread;
            x.mentions = mentions;
        });
        return;
    }
    if (type == "presence_change" || type == "dnd_updated_user") {
        const bool presence = type == "presence_change";
        const bool on =
            presence ? ev["presence"].str() == "active" : ev["dnd_status"]["dnd_enabled"].boolean();
        int     changed = 0;
        UserRef last    = kNoUser;
        auto    patch   = [&](std::string_view id) {
            const UserRef u = s.findUser(id);
            if (u == kNoUser)
                return;
            bool &field = presence ? s.user(u).active : s.user(u).dnd;
            if (field != on) {
                ++changed;
                last = u;
            }
            field = on;
        };
        patch(ev["user"].str());
        for (const json::Value u : ev["users"]) // batched presence
            patch(u.str());
        if (changed == 1)
            s.usersChanged(last);
        else if (changed)
            s.usersChanged();
        return;
    }
    if (type == "channel_created") {
        if (!ours)
            return;
        // Upsert: a listed one takes the fresh copy (keeping local state), a
        // new one joins the list when we are in it.
        model::Conversation fresh = mapjson::toConversation(ev["channel"], s);
        if (fresh.id.empty())
            return;
        if (s.findConversation(fresh.id) != kNoConv || fresh.member) {
            b.markAlive(fresh.id);
            b.mergeConversation(std::move(fresh));
        }
        return;
    }
    if (type == "member_joined_channel") {
        // Every member's join fires; only ours matters (we were added): pull
        // the channel in when it isn't a member conversation yet.
        const std::string_view id = ev["channel"].str();
        const ConvRef          c  = s.findConversation(id);
        if (s.me == kNoUser || ev["user"].str() != s.user(s.me).id || (c == kNoConv && !ours))
            return;
        if (c == kNoConv || !s.conversation(c).member)
            fetchJoined(std::string(id));
        return;
    }
    if (type == "user_change") {
        // The full user object (users.list's shape); presence and DND are
        // not in it and stay as they are.
        model::User u = mapjson::toUser(ev["user"]);
        if (u.id.empty())
            return;
        if (!ours && s.findUser(u.id) == kNoUser)
            return;
        s.usersChanged(b.mergeUser(std::move(u)));
        return;
    }
    if (type == "subteam_created" || type == "subteam_updated" ||
        type == "subteam_members_changed" || type == "subteam_self_added" ||
        type == "subteam_self_removed") {
        if (!ours || patchUsergroup(type, ev))
            return;
        // Not enough said to patch: all of them again, debounced (a bulk
        // membership change is one event per edit).
        if (groupsTimer)
            b._app.cancelTimer(groupsTimer);
        groupsTimer = b._app.addTimer(int(kUsergroupsDebounceMs / speed), false, [this] {
            groupsTimer = 0;
            b.reloadUsergroups();
        });
    }
}

// A user group event into the Store: only whether I am in a group is kept
// (and its handle and name), and the events say that much. False when one
// doesn't (a group we don't hold, a payload without the fields): the caller
// reads them all again.
bool SlackBackend::Live::patchUsergroup(std::string_view type, const json::Value &ev) {
    const std::string     &me = s.user(s.me).id;
    const json::Value      g  = ev["subteam"];
    const std::string_view id = g.has("id") ? g["id"].str() : ev["subteam_id"].str();
    if (me.empty() || id.empty())
        return false;
    std::vector<model::Store::Usergroup> groups = s.usergroups();
    auto                                 it     = groups.begin();
    while (it != groups.end() && it->id != id)
        ++it;
    const auto has = [&me](const json::Value &ids) {
        for (const json::Value u : ids)
            if (u.str() == me)
                return true;
        return false;
    };
    if (type == "subteam_created" || type == "subteam_updated") {
        if (g["handle"].str().empty())
            return false;
        if (g["date_delete"].integer() != 0) { // disabled: usergroups.list leaves it out
            if (it != groups.end())
                groups.erase(it);
        } else {
            if (it == groups.end()) {
                groups.emplace_back();
                it     = groups.end() - 1;
                it->id = std::string(id);
            }
            it->handle = std::string(g["handle"].str());
            it->name   = std::string(g["name"].str());
            if (g.has("users"))
                it->mine = has(g["users"]);
        }
    } else if (it == groups.end()) {
        return false;
    } else if (type == "subteam_members_changed") {
        if (!ev.has("added_users") && !ev.has("removed_users"))
            return false;
        if (has(ev["added_users"]))
            it->mine = true;
        else if (has(ev["removed_users"]))
            it->mine = false;
    } else {
        it->mine = type == "subteam_self_added";
    }
    if (groups != s.usergroups())
        s.setUsergroups(std::move(groups));
    return true;
}

void SlackBackend::Live::onMessage(const json::Value &ev, bool ours) {
    const std::string_view sub = ev["subtype"].str();
    const std::string_view id  = ev["channel"].str();
    const ConvRef          c   = s.findConversation(id);
    if (sub == "message_deleted") {
        if (c == kNoConv)
            return;
        const Ts ts = model::parseTs(ev["deleted_ts"].str());
        if (s.removeMessage(c, ts))
            return; // a removed reply decrements its root itself
        // A reply we never loaded only counted on its root.
        const json::Value prev = ev["previous_message"];
        const Ts          root = model::parseTs(prev["thread_ts"].str());
        if (root && root != ts)
            s.updateMessage(c, root, [](model::Message &r) {
                if (r.replyCount)
                    --r.replyCount;
            });
        return;
    }
    // A huddle_thread message starts a huddle, its edit
    // (the room gaining date_end / has_ended) ends or changes it. The message
    // itself goes on as any other.
    if (c != kNoConv) {
        const json::Value   inner = sub == "message_changed" ? ev["message"] : ev;
        mapjson::HuddleRoom h;
        if (inner["subtype"].str() == "huddle_thread" && mapjson::toHuddleRoom(inner["room"], s, h))
            b.setHuddle(c, h.active, std::move(h.link), std::move(h.participants));
    }
    if (sub == "message_changed" || sub == "message_replied") {
        // Only a message we hold changes: anything else isn't worth mapping.
        if (c == kNoConv || !s.findMessage(c, model::parseTs(ev["message"]["ts"].str())))
            return;
        // The new shape of a message we may hold: replace what the server
        // says, keep where it sits and what only we know.
        model::Message m = mapjson::toMessage(sub == "message_replied" ? ev["message"] : ev, s);
        if (!m.ts)
            return;
        s.updateMessage(c, m.ts, [&m](model::Message &x) {
            const Ts   thread = x.threadTs;
            const bool saved = x.saved, pending = x.pending;
            x          = std::move(m);
            x.threadTs = thread;
            x.saved    = saved;
            x.pending  = pending;
        });
        return;
    }
    // Another workspace's (the socket is shared): not mapped, so its users
    // don't land in this Store as placeholders.
    if (c == kNoConv && (!ours || id.empty()))
        return;
    model::Message m = mapjson::toMessage(ev, s);
    if (!m.ts)
        return;
    const bool parentIsMe = s.me != kNoUser && ev["parent_user_id"].str() == s.user(s.me).id;
    if (c == kNoConv) {
        // A conversation we don't list yet — most often a new group DM, for
        // which Slack sends nothing but this message. Ours only: another
        // workspace's channel would be channel_not_found here.
        if (ours && !id.empty())
            fetchUnknown(std::string(id), std::move(m), parentIsMe);
        return;
    }
    // The same message can come twice (the send's own answer, a redelivered
    // envelope): deliver() drops what is there already.
    b.deliver(c, std::move(m), parentIsMe);
}

void SlackBackend::Live::fetchUnknown(const std::string &id, model::Message m, bool parentIsMe) {
    if (b.isDead(id))
        return; // known not to be ours: no conversations.info per message
    auto      &queue = unknown[id];
    const bool asked = !queue.empty();
    queue.push_back({std::move(m), parentIsMe});
    if (asked)
        return; // the burst rides the fetch already in flight
    fetchInfo(id, [this, id](model::Conversation info, const std::string &err) {
        std::vector<Pending> backlog = std::move(unknown[id]);
        unknown.erase(id);
        if (err == "channel_not_found") {
            b.markDead(id);
            return;
        }
        if (info.id.empty())
            return; // a later message tries again
        // The replay below counts them (as every other message is); the
        // server's numbers already include them.
        info.unread = info.mentions = 0;
        info.member                 = true;
        const ConvRef c             = b.mergeConversation(std::move(info));
        for (Pending &p : backlog)
            b.deliver(c, std::move(p.m), p.parentIsMe);
    });
}

void SlackBackend::Live::fetchJoined(const std::string &id) {
    b.markAlive(id);
    fetchInfo(id, [this](model::Conversation info, const std::string &) {
        if (info.id.empty())
            return;
        const ConvRef c = s.findConversation(info.id);
        if (c != kNoConv && s.conversation(c).member)
            return; // a concurrent fetch added it already
        info.member = true;
        b.mergeConversation(std::move(info));
    });
}

void SlackBackend::Live::fetchInfo(
    const std::string                                                    &id,
    std::function<void(model::Conversation info, const std::string &err)> then
) {
    // The count is opt-in on conversations.info.
    b.readCall(
        "conversations.info",
        net::formEncode({{"channel", id}, {"include_num_members", "true"}}),
        [this, then = std::move(then)](const json::Document &doc, const std::string &err) {
            model::Conversation info;
            if (err.empty())
                info = mapjson::toConversation(doc.root()["channel"], s);
            then(std::move(info), err);
        },
        false
    );
}

void SlackBackend::Live::onReconnected() {
    if (b._authLost)
        return;
    // The socket came back after a gap Slack won't replay. The roster
    // catches the cursors up (coalesced: conversations.list is rate-limited
    // and a flapping socket fires this often), the open chat backfills its
    // head page, and resyncUnreads recovers the DM badges conversations.list
    // can't (it reports no unread counts).
    if (due(lastReload, kReconnectReloadGapMs))
        b.reloadConversations();
    b.backfillOpen();
    resyncUnreads();
}

void SlackBackend::Live::onContended() {
    // Another device on the same app keys keeps evicting the socket: a
    // persistent notice (the shell's parallel-usage banner), throttled.
    if (!due(lastContention, kContentionNoticeGap))
        return;
    if (b.onParallelUsage) {
        auto fn = b.onParallelUsage;
        fn();
    }
}

// ── resyncUnreads ───────────────────────────────────────────────────────────

// Unread resync: a stalled socket drops the messages of many
// conversations at once, and only conversations.info reports my real unread
// count. DMs and group DMs only (a channel's missed mention needs a history
// scan), 1:1 first; upward merges only, so it composes with the roster
// reload in any order. On the paced lane; one sweep at a time, at most one
// per 2 minutes.
void SlackBackend::Live::resyncUnreads() {
    if (resyncInFlight > 0 || (lastResync >= 0 && now() - lastResync < kUnreadResyncGapMs))
        return;
    std::vector<std::string> ids = b.directConversationIds();
    if (ids.empty())
        return;
    lastResync = now();
    if (lastFullResync >= 0 && now() - lastFullResync < kUnreadFullResyncGapMs &&
        ids.size() > kUnreadResyncTop) {
        // The most recently active (a selection: K passes, no sort).
        std::vector<Ts> latest(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
            latest[i] = s.conversation(s.findConversation(ids[i])).latest;
        for (size_t i = 0; i < kUnreadResyncTop; ++i) {
            size_t best = i;
            for (size_t j = i + 1; j < ids.size(); ++j)
                if (latest[j] > latest[best])
                    best = j;
            std::swap(ids[i], ids[best]);
            std::swap(latest[i], latest[best]);
        }
        ids.resize(kUnreadResyncTop);
    } else {
        lastFullResync = now();
    }
    resyncInFlight = int(ids.size());
    LOG_INFO("slack", "resyncUnreads: recovering unread for %zu DMs/MPDMs", ids.size());
    for (const std::string &id : ids)
        b.sweepInfo(id, [this, id](model::Conversation *info) {
            if (resyncInFlight > 0)
                --resyncInFlight;
            if (!info)
                return;
            // Batched: one Store update per burst of answers.
            patches.emplace_back(id, std::move(*info));
            if (!patchTimer)
                patchTimer = b._app.addTimer(kUnreadPatchMs, false, [this] {
                    patchTimer = 0;
                    applyPatches();
                });
        });
}

void SlackBackend::Live::applyPatches() {
    for (const auto &[id, info] : patches) {
        const ConvRef r = s.findConversation(id);
        if (r == kNoConv)
            continue;
        const model::Conversation &c = s.conversation(r);
        // For a DM every unread is a red-badge mention.
        if (info.lastRead <= c.lastRead && info.latest <= c.latest && info.unread <= c.unread &&
            info.unread <= c.mentions)
            continue;
        s.updateConversation(r, [&info](model::Conversation &x) {
            x.lastRead = std::max(x.lastRead, info.lastRead);
            x.latest   = std::max(x.latest, info.latest);
            x.unread   = std::max(x.unread, info.unread);
            x.mentions = std::max(x.mentions, info.unread);
        });
    }
    patches.clear();
}

// ── The presence link ───────────────────────────────────────────────────────

void SlackBackend::setPresenceMode(PresenceMode mode) {
    if (_live->rtm)
        _live->rtm->setMode(mode);
}

model::Backend::PresenceLink SlackBackend::presenceLink() const {
    return _live->rtm ? _live->rtm->state() : PresenceLink::Off;
}

RtmPresence *SlackBackend::presenceLinkForTest() const {
    return _live->rtm.get();
}

void SlackBackend::noteUserActivity() {
    if (_live->rtm)
        _live->rtm->noteActivity();
}

// ── Token refresh ───────────────────────────────────────────────────────────

void SlackBackend::setAppConfig(AppConfig cfg) {
    _live->cfg = std::move(cfg);
}

bool SlackBackend::canRefresh() const {
    return !_creds.refreshToken.empty();
}

bool SlackBackend::refreshInFlight() const {
    return _live->refreshing;
}

void SlackBackend::refreshToken(std::function<void(bool ok)> then) {
    _live->refresh(std::move(then));
}

void SlackBackend::Live::maybeProactiveRefresh() {
    if (b._authLost || b._creds.refreshToken.empty() || b._creds.expiresAt == 0 || refreshing)
        return;
    const int64_t left = b._creds.expiresAt - base::nowSecs();
    if (left > kRefreshAheadSecs)
        return;
    LOG_INFO("slack", "token expires in %llds, refreshing now", (long long)left);
    refresh(nullptr);
}

// oauth.v2.access with grant_type=refresh_token (the rotation refresh; the
// refresh token itself is single-use and comes back renewed).
void SlackBackend::Live::refresh(std::function<void(bool)> then) {
    if (then)
        waiters.push_back(std::move(then));
    if (refreshing)
        return; // the one in flight answers everyone
    refreshing = true;
    if (b._creds.refreshToken.empty()) {
        finishRefresh(2, "no_refresh_token");
        return;
    }
    LOG_INFO("slack", "refreshing the token for %s", b._creds.teamId.c_str());
    refreshReq = apiCall(
        b._client,
        Auth{}, // no token: the client secret is the credential
        "oauth.v2.access",
        net::formEncode(
            {{"grant_type", "refresh_token"},
             {"client_id", cfg.clientId},
             {"client_secret", cfg.clientSecret},
             {"refresh_token", b._creds.refreshToken}}
        ),
        [this](const json::Document &doc, const std::string &err) {
            refreshReq = 0;
            if (!err.empty()) {
                LOG_WARN("slack", "token refresh failed: %s", err.c_str());
                finishRefresh(transientRefreshError(doc, err) ? 1 : 2, err);
                return;
            }
            const json::Value      o     = doc.root();
            const std::string_view token = o["access_token"].str();
            if (token.empty()) {
                LOG_WARN("slack", "token refresh: no access_token in a successful answer");
                finishRefresh(1, "no_access_token");
                return;
            }
            b._auth.token = b._creds.token = std::string(token);
            if (!o["refresh_token"].str().empty())
                b._creds.refreshToken = std::string(o["refresh_token"].str());
            if (const int64_t in = o["expires_in"].integer(); in > 0)
                b._creds.expiresAt = base::nowSecs() + in;
            LOG_INFO(
                "slack",
                "token refreshed for %s, next expiry in %llds",
                b._creds.teamId.c_str(),
                (long long)o["expires_in"].integer()
            );
            if (b.onCredentialsChanged) {
                auto fn = b.onCredentialsChanged;
                fn(b._creds);
            }
            finishRefresh(0, {});
        }
    );
}

// result: 0 refreshed, 1 transient (stay signed in; the 60 s check retries),
// 2 the keys are dead (sign in again).
void SlackBackend::Live::finishRefresh(int result, const std::string &err) {
    refreshing = false;
    // Dead keys are reported first, so the calls failing below say so.
    if (result == 2) {
        if (refreshTimer)
            b._app.cancelTimer(refreshTimer);
        refreshTimer = 0;
        b.loseAuth(err.empty() ? std::string("invalid_auth") : err);
    }
    std::vector<std::function<void(bool)>> w = std::move(waiters);
    waiters.clear();
    for (auto &fn : w)
        fn(result == 0);
}

SlackBackend::Live *SlackBackend::newLive(SlackBackend &b) {
    return new Live(b);
}

void SlackBackend::deleteLive(Live *l) {
    delete l;
}

} // namespace slack

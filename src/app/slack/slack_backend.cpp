// SlackBackend's read side (see slack_backend.h): connect (identity, roster,
// users, emoji, user groups, stars, saved items), history and threads, and
// the polling that stands in for realtime.
//
// A workspace with Socket Mode push (app keys: slack_realtime.cpp) polls
// only as a safety net (hasRealtimePush):
// no roster/counts/threads polls, and a poll that finds a message the
// socket should have pushed re-establishes it. The open-chat cadence is
// 5 s / 60 s (session / app keys — conversations.history is 1/min for unlisted apps).
#include "app/slack/slack_backend.h"

#include "app/cache/workspace_cache.h"
#include "app/model/timers.h"
#include "app/slack/slack_json.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "net/net.h"
#include "plat/plat.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace slack {

using model::ConvRef;
using model::kNoConv;
using model::kNoUser;
using model::Ts;
using model::UserRef;

namespace {

// The realtime health check's cadences. The roster reloads every minute
// only where it is the activity source (no client.counts: OAuth, or counts
// given up on); with counts it reloads when a snapshot names a conversation
// the roster lacks (a new DM or channel) or drops a member one (left or
// closed elsewhere), at most every kRosterSoonestGapMs, and otherwise every
// kRosterFallbackGapMs for renames and topics.
constexpr int64_t     kRosterReloadGapMs      = 60'000;
constexpr int64_t     kRosterSoonestGapMs     = 10'000;
constexpr int64_t     kRosterFallbackGapMs    = 15 * 60'000;
constexpr int64_t     kDirectoryGapMs         = 10 * 60'000; // the channel browser's list
constexpr int64_t     kCountsPollGapMs        = 10'000;
constexpr int64_t     kThreadsPollGapMs       = 20'000;
constexpr int64_t     kBackgroundPollGapMs    = 2 * 60'000;
constexpr int64_t     kPresencePollGapMs      = 60'000;
constexpr int64_t     kAwayProbeGapMs         = 5 * 60'000; // a posting "away" author, per user
// The open chat of a session workspace while the window is hidden: client.counts
// still brings it forward the moment it moves (applyActivity).
constexpr int64_t     kHiddenOpenPollGapMs    = 60'000;
constexpr int64_t     kSelfPresenceGapMs      = 60'000; // _selfPresenceTimer
constexpr int64_t     kStarredGapMs           = 5 * 60'000;
constexpr int64_t     kSavedGapMs             = 5 * 60'000; // kRemindersRefreshGapMs
constexpr int64_t     kScheduledGapMs         = 5 * 60'000; // ones scheduled elsewhere
constexpr int64_t     kScheduledSentSlackMs   = 15'000;     // re-list this long after one is due
constexpr int64_t     kUsersRefreshGapMs      = 24 * 60 * 60'000;
constexpr int64_t     kLookupRetryTransientMs = 10 * 60'000; // a failed users.info after a 5xx
constexpr int64_t     kDmSweepGapSecs         = 12 * 3600;   // across restarts, via the cache
constexpr int         kOffRosterProbeMs       = 90'000;
constexpr int         kPacedMs                = 1200; // the _infoApi background lane
constexpr int         kPresenceHot = 8, kPresenceRotate = 4;
constexpr int         kMaxDiffPolls = 8, kCountsFailureLimit = 3;
constexpr int         kMaxThreadInjects = 12, kMaxThreadBacklog = 3;
// subscriptions.thread.getTimestamps as the web client asks it; a card of
// that feed shows up to kSubThreadReplies of its replies.
constexpr const char *kSubsForm =
    "mode=all_subscriptions&limit=10&priority_mode=all&fetch_threads_state=false";
constexpr const char *kSubThreadReplies = "10";
// Lost connections and gateway pages: 10 tries back off 0, 1, 2 … 60 s, about
// four minutes in all (bounded, so a long outage surfaces as an error).
constexpr int         kMaxUserReprobes = 20, kMaxTransientRetries = 6, kMaxTransportRetries = 10;
constexpr const char *kHistoryLimit            = "50";
constexpr int64_t     kRateLimitNoticeGapMs    = 15'000;
// A reminder more than a week overdue is marked fired without a
// notification; the alarm sleeps at most 6 h at a time.
constexpr int64_t     kMaxReminderLatenessSecs = 7 * 24 * 3600;
constexpr int64_t     kMaxReminderSleepSecs    = 6 * 3600;
// The thread export's backstop: 400 pages × 50 replies is far beyond any
// real thread; past it the cursor is looping.
constexpr int         kMaxThreadPages          = 400;
// Per-thread state kept for the run (followed threads are also persisted):
// past twice this many, the threads with the oldest roots go.
constexpr size_t      kMaxFollowed = 200, kMaxThreadState = 500;
// Watched threads (agent thread links): the poll interval, busy or quiet,
// and the first polls after a start this far apart. Someone calling the
// agent waits for at most a quiet poll, feed or not (a link lives only as
// long as its session).
constexpr int64_t     kWatchHotMs = 15'000, kWatchQuietMs = 60'000, kWatchStaggerMs = 3'000;
constexpr const char *kRepliesLimit = "200";

// A user id's prefix only (U…, W… on Enterprise Grid), not its full shape.
bool hasUserIdPrefix(std::string_view id) {
    return id.size() > 1 && (id[0] == 'U' || id[0] == 'W');
}

std::string threadKey(ConvRef c, Ts root) {
    return str::concat({str::number(c), ":", str::number(root)});
}

// threadKey's parts back.
ConvRef keyConv(const std::string &k) {
    return ConvRef(std::atoll(k.c_str()));
}
Ts keyTs(const std::string &k) {
    const size_t colon = k.find(':');
    return colon == std::string::npos ? 0 : Ts(std::atoll(k.c_str() + colon + 1));
}

// The root ts below which thread-keyed state goes so that `keep` of the
// threads remain; 0 while there are no more than 2 × keep.
Ts pruneCut(std::vector<Ts> roots, size_t keep) {
    if (roots.size() <= 2 * keep)
        return 0;
    std::nth_element(roots.begin(), roots.end() - long(keep), roots.end());
    return roots[roots.size() - keep];
}

} // namespace

struct SlackBackend::Read {
    explicit Read(SlackBackend &b);
    ~Read();

    SlackBackend &b;
    model::Store &s;
    const bool    session;   // xoxc + cookie
    int64_t       speed = 1; // MSGA_SLACK_TEST_SPEEDUP: tests compress every delay

    // ── Plumbing ────────────────────────────────────────────────────────────
    enum class Lane : uint8_t { Normal, Background };
    struct Call {
        std::string method, form;
        ApiDone     done;
        int         attempt = 0;
        Lane        lane    = Lane::Normal;
        bool        quiet   = false; // a 429 waits without the banner
    };
    model::OneShotTimers                     timers{b._app}; // pending one-shots
    plat::TimerId                            tickTimer = 0;
    std::deque<Call>                         paced;
    // The queued (not yet issued) paced calls by method + form: a repeat
    // joins the queued one instead of costing another request. Deque
    // references survive push_back/pop_front of other elements.
    std::unordered_map<std::string, Call *>  pacedByKey;
    bool                                     pacedBusy     = false;
    // Normal-lane calls not yet answered (in flight, backing off or waiting
    // out a 429): the paced lane holds while any is.
    int                                      normalPending = 0;
    std::unordered_map<std::string, int64_t> readyAt; // per-method 429 cooldown

    int64_t now() const { return base::monotonicMs() * speed; }
    void    later(int64_t ms, std::function<void()> fn);
    void    call(
        std::string method,
        std::string form,
        ApiDone     done,
        Lane        lane  = Lane::Normal,
        bool        quiet = false
    );
    void issue(Call c);
    void pumpPaced();
    struct Pager {
        std::string                              method, form, key;
        std::function<void(const json::Value &)> onPage;
        std::function<void(const std::string &)> onDone;
        int                                      maxPages = 0, pages = 0; // 0: no cap
        Lane                                     lane  = Lane::Normal;
        bool                                     quiet = false;
    };
    // maxPages > 0: a backstop against a cursor loop (a server repeating the
    // same next_cursor forever); reaching it fails with "page_limit". quiet:
    // a load nobody waits on, whose 429s only wait (no banner).
    void paginate(
        std::string                              method,
        std::string                              form,
        std::string                              key,
        std::function<void(const json::Value &)> onPage,
        std::function<void(const std::string &)> onDone,
        int                                      maxPages = 0,
        Lane                                     lane     = Lane::Normal,
        bool                                     quiet    = false
    );
    void        pageFrom(std::shared_ptr<Pager> p, std::string cursor);
    std::string teamForm(std::initializer_list<std::pair<std::string_view, std::string_view>> kv);
    std::string threadsForm(std::string form);
    bool        threadsRefused(const std::string &err, bool sentTeam, bool sentSubs);
    void        applyApiBase(std::string_view workspaceUrl);

    // ── Connect and the roster ──────────────────────────────────────────────
    Backend::Done connectDone;
    int           connectPending = 0;
    std::string   connectError;
    bool          usersLoaded = false, usersLoading = false, convsLoaded = false, started = false;
    bool          convListRestricted = false, dmActivitySwept = false;
    bool          directoryLoading = false; // loadChannelDirectory
    int64_t       lastDirectory    = 0;     // now() of its last complete list

    int64_t lastRateNotice = -1; // the rate-limit banner, at most every 15 s
    void    noteRateLimited(const std::string &method, int64_t secs);

    void loadUsers(std::function<void(const std::string &)> done, Lane lane = Lane::Normal);

    void                          startLoads();
    void                          connectSettled();
    void                          mergeUsers(std::vector<model::User> users);
    void                          noteOffRoster(const std::unordered_set<std::string> &inSnapshot);
    void                          loadConversations(std::function<void(const std::string &)> done);
    void                          loadViaWebClient(std::function<void(const std::string &)> done);
    void                          loadChannelDirectory();
    void                          applyRoster(std::vector<model::Conversation> convs);
    void                          fixGroupMembers();
    void                          enrichDmActivity();
    void                          loadEmoji();
    void                          loadUsergroups();
    void                          loadCommands();
    std::vector<Backend::Command> serverCommands;     // commands.list (session tokens)
    uint64_t                      commandsRev    = 1; // moves with serverCommands
    bool                          commandsLoaded = false;

    // ── Names (setNamesMode) ────────────────────────────────────────────────
    // Slack's "Names" preference as last read (mapjson::namesOverride /
    // namesDefault); bootDefault: the default came from client.userBoot
    // (Grid: it, not team.prefs.get, is the workspace's). savedNames: the
    // last decided answer (mapjson::realNamesFrom) the cache kept, shown
    // until this run's prefs decide.
    NamesMode namesMode     = NamesMode::Service;
    int       namesOverride = mapjson::kNoPref, namesDefault = mapjson::kNoPref;
    int       savedNames  = mapjson::kNoPref;
    bool      bootDefault = false;
    bool      namesAsked  = false; // loadNames ran
    void      loadNames();
    void      bootNames(const json::Value &boot); // a client.userBoot answer
    void      applyNames();

    // ── Users ───────────────────────────────────────────────────────────────
    std::unordered_set<std::string>          pendingUsers, presenceUnavailable, offRoster;
    std::unordered_map<std::string, int64_t> probedAt;
    // users.info/bots.info lookups that failed (user_not_found, a deleted or
    // invisible account) or answered no user: not asked again until the
    // stored now() value (a day; 10 min after a transient Slack error), or
    // every 5 s poll of a channel showing their posts would re-request them.
    std::unordered_map<std::string, int64_t> lookupRetryAt;
    SelfPresence                             self;
    size_t                                   presenceIdx = 0;
    std::vector<int64_t> awayProbeAt; // by UserRef: now() of the last away-author probe

    void fetchUserIfNeeded(UserRef u);
    void fetchMissingDmUsers();
    void resolveAuthors(const std::vector<model::Message> &page);
    void reprobeOffRoster();
    void requestPresence(UserRef u, bool background);
    void pollDmPresence();
    void refreshSelfPresence(std::function<void()> then = {});

    // ── Stars and saved items (server snapshots, diffed) ────────────────────
    bool                                     starsUnavailable = false, starsPrimed = false;
    std::unordered_set<std::string>          serverStars;
    bool                                     savedUnavailable = false, savedPrimed = false;
    std::unordered_map<std::string, int64_t> serverSaved; // "conv:ts" → due (0 = none)
    void                                     refreshStarred();
    void                                     refreshSaved();

    // ── Sidebar sections (users.channelSections.*, session tokens only) ─────
    bool                                         sectionsUnavailable = false;
    int64_t                                      lastSections        = 0;
    int                                          sectionWrites       = 0; // edits in flight
    bool                                         sectionsStale = false;   // a list waits on them
    // Slack's own sections by type ("channels", "direct_messages",
    // "recent_apps"): where a conversation taken out of its section goes.
    std::unordered_map<std::string, std::string> systemSections;
    void                                         refreshSections();
    void                                         sectionWriteDone(const std::string &err);

    // ── Scheduled messages (the sidebar's "Scheduled messages") ─────────────
    bool     scheduledUnavailable = false;
    int64_t  lastScheduled        = 0;
    uint64_t scheduledWake        = 0; // the due-time re-list armed last
    void     refreshScheduled();

    // ── Message reminders ───────────────────────────────────────────────────
    plat::TimerId reminderTimer = 0;
    void          armReminders();
    void          fireDueReminders();
    void          announceReminder(ConvRef c, Ts ts);

    // ── Dead conversations: channel_not_found for us —
    // another workspace's over the shared socket, a dead DM. Persisted.
    std::unordered_set<std::string> dead;
    void                            markDead(const std::string &id);
    void                            markAlive(const std::string &id);
    void                            reconcileDead();

    // ── Mentioned channels the roster lacks ─────────────────────────────────
    std::unordered_set<std::string> pendingChannels;

    // ── Huddles ─────────────────────────────────────────────────────────────
    // A head page's newest huddle_thread room (the first page's check):
    // never clears a live huddle (an older page may hold a long-ended one).
    void applyHuddleRoom(ConvRef c, const json::Value &messages);

    // ── History and threads ─────────────────────────────────────────────────
    std::vector<model::Message> mapPage(ConvRef c, const json::Value &arr, bool topLevel);
    void                        loadThreadPages(ConvRef c, Ts root, bool live, Backend::Done done);
    // The open thread whose root the head page no longer carries: its root
    // alone (limit 1) tells whether latest_reply moved.
    void                        checkThreadTip(ConvRef c, Ts root);
    Ts                          newestHeldReply(ConvRef c, Ts root) const;

    // ── Polling (the realtime health check) ─────────────────────────────────
    ConvRef         openConv   = kNoConv;
    Ts              openThread = 0;
    int64_t         lastRoster = 0, lastCounts = 0, lastThreads = 0, lastFg = 0, lastBg = 0;
    int64_t         lastPresence = 0, lastSelf = 0, lastStarred = 0, lastSaved = 0, lastUsers = 0;
    bool            visible = true; // the window shows (setWindowVisible)
    // By ConvRef (0: none yet): the newest ts a poll saw (baselineOf), and
    // the revision of the last poll that landed.
    std::vector<Ts> pollBaseline;
    std::vector<uint64_t> completedPoll;
    uint64_t              pollRevision = 0;
    Ts                   &baselineOf(ConvRef c);
    size_t                bgIdx             = 0;
    bool                  countsUnavailable = false, countsDisabled = false, activityPrimed = false;
    int                   countsFailures = 0;
    std::unordered_map<std::string, mapjson::Counts> activity;
    // A counts snapshot named a conversation the roster lacks, or dropped
    // a member one: reload the roster soon. rosterAsked: the unknown ids
    // that already asked once (one the reload doesn't bring asks no more).
    bool                                             rosterWanted = false;
    std::unordered_set<std::string>                  rosterAsked;
    bool        threadsUnavailable = false, threadsPrimed = false;
    std::string threadsError;           // the refusal that gave up on the feed
    bool        threadsViaSubs = false; // getView refused: the feed is getTimestamps
    std::unordered_map<std::string, Ts> threadBaseline;
    std::unordered_set<std::string>     followed; // threads I started, replied in, or follow
    // Followed threads with an
    // unread reply (key → its newest), and how far I read each one.
    std::unordered_map<std::string, Ts> unreadThreads, threadReadFloor;
    void                                noteUnreadThreadReply(ConvRef c, Ts root, Ts ts);
    void                                threadRead(ConvRef c, Ts root, Ts upTo);
    void                                publishUnreadThreads();
    bool                                follow(const std::string &key); // true: newly followed
    void                                pruneThreadState();

    void tick();
    void pollUnreadCounts();
    // fromCounts: a client.counts snapshot (else the roster's own numbers).
    void applyActivity(const std::vector<mapjson::Counts> &snapshot, bool fromCounts);
    void pollConversation(ConvRef c, bool foreground, Ts hint = 0);
    void pollThreadReplies();
    void pollThreadSubs();
    bool feedThread(
        const std::string &key,
        ConvRef            c,
        Ts                 root,
        Ts                 newest,
        Ts                 lastRead,
        bool               priming,
        Ts                &baseline
    );
    void announceReplies(
        const std::string           &key,
        ConvRef                      c,
        Ts                           baseline,
        Ts                           newest,
        std::vector<model::Message> &replies,
        const std::vector<char>     &parentIsMe,
        int                         &injected
    );
    void    threadSubsPage(std::string cursor, ThreadsViewDone done);
    void    loadSubThread(const mapjson::ThreadSub &t, std::function<void(FollowedThread)> done);
    ConvRef nextBackgroundTarget();
    bool    inject(ConvRef c, model::Message m, bool parentIsMe);
    void    carryLocal(model::Conversation &fresh, const model::Conversation &old) const;

    // ── Watched threads (Backend::watchThread) ──────────────────────────────
    struct Watch {
        ConvRef  conv = kNoConv;
        Ts       root = 0, seen = 0; // seen: reported up to here
        Ts       lastReply = 0;      // the newest message known: how quiet it is
        int64_t  due       = 0;      // now() of the next poll; 0: the first, not placed yet
        uint32_t gen       = 0;      // tells a re-watch from the watch a poll was for
        bool     busy = false, inFlight = false, pulled = false, failing = false;
    };
    std::vector<Watch> watches; // few: linear is fine
    uint32_t           watchGen      = 0;
    int64_t            nextFirstPoll = 0;     // the stagger of the first polls
    bool               threadsUnread = false; // client.counts' threads.has_unreads
    Watch             *findWatch(ConvRef c, Ts root);
    int64_t            watchInterval(const Watch &w) const;
    void               pollWatches();
    void               pollWatch(Watch &w);
    // A message of thread root seen elsewhere (the feed, a push, the open
    // thread's poll): a watched thread's poll comes forward.
    void               pullWatch(ConvRef c, Ts root, Ts ts);
    void               readReplies(ConvRef c, Ts root, Ts after, Lane lane, RepliesDone done);

    // ── The workspace cache ─────────────────────────────────────────────────
    int64_t sweepAt = 0; // the last DM activity sweep (unix secs)
    int64_t usersAt = 0; // the last complete users.list (unix secs)
    void    loadExtras(const json::Value &x);
    void    saveExtras(json::Writer &w);
    void    extrasChanged() {
        if (cache)
            cache->extrasChanged();
    }
    void                                   refreshCachedHead(ConvRef c, Ts cachedNewest);
    // Last: destroyed first, its final write still sees every member above.
    std::unique_ptr<cache::WorkspaceCache> cache;
};

// ── Plumbing ────────────────────────────────────────────────────────────────

SlackBackend::Read::Read(SlackBackend &b) : b(b), s(b.store()), session(b._creds.sessionAuth()) {
    speed          = testSpeedup();
    // OAuth workspaces have no client.counts (no activity
    // snapshot for them): the roster diff is the activity source.
    countsDisabled = !session;
    applyApiBase(b._creds.workspaceUrl);
}

SlackBackend::Read::~Read() {
    for (plat::TimerId id : {tickTimer, reminderTimer})
        if (id)
            b._app.cancelTimer(id);
}

void SlackBackend::Read::later(int64_t ms, std::function<void()> fn) {
    // Clamped to what the timer takes: a due time weeks away (a scheduled
    // message) would otherwise wrap and fire at once. It re-arms then.
    timers.after(int(std::clamp<int64_t>(ms / speed, 0, INT32_MAX)), std::move(fn));
}

void SlackBackend::Read::call(
    std::string method, std::string form, ApiDone done, Lane lane, bool quiet
) {
    Call c{std::move(method), std::move(form), std::move(done), 0, lane, quiet};
    if (lane == Lane::Normal) {
        ++normalPending;
        issue(std::move(c));
        return;
    }
    // Background sweeps (conversations.info, users.getPresence, users.info)
    // trickle out one per 1.2 s so a sweep of a busy workspace stays under
    // its rate tier and never crowds out what the user is waiting for.
    std::string key = str::concat({c.method, "\n", c.form});
    if (const auto it = pacedByKey.find(key); it != pacedByKey.end()) {
        // The same call is already waiting (a presence round while the lane
        // is held): one request answers both callers.
        Call &queued = *it->second;
        if (c.done) {
            if (!queued.done) {
                queued.done = std::move(c.done);
            } else {
                queued.done = [a = std::move(queued.done), b = std::move(c.done)](
                                  const json::Document &doc, const std::string &err
                              ) {
                    a(doc, err);
                    b(doc, err);
                };
            }
        }
        return;
    }
    paced.push_back(std::move(c));
    pacedByKey.emplace(std::move(key), &paced.back());
    pumpPaced();
}

void SlackBackend::Read::pumpPaced() {
    // Interactive work first: the lane only moves while no Normal call is
    // outstanding (one merely cooling down on a 429 still holds it).
    if (pacedBusy || paced.empty() || normalPending > 0)
        return;
    pacedBusy = true;
    pacedByKey.erase(str::concat({paced.front().method, "\n", paced.front().form}));
    Call next = std::move(paced.front());
    paced.pop_front();
    issue(std::move(next));
    later(kPacedMs, [this] {
        pacedBusy = false;
        pumpPaced();
    });
}

void SlackBackend::Read::issue(Call c) {
    // Slack throttles per method: only this method waits out Retry-After.
    if (const auto it = readyAt.find(c.method); it != readyAt.end() && it->second > now()) {
        const int64_t wait = it->second - now();
        later(wait, [this, c = std::move(c)]() mutable { issue(std::move(c)); });
        return;
    }
    const std::string method = c.method, form = c.form;
    const bool        quiet = c.quiet;
    b.issueApi(
        method,
        form,
        [this, c = std::move(c)](const json::Document &doc, const std::string &err) mutable {
            if (err == "ratelimited") {
                const int64_t secs = std::max<int64_t>(doc.root()["retry_after"].integer(1), 1);
                LOG_INFO(
                    "slack", "%s rate-limited, retrying in %llds", c.method.c_str(), (long long)secs
                );
                readyAt[c.method] = now() + secs * 1000;
                later(secs * 1000, [this, c = std::move(c)]() mutable { issue(std::move(c)); });
                return;
            }
            // Transient Slack errors and lost connections: a bounded backoff
            // then the caller hears it. Reads retry a lost answer too (5xx on
            // idempotent calls).
            const bool transient = isTransientSlackError(err) && c.attempt < kMaxTransientRetries;
            const bool transport = isTransportError(err) && c.attempt < kMaxTransportRetries;
            if (transient || transport) {
                const int64_t delay = c.attempt == 0 ? 0 : retryBackoffMs(c.attempt - 1);
                ++c.attempt;
                later(delay, [this, c = std::move(c)]() mutable { issue(std::move(c)); });
                return;
            }
            const bool held  = c.lane == Lane::Normal && --normalPending == 0;
            const auto alive = b._alive; // `done` may end the backend
            if (c.done)
                c.done(doc, err);
            if (held && *alive)
                pumpPaced();
        },
        false,
        quiet
    );
}

// Rate limited: the error banner names the first method that
// tripped, at most once per 15 s.
void SlackBackend::Read::noteRateLimited(const std::string &method, int64_t secs) {
    const int64_t t = now();
    if (lastRateNotice >= 0 && t - lastRateNotice < kRateLimitNoticeGapMs)
        return;
    lastRateNotice = t;
    if (b.onError)
        b.onError(
            i18n::arg(
                i18n::trn(
                    "Slack is rate-limiting requests (%1) \xE2\x80\x94 retrying in %n second.",
                    "Slack is rate-limiting requests (%1) \xE2\x80\x94 retrying in %n seconds.",
                    secs
                ),
                method
            )
        );
}

void SlackBackend::Read::paginate(
    std::string                              method,
    std::string                              form,
    std::string                              key,
    std::function<void(const json::Value &)> onPage,
    std::function<void(const std::string &)> onDone,
    int                                      maxPages,
    Lane                                     lane,
    bool                                     quiet
) {
    auto p      = std::make_shared<Pager>(Pager{
        std::move(method), std::move(form), std::move(key), std::move(onPage), std::move(onDone)
    });
    p->maxPages = maxPages;
    p->lane     = lane;
    p->quiet    = quiet;
    pageFrom(std::move(p), {});
}

void SlackBackend::Read::pageFrom(std::shared_ptr<Pager> p, std::string cursor) {
    if (p->maxPages > 0 && ++p->pages > p->maxPages) {
        p->onDone("page_limit");
        return;
    }
    std::string form = p->form;
    if (!cursor.empty())
        form.append(str::concat({form.empty() ? "" : "&", "cursor=", net::percentEncode(cursor)}));
    call(
        p->method,
        std::move(form),
        [this, p](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                p->onDone(err);
                return;
            }
            const json::Value root = doc.root();
            p->onPage(root[p->key]);
            const std::string_view next = root["response_metadata"]["next_cursor"].str();
            if (next.empty())
                p->onDone({});
            else if (p->lane == Lane::Normal && !paced.empty())
                // A long listing lets the paced lane in between its pages
                // (it only moves while no Normal call is outstanding).
                later(0, [this, p, next = std::string(next)] { pageFrom(p, next); });
            else
                pageFrom(p, std::string(next));
        },
        p->lane,
        p->quiet
    );
}

// team_id is required for an org-level (Enterprise Grid) token and ignored
// for a workspace one, so it always goes with the listing methods.
std::string SlackBackend::Read::teamForm(
    std::initializer_list<std::pair<std::string_view, std::string_view>> kv
) {
    std::string        f    = net::formEncode(kv);
    const std::string &team = s.workspaceId.empty() ? b._creds.teamId : s.workspaceId;
    if (!team.empty())
        f.append(str::concat({f.empty() ? "" : "&", "team_id=", net::percentEncode(team)}));
    return f;
}

// The feed's form: on Grid, once refused without it, with team_id.
std::string SlackBackend::Read::threadsForm(std::string form) {
    if (b._threadsTeamId)
        form.append("&").append(teamForm({}));
    return form;
}

// Slack said no to the feed itself (not the network, a throttle, a hiccup
// or dead credentials).
namespace {
bool isFeedRefusal(const std::string &err) {
    return !err.empty() && err != "cancelled" && err != "ratelimited" && !isTransportError(err) &&
           !isTransientSlackError(err) && !isAuthError(err);
}
} // namespace

// A refusal of the feed (issue #118). On Grid a session token may resolve
// in the org: a method-level refusal goes once more with team_id. Then
// getView gives way to getTimestamps, which today's web client reads
// instead; only a method-level refusal of that gives up on the feed.
// sentTeam / sentSubs: what the refused call was (one sent before a
// switch, the poll's and the page's crossing, still retries). true: try
// again.
bool SlackBackend::Read::threadsRefused(const std::string &err, bool sentTeam, bool sentSubs) {
    if (!isFeedRefusal(err))
        return false;
    if (b._grid && !sentTeam && isMethodUnavailable(err)) {
        b._threadsTeamId = true;
        return true;
    }
    if (!sentSubs) {
        if (!threadsViaSubs)
            LOG_INFO("slack", "the thread feed falls back to subscriptions.thread.getTimestamps");
        threadsViaSubs = true;
        return true;
    }
    if (!isMethodUnavailable(err))
        return false;
    threadsUnavailable = true;
    threadsError       = err;
    return false;
}

// A session token resolves in the context of the host it is called on; on
// slack.com a Grid token lands in the org and is refused workspace methods
// (issue #49). OAuth tokens are workspace-scoped: slack.com is fine.
void SlackBackend::Read::applyApiBase(std::string_view workspaceUrl) {
    net::Url u;
    if (!session || !u.parse(str::trim(workspaceUrl)) || u.scheme != "https" || u.host.empty())
        return;
    b._auth.base = str::concat({"https://", u.host, "/api/"});
}

// ── Connect ─────────────────────────────────────────────────────────────────

void SlackBackend::connect(Done done) {
    _read->call(
        "auth.test",
        {},
        [this, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                LOG_WARN("slack", "%s: auth.test: %s", _creds.teamId.c_str(), err.c_str());
                if (done)
                    done(false, err);
                return;
            }
            const json::Value o  = doc.root();
            _store.workspaceId   = std::string(o["team_id"].str(_creds.teamId));
            _store.workspaceName = std::string(o["team"].str(_creds.teamName));
            _store.workspaceUrl  = std::string(o["url"].str(_creds.workspaceUrl));
            // The icon is the one sign-in stored (team.info, image_88).
            if (_store.workspaceIcon.empty())
                _store.workspaceIcon = _creds.iconUrl;
            // auth.test's url is the authoritative host (stored credentials may
            // predate workspaceUrl or carry a stale one).
            _read->applyApiBase(_store.workspaceUrl);
            _grid = o.has("enterprise_id");
            if (_grid)
                LOG_INFO(
                    "slack",
                    "%s is part of Enterprise Grid org %.*s",
                    _store.workspaceId.c_str(),
                    int(o["enterprise_id"].str().size()),
                    o["enterprise_id"].str().data()
                );
            _store.me          = _store.internUser(o["user_id"].str());
            _read->connectDone = std::move(done);
            _read->startLoads();
        }
    );
}

void SlackBackend::Read::startLoads() {
    // In this order: self presence, conversations (then emoji), stars,
    // users, user groups, saved items. A users.list less than a day old
    // stands in the cache: no new one (a big org's takes hours), people it
    // lacks are looked up one by one (fetchUserIfNeeded), and the daily
    // refresh brings the rest.
    const bool cachedUsers = usersAt > 0 && base::nowSecs() - usersAt < kUsersRefreshGapMs / 1000;
    connectPending         = cachedUsers ? 1 : 2;
    connectError.clear();
    refreshSelfPresence();
    loadConversations([this](const std::string &err) {
        if (!err.empty()) {
            connectError = err;
        } else if (connectDone) {
            // The conversation list is what the window waits for (its column
            // shows the moment it arrives); users.list may take many
            // pages more, and names fill in as it lands.
            Backend::Done done = std::move(connectDone);
            connectDone        = nullptr;
            done(true, {});
        }
        connectSettled();
    });
    if (cachedUsers) {
        usersLoaded = true;
        for (const auto &[id, at] : probedAt) // the cached off-roster users
            offRoster.insert(id);
        later(kOffRosterProbeMs, [this] { reprobeOffRoster(); });
    } else {
        loadUsers([this](const std::string &) { connectSettled(); });
    }
    refreshStarred();
    refreshSections();
    loadUsergroups();
    refreshSaved();
    loadCommands();
    loadNames();
}

void SlackBackend::Read::connectSettled() {
    if (--connectPending > 0)
        return;
    Backend::Done done = std::move(connectDone);
    connectDone        = nullptr;
    if (!convsLoaded) {
        // No roster: the caller retries connect (nothing to render yet).
        if (done)
            done(false, connectError.empty() ? std::string("no_conversations") : connectError);
        return;
    }
    if (done)
        done(true, {});
    if (started)
        return;
    started         = true;
    const int64_t t = now();
    lastRoster = lastUsers = lastStarred = lastSaved = lastScheduled = lastSelf = lastPresence = t;
    lastBg                                                                                     = t;
    // The daily users.list counts from the last one, this run's or the cache's.
    if (usersAt > 0)
        lastUsers = t - std::max<int64_t>(base::nowSecs() - usersAt, 0) * 1000 * speed;
    // The first activity snapshot seeds the unread badges at once (the
    // cache may already have them); presence starts with the hot set.
    pollUnreadCounts();
    lastCounts = t;
    pollDmPresence();
    // After the roster: the list names its conversations by id.
    refreshScheduled();
    loadEmoji();
    // The safety timer: min(15 s, the open-chat cadence).
    const int tickMs = int((session ? 5'000 : 15'000) / speed);
    tickTimer        = b._app.addTimer(std::max(tickMs, 1), true, [this] { tick(); });
}

void SlackBackend::Read::loadUsers(std::function<void(const std::string &)> done, Lane lane) {
    // Each page lands as it comes (a big org's roster takes many pages, and
    // one that gives up keeps the pages before it); who is off the roster is
    // only known once the last page is in.
    auto seen    = std::make_shared<std::unordered_set<std::string>>();
    usersLoading = true;
    paginate(
        "users.list",
        teamForm({{"limit", "200"}}),
        "members",
        [this, seen](const json::Value &arr) {
            std::vector<model::User> page;
            for (const json::Value u : arr) {
                model::User m = mapjson::toUser(u);
                if (!m.id.empty()) {
                    seen->insert(m.id);
                    page.push_back(std::move(m));
                }
            }
            mergeUsers(std::move(page));
        },
        [this, seen, done = std::move(done)](const std::string &err) {
            usersLoading = false;
            if (!err.empty()) {
                LOG_WARN("slack", "users.list: %s", err.c_str());
            } else {
                noteOffRoster(*seen);
                usersLoaded = true;
                usersAt     = base::nowSecs();
                extrasChanged();
                fetchMissingDmUsers();
                fixGroupMembers();
                later(kOffRosterProbeMs, [this] { reprobeOffRoster(); });
            }
            done(err);
        },
        0,
        lane,
        true // a throttled page just waits it out: no banner
    );
}

// A users.list page merges: its rows win, known enrichment fills their gaps,
// and users it omits are kept (Slack Connect peers are never in users.list;
// departed members come back as deleted rows, not absences).
void SlackBackend::Read::mergeUsers(std::vector<model::User> users) {
    if (users.empty())
        return;
    for (model::User &u : users) {
        if (const UserRef r = s.findUser(u.id); r != kNoUser) {
            const model::User &old = s.user(r);
            if (!old.placeholder) {
                if (u.avatar.empty())
                    u.avatar = old.avatar;
                if (u.displayName.empty()) { // no name at all: the known ones
                    u.displayName = old.displayName;
                    u.realName    = old.realName;
                    u.profileName = old.profileName;
                }
                if (u.name.empty())
                    u.name = old.name;
            }
            // Presence is polled separately (and may already have answered
            // for a user only interned so far).
            u.active = old.active;
            u.dnd    = old.dnd;
        }
        s.addUser(std::move(u));
    }
    s.usersChanged();
}

// The whole users.list is in: known users it never named are off the roster.
void SlackBackend::Read::noteOffRoster(const std::unordered_set<std::string> &inSnapshot) {
    if (inSnapshot.empty())
        return; // an empty snapshot says nothing
    std::unordered_set<std::string> off;
    for (size_t i = 0; i < s.userCount(); ++i) {
        const model::User &u = s.user(UserRef(i));
        if (!u.placeholder && !inSnapshot.count(u.id))
            off.insert(u.id);
    }
    offRoster = std::move(off);
    for (auto it = probedAt.begin(); it != probedAt.end();)
        it = offRoster.count(it->first) ? std::next(it) : probedAt.erase(it);
}

void SlackBackend::Read::loadConversations(std::function<void(const std::string &)> done) {
    if (convListRestricted) {
        loadViaWebClient(std::move(done));
        return;
    }
    auto acc = std::make_shared<std::vector<model::Conversation>>();
    // users.conversations: only what I'm in (conversations.list pages through
    // every public channel of the workspace — thousands in a big one — on its
    // tight Tier 2; the channel browser asks for those: loadChannelDirectory).
    paginate(
        "users.conversations",
        teamForm(
            {{"types", "public_channel,private_channel,im,mpim"},
             {"exclude_archived", "true"},
             {"limit", "1000"}}
        ),
        "channels",
        [this, acc](const json::Value &arr) {
            for (const json::Value c : arr) {
                model::Conversation m = mapjson::toConversation(c, s);
                if (!m.id.empty())
                    acc->push_back(std::move(m));
            }
        },
        [this, acc, done = std::move(done)](const std::string &err) mutable {
            if (err == "enterprise_is_restricted") {
                // Grid: the listings are never served to a session token.
                if (!convListRestricted)
                    LOG_INFO(
                        "slack", "users.conversations restricted (Grid): client.userBoot + im.list"
                    );
                convListRestricted = true;
                loadViaWebClient(std::move(done));
                return;
            }
            if (!err.empty()) {
                LOG_WARN("slack", "users.conversations: %s", err.c_str());
                done(err);
                return;
            }
            applyRoster(std::move(*acc));
            done({});
        }
    );
}

// The channel browser's list: the workspace's public channels, which the
// roster (mine only) lacks. Those I'm not in are kept as non-members; mine
// stay the roster's. At most every kDirectoryGapMs; none on Grid session
// tokens (conversations.list is refused there).
void SlackBackend::Read::loadChannelDirectory() {
    if (convListRestricted || directoryLoading ||
        (lastDirectory && now() - lastDirectory < kDirectoryGapMs))
        return;
    directoryLoading = true;
    auto acc         = std::make_shared<std::vector<model::Conversation>>();
    paginate(
        "conversations.list",
        teamForm({{"types", "public_channel"}, {"exclude_archived", "true"}, {"limit", "1000"}}),
        "channels",
        [this, acc](const json::Value &arr) {
            for (const json::Value c : arr)
                if (model::Conversation m = mapjson::toConversation(c, s); !m.id.empty())
                    acc->push_back(std::move(m));
        },
        [this, acc](const std::string &err) {
            directoryLoading = false;
            if (!err.empty()) {
                LOG_WARN("slack", "conversations.list: %s", err.c_str());
                return;
            }
            lastDirectory = now();
            std::vector<model::Conversation> others;
            for (model::Conversation &m : *acc) {
                const ConvRef r = s.findConversation(m.id);
                if (r != kNoConv && s.conversation(r).member)
                    continue; // the roster's (joined meanwhile, too)
                m.member = false;
                if (r != kNoConv)
                    carryLocal(m, s.conversation(r));
                others.push_back(std::move(m));
            }
            s.addConversations(std::move(others));
        },
        0,
        Lane::Normal,
        true // nobody waits on it: a 429 only waits
    );
}

// How Slack's own client boots: client.userBoot (joined channels and MPDMs)
// + im.list (every DM; userBoot only carries the open ones). Both halves
// must land, or the roster would be replaced by half of itself.
void SlackBackend::Read::loadViaWebClient(std::function<void(const std::string &)> done) {
    struct Acc {
        std::vector<model::Conversation>         convs;
        int                                      pending = 2;
        std::string                              error;
        std::function<void(const std::string &)> done;
    };
    auto acc  = std::make_shared<Acc>();
    acc->done = std::move(done);
    auto add  = [this, acc](const json::Value &arr) {
        for (const json::Value c : arr)
            if (!c["is_archived"].boolean()) // userBoot has no exclude_archived
                if (model::Conversation m = mapjson::toConversation(c, s); !m.id.empty())
                    acc->convs.push_back(std::move(m));
    };
    auto finish = [this, acc](const std::string &err) {
        if (!err.empty())
            acc->error = err;
        if (--acc->pending > 0)
            return;
        if (acc->error.empty())
            applyRoster(std::move(acc->convs));
        acc->done(acc->error);
    };
    call(
        "client.userBoot",
        "min_channel_updated=0",
        [this, add, finish](const json::Document &doc, const std::string &err) {
            if (err.empty()) {
                add(doc.root()["channels"]);
                bootNames(doc.root());
            } else {
                LOG_WARN("slack", "client.userBoot: %s", err.c_str());
            }
            finish(err);
        }
    );
    paginate(
        "im.list",
        "get_latest=true&get_read_state=true&limit=1000",
        "ims",
        add,
        [finish](const std::string &err) {
            if (!err.empty())
                LOG_WARN("slack", "im.list: %s", err.c_str());
            finish(err);
        }
    );
}

// ── Names ───────────────────────────────────────────────────────────────────

void SlackBackend::setNamesMode(NamesMode mode) {
    Read &r     = *_read;
    r.namesMode = mode;
    r.applyNames();
    // Switched to Slack's own while connected: ask it now.
    if (mode == NamesMode::Service && !r.namesAsked && r.started)
        r.loadNames();
}

// Slack's "Names" preference, once per connect while it is asked for:
// users.prefs.get, and team.prefs.get only when the user follows the
// workspace default. Session tokens only (OAuth ones are refused both). On
// the paced lane: after what the connect waits for.
void SlackBackend::Read::loadNames() {
    if (!session || namesMode != NamesMode::Service)
        return;
    namesAsked = true;
    call(
        "users.prefs.get",
        {},
        [this](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                LOG_WARN("slack", "users.prefs.get: %s", err.c_str());
                return;
            }
            namesOverride = mapjson::namesOverride(doc.root()["prefs"]);
            applyNames();
            if (namesOverride != 0 || bootDefault)
                return;
            call(
                "team.prefs.get",
                {},
                [this](const json::Document &doc, const std::string &err) {
                    if (!err.empty()) {
                        LOG_WARN("slack", "team.prefs.get: %s", err.c_str());
                        return;
                    }
                    if (bootDefault) // userBoot answered meanwhile: it wins
                        return;
                    namesDefault = mapjson::namesDefault(doc.root()["prefs"]);
                    applyNames();
                },
                Lane::Background
            );
        },
        Lane::Background
    );
}

// client.userBoot (the Grid roster) carries both prefs: the user's, and the
// workspace's in team.prefs or in its workspaces[] entry.
void SlackBackend::Read::bootNames(const json::Value &boot) {
    if (const int o = mapjson::namesOverride(boot["prefs"]); o != mapjson::kNoPref)
        namesOverride = o;
    int d = mapjson::namesDefault(boot["team"]["prefs"]);
    for (const json::Value w : boot["workspaces"])
        if (d == mapjson::kNoPref && w["id"].str() == s.workspaceId)
            d = mapjson::namesDefault(w["prefs"]);
    if (d != mapjson::kNoPref) {
        namesDefault = d;
        bootDefault  = true;
    }
    applyNames();
}

void SlackBackend::Read::applyNames() {
    bool real = namesMode != NamesMode::Display;
    if (namesMode == NamesMode::Service) {
        const int decided = mapjson::realNamesFrom(namesOverride, namesDefault);
        if (decided != mapjson::kNoPref && decided != savedNames) {
            savedNames = decided;
            extrasChanged();
        }
        real = savedNames != 0; // not decided yet, nor ever before: full names
    }
    s.setRealNames(real);
}

// A roster reload: what the API cannot
// tell (local badges, mute, notify level, cursors, members, local name)
// survives a reload.
void SlackBackend::Read::applyRoster(std::vector<model::Conversation> convs) {
    // The roster's own numbers are the activity source once client.counts is
    // unavailable (OAuth, or given up on) — taken before the local merge.
    std::vector<mapjson::Counts> serverActivity;
    if (countsDisabled)
        for (const model::Conversation &c : convs)
            serverActivity.push_back({c.id, c.latest, c.lastRead, c.unread, c.mentions});

    std::unordered_set<std::string> listed;
    for (model::Conversation &fresh : convs) {
        listed.insert(fresh.id);
        if (const ConvRef r = s.findConversation(fresh.id); r != kNoConv) {
            carryLocal(fresh, s.conversation(r));
        } else if (starsPrimed && serverStars.count(fresh.id)) {
            fresh.starred = true; // stars.list answered before the roster did
        }
    }
    s.addConversations(std::move(convs)); // a cold start: one Roster emit, not one per row
    // The list is replaced: what is no longer listed (left, archived) is
    // no longer a member conversation.
    for (ConvRef r = 0; r < s.conversationCount(); ++r)
        if (s.conversation(r).member && !listed.count(s.conversation(r).id))
            s.updateConversation(r, [](model::Conversation &c) { c.member = false; });
    convsLoaded = true;
    reconcileDead();
    if (!serverActivity.empty())
        applyActivity(serverActivity, false);
    fixGroupMembers();
    fetchMissingDmUsers();
    enrichDmActivity();
}

void SlackBackend::Read::carryLocal(
    model::Conversation &fresh, const model::Conversation &old
) const {
    fresh.unread   = std::max(fresh.unread, old.unread);
    fresh.mentions = std::max(fresh.mentions, old.mentions);
    fresh.starred  = fresh.starred || old.starred; // stars.list decides
    if (fresh.notify == model::NotifyLevel::Default)
        fresh.notify = old.notify; // no read API for the per-channel level
    fresh.muted = fresh.muted || old.muted;
    if (fresh.localName.empty())
        fresh.localName = old.localName;
    if (fresh.members.empty())
        fresh.members = old.members;
    fresh.lastRead = std::max(fresh.lastRead, old.lastRead);
    fresh.latest   = std::max(fresh.latest, old.latest);
    if (fresh.canvasTitle.empty()) { // conversations.list omits properties
        fresh.canvasTitle = old.canvasTitle;
        fresh.canvasId    = old.canvasId;
    }
    if (fresh.memberCount == 0)
        fresh.memberCount = old.memberCount;
    // The list carries no room: a live huddle stays until its end is seen
    // (the reload's merge).
    if (old.huddleActive && !fresh.huddleActive) {
        fresh.huddleActive       = true;
        fresh.huddleLink         = old.huddleLink;
        fresh.huddleParticipants = old.huddleParticipants;
    }
}

// conversations.list leaves a group DM's members out; they are read
// from the "mpdm-alice--bob-1" name. The Store names groups by members.
void SlackBackend::Read::fixGroupMembers() {
    if (!usersLoaded)
        return;
    std::unordered_map<std::string, UserRef> byHandle;
    for (ConvRef r = 0; r < s.conversationCount(); ++r) {
        const model::Conversation &c = s.conversation(r);
        if (c.kind != model::ConvKind::Group || !c.members.empty() ||
            !str::startsWith(c.name, "mpdm-"))
            continue;
        if (byHandle.empty())
            for (size_t i = 0; i < s.userCount(); ++i)
                if (!s.user(UserRef(i)).name.empty())
                    byHandle.emplace(s.user(UserRef(i)).name, UserRef(i));
        std::string_view n = std::string_view(c.name).substr(5);
        if (const size_t dash = n.rfind('-');
            dash != std::string_view::npos && dash + 1 < n.size() &&
            std::all_of(n.begin() + dash + 1, n.end(), [](char ch) {
                return ch >= '0' && ch <= '9';
            }))
            n = n.substr(0, dash);
        std::vector<UserRef> members;
        while (!n.empty()) {
            const size_t     sep    = n.find("--");
            std::string_view handle = n.substr(0, sep);
            if (const auto it = byHandle.find(std::string(handle)); it != byHandle.end())
                members.push_back(it->second);
            n = sep == std::string_view::npos ? std::string_view() : n.substr(sep + 2);
        }
        if (s.me != kNoUser && std::find(members.begin(), members.end(), s.me) == members.end())
            members.push_back(s.me);
        if (members.size() > 1)
            s.updateConversation(r, [&](model::Conversation &x) {
                x.members = std::move(members);
            });
    }
}

// conversations.list carries no last_read / latest, so DMs and MPDMs get them
// from conversations.info on the paced lane. Only needed without
// client.counts (which reports them for everything); once per run at most.
void SlackBackend::Read::enrichDmActivity() {
    if (!countsDisabled || dmActivitySwept)
        return;
    dmActivitySwept = true;
    // One sweep per 12 h across restarts (the cache keeps the stamp
    // and the cursors it found).
    if (base::nowSecs() - sweepAt < kDmSweepGapSecs)
        return;
    const std::vector<std::string> ids       = b.directConversationIds();
    auto                           remaining = std::make_shared<size_t>(ids.size());
    for (const std::string &id : ids)
        b.sweepInfo(id, [this, id, remaining](model::Conversation *info) {
            if (--*remaining == 0) { // every call settled: the sweep is done
                sweepAt = base::nowSecs();
                extrasChanged();
            }
            const ConvRef r = s.findConversation(id);
            if (!info || r == kNoConv)
                return;
            const model::Conversation &c = s.conversation(r);
            if (info->lastRead <= c.lastRead && info->latest <= c.latest)
                return;
            s.updateConversation(r, [&](model::Conversation &x) {
                x.lastRead = std::max(x.lastRead, info->lastRead);
                x.latest   = std::max(x.latest, info->latest);
            });
        });
}

// The DMs and group DMs a conversations.info sweep reads, 1:1 first; the
// dead are left out, and so is a DM whose peer was deactivated (it answers
// channel_not_found).
std::vector<std::string> SlackBackend::directConversationIds() const {
    std::vector<std::string> ids;
    for (int pass = 0; pass < 2; ++pass)
        for (ConvRef r = 0; r < _store.conversationCount(); ++r) {
            const model::Conversation &c = _store.conversation(r);
            if (c.kind != (pass ? model::ConvKind::Group : model::ConvKind::Dm) || isDead(c.id))
                continue;
            if (c.kind == model::ConvKind::Dm && _store.user(c.dmUser).deleted)
                continue;
            ids.push_back(c.id);
        }
    return ids;
}

// One conversations.info of a sweep, on the paced lane: channel_not_found
// marks it dead; `done` gets the mapped channel, or null on any failure.
void SlackBackend::sweepInfo(
    const std::string &id, std::function<void(model::Conversation *info)> done
) {
    _read->call(
        "conversations.info",
        net::formEncode({{"channel", id}}),
        [this, id, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "channel_not_found")
                markDead(id);
            if (!err.empty()) {
                done(nullptr);
                return;
            }
            model::Conversation info = mapjson::toConversation(doc.root()["channel"], _store);
            done(&info);
        },
        Read::Lane::Background
    );
}

void SlackBackend::Read::loadEmoji() {
    call("emoji.list", {}, [this](const json::Document &doc, const std::string &err) {
        if (!err.empty()) {
            LOG_WARN("slack", "emoji.list: %s", err.c_str());
            return;
        }
        // name → image URL, or "alias:other" (resolved by the Store). The
        // whole set: one removed since the last load goes.
        std::unordered_map<std::string, std::string> all;
        for (const json::Value e : doc.root()["emoji"])
            all.emplace(std::string(e.key()), std::string(e.str()));
        s.replaceCustomEmoji(std::move(all));
        s.usersChanged(); // repaint: emoji in names, statuses and messages
        if (cache)
            cache->emojiChanged();
    });
}

void SlackBackend::Read::loadUsergroups() {
    // Every group with its handle and name
    // (mentions show "@handle"); include_users: a mention of a group I
    // belong to counts as a mention.
    call(
        "usergroups.list",
        teamForm({{"include_users", "1"}}),
        [this](const json::Document &doc, const std::string &err) {
            if (!err.empty()) // missing_scope on an older OAuth token: the cache stays
                return;
            // Only whether I am in each: the member lists themselves are
            // not kept (a big org's run to millions of ids).
            const std::string                   &meId = s.user(s.me).id;
            std::vector<model::Store::Usergroup> groups;
            for (const json::Value g : doc.root()["usergroups"]) {
                model::Store::Usergroup x;
                x.id     = std::string(g["id"].str());
                x.handle = std::string(g["handle"].str());
                x.name   = std::string(g["name"].str());
                for (const json::Value u : g["users"])
                    if (!meId.empty() && u.str() == meId) {
                        x.mine = true;
                        break;
                    }
                if (!x.id.empty())
                    groups.push_back(std::move(x));
            }
            // An empty snapshot says nothing (the cache stays).
            if (groups.empty() || groups == s.usergroups())
                return;
            s.setUsergroups(std::move(groups)); // the workspace cache keeps them
        }
    );
}

// The workspace's slash commands (an array, or an
// object keyed by name); session tokens only (OAuth answers
// not_allowed_token_type).
void SlackBackend::Read::loadCommands() {
    if (!session)
        return;
    call("commands.list", {}, [this](const json::Document &doc, const std::string &err) {
        if (!err.empty()) {
            if (err != "not_allowed_token_type" && err != "cancelled")
                LOG_WARN("slack", "commands.list: %s", err.c_str());
            return;
        }
        std::vector<Backend::Command> out;
        for (const json::Value c : doc.root()["commands"]) {
            Backend::Command x;
            std::string_view name = c["name"].str();
            if (name.empty())
                name = c.key();
            if (!name.empty() && name.front() == '/')
                name.remove_prefix(1);
            if (name.empty())
                continue;
            x.name  = std::string(name);
            x.desc  = std::string(c["desc"].str());
            x.usage = std::string(c["usage"].str());
            x.local = true; // run by runLocalCommand (chat.command), never sent as text
            // The row label: "App · <name>" for an app's, "Slack" else.
            x.app   = c["type"].str() == "app";
            if (x.app) {
                const std::string_view appName = c["app_name"].str();
                x.source = appName.empty() ? std::string(i18n::tr("App"))
                                           : str::concat({i18n::tr("App"), " \xC2\xB7 ", appName});
                x.icon   = std::string(c["icon_url"].str());
                if (x.icon.empty())
                    x.icon = mapjson::firstIcon(c["icons"]);
            } else {
                x.source = "Slack";
            }
            out.push_back(std::move(x));
        }
        serverCommands = std::move(out);
        ++commandsRev;
        commandsLoaded = true;
    });
}

// ── Users ───────────────────────────────────────────────────────────────────

// users.info for someone users.list never lists (Slack Connect peers, USLACK,
// deactivated accounts).
void SlackBackend::Read::fetchUserIfNeeded(UserRef ref) {
    if (ref == kNoUser || !s.user(ref).placeholder)
        return;
    const std::string id = s.user(ref).id;
    // No isSlackSystemUser skip: users.list omits USLACK and USLACKBOT, and
    // this one users.info is what names them ("Slack", its logo). Once
    // resolved they are no longer placeholders, so they are never re-asked.
    if (pendingUsers.count(id))
        return;
    if (const auto it = lookupRetryAt.find(id); it != lookupRetryAt.end() && now() < it->second)
        return;
    const bool bot = id.size() > 1 && id[0] == 'B';
    if (!bot && !hasUserIdPrefix(id))
        return;
    pendingUsers.insert(id);
    // A bot id (a bot post without a profile) resolves through bots.info.
    call(
        bot ? "bots.info" : "users.info",
        net::formEncode({{bot ? "bot" : "user", id}}),
        [this, id, bot](const json::Document &doc, const std::string &err) {
            pendingUsers.erase(id);
            // A lost connection or a cancel says nothing about the id: the
            // next page asks again. Anything else waits out the backoff.
            if (err == "cancelled" || isTransportError(err))
                return;
            if (!err.empty()) {
                const bool transient = isTransientSlackError(err);
                lookupRetryAt[id] =
                    now() + (transient ? kLookupRetryTransientMs : kUsersRefreshGapMs);
                return;
            }
            lookupRetryAt.erase(id);
            model::User u;
            if (bot) {
                const json::Value o = doc.root()["bot"];
                u.id                = id;
                u.name = u.displayName = std::string(o["name"].str());
                u.avatar               = mapjson::firstIcon(o["icons"]);
                u.bot                  = true;
            } else {
                u = mapjson::toUser(doc.root()["user"]);
                if (u.id.empty()) {
                    lookupRetryAt[id] = now() + kUsersRefreshGapMs;
                    return;
                }
                offRoster.insert(u.id); // only the daily re-probe refreshes it
                probedAt[u.id] = now();
                extrasChanged();
            }
            s.usersChanged(s.addUser(std::move(u)));
        }
    );
}

void SlackBackend::Read::fetchMissingDmUsers() {
    if (!usersLoaded)
        return; // every peer would look missing; the users load calls again
    for (ConvRef r = 0; r < s.conversationCount(); ++r) {
        const model::Conversation &c = s.conversation(r);
        if (c.kind == model::ConvKind::Dm)
            fetchUserIfNeeded(c.dmUser);
        else if (c.kind == model::ConvKind::Group)
            for (UserRef u : std::vector<UserRef>(c.members))
                fetchUserIfNeeded(u);
    }
}

// Authors the roster doesn't know.
void SlackBackend::Read::resolveAuthors(const std::vector<model::Message> &page) {
    if (!usersLoaded)
        return;
    for (const model::Message &m : page) {
        fetchUserIfNeeded(m.user);
        if (m.isHuddle()) // a huddle row's attendees too
            for (UserRef u : m.extra->huddle.attendees)
                fetchUserIfNeeded(u);
    }
}

// Users known only through users.info get a
// daily refresh, oldest first, capped per pass, on the paced lane.
void SlackBackend::Read::reprobeOffRoster() {
    const int64_t                                t = now();
    std::vector<std::pair<int64_t, std::string>> due;
    for (const std::string &id : offRoster) {
        if (!hasUserIdPrefix(id) || mapjson::isSlackSystemUser(id) || pendingUsers.count(id))
            continue;
        const auto    it   = probedAt.find(id);
        const int64_t last = it == probedAt.end() ? 0 : it->second;
        if (it != probedAt.end() && t - last < kUsersRefreshGapMs)
            continue;
        due.emplace_back(last, id);
    }
    std::sort(due.begin(), due.end());
    if (due.size() > size_t(kMaxUserReprobes))
        due.resize(kMaxUserReprobes);
    if (!due.empty())
        extrasChanged();
    for (const auto &[last, id] : due) {
        probedAt[id] = t; // at request time: a failure waits a day
        call(
            "users.info",
            net::formEncode({{"user", id}}),
            [this](const json::Document &doc, const std::string &err) {
                if (!err.empty())
                    return;
                model::User   u = mapjson::toUser(doc.root()["user"]);
                const UserRef r = s.findUser(u.id);
                if (u.id.empty() || r == kNoUser)
                    return;
                u.active = s.user(r).active;
                u.dnd    = s.user(r).dnd;
                s.usersChanged(s.addUser(std::move(u)));
            },
            Lane::Background
        );
    }
}

// users.getPresence answers internal_error for bots,
// system accounts and users not presence-visible to us — skip those, and
// stop sweeping someone after a failed sweep probe.
void SlackBackend::Read::requestPresence(UserRef ref, bool background) {
    const model::User &u = s.user(ref);
    if (ref == kNoUser || u.placeholder || u.bot || mapjson::isSlackSystemUser(u.id))
        return;
    if (background && presenceUnavailable.count(u.id))
        return;
    const std::string id = u.id;
    call(
        "users.getPresence",
        net::formEncode({{"user", id}}),
        [this, id, background](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                if (background)
                    presenceUnavailable.insert(id);
                return;
            }
            presenceUnavailable.erase(id);
            const bool    active = doc.root()["presence"].str() == "active";
            const UserRef r      = s.findUser(id);
            if (r == kNoUser || s.user(r).active == active)
                return; // unchanged: no repaint per sweep
            s.user(r).active = active;
            s.usersChanged(r);
        },
        background ? Lane::Background : Lane::Normal
    );
}

// The 8 most recently active DM partners every
// round, plus a rotating window of 4 over the rest.
void SlackBackend::Read::pollDmPresence() {
    std::vector<std::pair<Ts, UserRef>> dms;
    for (ConvRef r = 0; r < s.conversationCount(); ++r) {
        const model::Conversation &c = s.conversation(r);
        if (c.kind == model::ConvKind::Dm && c.dmUser != kNoUser && c.dmUser != s.me)
            dms.emplace_back(c.latest, c.dmUser);
    }
    std::sort(dms.begin(), dms.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    const size_t n = dms.size(), hot = std::min<size_t>(kPresenceHot, n);
    for (size_t i = 0; i < hot; ++i)
        requestPresence(dms[i].second, true);
    const size_t rest = n - hot;
    if (rest == 0) {
        presenceIdx = 0;
        return;
    }
    presenceIdx %= rest;
    const size_t batch = std::min<size_t>(kPresenceRotate, rest);
    for (size_t i = 0; i < batch; ++i)
        requestPresence(dms[hot + (presenceIdx + i) % rest].second, true);
    presenceIdx = (presenceIdx + batch) % rest;
}

void SlackBackend::Read::refreshSelfPresence(std::function<void()> then) {
    // No "user": Slack answers the rich self snapshot (online, manual_away…).
    call(
        "users.getPresence",
        {},
        [this, then = std::move(then)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            if (err.empty()) {
                const json::Value  o    = doc.root();
                const SelfPresence prev = self;
                self.loaded             = true;
                self.active             = o["presence"].str() == "active";
                self.online             = o["online"].boolean();
                self.manualAway         = o["manual_away"].boolean();
                bool changed = !prev.loaded || prev.active != self.active ||
                               prev.online != self.online || prev.manualAway != self.manualAway;
                if (s.me != kNoUser && s.user(s.me).active != self.active) {
                    s.user(s.me).active = self.active;
                    changed             = true;
                }
                // My presence is a value the footer follows: a new
                // manual_away alone must reach it too.
                if (changed && s.me != kNoUser)
                    s.usersChanged(s.me);
            }
            if (then)
                then();
        }
    );
}

// ── Stars and saved items ───────────────────────────────────────────────────

// stars.list is the only read path for conversation stars. Only rows whose
// SERVER state moved since the previous snapshot change, which
// never fights a local toggle still on its way.
void SlackBackend::Read::refreshStarred() {
    lastStarred = now();
    if (starsUnavailable)
        return;
    auto ids = std::make_shared<std::vector<std::string>>();
    paginate(
        "stars.list",
        "limit=200",
        "items",
        [ids](const json::Value &items) { mapjson::starredConversationIds(items, *ids); },
        [this, ids](const std::string &err) {
            if (!err.empty()) {
                if (isMethodUnavailable(err))
                    starsUnavailable = true;
                return;
            }
            std::unordered_set<std::string> now(ids->begin(), ids->end());
            for (ConvRef r = 0; r < s.conversationCount(); ++r) {
                const std::string &id   = s.conversation(r).id;
                const bool         star = now.count(id) > 0;
                if (starsPrimed && star == (serverStars.count(id) > 0))
                    continue;
                if (s.conversation(r).starred != star)
                    s.updateConversation(r, [star](model::Conversation &c) { c.starred = star; });
            }
            serverStars = std::move(now);
            starsPrimed = true;
        }
    );
}

// users.channelSections.list (session tokens only): the custom sidebar
// sections. Never while an edit of ours is in flight: the list could predate
// it (sectionWriteDone lists again once they are all in).
void SlackBackend::Read::refreshSections() {
    lastSections = now();
    if (!session || sectionsUnavailable)
        return;
    if (sectionWrites > 0) {
        sectionsStale = true;
        return;
    }
    call(
        "users.channelSections.list",
        {},
        [this](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                if (isMethodUnavailable(err))
                    sectionsUnavailable = true;
                return;
            }
            if (sectionWrites > 0) {
                sectionsStale = true;
                return;
            }
            std::vector<model::Store::Section> list;
            systemSections.clear();
            mapjson::toSections(doc.root(), list, systemSections);
            s.setSections(std::move(list));
        }
    );
}

void SlackBackend::Read::sectionWriteDone(const std::string &err) {
    if (sectionWrites > 0)
        --sectionWrites;
    if (!err.empty() && err != "cancelled")
        LOG_WARN("slack", "users.channelSections: %s", err.c_str());
    // A refused edit is undone by Slack's own list.
    if (!err.empty())
        sectionsStale = true;
    if (sectionWrites == 0 && std::exchange(sectionsStale, false))
        refreshSections();
}

// The edits: shown in the Store at once (the local defaults), then sent; a
// refused one is set right by the list that follows it.
namespace {
void addParam(std::string &form, std::string_view k, std::string_view v) {
    if (!form.empty())
        form += '&';
    form += net::percentEncode(k);
    form += '=';
    form += net::percentEncode(v);
}

// [{"channel_section_id":…,"channel_ids":[…]}] (bulkUpdate's insert/remove).
std::string sectionChannels(const std::string &section, const std::string &channel) {
    json::Writer w;
    w.beginArray().beginObject();
    w.key("channel_section_id").value(section);
    w.key("channel_ids").beginArray().value(channel).endArray();
    w.endObject().endArray();
    return w.str();
}
} // namespace

void SlackBackend::sectionCall(
    std::string_view method, std::string form, std::function<void(const json::Document &)> onOk
) {
    ++_read->sectionWrites;
    api(method,
        std::move(form),
        [this, onOk = std::move(onOk)](const json::Document &doc, const std::string &err) {
            if (err.empty() && onOk)
                onOk(doc);
            if (!err.empty() && err != "cancelled" && onError)
                onError(i18n::arg(i18n::tr("Couldn't change the sidebar sections: %1"), err));
            if (err != "cancelled")
                _read->sectionWriteDone(err);
        });
}

void SlackBackend::createSection(std::string name, std::string emoji, ConvRef first) {
    std::string form;
    addParam(form, "name", name);
    if (!emoji.empty())
        addParam(form, "emoji", emoji);
    // The id is Slack's: the section shows once it answers.
    sectionCall(
        "users.channelSections.create",
        std::move(form),
        [this, name, emoji, first](const json::Document &doc) {
            const std::string id(doc.root()["channel_section_id"].str());
            if (id.empty())
                return;
            std::vector<model::Store::Section> all = _store.sections();
            all.insert(all.begin(), model::Store::Section{id, name, emoji, {}});
            _store.setSections(std::move(all));
            if (first != kNoConv)
                moveToSection(first, id);
        }
    );
}

void SlackBackend::updateSection(const std::string &id, std::string name, std::string emoji) {
    std::string form;
    addParam(form, "channel_section_id", id);
    addParam(form, "name", name);
    addParam(form, "emoji", emoji);
    Backend::updateSection(id, std::move(name), std::move(emoji));
    sectionCall("users.channelSections.update", std::move(form), {});
}

void SlackBackend::deleteSection(const std::string &id) {
    Backend::deleteSection(id);
    sectionCall("users.channelSections.delete", net::formEncode({{"channel_section_id", id}}), {});
}

void SlackBackend::moveToSection(ConvRef conv, const std::string &id) {
    if (conv >= _store.conversationCount())
        return;
    const model::Conversation   &cv      = _store.conversation(conv);
    const std::string            channel = cv.id;
    const model::Store::Section *from    = _store.sectionOf(channel);
    const std::string            old     = from ? from->id : std::string();
    if (old == id)
        return;
    // Out of a section it goes back to Slack's own one for its kind.
    std::string to = id;
    if (to.empty()) {
        const bool app = cv.kind == model::ConvKind::Dm && cv.dmUser != model::kNoUser &&
                         _store.user(cv.dmUser).bot;
        const auto it  = _read->systemSections.find(
            app             ? "recent_apps"
            : cv.isDirect() ? "direct_messages"
                            : "channels"
        );
        if (it != _read->systemSections.end())
            to = it->second;
    }
    Backend::moveToSection(conv, id);
    std::string form;
    if (!to.empty())
        addParam(form, "insert", sectionChannels(to, channel));
    if (!old.empty())
        addParam(form, "remove", sectionChannels(old, channel));
    sectionCall("users.channelSections.channels.bulkUpdate", std::move(form), {});
}

void SlackBackend::moveSection(const std::string &id, int step) {
    // Slack chains its sections: one step up puts this one before its upper
    // neighbour; one down puts the lower neighbour before it.
    const auto &all = _store.sections();
    size_t      i   = 0;
    while (i < all.size() && all[i].id != id)
        ++i;
    const int64_t j = int64_t(i) + step;
    if (i >= all.size() || j < 0 || j >= int64_t(all.size()))
        return;
    const std::string other = all[size_t(j)].id;
    const std::string first = step < 0 ? id : other, second = step < 0 ? other : id;
    Backend::moveSection(id, step);
    std::string form;
    addParam(form, "channel_section_id", first);
    addParam(form, "next_channel_section_id", second);
    sectionCall("users.channelSections.update", std::move(form), {});
}

// saved.list (session tokens only): "save for later"
// items and their due times, diffed against the previous snapshot as above.
void SlackBackend::Read::refreshSaved() {
    lastSaved = now();
    if (!session || savedUnavailable)
        return;
    call(
        "saved.list",
        "limit=50&filter=saved",
        [this](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                if (isMethodUnavailable(err))
                    savedUnavailable = true;
                return;
            }
            std::unordered_map<std::string, int64_t>      fresh;
            std::vector<std::tuple<ConvRef, Ts, int64_t>> refs; // + date_created
            for (const json::Value it : doc.root()["saved_items"]) {
                if (it["item_type"].str() != "message" || it["state"].str() == "completed" ||
                    it["is_archived"].boolean())
                    continue;
                const ConvRef c  = s.findConversation(it["item_id"].str());
                const Ts      ts = model::parseTs(it["ts"].str());
                if (c == kNoConv || !ts)
                    continue;
                fresh[threadKey(c, ts)] = it["date_due"].integer();
                refs.emplace_back(c, ts, it["date_created"].integer());
            }
            auto apply = [this](ConvRef c, Ts ts, bool saved, int64_t due, int64_t savedAt) {
                s.updateMessage(c, ts, [saved](model::Message &m) { m.saved = saved; });
                s.setReminderAt(c, ts, due);
                s.setSavedItem(c, ts, saved, due, savedAt); // the Saved messages page
            };
            for (const auto &[c, ts, created] : refs) {
                const std::string k  = threadKey(c, ts);
                const auto        it = serverSaved.find(k);
                if (!savedPrimed || it == serverSaved.end() || it->second != fresh[k])
                    apply(c, ts, true, fresh[k], created);
            }
            for (const auto &[k, due] : serverSaved)
                if (!fresh.count(k))
                    apply(keyConv(k), keyTs(k), false, 0, 0);
            serverSaved = std::move(fresh);
            savedPrimed = true;
            extrasChanged();
            armReminders();
        }
    );
}

// The messages still waiting to be posted. A session lists the web client's
// dated drafts (drafts.list; chat.scheduledMessages.list refuses its token),
// a token Slack's scheduled messages. Either way the list replaces the
// Store's, and a re-list follows each due time so a sent one goes.
void SlackBackend::Read::refreshScheduled() {
    lastScheduled = now();
    if (scheduledUnavailable)
        return;
    const bool        drafts = session;
    const char *const method = drafts ? "drafts.list" : "chat.scheduledMessages.list";
    call(
        method,
        drafts ? "is_active=true&limit=100" : "limit=100",
        [this, drafts, method](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                if (isMethodUnavailable(err)) {
                    LOG_INFO(
                        "slack", "%s: %s, not listing scheduled messages", method, err.c_str()
                    );
                    scheduledUnavailable = true;
                }
                return;
            }
            const auto secs = mapjson::epochSecs; // a number or a string
            std::vector<model::Store::ScheduledItem> items;
            for (const json::Value d : doc.root()[drafts ? "drafts" : "scheduled_messages"]) {
                model::Store::ScheduledItem it;
                if (drafts) {
                    // Only dated drafts are scheduled; a sent or deleted one
                    // may linger in the list.
                    it.at = secs(d["date_scheduled"]);
                    if (it.at <= 0 || d["is_sent"].boolean() || d["is_deleted"].boolean())
                        continue;
                    const json::Value dest = d["destinations"][0];
                    it.id                  = std::string(d["id"].str());
                    it.version             = std::string(d["last_updated_ts"].str());
                    it.conv                = s.findConversation(dest["channel_id"].str());
                    it.thread              = model::parseTs(dest["thread_ts"].str());
                    it.text                = mapjson::blocksToMrkdwn(d["blocks"]);
                } else {
                    // No thread in this list: a reply can't be sent early.
                    it.id          = std::string(d["id"].str());
                    it.at          = secs(d["post_at"]);
                    it.conv        = s.findConversation(d["channel_id"].str());
                    it.threadKnown = false;
                    it.text        = std::string(d["text"].str());
                }
                if (!it.id.empty() && it.conv != kNoConv)
                    items.push_back(std::move(it));
            }
            int64_t soonest = 0;
            for (const auto &it : items)
                if (!soonest || it.at < soonest)
                    soonest = it.at;
            s.setScheduled(std::move(items));
            if (soonest) {
                const uint64_t wake = ++scheduledWake;
                const int64_t  ms   = std::max<int64_t>(
                    (soonest - base::nowSecs()) * 1000 + kScheduledSentSlackMs,
                    kScheduledSentSlackMs
                );
                later(ms, [this, wake] {
                    if (wake == scheduledWake)
                        refreshScheduled();
                });
            }
        }
    );
}

void SlackBackend::refreshScheduled() {
    _read->refreshScheduled();
}

// ── Message reminders ───────────────────────────────────────────────────────

// Wake at the nearest due reminder not fired yet
// (at least 1 s out, so firing never runs inside a Store change; at most
// 6 h, then look again).
void SlackBackend::Read::armReminders() {
    if (reminderTimer)
        b._app.cancelTimer(reminderTimer);
    reminderTimer   = 0;
    int64_t nearest = 0;
    for (const model::Store::SavedItem &it : s.savedList())
        if (it.due > 0 && !it.fired && (!nearest || it.due < nearest))
            nearest = it.due;
    if (!nearest)
        return;
    const int64_t secs = std::clamp<int64_t>(nearest - base::nowSecs(), 1, kMaxReminderSleepSecs);
    reminderTimer      = b._app.addTimer(int(secs * 1000 / speed), false, [this] {
        reminderTimer = 0;
        fireDueReminders();
    });
}

// Each due reminder is marked fired (and saved)
// first, then announced — unless it is over a week late (a machine that was
// off), which goes quietly. The item stays listed.
void SlackBackend::Read::fireDueReminders() {
    const int64_t                              t = base::nowSecs();
    std::vector<std::pair<ConvRef, Ts>>        due;
    // A copy: marking one fired reorders the Store's list.
    const std::vector<model::Store::SavedItem> items = s.savedList();
    for (const model::Store::SavedItem &it : items) {
        if (it.due <= 0 || it.fired || it.due > t)
            continue;
        s.setReminderFired(it.conv, it.ts, true);
        if (t - it.due <= kMaxReminderLatenessSecs)
            due.emplace_back(it.conv, it.ts);
    }
    if (!due.empty())
        extrasChanged();
    armReminders();
    for (const auto &[c, ts] : due)
        announceReminder(c, ts);
}

// The notification wants the message's text; a
// reminder whose preview isn't known looks it up first (once).
void SlackBackend::Read::announceReminder(ConvRef c, Ts ts) {
    const model::Store::SavedItem *it = s.findSaved(c, ts);
    if (!it)
        return; // removed meanwhile
    auto emit = [this, c, ts] {
        if (s.findSaved(c, ts) && b.onReminderDue) {
            auto fn = b.onReminderDue;
            fn(c, ts);
        }
    };
    if (it->previewed) {
        emit();
        return;
    }
    if (const model::Message *m = s.findMessage(c, ts)) {
        s.setSavedPreview(c, ts, m);
        emit();
        return;
    }
    b.loadMessage(c, ts, [this, c, ts, emit](bool ok, model::Message m) {
        s.setSavedPreview(c, ts, ok ? &m : nullptr);
        emit();
    });
}

// ── Dead conversations ──────────────────────────────────────────────────────

void SlackBackend::Read::markDead(const std::string &id) {
    if (!id.empty() && dead.insert(id).second)
        extrasChanged();
}

void SlackBackend::Read::markAlive(const std::string &id) {
    if (dead.erase(id))
        extrasChanged();
}

// A fresh roster revives what it lists — a DM
// only once its peer is known and not deactivated.
void SlackBackend::Read::reconcileDead() {
    if (dead.empty())
        return;
    for (ConvRef r = 0; r < s.conversationCount(); ++r) {
        const model::Conversation &c = s.conversation(r);
        if (!c.member || !dead.count(c.id))
            continue;
        if (c.kind == model::ConvKind::Dm) {
            const model::User &u = s.user(c.dmUser);
            if (u.placeholder || u.deleted)
                continue;
        }
        markAlive(c.id);
    }
}

// ── Huddles ─────────────────────────────────────────────────────────────────

void SlackBackend::Read::applyHuddleRoom(ConvRef c, const json::Value &messages) {
    mapjson::HuddleRoom h;
    if (c >= s.conversationCount() || !mapjson::newestHuddleRoom(messages, s, h))
        return;
    b.setHuddle(c, h.active, std::move(h.link), std::move(h.participants));
}

void SlackBackend::setHuddle(
    ConvRef c, bool active, std::string link, std::vector<UserRef> participants
) {
    if (c >= _store.conversationCount())
        return;
    const model::Conversation &x = _store.conversation(c);
    if (x.huddleActive == active && x.huddleLink == link && x.huddleParticipants == participants)
        return;
    _store.updateConversation(c, [&](model::Conversation &y) {
        y.huddleActive       = active;
        y.huddleLink         = std::move(link);
        y.huddleParticipants = std::move(participants);
    });
}

// A thread I follow, or
// one I started.
bool SlackBackend::threadFollowed(ConvRef c, Ts root) const {
    if (_read->followed.count(threadKey(c, root)))
        return true;
    const model::Message *r = _store.findMessage(c, root);
    return r && _store.me != kNoUser && r->user == _store.me;
}

void SlackBackend::resolveUser(UserRef u) {
    _read->fetchUserIfNeeded(u);
}

void SlackBackend::requestPresence(UserRef u) {
    if (u != kNoUser && u != _store.me)
        _read->requestPresence(u, false);
}

// ── Mentioned channels ──────────────────────────────────────────────────────

void SlackBackend::resolveChannel(const std::string &id) {
    Read &r = *_read;
    if (id.empty() || _store.findConversation(id) != kNoConv || _store.channelName(id) ||
        !r.pendingChannels.insert(id).second)
        return;
    r.call(
        "conversations.info",
        net::formEncode({{"channel", id}}),
        [this, id](const json::Document &doc, const std::string &err) {
            _read->pendingChannels.erase(id); // a passing failure tries again later
            if (err == "channel_not_found") {
                _store.setChannelName(id, {});
                return;
            }
            const std::string_view name = doc.root()["channel"]["name"].str();
            if (err.empty() && !name.empty())
                _store.setChannelName(id, std::string(name));
        },
        Read::Lane::Background
    );
}

// ── History and threads ─────────────────────────────────────────────────────

// Maps a page (any order) oldest first. History pages are top-level lists:
// an "also sent to channel" reply sits there as a top-level copy. Local
// state the server doesn't echo (my saved flag) carries over.
std::vector<model::Message>
SlackBackend::Read::mapPage(ConvRef c, const json::Value &arr, bool topLevel) {
    std::vector<model::Message> page;
    page.reserve(arr.size());
    for (const json::Value v : arr) {
        model::Message m = mapjson::toMessage(v, s);
        if (!m.ts)
            continue;
        if (topLevel && m.isReply())
            m.threadTs = 0;
        if (const model::Message *old = s.findMessage(c, m.ts))
            m.saved = old->saved;
        else
            m.saved = serverSaved.count(threadKey(c, m.ts)) > 0;
        page.push_back(std::move(m));
    }
    model::sortByTs(page);
    return page;
}

void SlackBackend::loadHistory(ConvRef conv, Ts before, Done done) {
    const std::string &id = convId(conv);
    if (id.empty()) {
        _app.post([done, alive = _alive] {
            if (*alive && done)
                done(false, "channel_not_found");
        });
        return;
    }
    // Paging with next_cursor or `latest` (exclusive) gives the same page.
    std::string form = net::formEncode({{"channel", id}, {"limit", kHistoryLimit}});
    if (before)
        form.append(str::concat({"&latest=", model::formatTs(before), "&inclusive=false"}));
    else if (conv == _read->openConv)
        _read->lastFg = _read->now(); // the open chat's head: the poll waits its turn
    _read->call(
        "conversations.history",
        std::move(form),
        [this, conv, before, done = std::move(done)](
            const json::Document &doc, const std::string &err
        ) {
            if (!err.empty()) {
                LOG_WARN("slack", "conversations.history: %s", err.c_str());
                if (done)
                    done(false, err);
                return;
            }
            std::vector<model::Message> page = _read->mapPage(conv, doc.root()["messages"], true);
            _read->resolveAuthors(page);
            if (!before)
                _read->applyHuddleRoom(conv, doc.root()["messages"]);
            const bool more = doc.root()["has_more"].boolean();
            _store.addPage(conv, std::move(page));
            if (_store.conversation(conv).hasMoreBefore != more)
                _store.updateConversation(conv, [more](model::Conversation &c) {
                    c.hasMoreBefore = more;
                });
            if (done)
                done(true, {});
        }
    );
}

void SlackBackend::loadThread(ConvRef conv, Ts root, Done done) {
    _read->loadThreadPages(conv, root, false, std::move(done));
}

// conversations.replies, every page (oldest first; the first row is the
// root). live: a refresh of the open thread — replies that are new to us are
// delivered as live messages (badges, reply counters).
void SlackBackend::Read::loadThreadPages(ConvRef c, Ts root, bool live, Backend::Done done) {
    const std::string &id = b.convId(c);
    if (id.empty() || !root) {
        if (done)
            later(0, [done] { done(false, "thread_not_found"); });
        return;
    }
    struct Acc {
        std::vector<model::Message> replies;
        model::Message              rootMsg;
        bool                        haveRoot = false;
        std::vector<char>           parentIsMe; // per reply
    };
    auto acc = std::make_shared<Acc>();
    paginate(
        "conversations.replies",
        net::formEncode({{"channel", id}, {"ts", model::formatTs(root)}, {"limit", kHistoryLimit}}),
        "messages",
        [this, c, root, acc](const json::Value &arr) {
            const std::string &me = s.user(s.me).id;
            for (const json::Value v : arr) {
                model::Message m = mapjson::toMessage(v, s);
                if (m.ts == root) {
                    acc->rootMsg  = std::move(m);
                    acc->haveRoot = true;
                } else if (m.ts) {
                    m.threadTs = root;
                    if (const model::Message *old = s.findMessage(c, m.ts))
                        m.saved = old->saved;
                    acc->parentIsMe.push_back(!me.empty() && v["parent_user_id"].str() == me);
                    acc->replies.push_back(std::move(m));
                }
            }
        },
        [this, c, root, live, acc, done = std::move(done)](const std::string &err) {
            if (!err.empty()) {
                LOG_WARN("slack", "conversations.replies: %s", err.c_str());
                if (done)
                    done(false, err);
                return;
            }
            if (c >= s.conversationCount()) { // the Store was cleared meanwhile
                if (done)
                    done(false, "channel_not_found");
                return;
            }
            // The root stays in the channel list: refresh its thread fields.
            if (acc->haveRoot)
                s.updateMessage(c, root, [&](model::Message &r) {
                    r.replyCount  = acc->rootMsg.replyCount;
                    r.latestReply = acc->rootMsg.latestReply;
                    r.replyUsers  = acc->rootMsg.replyUsers;
                    r.threadTs    = root;
                });
            resolveAuthors(acc->replies);
            const std::vector<model::Message> *have = s.replies(c, root);
            // Every page was read, so the set is complete: a reply we hold
            // that the server no longer has was deleted elsewhere.
            if (have) {
                std::vector<Ts> fetched, gone;
                fetched.reserve(acc->replies.size());
                for (const model::Message &x : acc->replies)
                    fetched.push_back(x.ts);
                std::sort(fetched.begin(), fetched.end());
                for (const model::Message &m : *have)
                    if (!m.pending && !std::binary_search(fetched.begin(), fetched.end(), m.ts))
                        gone.push_back(m.ts);
                for (Ts ts : gone)
                    s.removeMessage(c, ts);
                have = s.replies(c, root);
            }
            if (live && have) {
                const Ts newest = have->empty() ? 0 : have->back().ts;
                for (size_t i = 0; i < acc->replies.size(); ++i)
                    if (acc->replies[i].ts > newest)
                        inject(c, acc->replies[i].clone(), acc->parentIsMe[i]);
            }
            if (!acc->replies.empty())
                s.addPage(c, std::move(acc->replies));
            if (done)
                done(true, {});
        },
        kMaxThreadPages
    );
}

void SlackBackend::setActiveConversation(ConvRef conv, Ts thread) {
    Read &r = *_read;
    if (r.openConv != conv) {
        // The newly opened chat is polled on the next tick (the shared
        // cooldown resets, so hopping between chats can't starve one).
        r.lastFg = 0;
    }
    r.openConv   = conv;
    r.openThread = thread;
    if (!r.cache || conv == kNoConv || conv >= _store.conversationCount())
        return;
    // Opening a conversation: the cached messages show at once, the network
    // page is merged in when it comes (which is this open's head fetch: the
    // poll waits its turn); and this is the chat to reopen.
    r.cache->setLastConversation(conv);
    if (const Ts newest = r.cache->loadMessages(conv)) {
        r.lastFg = r.now();
        r.refreshCachedHead(conv, newest);
    }
}

// ── The workspace cache ─────────────────────────────────────────────────────

bool SlackBackend::openCache(std::string dir) {
    Read &r = *_read;
    if (r.cache)
        return false;
    r.cache             = std::make_unique<cache::WorkspaceCache>(_app, _store, std::move(dir));
    r.cache->saveExtras = [&r](json::Writer &w) { r.saveExtras(w); };
    json::Document meta;
    const bool     warm = r.cache->load(&meta);
    if (warm)
        r.loadExtras(meta.root()["x"]);
    // A cache written before toUser flagged them still says is_bot=false.
    for (const char *id : {"USLACKBOT", "USLACK"})
        if (const UserRef u = _store.findUser(id); u != kNoUser)
            _store.user(u).bot = true;
    return warm;
}

void SlackBackend::closeCache(bool keep) {
    if (_read->cache)
        _read->cache->close(keep);
}

ConvRef SlackBackend::lastConversation() const {
    return _read->cache ? _store.findConversation(_read->cache->lastConversation()) : kNoConv;
}

bool SlackBackend::connecting() const {
    return _read->connectPending > 0;
}

// A saved item's preview as meta.json keeps it: the card shows one line, so
// a long message is cut (at a character, never inside a <@U…> token).
namespace {
std::string previewText(const std::string &text) {
    constexpr size_t kMax = 600;
    if (text.size() <= kMax)
        return text;
    size_t       n    = utf8::truncateAt(text, kMax);
    const size_t open = text.rfind('<', n);
    if (open != std::string::npos && text.find('>', open) >= n)
        n = open;
    return text.substr(0, n);
}
} // namespace

// What only this backend knows, kept in meta.json's "x": the saved items
// (the saved flag of a message not loaded yet, and
// what to unsave when the server's list drops it), the threads I follow
// (a reply right after a start still badges as a followed-thread one), when
// each off-roster user was last re-probed, and the DM activity sweep.
void SlackBackend::Read::saveExtras(json::Writer &w) {
    const auto conv = [this](const std::string &k) -> const std::string & {
        return b.convId(keyConv(k));
    };
    w.key("saved").beginArray();
    for (const auto &[k, due] : serverSaved)
        if (!conv(k).empty()) {
            const model::Store::SavedItem *it = s.findSaved(keyConv(k), keyTs(k));
            w.beginArray().value(conv(k)).value(int64_t(keyTs(k))).value(due);
            w.value(it ? it->savedAt : int64_t(0)).value(it && it->fired);
            // Reminder previews: what the message said, so the Saved
            // page and a due reminder show it without fetching it again.
            if (it && it->previewed &&
                (!it->text.empty() || it->author != kNoUser || !it->botName.empty())) {
                w.value(previewText(it->text))
                    .value(it->author != kNoUser ? s.user(it->author).id : std::string())
                    .value(int64_t(it->thread))
                    .value(it->botName)
                    .value(it->botAvatar);
            }
            w.endArray();
        }
    w.endArray().key("followed").beginArray();
    for (const std::string &k : followed)
        if (!conv(k).empty())
            w.beginArray().value(conv(k)).value(int64_t(keyTs(k))).endArray();
    // Probe times run on the monotonic clock (now()); disk keeps unix secs.
    const int64_t t = now(), wall = base::nowSecs();
    w.endArray().key("probed").beginArray();
    for (const auto &[id, at] : probedAt)
        w.beginArray().value(id).value(wall - (t - at) / (1000 * speed)).endArray();
    w.endArray().key("sweep").value(sweepAt);
    w.key("users").value(usersAt);
    if (savedNames != mapjson::kNoPref)
        w.key("names").value(int64_t(savedNames));
    w.key("dead").beginArray();
    for (const std::string &id : dead)
        w.value(id);
    w.endArray();
}

void SlackBackend::Read::loadExtras(const json::Value &x) {
    for (const json::Value v : x["saved"])
        if (const ConvRef c = s.findConversation(v[0].str()); c != kNoConv && v[1].integer()) {
            serverSaved[threadKey(c, v[1].integer())] = v[2].integer();
            // Saved messages lists it before the first saved.list answers.
            s.setSavedItem(c, v[1].integer(), true, v[2].integer(), v[3].integer());
            if (v[4].boolean())
                s.setReminderFired(c, v[1].integer(), true);
            if (v[5].isString()) {
                model::Message m;
                m.ts       = v[1].integer();
                m.threadTs = v[7].integer();
                m.text     = v[5].str();
                if (!v[6].str().empty())
                    m.user = s.internUser(v[6].str());
                if (!v[8].str().empty() || !v[9].str().empty()) {
                    m.extra            = std::make_unique<model::MessageExtras>();
                    m.extra->botName   = v[8].str();
                    m.extra->botAvatar = v[9].str();
                }
                s.setSavedPreview(c, m.ts, &m);
            }
        }
    for (const json::Value v : x["followed"])
        if (const ConvRef c = s.findConversation(v[0].str()); c != kNoConv && v[1].integer())
            followed.insert(threadKey(c, v[1].integer()));
    const int64_t t = now(), wall = base::nowSecs();
    for (const json::Value v : x["probed"])
        if (!v[0].str().empty())
            probedAt[std::string(v[0].str())] =
                t - std::max<int64_t>(wall - v[1].integer(), 0) * 1000 * speed;
    sweepAt = x["sweep"].integer();
    usersAt = x["users"].integer();
    if (const int64_t n = x["names"].integer(mapjson::kNoPref); n == 0 || n == 1) {
        savedNames = int(n);
        applyNames();
    }
    for (const json::Value v : x["dead"])
        if (!v.str().empty())
            dead.emplace(v.str());
    armReminders();
}

// The head page of a conversation shown from the cache: what the server no longer has inside the
// page's span was deleted while we were away, and a cached run that doesn't reach the page is cut
// loose — kept, it would leave a hole that paging from its oldest message could never fill, so it
// goes and paging starts from the head. Only messages that came from the cache are judged (ts at
// most cachedNewest); anything that arrived live since is left alone.
void SlackBackend::Read::refreshCachedHead(ConvRef c, Ts cachedNewest) {
    call(
        "conversations.history",
        net::formEncode({{"channel", b.convId(c)}, {"limit", kHistoryLimit}}),
        [this, c, cachedNewest](const json::Document &doc, const std::string &err) {
            if (!err.empty() || c >= s.conversationCount())
                return;
            std::vector<model::Message> page = mapPage(c, doc.root()["messages"], true);
            const bool                  more = doc.root()["has_more"].boolean();
            applyHuddleRoom(c, doc.root()["messages"]);
            const auto inPage = [&page](Ts ts) {
                const auto it = std::lower_bound(
                    page.begin(), page.end(), ts, [](const model::Message &m, Ts t) {
                        return m.ts < t;
                    }
                );
                return it != page.end() && it->ts == ts;
            };
            const Ts lo      = page.empty() ? INT64_MAX : page.front().ts;
            bool     overlap = false;
            for (const model::Message &m : s.conversation(c).messages)
                overlap = overlap || (!m.pending && m.ts <= cachedNewest && inPage(m.ts));
            const bool      cut = more && !page.empty() && !overlap;
            std::vector<Ts> gone;
            for (const model::Message &m : s.conversation(c).messages) {
                if (m.pending || m.ts > cachedNewest || inPage(m.ts))
                    continue;
                if (m.ts >= lo || !more || cut)
                    gone.push_back(m.ts);
            }
            for (Ts ts : gone)
                s.removeMessage(c, ts);
            resolveAuthors(page);
            if (!page.empty()) {
                Ts &base = baselineOf(c);
                base     = std::max(base, page.back().ts);
            }
            s.addPage(c, std::move(page));
            if ((cut || !more) && s.conversation(c).hasMoreBefore != more)
                s.updateConversation(c, [more](model::Conversation &x) { x.hasMoreBefore = more; });
        }
    );
}

// ── Polling ─────────────────────────────────────────────────────────────────

void SlackBackend::Read::tick() {
    if (b._authLost)
        return;
    const int64_t t = now();
    // (1) The socket (if any) is still connected: a no-op while healthy.
    b.realtimeTick();
    const bool    push      = b.hasRealtimePush();
    // (0) No push: reload the roster ourselves (new DMs and channels). A
    // push workspace hears of them (and backfills on a reconnect).
    const int64_t rosterGap = countsDisabled ? kRosterReloadGapMs : kRosterFallbackGapMs;
    if (!push &&
        (t - lastRoster >= rosterGap || (rosterWanted && t - lastRoster >= kRosterSoonestGapMs))) {
        lastRoster   = t;
        rosterWanted = false;
        loadConversations([](const std::string &) {});
    }
    pruneThreadState();
    // (0b) One request reports every conversation's activity.
    if (!push && !countsDisabled && t - lastCounts >= kCountsPollGapMs) {
        lastCounts = t;
        pollUnreadCounts();
    }
    // (0c) Thread replies move no channel's `latest`: only the feed sees them.
    if (!push && session && !threadsUnavailable && t - lastThreads >= kThreadsPollGapMs) {
        lastThreads = t;
        pollThreadReplies();
    }
    // (0d) Watched threads (agent thread links): the backstop polls.
    if (!watches.empty())
        pollWatches();
    // (2) The open chat; hidden, client.counts brings it forward when it
    // moves (applyActivity).
    const int64_t fgGap = !session ? 60'000 : visible ? 5'000 : kHiddenOpenPollGapMs;
    if (openConv != kNoConv && t - lastFg >= fgGap) {
        lastFg = t;
        pollConversation(openConv, true);
    }
    // (2b) Renames and avatars: a day after the last users.list.
    if (t - lastUsers >= kUsersRefreshGapMs) {
        lastUsers = t;
        // Names are there already: behind whatever the user waits for.
        loadUsers([](const std::string &) {}, usersLoaded ? Lane::Background : Lane::Normal);
        loadUsergroups();
    } else if (!usersLoaded && !usersLoading && t - lastUsers >= kRosterReloadGapMs) {
        // users.list never came (a flaky start outlasted its retries):
        // without it nobody gets a name, so again at the roster cadence.
        lastUsers = t;
        loadUsers([](const std::string &) {});
    }
    if (t - lastSaved >= kSavedGapMs)
        refreshSaved();
    if (t - lastScheduled >= kScheduledGapMs)
        refreshScheduled();
    if (t - lastStarred >= kStarredGapMs)
        refreshStarred();
    if (t - lastSections >= kStarredGapMs)
        refreshSections();
    // Others' presence draws only in the window: none while it is hidden
    // (showing it again asks at once: setWindowVisible).
    if (visible && t - lastPresence >= kPresencePollGapMs) {
        lastPresence = t;
        pollDmPresence();
    }
    if (t - lastSelf >= kSelfPresenceGapMs) {
        lastSelf = t;
        refreshSelfPresence();
    }
    // Background rotation over the other member conversations.
    if (t - lastBg >= kBackgroundPollGapMs) {
        lastBg = t;
        if (const ConvRef bg = nextBackgroundTarget(); bg != kNoConv)
            pollConversation(bg, false);
    }
}

// Most recently active first, but a cursor walks the whole list so quiet
// channels get their turn too.
ConvRef SlackBackend::Read::nextBackgroundTarget() {
    std::vector<ConvRef> cands;
    for (ConvRef r = 0; r < s.conversationCount(); ++r)
        if (s.conversation(r).member && r != openConv)
            cands.push_back(r);
    if (cands.empty())
        return kNoConv;
    std::sort(cands.begin(), cands.end(), [this](ConvRef a, ConvRef b2) {
        return s.conversation(a).latest > s.conversation(b2).latest;
    });
    bgIdx %= cands.size();
    return cands[bgIdx++];
}

void SlackBackend::Read::pollUnreadCounts() {
    if (countsDisabled || countsUnavailable)
        return;
    call("client.counts", {}, [this](const json::Document &doc, const std::string &err) {
        if (err.empty()) {
            countsFailures = 0;
            applyActivity(mapjson::toCounts(doc.root()), true);
            // A followed thread has unread replies: maybe a watched one.
            const bool unread = doc.root()["threads"]["has_unreads"].boolean();
            if (unread && !threadsUnread)
                for (Watch &w : watches)
                    if (w.due)
                        w.due = std::min(w.due, now());
            threadsUnread = unread;
            return;
        }
        if (isMethodUnavailable(err))
            countsUnavailable = true;
        // A blip must not cost the mechanism; a few failures in a row do.
        if (countsUnavailable || ++countsFailures >= kCountsFailureLimit) {
            LOG_WARN(
                "slack",
                "client.counts unavailable (%s): falling back to the roster diff",
                err.c_str()
            );
            countsDisabled = true;
            activity.clear();
            activityPrimed = false;
        }
    });
}

// An activity snapshot: fold the cursors in (upward only), seed the
// badges of conversations seen for the first time, and poll the ones that
// moved since the previous snapshot.
void SlackBackend::Read::applyActivity(
    const std::vector<mapjson::Counts> &snapshot, bool fromCounts
) {
    if (snapshot.empty())
        return;
    const bool priming = !activityPrimed;
    activityPrimed     = true;
    if (fromCounts) {
        // The roster follows the snapshot: an id it lacks (a new DM or
        // channel, asked once), or a member one the snapshot dropped (left or
        // closed elsewhere), brings a reload.
        std::unordered_set<std::string> listed;
        for (const mapjson::Counts &c : snapshot) {
            listed.insert(c.id);
            if (s.findConversation(c.id) == kNoConv && !dead.count(c.id) &&
                rosterAsked.insert(c.id).second)
                rosterWanted = true;
        }
        for (auto it = activity.begin(); it != activity.end();) {
            if (listed.count(it->first)) {
                ++it;
                continue;
            }
            const ConvRef r = s.findConversation(it->first);
            if (r != kNoConv && s.conversation(r).member)
                rosterWanted = true;
            it = activity.erase(it);
        }
    }
    struct Moved {
        ConvRef         conv;
        mapjson::Counts prev, now;
    };
    std::vector<Moved> moved;
    for (const mapjson::Counts &c : snapshot) {
        const auto    it        = activity.find(c.id);
        const bool    firstSeen = priming || it == activity.end();
        const ConvRef r         = s.findConversation(c.id);
        if (r != kNoConv) {
            const model::Conversation &x      = s.conversation(r);
            uint32_t                   unread = x.unread, mentions = x.mentions;
            if (firstSeen && x.member && r != openConv) {
                // Every DM unread is a red-badge "mention"; a muted
                // conversation badges only explicit mentions.
                const bool     muted = x.muted || x.notify == model::NotifyLevel::Nothing;
                const uint32_t m     = x.isDirect() ? std::max(c.unread, c.mentions) : c.mentions;
                unread               = std::max(unread, muted ? m : std::max(c.unread, m));
                mentions             = std::max(mentions, m);
            }
            // Read up on the server (another client; or a badge cached from
            // before) with nothing newer here: no badge either.
            if (fromCounts && c.unread == 0 && c.lastRead >= c.latest && x.latest <= c.latest)
                unread = mentions = 0;
            if (c.latest > x.latest || c.lastRead > x.lastRead || unread != x.unread ||
                mentions != x.mentions)
                s.updateConversation(r, [&](model::Conversation &y) {
                    y.latest   = std::max(y.latest, c.latest);
                    y.lastRead = std::max(y.lastRead, c.lastRead);
                    y.unread   = unread;
                    y.mentions = mentions;
                });
        }
        if (firstSeen) {
            activity[c.id] = c;
            continue;
        }
        const mapjson::Counts prev = it->second;
        if (c.latest > prev.latest || c.unread > prev.unread || c.mentions > prev.mentions)
            moved.push_back({r, prev, c});
        else
            it->second = c;
    }
    std::sort(moved.begin(), moved.end(), [](const Moved &a, const Moved &b2) {
        return a.now.latest > b2.now.latest;
    });
    int budget = kMaxDiffPolls;
    for (const Moved &m : moved) {
        const model::Conversation *c = m.conv == kNoConv ? nullptr : &s.conversation(m.conv);
        const bool mutedQuiet = c && (c->muted || c->notify == model::NotifyLevel::Nothing) &&
                                m.now.mentions <= m.prev.mentions;
        // The open chat has its own poll; hidden, that one is slow, so it
        // comes forward instead.
        if (!c || !c->member || mutedQuiet || (m.conv == openConv && visible)) {
            activity[m.now.id] = m.now;
            continue;
        }
        if (budget-- <= 0)
            break; // left stale on purpose: still "moved" next time
        activity[m.now.id] = m.now;
        if (m.conv == openConv) {
            lastFg = now();
            pollConversation(m.conv, true);
        } else {
            pollConversation(m.conv, false, m.prev.latest);
        }
    }
}

Ts &SlackBackend::Read::baselineOf(ConvRef c) {
    if (pollBaseline.size() <= c)
        pollBaseline.resize(c + 1, 0);
    return pollBaseline[c];
}

// The head page of a conversation; what is
// newer than the baseline arrives as live messages. Foreground (the open
// chat) also merges the whole page (edits, reactions, reply counts, a buried
// gap), detects deletions and refreshes an open thread whose root moved.
void SlackBackend::Read::pollConversation(ConvRef c, bool foreground, Ts hint) {
    if (c >= s.conversationCount())
        return;
    Ts lastKnown = c < pollBaseline.size() ? pollBaseline[c] : 0;
    if (!lastKnown)
        lastKnown = hint;
    if (!lastKnown)
        lastKnown = s.conversation(c).latest;
    // Never scanned: record the head without injecting (that page is old news).
    const bool     priming  = !lastKnown;
    const uint64_t revision = ++pollRevision;
    // The newest message held as the request leaves: anything newer arrived
    // while it was out, so the page can't speak for it.
    Ts             held     = 0;
    for (const model::Message &m : s.conversation(c).messages)
        if (!m.pending)
            held = std::max(held, m.ts);
    call(
        "conversations.history",
        net::formEncode({{"channel", b.convId(c)}, {"limit", kHistoryLimit}}),
        [this, c, foreground, lastKnown, priming, revision, held](
            const json::Document &doc, const std::string &err
        ) {
            if (!err.empty() || c >= s.conversationCount())
                return;
            // Stale intent: the open chat moved on (foreground), or this one
            // became the open chat (the foreground poll covers it now).
            if (foreground ? openConv != c : openConv == c)
                return;
            if (completedPoll.size() <= c)
                completedPoll.resize(c + 1, 0);
            if (revision < completedPoll[c])
                return;
            completedPoll[c]                 = revision;
            std::vector<model::Message> page = mapPage(c, doc.root()["messages"], true);
            applyHuddleRoom(c, doc.root()["messages"]);
            if (!page.empty())
                baselineOf(c) = page.back().ts;
            if (priming && !foreground)
                return;
            resolveAuthors(page);
            bool missed = false;
            if (!priming)
                for (const model::Message &m : page)
                    if (m.ts > lastKnown)
                        missed = inject(c, m.clone(), false) || missed;
            // The socket should have pushed that: it is compromised and
            // re-established (throttled). On a poll-only workspace
            // this poll IS the delivery, not a miss.
            if (missed && b.hasRealtimePush())
                b.realtimeMissed();
            if (!foreground || page.empty())
                return;
            // Deleted elsewhere: held from the page's oldest on, but not on
            // it (the page is sorted: mapPage). Judged against what we hold,
            // not an earlier page, so a deletion the socket never brought (or
            // made while another chat was open) goes on the first poll too.
            const Ts        oldest = page.front().ts;
            std::vector<Ts> gone;
            for (const model::Message &m : s.conversation(c).messages) {
                if (m.pending || m.ts < oldest || m.ts > held)
                    continue;
                const auto at = std::lower_bound(
                    page.begin(), page.end(), m.ts, [](const model::Message &x, Ts t) {
                        return x.ts < t;
                    }
                );
                if (at == page.end() || at->ts != m.ts)
                    gone.push_back(m.ts);
            }
            for (Ts ts : gone)
                s.removeMessage(c, ts);
            // An open thread: its root's latest_reply moving is the cue to
            // re-read the replies. A root off the page is asked for alone.
            if (openThread) {
                const auto it = std::lower_bound(
                    page.begin(), page.end(), openThread, [](const model::Message &m, Ts t) {
                        return m.ts < t;
                    }
                );
                const model::Message *root =
                    it != page.end() && it->ts == openThread ? &*it : nullptr;
                const Ts newest = newestHeldReply(c, openThread);
                if (!root)
                    checkThreadTip(c, openThread);
                else if (
                    (root->latestReply ? root->latestReply : root->ts) !=
                    (newest ? newest : root->ts)
                )
                    loadThreadPages(c, openThread, true, nullptr);
            }
            s.addPage(c, std::move(page));
        }
    );
}

// Thread replies (subscriptions.thread.getView, session tokens):
// the one endpoint that reports thread replies workspace-wide.
void SlackBackend::Read::pollThreadReplies() {
    if (threadsViaSubs) {
        pollThreadSubs();
        return;
    }
    call(
        "subscriptions.thread.getView",
        threadsForm("limit=10&priority_mode=all"),
        [this, sentTeam = b._threadsTeamId](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                if (isFeedRefusal(err)) {
                    LOG_WARN("slack", "subscriptions.thread.getView: %s", err.c_str());
                    threadsRefused(err, sentTeam, false); // the retry rides the next tick
                }
                return;
            }
            const bool priming = !threadsPrimed;
            threadsPrimed      = true;
            int injected       = 0;
            for (const json::Value t : doc.root()["threads"]) {
                mapjson::FeedThread ft;
                if (!mapjson::toFeedThread(t, s, ft))
                    continue;
                const std::string key    = threadKey(ft.conv, ft.root);
                const Ts          newest = ft.replies.empty() ? 0 : ft.replies.back().ts;
                Ts                baseline;
                if (feedThread(key, ft.conv, ft.root, newest, ft.lastRead, priming, baseline))
                    announceReplies(
                        key, ft.conv, baseline, newest, ft.replies, ft.parentIsMe, injected
                    );
            }
            // "All clear" from the server (the count is threads, not replies).
            if (doc.root()["total_unread_replies"].integer() == 0)
                unreadThreads.clear();
            publishUnreadThreads();
        }
    );
}

// One thread of either feed: followed, its watch pulled, the Threads
// entry's unread state; baseline: where its news starts. false: none to
// announce (it has nothing past its read cursor yet).
bool SlackBackend::Read::feedThread(
    const std::string &key, ConvRef c, Ts root, Ts newest, Ts lastRead, bool priming, Ts &baseline
) {
    follow(key); // the feed IS the subscription list
    pullWatch(c, root, newest);
    // The Threads entry: the feed's own
    // read cursor or mine, whichever is further; the first page
    // after a start restores it.
    const Ts floor = std::max(lastRead, threadReadFloor[key]);
    if (newest > floor && !s.threadMuted(c, root))
        unreadThreads[key] = newest;
    else
        unreadThreads.erase(key);
    baseline = 0;
    if (const auto it = threadBaseline.find(key); it != threadBaseline.end())
        baseline = it->second;
    if (!baseline) {
        // First page of the run primes; a thread that shows up
        // later uses its own read cursor as the floor.
        baseline = priming ? newest : lastRead;
        if (!baseline) {
            threadBaseline[key] = newest;
            return false;
        }
    }
    return true;
}

// The replies past a thread's baseline are delivered (at most the newest
// few of a backlog, kMaxThreadInjects a round); the baseline moves to
// newest.
void SlackBackend::Read::announceReplies(
    const std::string           &key,
    ConvRef                      c,
    Ts                           baseline,
    Ts                           newest,
    std::vector<model::Message> &replies,
    const std::vector<char>     &parentIsMe,
    int                         &injected
) {
    size_t unseen = 0;
    for (const model::Message &r : replies)
        unseen += r.ts > baseline;
    if (unseen > size_t(kMaxThreadBacklog))
        baseline = replies[replies.size() - kMaxThreadBacklog - 1].ts;
    Ts reached = newest;
    for (size_t i = 0; i < replies.size(); ++i) {
        const Ts ts = replies[i].ts;
        if (ts <= baseline)
            continue;
        if (injected >= kMaxThreadInjects) {
            reached = baseline; // the rest drains next tick
            break;
        }
        if (!s.findMessage(c, ts)) {
            inject(c, std::move(replies[i]), parentIsMe[i]);
            ++injected;
        }
        baseline = std::max(baseline, ts);
    }
    threadBaseline[key] = std::max(baseline, reached);
}

// The getTimestamps feed (where getView is refused) carries no messages:
// only a thread whose newest message moved past its baseline costs a
// conversations.replies for the news.
void SlackBackend::Read::pollThreadSubs() {
    call(
        "subscriptions.thread.getTimestamps",
        threadsForm(kSubsForm),
        [this, sentTeam = b._threadsTeamId](const json::Document &doc, const std::string &err) {
            if (!err.empty()) {
                if (isFeedRefusal(err)) {
                    LOG_WARN("slack", "subscriptions.thread.getTimestamps: %s", err.c_str());
                    threadsRefused(err, sentTeam, true);
                }
                return;
            }
            const bool priming = !threadsPrimed;
            threadsPrimed      = true;
            auto injected      = std::make_shared<int>(0);
            for (const json::Value v : doc.root()["subscriptions"]) {
                mapjson::ThreadSub t;
                if (!mapjson::toThreadSub(v, s, t))
                    continue;
                const std::string key = threadKey(t.conv, t.root);
                Ts                baseline;
                if (!feedThread(key, t.conv, t.root, t.latest, t.lastRead, priming, baseline))
                    continue;
                if (t.latest <= baseline) {
                    threadBaseline[key] = baseline;
                    continue;
                }
                call(
                    "conversations.replies",
                    net::formEncode(
                        {{"channel", b.convId(t.conv)},
                         {"ts", model::formatTs(t.root)},
                         {"oldest", model::formatTs(baseline)},
                         {"limit", kHistoryLimit}}
                    ),
                    [this, key, t, baseline, injected](
                        const json::Document &doc, const std::string &err
                    ) {
                        if (!err.empty() || t.conv >= s.conversationCount())
                            return; // the next tick asks again
                        const std::string          &me = s.user(s.me).id;
                        std::vector<model::Message> replies;
                        std::vector<char>           parentIsMe;
                        for (const json::Value r : doc.root()["messages"]) {
                            model::Message m = mapjson::toMessage(r, s);
                            if (!m.ts || m.ts == t.root || m.ts <= baseline)
                                continue;
                            m.threadTs = t.root;
                            parentIsMe.push_back(!me.empty() && r["parent_user_id"].str() == me);
                            replies.push_back(std::move(m));
                        }
                        // Read or announced meanwhile: from there.
                        const Ts from = std::max(baseline, threadBaseline[key]);
                        announceReplies(
                            key, t.conv, from, t.latest, replies, parentIsMe, *injected
                        );
                    }
                );
            }
            publishUnreadThreads();
        }
    );
}

// A live reply in a thread I follow (or one
// that mentions me) lights the Threads entry until I read up to it.
void SlackBackend::Read::noteUnreadThreadReply(ConvRef c, Ts root, Ts ts) {
    const std::string key = threadKey(c, root);
    if (ts <= threadReadFloor[key])
        return;
    Ts &newest = unreadThreads[key];
    newest     = std::max(newest, ts);
    publishUnreadThreads();
}

// The floor (and the poll's baseline) move up to
// upTo; the thread is read once its newest unread reply is.
void SlackBackend::Read::threadRead(ConvRef c, Ts root, Ts upTo) {
    const std::string key = threadKey(c, root);
    Ts               &f   = threadReadFloor[key];
    f                     = std::max(f, upTo);
    Ts &base              = threadBaseline[key];
    base                  = std::max(base, upTo);
    if (const auto it = unreadThreads.find(key); it != unreadThreads.end() && it->second <= upTo)
        unreadThreads.erase(it);
    publishUnreadThreads();
}

void SlackBackend::Read::publishUnreadThreads() {
    int n = 0;
    for (const auto &[k, ts] : unreadThreads)
        n += !s.threadMuted(keyConv(k), keyTs(k));
    s.setUnreadThreads(n);
}

// One page of the
// Threads page's feed (the endpoint the poll above reads).
void SlackBackend::loadThreadsView(std::string cursor, ThreadsViewDone done) {
    Read *r = _read;
    if (!r->session || r->threadsUnavailable) {
        r->later(0, [r, done = std::move(done)] {
            ThreadsView failed;
            failed.error = r->threadsError;
            if (done)
                done(false, std::move(failed));
        });
        return;
    }
    if (r->threadsViaSubs) {
        r->threadSubsPage(std::move(cursor), std::move(done));
        return;
    }
    std::string form = "limit=10&priority_mode=all"; // every followed thread
    // Continuation is the previous answer's max_ts (no next_cursor here).
    if (!cursor.empty())
        form += "&" + net::formEncode({{"current_ts", cursor}});
    r->call(
        "subscriptions.thread.getView",
        r->threadsForm(std::move(form)),
        [this, r, cursor, sentTeam = _threadsTeamId, done = std::move(done)](
            const json::Document &doc, const std::string &err
        ) mutable {
            if (err == "cancelled")
                return;
            if (!err.empty()) {
                LOG_WARN("slack", "subscriptions.thread.getView: %s", err.c_str());
                // Given up on only for a method-level refusal, never a transport failure.
                // A later page of getView can't go on in getTimestamps.
                if (r->threadsRefused(err, sentTeam, false) &&
                    (cursor.empty() || !r->threadsViaSubs)) {
                    loadThreadsView(std::move(cursor), std::move(done));
                    return;
                }
                ThreadsView failed;
                failed.error = err;
                if (done)
                    done(false, std::move(failed));
                return;
            }
            model::Store     &s    = r->s;
            const json::Value root = doc.root();
            ThreadsView       page;
            page.totalUnreadReplies = int(root["total_unread_replies"].integer());
            page.hasMore            = root["has_more"].boolean();
            page.nextCursor         = std::string(root["max_ts"].str());
            for (const json::Value t : root["threads"]) {
                mapjson::FeedThread ft;
                if (!mapjson::toFeedThread(t, s, ft))
                    continue;
                FollowedThread f;
                f.conv          = ft.conv;
                f.root          = mapjson::toMessage(t["root_msg"], s);
                f.lastRead      = ft.lastRead;
                f.latestReplies = std::move(ft.replies);
                if (!f.root.ts)
                    continue;
                page.threads.push_back(std::move(f));
            }
            if (done)
                done(true, std::move(page));
        }
    );
}

// A page of the getTimestamps feed: the threads (no messages), then one
// card each from loadSubThread, in the feed's order.
void SlackBackend::Read::threadSubsPage(std::string cursor, ThreadsViewDone done) {
    std::string form = kSubsForm;
    if (!cursor.empty())
        form += "&" + net::formEncode({{"cursor", cursor}});
    call(
        "subscriptions.thread.getTimestamps",
        threadsForm(std::move(form)),
        [this, cursor, sentTeam = b._threadsTeamId, done = std::move(done)](
            const json::Document &doc, const std::string &err
        ) mutable {
            if (err == "cancelled")
                return;
            if (!err.empty()) {
                LOG_WARN("slack", "subscriptions.thread.getTimestamps: %s", err.c_str());
                if (threadsRefused(err, sentTeam, true)) {
                    b.loadThreadsView(std::move(cursor), std::move(done));
                    return;
                }
                ThreadsView failed;
                failed.error = err;
                if (done)
                    done(false, std::move(failed));
                return;
            }
            struct Acc {
                ThreadsView     page;
                size_t          pending = 0;
                ThreadsViewDone done;
            };
            auto acc  = std::make_shared<Acc>();
            acc->done = std::move(done);
            acc->page.nextCursor =
                std::string(doc.root()["response_metadata"]["next_cursor"].str());
            acc->page.hasMore = !acc->page.nextCursor.empty();
            std::vector<mapjson::ThreadSub> subs;
            for (const json::Value v : doc.root()["subscriptions"]) {
                mapjson::ThreadSub t;
                if (mapjson::toThreadSub(v, s, t))
                    subs.push_back(t);
            }
            if (subs.empty()) {
                if (acc->done)
                    acc->done(true, std::move(acc->page));
                return;
            }
            acc->page.threads.resize(subs.size());
            acc->pending = subs.size();
            for (size_t i = 0; i < subs.size(); ++i)
                loadSubThread(subs[i], [acc, i](FollowedThread f) {
                    acc->page.threads[i] = std::move(f);
                    if (--acc->pending)
                        return;
                    // One that couldn't be read is left out.
                    std::erase_if(acc->page.threads, [](const FollowedThread &x) {
                        return !x.root.ts;
                    });
                    if (acc->done)
                        acc->done(true, std::move(acc->page));
                });
        }
    );
}

// One card of the getTimestamps feed from one conversations.replies: the
// root and the unread replies, or the newest one of a read thread (a
// root the answer leaves out is asked for alone). A failure answers an
// empty one.
void SlackBackend::Read::loadSubThread(
    const mapjson::ThreadSub &t, std::function<void(FollowedThread)> done
) {
    const bool unread = t.latest > t.lastRead;
    const Ts   from   = unread ? std::max(t.lastRead, t.root) : t.latest;
    call(
        "conversations.replies",
        net::formEncode(
            {{"channel", b.convId(t.conv)},
             {"ts", model::formatTs(t.root)},
             {"oldest", model::formatTs(from)},
             {"inclusive", unread ? "0" : "1"},
             {"limit", kSubThreadReplies}}
        ),
        [this, t, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            FollowedThread f;
            if (!err.empty() || t.conv >= s.conversationCount()) {
                if (done)
                    done(std::move(f));
                return;
            }
            f.conv     = t.conv;
            f.lastRead = t.lastRead;
            for (const json::Value v : doc.root()["messages"]) {
                model::Message m = mapjson::toMessage(v, s);
                if (m.ts == t.root) {
                    f.root = std::move(m);
                } else if (m.ts) {
                    m.threadTs = t.root;
                    f.latestReplies.push_back(std::move(m));
                }
            }
            if (f.root.ts) {
                if (done)
                    done(std::move(f));
                return;
            }
            auto held = std::make_shared<FollowedThread>(std::move(f));
            b.loadMessage(
                t.conv, t.root, [held, done = std::move(done)](bool ok, model::Message root) {
                    if (ok)
                        held->root = std::move(root);
                    if (done)
                        done(std::move(*held));
                }
            );
        }
    );
}

// conversations.replies answers for any ts in the
// conversation — a plain message and a thread root come back as
// messages[0] (limit 1 cuts the rest of the thread), and a reply's own ts
// returns just that reply, which conversations.history never lists.
void SlackBackend::loadMessage(ConvRef conv, Ts ts, MessageDone done) {
    Read              *r  = _read;
    const std::string &id = convId(conv);
    if (id.empty() || !ts) {
        r->later(0, [done = std::move(done)] {
            if (done)
                done(false, {});
        });
        return;
    }
    r->call(
        "conversations.replies",
        net::formEncode({{"channel", id}, {"ts", model::formatTs(ts)}, {"limit", "1"}}),
        [r, ts, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            if (err.empty())
                for (const json::Value v : doc.root()["messages"]) {
                    model::Message m = mapjson::toMessage(v, r->s);
                    if (m.ts != ts)
                        continue; // a thread root came back instead of the reply
                    if (done)
                        done(true, std::move(m));
                    return;
                }
            if (done)
                done(false, {});
        },
        Read::Lane::Background
    );
}

// ── Watched threads ─────────────────────────────────────────────────────────

// conversations.replies from `after` (exclusive) on; the root comes back on
// every page whatever `oldest` says, so the filter is ours too.
void SlackBackend::Read::readReplies(ConvRef c, Ts root, Ts after, Lane lane, RepliesDone done) {
    const std::string &id = b.convId(c);
    if (id.empty() || !root) {
        later(0, [done = std::move(done)] {
            if (done)
                done({}, "thread_not_found");
        });
        return;
    }
    std::string form = net::formEncode(
        {{"channel", id},
         {"ts", model::formatTs(root)},
         {"limit", kRepliesLimit},
         {"include_all_metadata", "true"}}
    );
    if (after)
        form.append(str::concat({"&oldest=", model::formatTs(after)}));
    auto acc = std::make_shared<std::vector<ThreadReply>>();
    paginate(
        "conversations.replies",
        std::move(form),
        "messages",
        [this, root, after, acc](const json::Value &arr) {
            for (const json::Value v : arr) {
                ThreadReply r;
                r.message = mapjson::toMessage(v, s);
                if (r.message.ts <= after)
                    continue;
                r.message.threadTs = root;
                r.agentReply       = v["metadata"]["event_type"].str() == Backend::kAgentReplyEvent;
                acc->push_back(std::move(r));
            }
        },
        [this, acc, done = std::move(done)](const std::string &err) {
            if (err == "cancelled")
                return;
            if (!err.empty()) {
                LOG_WARN("slack", "conversations.replies: %s", err.c_str());
                if (done)
                    done({}, err);
                return;
            }
            std::vector<ThreadReply> &rs = *acc;
            // Oldest first as the pages come: an insertion sort is a pass.
            for (size_t i = 1; i < rs.size(); ++i)
                for (size_t j = i; j > 0 && rs[j].message.ts < rs[j - 1].message.ts; --j)
                    std::swap(rs[j], rs[j - 1]);
            rs.erase(
                std::unique(
                    rs.begin(),
                    rs.end(),
                    [](const ThreadReply &x, const ThreadReply &y) {
                        return x.message.ts == y.message.ts;
                    }
                ),
                rs.end()
            );
            for (const ThreadReply &r : rs)
                fetchUserIfNeeded(r.message.user);
            if (done)
                done(std::move(rs), {});
        },
        kMaxThreadPages,
        lane
    );
}

void SlackBackend::loadThreadReplies(ConvRef conv, Ts root, Ts after, RepliesDone done) {
    _read->readReplies(conv, root, after, Read::Lane::Normal, std::move(done));
}

SlackBackend::Read::Watch *SlackBackend::Read::findWatch(ConvRef c, Ts root) {
    for (Watch &w : watches)
        if (w.conv == c && w.root == root)
            return &w;
    return nullptr;
}

// Busy, or a message in the last 10 min: 15 s; else 1 min. The feed's new
// replies (session tokens, a thread I follow) pull a poll forward still.
int64_t SlackBackend::Read::watchInterval(const Watch &w) const {
    const int64_t quiet = base::nowSecs() - model::tsSecs(w.lastReply);
    return w.busy || quiet < 600 ? kWatchHotMs : kWatchQuietMs;
}

// From the tick, so never before the connect settled; a watch's first poll
// is placed kWatchStaggerMs after the previous first one, so the links
// restored at a start trickle out instead of all at once.
void SlackBackend::Read::pollWatches() {
    const int64_t t = now();
    for (Watch &w : watches) {
        if (w.inFlight)
            continue;
        if (!w.due) {
            w.due         = std::max(t, nextFirstPoll);
            nextFirstPoll = w.due + kWatchStaggerMs;
        }
        if (w.due <= t)
            pollWatch(w);
    }
}

// The next poll is placed when this one goes out: a failure waits its turn.
void SlackBackend::Read::pollWatch(Watch &w) {
    w.inFlight          = true;
    w.pulled            = false;
    w.due               = now() + watchInterval(w);
    const ConvRef  c    = w.conv;
    const Ts       root = w.root;
    const uint32_t gen  = w.gen;
    readReplies(
        c,
        root,
        w.seen,
        Lane::Background,
        [this, c, root, gen](std::vector<ThreadReply> rs, const std::string &err) {
            Watch *w = findWatch(c, root);
            if (!w || w->gen != gen)
                return; // unwatched meanwhile
            w->inFlight = false;
            if (w->pulled) // news while this one was out
                w->due = now();
            if (!err.empty()) {
                // A lost connection or a 429 was waited out already; dead
                // credentials are onAuthLost's. The rest is the thread's
                // own, said once per streak.
                if (isTransportError(err) || isTransientSlackError(err) || isAuthError(err) ||
                    err == "ratelimited" || w->failing)
                    return;
                w->failing = true;
                if (auto fn = b.onThreadReplies)
                    fn(c, root, {}, err);
                return;
            }
            w->failing = false;
            std::erase_if(rs, [seen = w->seen](const ThreadReply &r) {
                return r.message.ts <= seen; // watchThread moved the cursor meanwhile
            });
            if (rs.empty())
                return;
            w->seen      = rs.back().message.ts;
            w->lastReply = std::max(w->lastReply, w->seen);
            w->due       = std::min(w->due, now() + watchInterval(*w)); // it just woke up
            // A copy: the callback may unwatch (w goes) or set another.
            if (auto fn = b.onThreadReplies)
                fn(c, root, std::move(rs), {});
        }
    );
}

void SlackBackend::Read::pullWatch(ConvRef c, Ts root, Ts ts) {
    Watch *w = findWatch(c, root);
    if (!w || ts <= w->seen)
        return;
    if (w->inFlight)
        w->pulled = true;
    else if (w->due)
        w->due = std::min(w->due, now());
}

void SlackBackend::watchThread(ConvRef conv, Ts root, Ts after, bool busy) {
    Read &r = *_read;
    if (conv >= _store.conversationCount() || !root)
        return;
    Read::Watch *w = r.findWatch(conv, root);
    if (!w) {
        r.watches.push_back({});
        w            = &r.watches.back();
        w->conv      = conv;
        w->root      = root;
        w->gen       = ++r.watchGen;
        w->lastReply = root;
    }
    w->busy      = busy;
    w->seen      = std::max(w->seen, after);
    w->lastReply = std::max(w->lastReply, w->seen);
    if (w->due && !w->inFlight) // busy now: no waiting out a quiet interval
        w->due = std::min(w->due, r.now() + r.watchInterval(*w));
}

void SlackBackend::unwatchThread(ConvRef conv, Ts root) {
    std::erase_if(_read->watches, [&](const Read::Watch &w) {
        return w.conv == conv && w.root == root;
    });
}

// A message that arrived: into the Store as a live message, then the badge
// by these rules — every DM message is a red badge,
// a muted conversation badges only mentions and followed-thread replies,
// plain channel thread replies don't badge the channel.
bool SlackBackend::Read::inject(ConvRef c, model::Message m, bool parentIsMe) {
    if (m.isReply())
        pullWatch(c, m.threadTs, m.ts);
    if (c >= s.conversationCount() || s.findMessage(c, m.ts))
        return false; // seen already (a history page, the send's own echo)
    const bool own  = s.me != kNoUser && m.user == s.me;
    const Ts   root = m.isReply() ? m.threadTs : 0;
    if (own) {
        // My send's echo, polled before chat.postMessage answered: its
        // pending copy is still there and the send's confirm replaces it.
        const std::vector<model::Message> *list =
            root ? s.replies(c, root) : &s.conversation(c).messages;
        if (list)
            for (size_t i = list->size(), n = 0; i-- > 0 && n < 10; ++n)
                if ((*list)[i].pending && (*list)[i].text == m.text)
                    return false;
    }
    const Ts          ts      = m.ts;
    const UserRef     author  = m.user;
    const bool        mention = s.mentionsMe(m.text);
    const std::string key     = root ? threadKey(c, root) : std::string();
    // Replying subscribes, as Slack does; a thread I started (parent_user_id)
    // is followed whether or not its root is loaded.
    if (root && (own || parentIsMe))
        follow(key);
    // Someone held as away just posted: one probe instead of waiting a round
    // (at most one per user per kAwayProbeGapMs: a busy channel's away
    // poster would ask on every message).
    if (!own && author != kNoUser && !s.user(author).active) {
        if (awayProbeAt.size() <= author)
            awayProbeAt.resize(author + 1, 0);
        int64_t &at = awayProbeAt[author];
        if (!at || now() - at >= kAwayProbeGapMs) {
            at = now();
            requestPresence(author, true);
        }
    }

    const model::Conversation &cv      = s.conversation(c);
    uint32_t                   unread0 = cv.unread, mentions0 = cv.mentions;
    const Ts                   lastRead = cv.lastRead;
    bool                       counted  = !own && cv.member && ts > lastRead;
    const bool                 isDm = cv.isDirect(), muted = cv.muted;

    if (root && !s.replies(c, root)) {
        // The thread isn't loaded: count the reply on its root only (the
        // panel reads the whole thread when it opens).
        s.updateMessage(c, root, [&](model::Message &r) {
            ++r.replyCount;
            r.latestReply = std::max(r.latestReply, ts);
            if (author != kNoUser && r.replyUsers.size() < 5 &&
                std::find(r.replyUsers.begin(), r.replyUsers.end(), author) == r.replyUsers.end())
                r.replyUsers.push_back(author);
        });
        // Still news: what notifies.
        s.announceReply(c, m);
    } else {
        s.addMessage(c, std::move(m));
        // The open, focused chat marks it read as it
        // lands (the shell's observer, inside addMessage). The Store has
        // recounted; count on top of that, and not this message.
        if (const model::Conversation &x = s.conversation(c); x.lastRead != lastRead) {
            unread0   = x.unread;
            mentions0 = x.mentions;
            counted   = counted && ts > x.lastRead;
        }
    }

    // A reply in a thread I follow, or one that
    // mentions me, makes the Threads entry unread.
    if (!own && root && !s.threadMuted(c, root) && (parentIsMe || followed.count(key) || mention))
        noteUnreadThreadReply(c, root, ts);

    uint32_t du = 0, dm = 0;
    if (counted) {
        const bool threadMuted = root && s.threadMuted(c, root);
        const bool isFollowed  = !threadMuted && root && (parentIsMe || followed.count(key));
        if (root && !mention && !isFollowed && (threadMuted || !isDm)) {
            // lives in the thread, not the channel
        } else if (!muted) {
            du = 1;
            dm = isDm || mention || isFollowed;
        } else if (!isDm && (mention || isFollowed)) {
            du = dm = 1;
        }
    }
    const model::Conversation &now = s.conversation(c);
    if (now.unread != unread0 + du || now.mentions != mentions0 + dm || now.latest < ts)
        s.updateConversation(c, [&](model::Conversation &x) {
            x.unread   = unread0 + du;
            x.mentions = mentions0 + dm;
            x.latest   = std::max(x.latest, ts); // replies too: list relevance
        });
    return true;
}

void SlackBackend::setWindowVisible(bool v) {
    Read &r = *_read;
    if (r.visible == v)
        return;
    r.visible = v;
    // Shown again: the open chat and the presence dots at the next tick.
    if (v)
        r.lastFg = r.lastPresence = 0;
}

// ── For the realtime half ───────────────────────────────────────────────────

bool SlackBackend::deliver(ConvRef c, model::Message m, bool parentIsMe) {
    return _read->inject(c, std::move(m), parentIsMe);
}

void SlackBackend::followThread(ConvRef c, Ts root) {
    if (root)
        _read->follow(threadKey(c, root));
}

bool SlackBackend::Read::follow(const std::string &key) {
    if (!followed.insert(key).second)
        return false;
    extrasChanged();
    return true;
}

// The thread state that would otherwise grow for the whole run: the followed
// set (persisted: the newest kMaxFollowed roots once it doubled), and the
// feed's per-thread baselines and read floors (ones I still follow or
// with an unread reply stay).
void SlackBackend::Read::pruneThreadState() {
    if (followed.size() > 2 * kMaxFollowed) {
        std::vector<Ts> roots;
        for (const std::string &k : followed)
            roots.push_back(keyTs(k));
        const Ts cut = pruneCut(std::move(roots), kMaxFollowed);
        std::erase_if(followed, [cut](const std::string &k) { return keyTs(k) < cut; });
        extrasChanged();
    }
    for (auto *m : {&threadBaseline, &threadReadFloor}) {
        if (m->size() <= 2 * kMaxThreadState)
            continue;
        std::vector<Ts> roots;
        for (const auto &[k, ts] : *m)
            roots.push_back(keyTs(k));
        const Ts cut = pruneCut(std::move(roots), kMaxThreadState);
        std::erase_if(*m, [&](const auto &kv) {
            return keyTs(kv.first) < cut && !followed.count(kv.first) &&
                   !unreadThreads.count(kv.first);
        });
    }
}

// The newest reply of a loaded thread that isn't a pending send (0: none).
Ts SlackBackend::Read::newestHeldReply(ConvRef c, Ts root) const {
    const std::vector<model::Message> *have = s.replies(c, root);
    if (have)
        for (size_t i = have->size(); i-- > 0;)
            if (!(*have)[i].pending)
                return (*have)[i].ts;
    return 0;
}

// conversations.replies with limit 1 answers the root alone (with its
// latest_reply): a quiet thread costs this one small call per poll instead
// of every page of it.
void SlackBackend::Read::checkThreadTip(ConvRef c, Ts root) {
    call(
        "conversations.replies",
        net::formEncode({{"channel", b.convId(c)}, {"ts", model::formatTs(root)}, {"limit", "1"}}),
        [this, c, root](const json::Document &doc, const std::string &err) {
            if (!err.empty() || openConv != c || openThread != root)
                return; // a failure waits for the next poll; a closed thread, for nothing
            for (const json::Value v : doc.root()["messages"]) {
                const model::Message r = mapjson::toMessage(v, s);
                if (r.ts != root)
                    continue;
                const Ts newest = newestHeldReply(c, root);
                if ((r.latestReply ? r.latestReply : r.ts) != (newest ? newest : r.ts))
                    loadThreadPages(c, root, true, nullptr);
                return;
            }
        }
    );
}

ConvRef SlackBackend::mergeConversation(model::Conversation fresh) {
    if (const ConvRef r = _store.findConversation(fresh.id); r != kNoConv)
        _read->carryLocal(fresh, _store.conversation(r));
    return _store.addConversation(std::move(fresh));
}

void SlackBackend::reloadConversations() {
    _read->loadConversations([](const std::string &) {});
}

void SlackBackend::loadChannelDirectory() {
    _read->loadChannelDirectory();
}

void SlackBackend::backfillOpen() {
    if (_read->openConv != kNoConv) {
        _read->lastFg = _read->now();
        _read->pollConversation(_read->openConv, true);
    }
}

void SlackBackend::reloadUsergroups() {
    _read->loadUsergroups();
}

std::vector<model::Backend::Command> SlackBackend::serverCommands() const {
    return _read->serverCommands;
}

uint64_t SlackBackend::commandsRev() const {
    return _read->commandsRev;
}

void SlackBackend::rearmReminders() {
    _read->armReminders();
}

void SlackBackend::noteRateLimited(const std::string &method, int64_t secs) {
    _read->noteRateLimited(method, secs);
}

bool SlackBackend::isDead(const std::string &id) const {
    return _read->dead.count(id) > 0;
}

void SlackBackend::markDead(const std::string &id) {
    _read->markDead(id);
}

void SlackBackend::markAlive(const std::string &id) {
    _read->markAlive(id);
}

void SlackBackend::threadRead(ConvRef c, Ts root, Ts upTo) {
    _read->threadRead(c, root, upTo);
}

void SlackBackend::readPages(
    std::string                              method,
    std::string                              form,
    std::string                              key,
    std::function<void(const json::Value &)> onPage,
    std::function<void(const std::string &)> onDone
) {
    _read->paginate(
        std::move(method), std::move(form), std::move(key), std::move(onPage), std::move(onDone)
    );
}

void SlackBackend::readCall(std::string method, std::string form, ApiDone done, bool background) {
    _read->call(
        std::move(method),
        std::move(form),
        std::move(done),
        background ? Read::Lane::Background : Read::Lane::Normal
    );
}

// ── Contract odds and ends ──────────────────────────────────────────────────

SlackBackend::Read *SlackBackend::newRead(SlackBackend &b) {
    return new Read(b);
}

void SlackBackend::deleteRead(Read *r) {
    delete r;
}

// The capabilities, for the fields the shell gates on.
model::Backend::Capabilities SlackBackend::capabilities() const {
    Capabilities c;
    c.huddles          = true;
    c.replyBroadcast   = true;
    c.scheduledSend    = true;                 // chat.scheduleMessage
    c.memberList       = true;                 // conversations.members
    c.threadsView      = _creds.sessionAuth(); // subscriptions.thread.getView: xoxc only
    c.messageReminders = _creds.sessionAuth(); // saved.*: xoxc only
    c.presence         = true;                 // polled users.getPresence
    c.selfStatus       = true;
    c.canvases         = true;
    c.fileUpload       = true;                 // files.getUploadURLExternal
    c.removePreview    = _creds.sessionAuth(); // chat.deleteAttachment: internal, xoxc only
    c.slashCommands    = true;                 // the built-ins, commands.list, chat.command
    c.sidebarTheme     = _creds.sessionAuth(); // users.prefs.get: xoxc only
    c.customSections   = _creds.sessionAuth(); // users.channelSections.*: xoxc only
    return c;
}

void SlackBackend::refreshSelfPresence(std::function<void()> then) {
    _read->refreshSelfPresence(std::move(then));
}

model::Backend::SelfPresence SlackBackend::selfPresence() const {
    return _read->self;
}

int64_t SlackBackend::nowSecs() const {
    return base::nowSecs();
}

} // namespace slack

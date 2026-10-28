// The Slack workspace backend: everything a signed-in workspace does, written into the Store.
//
// Two auth modes:
//   session  (xoxc token + `d` cookie): no Socket Mode; new messages come
//            from polling (the open chat every 5 s, counts in the background);
//            an RTM socket may be held only for presence (rtm_presence.h)
//   app keys (OAuth xoxp): the same Web API, events pushed over the app's
//            Socket Mode socket (socket_mode.h) with polling as the safety
//            net; a rotating token is refreshed (oauth.v2.access)
//
// The implementation is split by concern (one class, several files):
//   slack_backend.cpp   connect, roster, users, emoji, history, threads,
//                       polling for new messages and unread counts
//   slack_actions.cpp   every write: send/edit/delete, reactions, read
//                       marks, stars, mute/notify prefs, leave, DMs, pins,
//                       saved/reminders, files, presence/status/profile,
//                       typing, search, members, scheduled messages,
//                       channel canvases
//   slack_realtime.cpp  Socket Mode events into the Store, the reconnect
//                       backfill (resyncUnreads), the presence link, token
//                       refresh
//   slack_json.{h,cpp}  Web API JSON → model (users, conversations, messages)
#pragma once

#include "app/model/backend.h"
#include "app/slack/credentials.h"
#include "app/slack/web_api.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace plat {
class App;
}

namespace slack {

class SocketMode;
class RtmPresence;

class SlackBackend final : public model::Backend {
public:
    SlackBackend(model::Store &store, plat::App &app, net::Client &client, Credentials creds);
    ~SlackBackend() override; // cancels everything; no callback runs afterwards

    const Credentials                            &credentials() const { return _creds; }
    const Auth                                   &auth() const { return _auth; }
    // An answer that means the credentials are dead (not a network hiccup):
    // the workspace must be signed in again.
    static bool                                   authError(const std::string &error);
    // Fired (UI thread) when a later call finds the credentials dead.
    std::function<void(const std::string &error)> onAuthLost;

    // ── The workspace cache (slack_backend.cpp) ─────────────────────────────
    // Opens this workspace's disk cache (app/cache/workspace_cache.h: the
    // accounts controller does at activation; tests opt in) and fills the
    // empty Store from it at once — before connect(), whose answers then
    // merge into it. True if a snapshot was there.
    bool           openCache(std::string dir);
    // Writes what is pending (keep) or drops it, and stops writing: before
    // the Store is cleared (switch, sign-out) or the cache wiped.
    void           closeCache(bool keep);
    // The conversation open when the workspace was last shown (kNoConv).
    model::ConvRef lastConversation() const;
    // connect()'s loads still running (users.list may outlast done(true),
    // which comes as soon as the conversations are in).
    bool           connecting() const;

    // ── Read side (slack_backend.cpp) ───────────────────────────────────────
    void         connect(Done done) override;
    void         loadHistory(model::ConvRef conv, model::Ts before, Done done) override;
    void         loadThread(model::ConvRef conv, model::Ts root, Done done) override;
    void         setActiveConversation(model::ConvRef conv, model::Ts thread) override;
    Capabilities capabilities() const override;
    SelfPresence selfPresence() const override;
    int64_t      nowSecs() const override;
    // The Threads page: subscriptions.thread.getView (session tokens only).
    void         loadThreadsView(std::string cursor, ThreadsViewDone done) override;
    void         loadMessage(model::ConvRef conv, model::Ts ts, MessageDone done) override;
    void         resolveChannel(const std::string &id) override;
    bool         threadFollowed(model::ConvRef conv, model::Ts root) const override;
    void         resolveUser(model::UserRef u) override;
    void         requestPresence(model::UserRef u) override;

    // ── Write side (slack_actions.cpp) ──────────────────────────────────────
    void send(model::ConvRef conv, std::string text, model::Ts threadTs, Done done) override;
    void sendWithFiles(
        model::ConvRef           conv,
        std::string              text,
        model::Ts                threadTs,
        std::vector<std::string> files,
        Done                     done
    ) override;
    void edit(model::ConvRef conv, model::Ts ts, std::string text) override;
    void remove(model::ConvRef conv, model::Ts ts) override;
    void react(model::ConvRef conv, model::Ts ts, std::string_view name, bool add) override;
    void markRead(model::ConvRef conv, model::Ts ts) override;
    void markUnread(model::ConvRef conv, model::Ts ts) override;
    void setStarred(model::ConvRef conv, bool starred) override;
    void setMuted(model::ConvRef conv, bool muted) override;
    void setNotifyLevel(model::ConvRef conv, model::NotifyLevel level) override;
    void leave(model::ConvRef conv) override;
    void openDm(model::UserRef user, std::function<void(model::ConvRef)> done) override;
    void joinChannel(model::ConvRef conv, ConvDone done) override;
    void createChannel(std::string name, bool isPrivate, ConvDone done) override;
    void setPinned(model::ConvRef conv, model::Ts ts, bool pinned) override;
    void setSaved(model::ConvRef conv, model::Ts ts, bool saved) override;
    void deleteFile(model::ConvRef conv, model::Ts ts, const std::string &fileId) override;
    void deleteAttachment(model::ConvRef conv, model::Ts ts, int attachmentId, Done done) override;
    void downloadFile(const std::string &url, std::string toPath, Done done) override;
    void setReminder(model::ConvRef conv, model::Ts ts, int64_t dueSecs) override;
    void userTyping(model::ConvRef conv, model::Ts threadTs) override;
    void setPresence(bool away, Done done) override;
    void setStatus(std::string emoji, std::string text, int64_t expiry, Done done) override;
    void loadMyProfile(std::function<void(MyProfile)> done) override;
    void updateProfile(std::string name, std::string email, std::string phone, Done done) override;
    void setPhoto(std::string path, Done done) override;
    void
    sendBroadcast(model::ConvRef conv, std::string text, model::Ts threadTs, Done done) override;
    void scheduleMessage(
        model::ConvRef conv, std::string text, model::Ts thread, int64_t postAt, Done done
    ) override;
    void sendBlocks(
        model::ConvRef conv,
        std::string    text,
        std::string    blocks,
        model::Ts      threadTs,
        bool           broadcast,
        Done           done
    ) override;
    void
    editBlocks(model::ConvRef conv, model::Ts ts, std::string text, std::string blocks) override;
    void scheduleBlocks(
        model::ConvRef conv,
        std::string    text,
        std::string    blocks,
        model::Ts      thread,
        int64_t        postAt,
        Done           done
    ) override;
    void refreshScheduled() override;
    void cancelScheduled(const std::string &id, Done done) override;
    void loadMembers(model::ConvRef conv, MembersDone done) override;
    void search(std::string query, std::function<void(std::vector<SearchHit>)> done) override;
    void markThreadRead(model::ConvRef conv, model::Ts root, model::Ts ts) override;
    void loadChannelCanvas(model::ConvRef conv, std::function<void(std::string)> done) override;
    void loadCanvasMeta(const std::string &fileId, CanvasMetaDone done) override;
    void loadCanvasContent(const std::string &fileId, CanvasHtmlDone done) override;
    void
    createChannelCanvas(model::ConvRef conv, std::string markdown, CanvasCreated done) override;
    void
    editCanvas(const std::string &fileId, std::vector<CanvasChange> changes, Done done) override;
    void                 deleteCanvas(const std::string &fileId, Done done) override;
    // Slash commands: commands.list + msga's built-ins, run by runLocalCommand
    // (chat.command for the workspace's own).
    std::vector<Command> commands(model::ConvRef conv) override;
    uint64_t             commandsRevision(model::ConvRef conv) override;
    LocalResult          runLocalCommand(
        model::ConvRef conv, model::Ts thread, const std::string &name, const std::string &args
    ) override;
    // /dnd: dnd.setSnooze for `minutes`, dnd.endSnooze for 0.
    void setDndSnooze(int minutes, Done done);
    void
    pressButton(model::ConvRef conv, model::Ts ts, const std::string &buttonId, Done done) override;
    void loadSidebarTheme(std::function<void(SidebarTheme, std::string)> done) override;

    // ── Agent thread links ──────────────────────────────────────────────────
    // Posts (slack_actions.cpp): chat.postMessage / chat.update with message
    // metadata {event_type: kAgentReplyEvent}; a token that refuses metadata
    // (metadata_* / invalid_metadata_*) posts without it from then on.
    void postAgentReply(
        model::ConvRef conv, model::Ts root, std::string text, std::string blocks, PostDone done
    ) override;
    void editAgentReply(
        model::ConvRef conv, model::Ts ts, std::string text, std::string blocks, Done done
    ) override;
    void deleteAgentReply(model::ConvRef conv, model::Ts ts, Done done) override;
    // A send with files (sendWithFiles) that answers with the shared message's ts.
    void postAgentFiles(
        model::ConvRef           conv,
        model::Ts                root,
        std::string              text,
        std::vector<std::string> files,
        PostDone                 done
    ) override;
    // Reading and watching (slack_backend.cpp): conversations.replies with
    // include_all_metadata. A watched thread is polled on the paced lane at a
    // cadence by its quietness (15 s while busy or for 10 min after a reply,
    // then 1 min, 5 min after an hour, 15 min after a day; the first polls
    // after a start staggered); where the thread feed reports it (session
    // tokens, a thread I follow) at most every 5 min. A new reply the feed,
    // a push or an open-thread poll delivers, or client.counts' thread
    // unread flag turning on, brings the poll forward.
    void loadThreadReplies(
        model::ConvRef conv, model::Ts root, model::Ts after, RepliesDone done
    ) override;
    void watchThread(model::ConvRef conv, model::Ts root, model::Ts after, bool busy) override;
    void unwatchThread(model::ConvRef conv, model::Ts root) override;

    // ── Realtime, presence link, token refresh (slack_realtime.cpp) ─────────
    // The app's Socket Mode socket (one per app token, shared by every
    // workspace: see socket_mode.h), or null: polling only. Before connect().
    // Session workspaces never take one (session mode has no push).
    void                                     setRealtime(std::shared_ptr<SocketMode> socket);
    // True while events are pushed: the polls slow down to a safety net.
    bool                                     hasRealtimePush() const;
    // The app registration a rotating OAuth token is refreshed with.
    void                                     setAppConfig(AppConfig cfg);
    // Fired (UI thread) after a refresh rotated the token: persist creds.
    std::function<void(const Credentials &)> onCredentialsChanged;
    // Fired (at most every 5 min) when the app's socket is contended: the
    // same app keys run on another device and steal events.
    std::function<void()>                    onParallelUsage;
    void                                     setPresenceMode(PresenceMode mode) override;
    // Service: Slack's own "Names" preference, read at every connect
    // (session tokens only: OAuth ones show full names).
    void                                     setNamesMode(NamesMode mode) override;
    PresenceLink                             presenceLink() const override;
    void                                     noteUserActivity() override;
    void                                     setWindowVisible(bool visible) override;
    RtmPresence                             *presenceLinkForTest() const; // null: OAuth
    // readCall, for the tests of the lanes.
    void readCallForTest(std::string method, std::string form, ApiDone done, bool background) {
        readCall(std::move(method), std::move(form), std::move(done), background);
    }

    // ── Shared by both halves ───────────────────────────────────────────────
    // A Web API call with this workspace's auth; tracked so the destructor
    // cancels it (done never runs after ~SlackBackend). An auth error also
    // fires onAuthLost (once).
    void api(std::string_view method, std::string form, ApiDone done);
    // The same for a call whose answer is ok or an error (slack_actions.cpp):
    // a cancel is silent, a failure is logged and `done` hears it; on ok
    // onOk(doc) runs first, then done(true). Either may be null.
    void
                       api(std::string_view                            method,
                           std::string                                 form,
                           std::function<void(const json::Document &)> onOk,
                           Done                                        done);
    // "C0123" for a ConvRef ("" if unknown). Ts ↔ "1712345678.123456":
    // model::parseTs / model::formatTs.
    const std::string &convId(model::ConvRef conv) const;
    // The dead-conversation cache (channel_not_found
    // here — another workspace's over the shared socket, a dead DM),
    // persisted with the workspace cache; a fresh roster revives what it lists.
    bool               isDead(const std::string &id) const;
    void               markDead(const std::string &id);
    void               markAlive(const std::string &id);
    // Re-poll my presence snapshot (users.getPresence: manual_away, online…);
    // `then` runs once it answered (not after ~SlackBackend).
    void               refreshSelfPresence(std::function<void()> then);

    plat::App                   &app() { return _app; }
    net::Client                 &client() { return _client; }
    model::Store                &store() { return _store; }
    // Shared with timers and posted closures: false once destroyed.
    const std::shared_ptr<bool> &alive() const { return _alive; }

    // Each half keeps its own state, defined in its own file.
    struct Read;  // slack_backend.cpp
    struct Write; // slack_actions.cpp
    struct Live;  // slack_realtime.cpp

private:
    static Read  *newRead(SlackBackend &b); // slack_backend.cpp
    static void   deleteRead(Read *r);
    static Write *newWrite(SlackBackend &b); // slack_actions.cpp
    static void   deleteWrite(Write *w);
    static Live  *newLive(SlackBackend &b); // slack_realtime.cpp
    static void   deleteLive(Live *l);

    // What the realtime half asks of the read half (slack_backend.cpp).
    // A live message into the Store with the badge rules; false when it
    // was already there (a poll, the send's own echo).
    bool                 deliver(model::ConvRef c, model::Message m, bool parentIsMe);
    // What the write half asks: replying in a thread follows it (on
    // every send to a thread).
    void                 followThread(model::ConvRef c, model::Ts root);
    // Merges a conversation the server just described (channel_created,
    // conversations.info) keeping what only this client knows.
    model::ConvRef       mergeConversation(model::Conversation fresh);
    void                 reloadConversations();
    void                 backfillOpen(); // the open chat's head page, now
    void                 reloadUsergroups();
    // commands.list's answer (empty until it came, or without one).
    std::vector<Command> serverCommands() const;
    uint64_t             commandsRev() const; // moves with serverCommands()
    // A conversation's live huddle changed (Conversation::huddle*).
    void                 setHuddle(
        model::ConvRef c, bool active, std::string link, std::vector<model::UserRef> participants
    );
    // My read cursor in a followed thread moved (the Threads entry).
    void                     threadRead(model::ConvRef c, model::Ts root, model::Ts upTo);
    // The DM read-cursor sweeps (the DM activity sweep, resyncUnreads;
    // slack_backend.cpp): which conversations, and one conversations.info
    // on the paced lane (null: it failed; channel_not_found marks it dead).
    std::vector<std::string> directConversationIds() const;
    void sweepInfo(const std::string &id, std::function<void(model::Conversation *info)> done);
    // A read call — or a write that is safe to repeat — with rate limits,
    // lost connections and transient errors waited out; background: the
    // paced 1.2 s lane, which holds while a Normal call is outstanding.
    void readCall(std::string method, std::string form, ApiDone done, bool background = false);
    // Every page of a cursored read call (next_cursor), on the read lane:
    // onPage(root[key]) per page, then onDone("" or the first error).
    void readPages(
        std::string                              method,
        std::string                              form,
        std::string                              key,
        std::function<void(const json::Value &)> onPage,
        std::function<void(const std::string &)> onDone
    );
    // The read half's poll tick (slack_realtime.cpp): the socket's health
    // check, and a poll that found what the socket should have pushed.
    void realtimeTick();
    void realtimeMissed();
    // A rotating token was rejected or expires: refresh it, then `then(ok)`.
    void refreshToken(std::function<void(bool ok)> then);
    bool canRefresh() const;
    bool refreshInFlight() const;
    // quiet: a 429 is not put on the banner (the caller waits it out unseen).
    void issueApi(
        std::string method, std::string form, ApiDone done, bool refreshed, bool quiet = false
    );
    void loseAuth(const std::string &error);
    // A 429 on any call: the error banner, throttled.
    void noteRateLimited(const std::string &method, int64_t secs);
    // `done`, which also puts a failure on the error banner as tr(what) with
    // the re-auth hint (the profile and photo errors).
    Done bannerOnFailure(const char *what, Done done);
    // A reminder was set or moved here: the alarm re-arms (slack_backend.cpp).
    void rearmReminders();
    // cancelScheduled's calls (slack_actions.cpp): a session's draft (retry:
    // once more on draft_has_conflict), a token's scheduled message, and
    // what either answer does to the Store's list.
    void deleteDraft(const std::string &id, std::string version, bool retry, Done done);
    void deleteScheduled(
        const std::string &id, const std::string &channel, const char *method, Done done
    );
    void
    scheduledGone(const std::string &id, const char *method, const std::string &err, Done done);

    plat::App                         &_app;
    net::Client                       &_client;
    Credentials                        _creds;
    Auth                               _auth;
    std::vector<net::RequestId>        _inflight;
    // Uploads and downloads (minutes long) on their own worker pool, made on
    // first use, so they never hold up API calls (a separate download
    // limit). Its destructor cancels them: nothing to track.
    std::unique_ptr<net::Client>       _transfers;
    net::Client                       &transfers();
    std::vector<std::shared_ptr<Done>> _downloads; // downloadFile's, until answered
    bool                               _authLost = false;
    // Enterprise Grid (auth.test's enterprise_id), and the thread
    // methods there refused without team_id once: they send it since.
    bool                               _grid = false, _threadsTeamId = false;
    std::shared_ptr<bool>              _alive;
    Read                              *_read  = nullptr;
    Write                             *_write = nullptr;
    Live                              *_live  = nullptr;

    friend struct Read;
    friend struct Write;
    friend struct Live;
};

} // namespace slack

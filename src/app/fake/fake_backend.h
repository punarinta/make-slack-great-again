// FakeBackend — a fixture workspace (fixture.h), interactive: sends
// land (pending first, confirmed ~180 ms later) and draw the fixture's canned
// replies with typing indicators; reactions, edits, deletes and stars
// round-trip through the Store; threads and search work over the fixture
// data; a sent link listed under "unfurls" grows its preview a second later;
// mute, notification level, pins, saved items, leave/close and opening DMs
// change the Store directly (a real backend would round-trip first).
// Timers run on the plat event loop.
#pragma once

#include "app/fake/fixture.h"
#include "app/model/timers.h"
#include "app/model/backend.h"

#include <plat/plat.h>

#include <string>
#include <vector>

namespace fake {

// The first message of `conv` (top-level, then a loaded thread's replies)
// whose rendered text, file name or attachment title contains `fragment`,
// case-insensitively; 0 = none. Any backend's Store: the demo tours name
// messages this way on the Claude Code workspace too.
model::Ts findTs(const model::Store &store, model::ConvRef conv, std::string_view fragment);

// Not final: tests derive from it (e.g. to make a DM an agent session).
class FakeBackend : public model::Backend {
public:
    FakeBackend(model::Store &store, plat::App &app);
    ~FakeBackend() override; // cancels pending timers (they capture this)

    // Where the fixture lives (a directory with fixture.json, or the file)
    // and the moment relative times are anchored to. connect() loads it.
    void setFixture(std::string path, int64_t nowSecs);

    void connect(Done done) override;
    void loadHistory(model::ConvRef conv, model::Ts before, Done done) override;
    void loadThread(model::ConvRef conv, model::Ts root, Done done) override;
    void send(model::ConvRef conv, std::string text, model::Ts threadTs, Done done) override;
    // "Uploads" by pointing the message's File records at the local files.
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
    // A new, empty channel with me as its only member.
    void createChannel(std::string name, bool isPrivate, ConvDone done) override;
    void setPinned(model::ConvRef conv, model::Ts ts, bool pinned) override;
    void setSaved(model::ConvRef conv, model::Ts ts, bool saved) override;
    void setReminder(model::ConvRef conv, model::Ts ts, int64_t dueSecs) override;
    void deleteFile(model::ConvRef conv, model::Ts ts, const std::string &fileId) override;
    void userTyping(model::ConvRef conv, model::Ts threadTs) override;
    void search(std::string query, std::function<void(std::vector<SearchHit>)> done) override;
    // The demo feed: the threads I started or replied in, the last three
    // replies each, one page; every fixture thread starts out read.
    void loadThreadsView(std::string cursor, ThreadsViewDone done) override;
    void markThreadRead(model::ConvRef conv, model::Ts root, model::Ts ts) override;
    // The demo's activity: mentions of me, the newest message of each
    // direct message, replies in my threads, reactions to my messages —
    // unread past the read cursors; one page.
    void loadActivity(
        ActivityFilter filter, bool unreadOnly, std::string cursor, ActivityDone done
    ) override;
    void         setActivityRead(const ActivityItem &item, bool read) override;
    // The demo: members, the Threads entry and Saved messages; no huddles,
    // broadcast replies or scheduled sends. I look phantom-away (no official
    // client); the toggle and status round-trip but change nothing.
    Capabilities capabilities() const override;
    SelfPresence selfPresence() const override { return {true, false, false, false}; }
    void         setPresence(bool away, Done done) override;
    void         setStatus(std::string emoji, std::string text, int64_t expiry, Done done) override;
    void         loadMembers(model::ConvRef conv, MembersDone done) override;
    // My profile round-trips through the Store (the phone is kept here).
    void         loadMyProfile(std::function<void(MyProfile)> done) override;
    void updateProfile(std::string name, std::string email, std::string phone, Done done) override;
    void setPhoto(std::string path, Done done) override;
    // The stand-in GIPHY: every GIF in the fixture's gif directory, whatever the query.
    bool gifSearchAvailable() const override { return true; }
    int64_t nowSecs() const override;
    void    searchGifs(std::string query, std::function<void(std::vector<Gif>)> done) override;
    // Canvases in memory: the fixture's HTML until an edit, then the saved
    // markdown rendered back to the HTML Slack would serve.
    void    loadCanvasMeta(const std::string &fileId, CanvasMetaDone done) override;
    void    loadCanvasContent(const std::string &fileId, CanvasHtmlDone done) override;
    void
    createChannelCanvas(model::ConvRef conv, std::string markdown, CanvasCreated done) override;
    void
    editCanvas(const std::string &fileId, std::vector<CanvasChange> changes, Done done) override;
    void deleteCanvas(const std::string &fileId, Done done) override;
    // Nothing polls here; tests read what the shell said last.
    void setWindowVisible(bool visible) override { windowVisible = visible; }
    bool windowVisible = true;

    // ── Demo extras ─────────────────────────────────────────────────────────
    const Fixture   &fixture() const { return _fx; }
    // The stand-in AI endpoint: the reply whose `match` occurs in the
    // request (case-insensitive), else the default.
    std::string_view aiReply(std::string_view request) const;
    // Posts as another fixture user right now (tour scripts, tests).
    model::Ts
    postAs(model::ConvRef conv, model::UserRef user, std::string text, model::Ts thread = 0);
    // The first message (top-level or reply) whose rendered text contains
    // `fragment`, case-insensitively — how tour scripts name messages.
    model::Ts findTs(model::ConvRef conv, std::string_view fragment) const {
        return fake::findTs(_store, conv, fragment);
    }

    // Latencies, like a network backend (never synchronous; see backend.h).
    static constexpr int kReadLatencyMs = 25;
    static constexpr int kSendConfirmMs = 180; // long enough to see the pending copy
    static constexpr int kUnfurlDelayMs = 900;

private:
    enum class ConvFlag : uint8_t { Starred, Muted, Notify, Member };
    void           setConvFlag(model::ConvRef conv, ConvFlag f, int v);
    void           setMessageFlag(model::ConvRef conv, model::Ts ts, bool pinFlag, bool on);
    void           later(int ms, std::function<void()> fn);
    model::Ts      nextTs();
    void           scheduleAutoReply(model::ConvRef conv, model::Ts thread);
    void           scheduleUnfurl(model::ConvRef conv, model::Ts ts);
    // The conversation whose canvas `fileId` is (kNoConv: none).
    model::ConvRef canvasConv(std::string_view fileId) const;

    plat::App           &_app;
    std::string          _path;
    int64_t              _now = 0;
    Fixture              _fx;
    std::string          _replyUsed;    // a byte per _fx.autoReplies entry: 1 = posted
    model::OneShotTimers _timers{_app}; // they capture this
    model::Ts            _lastTs = 0;
    std::string          _phone;
    uint32_t             _uploads = 0;
    struct ThreadRead {
        model::ConvRef conv;
        model::Ts      root, read;
    };
    std::vector<ThreadRead> _threadRead; // my cursor per thread (few: linear is fine)
    model::Ts              &threadRead(model::ConvRef conv, model::Ts root);
    // Items marked read or unread on the Activity page, by key.
    std::vector<std::pair<std::string, bool>> _activityRead;
    struct CanvasEdit {
        std::string id, html; // the body as saved (no title heading)
    };
    std::vector<CanvasEdit> _canvasEdits;
    uint32_t                _canvasSeq = 0;
};

} // namespace fake

// WorkspaceCache — one workspace's Store on disk, so a start opens at once.
//
// What is kept, per workspace, in <cacheDir>/workspaces/<key>/ (app/identity.h; earlier
// versions kept their own format in <dataDir>/cache/<key>, left alone):
//   roster.json          conversations: kind, names, members, read cursors,
//                        unread/mention badges, star, mute, notify level,
//                        local name
//   users.json           every known user and bot
//   emoji.json           custom emoji, aliases included
//   usergroups.json      every user group: id, handle, name, and whether I
//                        am in it (meta.json's "x"."ug" held them before,
//                        with members; read once, then moved)
//   meta.json            me, the last open conversation, my user groups,
//                        muted threads, reminders, and the backend's own
//                        state ("x": Slack's saved items, followed threads,
//                        off-roster probe times, the DM activity sweep stamp)
//   messages/<conv>.json the newest kMaxMessages top-level messages of each
//                        conversation opened (no pending copies, no threads)
//
// Reading: load() fills an empty Store synchronously at activation; the
// backend then merges the network's answers into it with its usual rules.
// Messages load per conversation when it is first opened (loadMessages).
//
// Writing: the cache observes the Store and writes what changed at most
// once per kWriteDelayMs (1 s), on flush() and on close(), and only when a
// file's bytes differ from what it last wrote. A file is only re-serialised
// when something it holds moved: roster.json when a conversation's record
// differs from the one written, meta.json when the marks
// (Store::localRevision), my groups or the backend's state changed,
// usergroups.json with the groups. What moves all the time waits for the
// slow cadence (kSlowDelayMs): the read cursors and badges in roster.json
// (every new message moves them), and users.json once it exists (profiles
// trickle in one lookup at a time). The bytes are made on the UI thread
// (the Store is the UI thread's) and written on a worker, atomically and
// owner-only (0600); close() writes what is still queued itself.
//
// Wiping: remove() on sign-out of that workspace, clearAll() for Settings →
// "Clear cache" — after which caches still open write nothing more this
// run, so the next start is a cold one.
//
// Format: JSON, every record a positional array in the order of its field
// list (workspace_cache.cpp) — no key per field. A file whose "v" is not
// kVersion, or that does not parse, is ignored: the start is a cold one.
#pragma once

#include "app/model/store.h"
#include "base/json.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace plat {
class App;
}

namespace cache {

class WorkspaceCache {
public:
    static constexpr int kMaxMessages  = 50;     // per conversation
    static constexpr int kWriteDelayMs = 1000;   // batched writes, at most one a second
    static constexpr int kSlowDelayMs  = 60'000; // cursors, badges and profiles
    // Tests shorten the slow cadence (0: back to kSlowDelayMs).
    static void          setSlowDelayForTest(int ms);

    // `dir` is this workspace's directory (dirFor); "" keeps nothing.
    WorkspaceCache(plat::App &app, model::Store &store, std::string dir);
    ~WorkspaceCache(); // close(true)
    WorkspaceCache(const WorkspaceCache &)            = delete;
    WorkspaceCache &operator=(const WorkspaceCache &) = delete;

    // <cacheDir>/workspaces ("" when the OS has no cache directory).
    static std::string root(plat::App &app);
    // The directory of a workspace record key ("slack:T0123").
    static std::string dirFor(plat::App &app, std::string_view workspaceKey);
    // Bytes of every workspace's files (Settings → Storage).
    static int64_t     diskBytes(plat::App &app);
    // Settings → "Clear cache": every workspace's files.
    static void        clearAll(plat::App &app);
    // The same two with the walk on a worker (model::runInBackground):
    // `done` runs on the UI thread. clearAllAsync stops the open caches'
    // writes at once, like clearAll.
    static void        diskBytesAsync(plat::App &app, std::function<void(int64_t)> done);
    static void        clearAllAsync(plat::App &app, std::function<void()> done);
    // Sign-out: that workspace's files.
    static void        remove(plat::App &app, std::string_view workspaceKey);

    // Fills the Store from disk (users, conversations, emoji, my groups,
    // muted threads, reminders, me). False when there is no usable roster —
    // then nothing is loaded. *meta keeps meta.json for the caller's own
    // state (meta->root()["x"]).
    bool        load(json::Document *meta);
    // The conversation that was open last ("" if none).
    std::string lastConversation() const { return _last; }
    // Conversation c's cached messages into the Store (a page). Returns the
    // newest ts loaded, 0 if there was nothing. Once per conversation.
    model::Ts   loadMessages(model::ConvRef c);
    // From now on c's messages are kept (an opened conversation).
    void        track(model::ConvRef c);
    void        setLastConversation(model::ConvRef c);

    // The backend's own state, written as the members of meta's "x" object;
    // call extrasChanged() when it changed.
    std::function<void(json::Writer &)> saveExtras;
    void                                extrasChanged();
    void                                emojiChanged(); // the Store has no event for emoji

    // Writes what is pending now.
    void flush();
    // Stops observing; keep = flush first, else drop what is pending. No
    // write happens afterwards (call it before the Store is cleared).
    void close(bool keep);

private:
    enum Dirty : uint8_t { kConvs = 1, kUsers = 2, kEmoji = 4, kMeta = 8, kGroups = 16 };
    struct Disk; // the write queue the workers drain (workspace_cache.cpp)
    void        onChange(const model::Change &ch);
    void        noteRecord(model::ConvRef c); // after a Meta: mark what changed
    void        mark(uint8_t what);
    void        slow(uint8_t what); // mark on the slow cadence
    void        schedule();
    void        writePending();
    bool        tracked(model::ConvRef c) const { return c < _tracked.size() && _tracked[c]; }
    bool        writable() const;
    void        write(const char *name, std::string data);
    std::string messagesFile(model::ConvRef c) const;

    plat::App               &_app;
    model::Store            &_store;
    std::string              _dir, _last;
    uint32_t                 _clearGen; // clearAll() since: write nothing
    model::Store::ObserverId _observer = 0;
    uint64_t                 _timer = 0, _slowTimer = 0;
    uint8_t                  _dirty = 0, _slow = 0; // Dirty bits: due now / on the slow cadence
    // The Store's named-profile/presence revisions users.json holds: a
    // presence flip doesn't re-serialise the roster (it is saved at close,
    // or with the next profile change), nor does a placeholder.
    uint64_t                 _usersNamedRev = 0, _usersPresenceRev = 0;
    // The Store's localRevision / usergroupRevision meta.json and
    // usergroups.json were last marked for.
    uint64_t                 _localRev = 0, _groupsRev = 0;
    // Per conversation, as last written: the hash of its roster record
    // without the read cursors and badges, and of those alone.
    std::vector<uint64_t>    _convHash, _cursorHash;
    std::vector<uint8_t>     _tracked, _checked, _msgDirty; // by ConvRef
    std::unordered_map<std::string, uint64_t> _written;     // file → hash of its bytes
    std::shared_ptr<Disk>                     _disk;
};

} // namespace cache

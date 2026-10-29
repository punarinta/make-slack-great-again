// Store — the one owner of workspace data, and its change notifications.
//
// Backends write into it (see backend.h), views read from it and observe it.
// Everything runs on the UI thread. Notifications are synchronous: a mutator
// returns after every observer ran, so a view can re-read the Store inside
// its callback and see the new state.
//
// The message-list kinds are what a virtual list needs to keep its scroll
// anchor: Append/Prepend carry a count (items added at the end / the start),
// Insert/Update/Remove name one ts (find it with indexOf()).
#pragma once

#include "app/model/types.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace model {

enum class ChangeKind : uint8_t {
    Append,  // `count` messages added at the end of the list
    Prepend, // `count` older messages added at the start (keep the anchor!)
    Insert,  // one message `ts` added in the middle (a backfilled gap)
    Update,  // message `ts` changed (text, reactions, reply count, pending…)
    Remove,  // message `ts` deleted (its index is gone already)
    Reset,   // the whole list was replaced: rebuild
    Meta,    // conversation fields: unread, mentions, starred, name, topic…
    Typing,  // the conversation's typing list changed
    Roster,  // conversations added/removed (conv = kNoConv)
    Users,   // users added or changed (conv = kNoConv)
    // A live reply to a thread that isn't loaded: counted on its root only
    // (no list holds it), yet new — Store::arrived() is the message while
    // the observers run. thread = its root, ts = its ts.
    Arrived,
};

struct Change {
    ChangeKind kind   = ChangeKind::Update;
    ConvRef    conv   = kNoConv;
    Ts         thread = 0; // 0: the conversation's top-level list; else that thread's replies
    Ts         ts     = 0; // Insert / Update / Remove
    uint32_t   count  = 0; // Append / Prepend
};

// Oldest first: a newest-first page (conversations.history) is reversed,
// anything else insertion-sorted (pages are small; one sort for every
// caller, not an introsort each).
void sortByTs(std::vector<Message> &v);

class Store {
public:
    struct Mark { // a flag or time attached to a message (reminders, muted threads)
        ConvRef conv;
        Ts      ts;
        int64_t value; // reminders: due; muted threads: 1
    };
    // A transcript the user's AI made of an audio file: it replaces Slack's
    // transcript line under the player, `by` naming the provider. The
    // workspace cache keeps them.
    struct AiTranscript {
        std::string text, by;
    };
    // An unsent composer message: app-local, the workspace cache keeps it.
    struct Draft {
        ConvRef                  conv   = kNoConv;
        Ts                       thread = 0;
        std::string              html;
        std::vector<std::string> files; // absolute paths
    };
    Store();
    ~Store();
    Store(const Store &)            = delete;
    Store &operator=(const Store &) = delete;

    // ── Workspace ───────────────────────────────────────────────────────────
    std::string workspaceId, workspaceName, workspaceIcon;
    std::string workspaceUrl; // "https://lumen.slack.com/"; may be empty
    UserRef     me             = kNoUser;
    bool        workspaceMuted = false; // no notifications from this workspace

    // Web links, as Slack's "Copy link" makes them (base: workspaceUrl, else
    // slack.com). A reply's link names its thread so it opens there.
    std::string workspaceLink() const;
    std::string conversationLink(ConvRef c) const;
    std::string permalink(ConvRef c, Ts ts, Ts thread = 0) const;

    void clear(); // drop everything (sign-out, workspace switch); fires Reset/Roster/Users

    // ── Users ───────────────────────────────────────────────────────────────
    // Adds or merges (same id keeps its UserRef; a placeholder is filled in).
    UserRef     addUser(User u);
    // The ref for an id, creating a placeholder record if unknown — so a
    // mention or reaction never needs the roster to be loaded first.
    UserRef     internUser(std::string_view id);
    UserRef     findUser(std::string_view id) const; // kNoUser if unknown
    const User &user(UserRef u) const;               // a static empty User for kNoUser
    User       &user(UserRef u);
    size_t      userCount() const { return _users.size(); }
    // Names: full names (true, the default) or display names first (see
    // User::resolveName). A change re-resolves every user and fires Users.
    bool        realNames() const { return _realNames; }
    void        setRealNames(bool on);
    // After changing users in place (presence, status): fires Users.
    void        usersChanged();
    // The same after changing only user u in place: only u is re-hashed
    // (see userRevision).
    void        usersChanged(UserRef u);
    // What a Users change changed, for observers that only care about part
    // of it. Each counter only grows; every Users emit refreshes them first.
    //   profileRevision: any user's profile (all but presence/DND) changed
    //     (or users were added); userRevision(u): the one at u's last change.
    //   presenceRevision: any user's presence or DND changed.
    //   textRevision: custom emoji, user groups or channel names changed
    //     (they draw in names and messages too).
    uint64_t    profileRevision() const { return _profileRev; }
    //   namedRevision: the same, but a placeholder (known by id only)
    //     appearing or changing doesn't move it.
    uint64_t    namedRevision() const { return _namedRev; }
    uint64_t    userRevision(UserRef u) const {
        return u < _userRev.size() ? _userRev[u] : _profileRev;
    }
    uint64_t presenceRevision() const { return _presenceRev; }
    uint64_t textRevision() const { return _textRev; }
    // One thing a textRevision() step changed, so an observer can re-bind
    // only what draws it: a channel name (setChannelName), a custom emoji
    // (added, changed or removed) or a user group.
    struct TextChange {
        enum class Kind : uint8_t { Channel, Emoji, Usergroup };
        Kind        kind = Kind::Channel;
        std::string id; // "C0123"; an emoji name without colons ("party"); "S0123"
    };
    // Appends to *out every change after revision `since` (a textRevision()
    // seen earlier) and returns true; false when they can't be listed (too
    // many, or a change without ids such as clear()): treat all text as
    // changed. Nothing changed since: true with nothing appended.
    bool     textChangesSince(uint64_t since, std::vector<TextChange> *out) const;
    // Grows with every Meta, Roster or Users change: a conversation's
    // displayName() can change with nothing else.
    uint64_t metaRevision() const { return _metaRev; }
    // Grows with every change of the app-local marks the workspace cache
    // keeps in meta.json (muted threads, reminders, AI transcripts): their
    // Update/Meta emits look like any other.
    uint64_t localRevision() const { return _localRev; }
    // Grows with every setUsergroups.
    uint64_t usergroupRevision() const { return _groupRev; }

    // ── Conversations ───────────────────────────────────────────────────────
    ConvRef             addConversation(Conversation c); // merges by id like addUser
    // Many at once (a roster load): the same merges, but one Roster emit for
    // all the new ones (a changed existing one still fires its own Meta).
    void                addConversations(std::vector<Conversation> convs);
    ConvRef             findConversation(std::string_view id) const;
    const Conversation &conversation(ConvRef c) const; // an empty one for a ref it lacks
    Conversation       &conversation(ConvRef c);
    size_t              conversationCount() const { return _convs.size(); }
    // Title for lists and headers: "general", a DM peer's label, "Mira, Jonas".
    std::string         displayName(ConvRef c) const;
    // Changes conversation fields, then fires Meta.
    void updateConversation(ConvRef c, const std::function<void(Conversation &)> &fn);

    // ── Messages ────────────────────────────────────────────────────────────
    // A message by ts, top-level or in any loaded thread; null if absent.
    Message                    *findMessage(ConvRef c, Ts ts);
    const Message              *findMessage(ConvRef c, Ts ts) const;
    // Replies of a loaded thread (null if never loaded).
    const std::vector<Message> *replies(ConvRef c, Ts root) const;
    // Position of ts in the top-level list (thread == 0) or in a thread;
    // SIZE_MAX if absent. Binary search: the lists are sorted by ts.
    size_t                      indexOf(ConvRef c, Ts ts, Ts thread = 0) const;

    // A page of history or thread replies (any order, all in the same list:
    // top-level messages, or replies with the same threadTs). Existing ts are
    // replaced (Update); older ones Prepend, newer ones Append, others
    // Insert. Does not touch unread counts or roots' reply counters — a page
    // is what the server already counted.
    void addPage(ConvRef c, std::vector<Message> page);
    // One live message (a send, a push event). A top-level message bumps
    // `latest` and, when it's someone else's and after lastRead, the unread
    // (and mention) count; a reply updates its root's replyCount,
    // latestReply and replyUsers. Clears that user's typing indicator.
    void addMessage(ConvRef c, Message m);
    // Edits a message in place (top-level or reply), then fires Update.
    bool updateMessage(ConvRef c, Ts ts, const std::function<void(Message &)> &fn);
    bool removeMessage(ConvRef c, Ts ts); // a removed reply also decrements its root
    // Adds / removes `user`'s reaction `name` (count kept in step).
    bool setReaction(ConvRef c, Ts ts, std::string_view name, UserRef user, bool on);
    bool reactedByMe(const Reaction &r) const;

    // Moves the read cursor and recomputes unread/mentions from the loaded
    // messages after it. Fires Meta.
    void                     markRead(ConvRef c, Ts ts);
    // Muted threads get no notifications ("Mute thread"; app-local).
    bool                     threadMuted(ConvRef c, Ts root) const;
    void                     setThreadMuted(ConvRef c, Ts root, bool on); // fires Meta
    // Message reminders ("Remind me"): due time in epoch seconds, 0 = none.
    // Backends keep it here (a reminder is a saved item with a due date).
    int64_t                  reminderAt(ConvRef c, Ts ts) const;
    void                     setReminderAt(ConvRef c, Ts ts, int64_t due);
    // "Mark unread": the read cursor moves back to just before ts (and
    // unread is recounted from there). Fires Meta.
    void                     markUnread(ConvRef c, Ts ts);
    // True if mrkdwn `text` mentions the signed-in user: <@me>, <!here>,
    // <!channel>, <!everyone>, or a <!subteam^S…> group listed in myGroups.
    bool                     mentionsMe(std::string_view text) const;
    std::vector<std::string> myGroups; // user-group ids "me" belongs to
    // Every user group of the workspace (a <!subteam^S…>
    // mention shows "@handle"); setUsergroups also derives myGroups. Fires
    // Users (mentions repaint).
    struct Usergroup {
        std::string              id, handle, name; // handle without '@'
        // Member ids, from an importer that has no `mine` of its own:
        // setUsergroups folds them into it and drops them (groups can be
        // huge, and nothing else reads them).
        std::vector<std::string> users;
        bool                     mine                                = false; // "me" is a member
        bool                     operator==(const Usergroup &) const = default;
    };
    const std::vector<Usergroup> &usergroups() const { return _usergroups; }
    const Usergroup              *findUsergroup(std::string_view id) const;
    void                          setUsergroups(std::vector<Usergroup> groups);
    // Channels a message mentions that the roster doesn't list
    // (filled by Backend::resolveChannel): id → name,
    // "" = it doesn't exist for us. Null: not looked up. Fires Users.
    const std::string            *channelName(std::string_view id) const;
    void                          setChannelName(std::string id, std::string name);
    // Who wrote a message some other message links to (Slack's rich_text
    // message_mention names the author; a permalink URL alone doesn't): the
    // link chip reads "Author in #channel". Keyed
    // by the linked conversation id and its ts as the permalink has it.
    UserRef                       linkedAuthor(std::string_view conv, std::string_view ts) const;
    void           setLinkedAuthor(std::string_view conv, std::string_view ts, UserRef u);
    // The live reply a ChangeKind::Arrived announces (null outside one).
    const Message *arrived() const { return _arrived; }
    // One such reply: fires Arrived (the caller counted it on its root).
    void           announceReply(ConvRef c, const Message &m);
    // Followed threads with unread replies (the
    // sidebar's Threads entry is bright while > 0). Fires Meta (kNoConv).
    int            unreadThreads() const { return _unreadThreads; }
    void           setUnreadThreads(int n);
    // Unread items in the service's activity feed (the sidebar's Activity
    // entry is bright while > 0). Fires Meta (kNoConv).
    int            unreadActivity() const { return _unreadActivity; }
    void           setUnreadActivity(int n);
    // Agent sessions (Claude Code): every answer is meant for me, so each
    // one counts as a mention (the red badge) — except a progress note
    // (subtype "progress": a remark before tool calls, a tool card).
    bool           answersAreMentions = false;
    // Whether an unread message `m` counts as a mention (see above).
    bool           mentions(const Message &m) const;

    // ── Composer drafts ─────────────────────────────────────────────────────
    // The unsent message of (conversation, thread). Setting neither text nor
    // files erases the draft.
    const std::vector<Draft> &drafts() const { return _drafts; }
    const Draft              *draft(ConvRef c, Ts thread) const;
    void setDraft(ConvRef c, Ts thread, std::string html, std::vector<std::string> files);

    // ── Saved messages ──────────────────────────────────────────────────────
    // Message reminders (Slack's "Later" list): every saved message,
    // loaded or not, with what the Saved messages page shows of it. Backends
    // keep it next to Message::saved and reminderAt.
    struct SavedItem {
        ConvRef     conv = kNoConv;
        Ts          ts = 0, thread = 0; // thread: a saved reply's root
        int64_t     due     = 0;        // a reminder's time (epoch secs); 0 = saved for later
        int64_t     savedAt = 0;        // epoch secs; orders the plain bookmarks
        // The preview, from the message once known ("" = not yet): its mrkdwn,
        // its author, or a bot post's name and picture.
        std::string text;
        UserRef     author = kNoUser;
        std::string botName, botAvatar;
        bool        previewed = false; // the message was found (text may still be "")
        // The reminder went off: it stays
        // listed, it doesn't go off again; a new due time re-arms it.
        bool        fired     = false;
    };
    // Reminders soonest due first, then the bookmarks newest saved first.
    const std::vector<SavedItem> &savedList() const { return _saved; }
    const SavedItem              *findSaved(ConvRef c, Ts ts) const;
    bool                          hasSaved() const { return !_saved.empty(); }
    // Adds or updates an item (keeping its savedAt and preview unless given)
    // or removes it (on = false); the preview comes from the message when it
    // is loaded. savedAt 0 = now for a new item. Fires Update for ts.
    void setSavedItem(ConvRef c, Ts ts, bool on, int64_t due = 0, int64_t savedAt = 0);
    // The preview of an item from its message (fetched: Backend::loadMessage;
    // null = it is gone). Fires Update for ts.
    void setSavedPreview(ConvRef c, Ts ts, const Message *m);
    // A reminder went off (or was found long overdue). Fires Update for ts.
    void setReminderFired(ConvRef c, Ts ts, bool fired);

    // ── Scheduled messages ──────────────────────────────────────────────────
    // Messages waiting on the service to post them (Slack's "Scheduled"),
    // as the backend last listed them: the sidebar's "Scheduled messages"
    // entry shows while there are any.
    struct ScheduledItem {
        std::string id;      // the service's (a draft id or a scheduled message id)
        std::string version; // what cancelling it needs besides the id ("" = none)
        ConvRef     conv        = kNoConv;
        Ts          thread      = 0;    // a reply's thread
        bool        threadKnown = true; // false: the service doesn't say (Send now is off)
        int64_t     at          = 0;    // epoch secs
        std::string text;               // mrkdwn
    };
    // Soonest first.
    const std::vector<ScheduledItem> &scheduled() const { return _scheduled; }
    bool                              hasScheduled() const { return !_scheduled.empty(); }
    // Replaces the list (sorted here); fires Meta with conv = kNoConv when it
    // changed.
    void                              setScheduled(std::vector<ScheduledItem> items);
    // Drops one (cancelled or sent from here); fires Meta like setScheduled.
    void                              removeScheduled(const std::string &id);

    // ── Sidebar sections ────────────────────────────────────────────────────
    // The user's own sidebar sections (Slack's custom sections), top to
    // bottom: each lists its conversations by id, in the order they were
    // put there. A conversation is in at most one, and a section's rows
    // replace the Starred / Channels / Direct messages ones. Only this user
    // sees them.
    struct Section {
        std::string              id, name;
        std::string              emoji; // shortcode without colons ("" = none)
        std::vector<std::string> convs;
        bool                     operator==(const Section &) const = default;
    };
    const std::vector<Section> &sections() const { return _sections; }
    // The one holding conversation `convId` (null: none).
    const Section              *sectionOf(std::string_view convId) const;
    const Section              *findSection(std::string_view id) const;
    // Replaces the list; fires Meta with conv = kNoConv when it changed.
    void                        setSections(std::vector<Section> sections);
    // Grows with every change of the list.
    uint64_t                    sectionsRevision() const { return _sectionsRev; }

    // ── "Transcribe with AI" ────────────────────────────────────────────────
    // AI transcripts of audio files (app-local: Slack has no write API
    // for a file's transcript). Setting one fires Update for every loaded
    // message with the file.
    const AiTranscript *aiTranscript(const std::string &fileId) const;
    void setAiTranscript(const std::string &fileId, std::string text, std::string by);
    const std::unordered_map<std::string, AiTranscript> &aiTranscripts() const {
        return _aiTranscripts;
    }

    // ── Typing ──────────────────────────────────────────────────────────────
    // sinceMs: an agent "thinking" since then (Typing::sinceMs); 0 = typing.
    void setTyping(ConvRef c, UserRef u, Ts thread, bool on, int64_t sinceMs = 0);
    const std::vector<Typing> &typing(ConvRef c) const;

    // ── Custom emoji ────────────────────────────────────────────────────────
    // name → image path/URL, or "alias:other".
    void setCustomEmoji(std::string name, std::string value);
    // The whole set at once (emoji.list): a name not in it is gone.
    void replaceCustomEmoji(std::unordered_map<std::string, std::string> all);
    struct CustomEmoji {
        std::string_view name, image;
    };
    // Every custom emoji that is an image (not an alias), sorted by name, and
    // every name (aliases too), sorted: built once per change of the set and
    // valid until the next one (a revision later).
    const std::vector<CustomEmoji>      &customEmojiImages() const;
    const std::vector<std::string_view> &customEmojiNames() const;
    // Grows with every change of the set.
    uint64_t                             customEmojiRevision() const { return _emojiRev; }
    // Everything set above, aliases included (the workspace cache saves it).
    const std::unordered_map<std::string, std::string> &customEmoji() const { return _customEmoji; }
    // The marks as kept (the workspace cache saves them).
    const std::vector<Mark> &mutedThreads() const { return _mutedThreads; }
    const std::vector<Mark> &reminders() const { return _reminders; }
    struct EmojiGlyph {
        std::string unicode; // non-empty: draw these code points
        std::string image;   // non-empty: a custom emoji image
        bool        resolved() const { return !unicode.empty() || !image.empty(); }
    };
    // Resolves a shortcode in this order: skin-tone
    // suffixes, the Unicode table, a raw glyph passed as a name (Teams),
    // then custom emoji with alias chains (bounded).
    EmojiGlyph emojiFor(std::string_view name) const;

    // ── Observers ───────────────────────────────────────────────────────────
    using Observer                    = std::function<void(const Change &)>;
    using ObserverId                  = uint32_t;
    static constexpr ConvRef kAnyConv = kNoConv - 1;
    // conv = one conversation's changes (+ Roster/Users, which concern all),
    // or kAnyConv for everything. Safe to call from inside a callback.
    ObserverId               observe(ConvRef conv, Observer fn);
    void                     unobserve(ObserverId id); // safe inside a callback, even its own

private:
    Thread               *findThread(Conversation &c, Ts root);
    const Thread         *findThread(const Conversation &c, Ts root) const;
    std::vector<Message> *listFor(Conversation &c, Ts thread, bool create);
    void                  recountUnread(Conversation &c);
    void                  emit(const Change &ch);
    void                  noteUserRevisions(); // before a Users emit
    ConvRef               mergeConversation(Conversation c, bool *added);
    void                  sortSaved();
    void                  logText(TextChange::Kind kind, std::string id);
    void                  textUnlisted(); // a step whose changes have no ids

    std::vector<User>                            _users;
    std::unordered_map<std::string, UserRef>     _userIndex;
    std::vector<Conversation>                    _convs;
    std::unordered_map<std::string, ConvRef>     _convIndex;
    std::vector<std::vector<Typing>>             _typing; // by ConvRef
    std::unordered_map<std::string, std::string> _customEmoji;
    // The sorted views of _customEmoji, rebuilt on first use after a change.
    mutable std::vector<CustomEmoji>             _emojiImages;
    mutable std::vector<std::string_view>        _emojiNames;
    mutable bool                                 _emojiSorted = true;
    uint64_t                                     _emojiRev    = 0;
    void                                         customEmojiChanged();
    void                                         sortCustomEmoji() const;
    std::vector<Mark>                            _mutedThreads, _reminders; // few: linear is fine
    std::vector<Draft>                           _drafts;                   // few: linear is fine
    std::vector<SavedItem>                       _saved;
    std::vector<ScheduledItem>                   _scheduled;
    std::vector<Section>                         _sections;
    uint64_t                                     _sectionsRev = 0;
    std::vector<Usergroup>                       _usergroups;
    std::unordered_map<std::string, std::string> _channelNames;
    std::unordered_map<std::string, UserRef>     _linkedAuthors; // "conv/ts"
    const Message                               *_arrived        = nullptr;
    int                                          _unreadThreads  = 0;
    int                                          _unreadActivity = 0;
    bool                                         _realNames      = true;

    std::unordered_map<std::string, AiTranscript> _aiTranscripts;

    // noteUserRevisions' memory: per user, the hashes last seen and the
    // profile revision of its last change.
    std::vector<uint64_t> _profileHash, _presenceHash, _userRev;
    uint64_t              _profileRev = 0, _namedRev = 0, _presenceRev = 0, _textRev = 0;
    uint64_t              _metaRev  = 0;
    uint64_t              _localRev = 0, _groupRev = 0;
    // Users touched since the last Users emit (addUser, usersChanged(u));
    // _allUsersDirty: changed in place without saying who (re-hash all).
    std::vector<UserRef>  _touchedUsers;
    bool                  _allUsersDirty = true;
    // textChangesSince's memory: the newest changes with their revisions;
    // anything at or below _textLogFloor is no longer listed.
    struct LoggedText {
        uint64_t   rev;
        TextChange change;
    };
    std::vector<LoggedText> _textLog;
    uint64_t                _textLogFloor = 0;

    struct Slot {
        ObserverId id;
        ConvRef    conv;
        Observer   fn;
    };
    std::vector<Slot> _observers;
    std::vector<Slot> _joining; // observe() calls made during dispatch
    ObserverId        _nextObserver = 1;
    int               _dispatching  = 0; // nesting depth of emit()
    bool              _needsCompact = false;
};

} // namespace model

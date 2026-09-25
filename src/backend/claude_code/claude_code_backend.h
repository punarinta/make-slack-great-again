// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Claude Code as a messaging backend (docs/backend-modules-plan.md §5, §10):
// every Claude Code session on this machine is a DM with its own assistant
// user, so each session gets its own status dot, unread state and notifications.
//
//   • Roster: ~/.claude/sessions/*.json (interactive sessions and background
//     workers) and ~/.claude/jobs/*/state.json (background sessions), watched,
//     plus the ended sessions msga has seen, kept until Claude Code drops their
//     transcript (known-sessions file in app data).
//   • Messages: the session transcripts, tailed while a session is live.
//   • Writing: a session a terminal or another program drives is read-only.
//     Everything msga sends goes through BACKGROUND sessions (Launcher), which
//     Claude Code's daemon runs — they survive msga crashing or quitting. A
//     message sent while Claude is still on a turn waits for the turn to end.
//
// Nothing runs until this backend is constructed, i.e. until a Claude Code
// workspace exists (§4.5).
#pragma once

#include "backend/backend.h"
#include "backend/claude_code/cc_launcher.h"
#include "backend/claude_code/cc_roles.h"
#include "backend/claude_code/cc_roster.h"
#include "backend/claude_code/cc_transcript.h"
#include "backend/claude_code/claude_code_auth.h"
#include "rpl/variable.h"

#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <memory>

class QFileSystemWatcher;
class QObject;
class QTimer;

namespace claude_code {

class Backend : public ::Backend {
public:
    explicit Backend(const Credentials &creds);
    ~Backend() override;

    // --- Lifecycle ---
    rpl::producer<AuthState> authState() const override;
    Capabilities             capabilities() const override;
    void                     connectRealtime() override;
    void                     disconnectRealtime() override;
    // Local files, re-read on change: no reason to poll the open chat often.
    int                      foregroundPollGapMs() const override { return 60'000; }

    bool isBotId(UserId id) const override;
    bool isUserId(UserId id) const override;

    // --- Snapshot loads ---
    rpl::producer<UserId>                    loadMe() override;
    rpl::producer<std::vector<Conversation>> loadConversations() override;
    rpl::producer<std::vector<User>>         loadUsers() override;
    rpl::producer<bool>                      loadPresence(UserId) override;
    rpl::producer<Conversation> loadConversationInfo(ConversationId, bool background) override;
    rpl::producer<MessagePage>  loadHistory(ConversationId, std::optional<QString>) override;
    rpl::producer<MessagePage>  loadThread(ConversationId, Ts, std::optional<QString>) override;

    // --- Commands ---
    void sendMessage(
        ConversationId, OutgoingMessage, std::function<void(bool ok, QString err)> done = {}
    ) override;
    void editMessage(ConversationId, Ts, OutgoingMessage) override {}
    // Takes a prompt or an answer out of the session's transcript, so Claude
    // doesn't have it either when the session goes on (removeFromTranscript).
    void deleteMessage(ConversationId, Ts) override;
    void addReaction(ConversationId, Ts, QString) override {}
    void removeReaction(ConversationId, Ts, QString) override {}
    void markRead(ConversationId, Ts) override;
    // "Remove from msga": hides the session here — Claude Code keeps it — and
    // stops it if it's a background one (with all it runs, see Launcher::stop).
    // It comes back if it gets new activity after that (resumed elsewhere).
    void leaveConversation(ConversationId) override;
    // "Rename session…": a name only msga shows (Claude Code keeps its own). It
    // goes on the session's user, which titles the DM everywhere.
    void setConversationLocalName(ConversationId, const QString &name) override;
    // Zen mode hides the tool-call cards.
    void setZenMode(bool on) override;

    // --- Your profile: a name and a picture, kept in msga's app data ---
    void loadMyProfile(std::function<void(MyProfile)> done) override;
    void updateProfile(
        const QHash<QString, QString> &fields, std::function<void(bool ok, QString err)> done
    ) override;
    void setPhoto(
        const QString &filePath, std::function<void(bool ok, QString err, QString url)> done
    ) override;
    void startAgentSession(
        const QString                      &directory,
        bool                                skipPermissionChecks,
        const QString                      &role,
        std::function<void(ConversationId)> onSuccess,
        std::function<void(QString)>        onError
    ) override;
    QString agentSessionBlocker(const QString &directory) override {
        return cannotStartIn(directory);
    }
    // The team (cc_roles): the generalist, the specialists, and teammates the
    // user added — editable, kept in msga's app data.
    std::vector<AgentRole> agentRoles() override;
    QString                saveAgentRole(const AgentRole &role, QString *error) override;
    void                   removeAgentRole(const QString &id) override;
    void                   restoreAgentRole(const QString &id) override;
    // "Find a session": every transcript Claude Code has, from every folder
    // (cc_catalog), read on a worker thread.
    void           findAgentSessions(std::function<void(std::vector<FoundSession>)> done) override;
    ConversationId addFoundSession(const QString &sessionId) override;
    // "Stop": a background session's worker is stopped (`claude stop`) with all
    // it runs, mid-turn, waiting on an approval or idle. Sessions a terminal or
    // another program drives are theirs to stop.
    bool           canStopAgentSession(ConversationId) override;
    void           stopAgentSession(ConversationId) override;

    // --- Search / emoji / files ---
    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &query) override;
    void                                     uploadFiles(
        ConversationId,
        const QStringList &,
        const QString &,
        std::optional<Ts>,
        std::function<void(bool, QString)> done
    ) override;
    void downloadFile(
        const QString &, std::function<void(QByteArray)>, std::function<void(QString)>
    ) override;

    // Claude Code's commands for the session's folder (its project and skills
    // count), asked of Claude Code and kept for a few minutes — plus msga's
    // own /btw.
    std::vector<SlashCommand> conversationCommands(ConversationId) override;
    // /status: what msga knows of the session and the login, for a dialog.
    // /clear: a fresh session in the same folder, with the same permission
    // setting (the old one stays as it is).
    LocalCommandResult
         runLocalCommand(ConversationId, const QString &name, const QString &args) override;
    // A /btw thread is a session branched off this one: replies continue it.
    bool threadAcceptsReplies(ConversationId, Ts root) override;
    bool threadOpensAsSession(ConversationId, Ts root) override;
    // Not while the session is working or driven from elsewhere, nor tool calls
    // or history a /btw thread shares.
    bool canDeleteMessage(ConversationId, Ts) override;
    // "Open as session": the branched session gets its own place in the list.
    ConversationId openThreadAsSession(ConversationId, Ts root) override;

    rpl::producer<Event> events() const override;

private:
    struct Tracked;

    User me() const;
    void loadProfile();
    void saveProfile() const;

    void     refresh();
    void     scheduleRefresh();
    void     loadKnown();
    void     saveKnown();
    void     scheduleSaveKnown();
    Tracked &ensureTracked(const QString &convId);
    Tracked *find(const QString &convId);
    QString  convIdFor(const QString &sessionId) const;
    void     tail(Tracked &t);
    void     diffAndAnnounce(Tracked &t);
    void     hideSession(const QString &convId); // "Remove from msga", one session
    // Stop a background session that was removed from msga, keeping it hidden.
    void     stopRemoved(const QString &sessionId, const QString &cwd);
    std::vector<std::pair<QString, QString>> conversationStatus(Tracked &t);
    // Why no session can be started in `dir` ("" = it can), and a new one there
    // (a "+" session: it starts with its first message).
    QString                                  cannotStartIn(const QString &dir) const;
    Conversation  createSession(const QString &dir, bool skipPermissionChecks, const QString &role);
    QString       roleOf(const Tracked &t) const;  // its teammate's role id
    Role          roleFor(const Tracked &t) const; // …and the teammate, as shown
    QStringList   roleIds() const;                 // every role a session may have
    void          teamChanged(const QString &id);
    bool          roleBusy(const QString &role) const;
    bool          roleUnavailable(const QString &role) const; // …or yellow
    User          teammateUser(const Role &r) const;
    // Branched sessions (/btw threads): see detectForks.
    QSet<QString> detectForks(); // parents whose threads changed
    const Message       &renderedAt(Tracked &t, size_t i);
    bool                 asThread(const Tracked &t) const;
    Tracked             *forkFor(const QString &parentConv, const Ts &root);
    std::vector<Message> threadMessages(Tracked &fork);
    std::vector<Message> shownMessages(Tracked &t); // thread replies for a fork
    // msga's copies of the messages sent but not in the transcript yet.
    void                 appendOutgoing(
        const Tracked &t, std::vector<Message> &out, const std::optional<Ts> &threadRoot
    ) const;
    bool     isOutgoingCopy(const Tracked &t, const Ts &ts) const;
    Tracked *queuedHolder(const ConversationId &conv, const Ts &ts, int *index);
    void
    startFork(Tracked &parent, const QString &question, std::function<void(bool, QString)> done);
    void                  dispatch(Tracked &t);
    bool                  typesLive(const Tracked &t) const;
    void                  typeLive(Tracked &t);
    void                  failSends(Tracked &t, const QString &reason);
    void                  stopWorker(Tracked &t);
    // The chat goes on in session `copyId`, a copy Claude Code made of its own.
    void                  adoptCopy(Tracked &t, const QString &copyId);
    void                  watchLive();
    void                  pumpTyping();
    std::vector<Message>  visibleMessages(Tracked &t);
    Conversation          conversationFor(const Tracked &t) const;
    User                  assistantUser(const Tracked &t) const;
    QString               titleOf(const Tracked &t) const;          // Claude Code's name for it
    QString               shownTitle(const Tracked &t) const;       // the user's name, else titleOf
    QString               teammateNames(const QString &text) const; // "@claude:role:x" → "@X"
    void                  announceChanged(Tracked &t);
    QString               readOnlyReason(const Tracked &t) const;
    bool                  busy(const Tracked &t) const;
    bool                  needsUser(const Tracked &t) const;
    const TranscriptItem *deletableItem(Tracked &t, const Ts &ts);
    int                   subagentReplyCount(const Tracked &t, const QString &agentId, Ts *latest);
    QString subagentOf(const Tracked &t, const Ts &root) const; // "" = none/not started

    Credentials                              _creds;
    Paths                                    _paths;
    Team                                     _team;
    UserId                                   _me{QStringLiteral("me")};
    rpl::variable<AuthState>                 _authState = AuthState::LoggedIn;
    mutable rpl::event_stream<Event>         _events;
    // By conversation id — the session id, except for a session started with
    // "+": its conversation exists before Claude Code picks the session's id on
    // the first message, so it keeps its own id and _convOf maps the session to it.
    QHash<QString, std::shared_ptr<Tracked>> _sessions;
    QHash<QString, QString>                  _convOf; // session id → conversation id
    // Sessions removed from msga, by session id: when, and the transcript whose
    // later growth brings the session back (forgotten once Claude Code drops it).
    struct Hidden {
        qint64  atMs = 0;
        QString transcript;
        qint64  seenSize = -1;    // transcript bytes already known not to be new activity
        bool    stopping = false; // its worker is being stopped (stopRemoved)
    };
    QHash<QString, Hidden> _hidden;
    QObject               *_ctx                = nullptr; // owns the Qt objects
    Launcher              *_launcher           = nullptr;
    QFileSystemWatcher    *_watcher            = nullptr;
    QTimer                *_debounce           = nullptr;
    QTimer                *_safetyPoll         = nullptr;
    QTimer                *_saveKnownTimer     = nullptr;
    QTimer                *_typingTimer        = nullptr;
    bool                   _started            = false;
    bool                   _firstScanDone      = false;
    // Typing into live workers (typeLive): misses in a row, and off until when.
    int                    _typeLiveMisses     = 0;
    qint64                 _typeLiveOffUntilMs = 0;
    bool                   _zen                = false;
    QString                _myName;       // "" = the login name
    QString                _myAvatarPath; // a copy in app data; "" = initials
    // Subagent transcripts are only re-parsed when they grow.
    struct SubagentCount {
        qint64 size     = -1;
        int    count    = 0;
        int    zenCount = 0; // without the tool-call cards
        Ts     latest;
    };
    QHash<QString, SubagentCount> _subagentCounts; // by transcript path
    struct CommandList {
        std::vector<SlashCommand> commands;
        qint64                    fetchedMs = 0;
        bool                      loading   = false;
    };
    QHash<QString, CommandList> _commands;        // by session folder
    QSet<QString>               _forkingIn;       // folders a /btw is being launched in
    QHash<QString, bool>        _roleBusy;        // by role: announced as working
    QHash<QString, bool>        _roleUnavailable; // by role: announced as yellow
    QJsonObject                 _account;         // the login, as Claude Code last reported it
};

// Whether Claude Code trusts `dir` (it or a parent folder was accepted in its
// trust prompt) — background sessions refuse untrusted folders. Reads the
// "projects" map of Claude Code's global config (~/.claude.json, or
// $CLAUDE_CONFIG_DIR/.claude.json when that is set).
bool isFolderTrusted(const QString &dir);

} // namespace claude_code

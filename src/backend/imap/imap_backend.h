// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Email (IMAP) backend, below the Backend seam. Phase 1: read path only —
// connects/logs in via imap::ImapClient, scans INBOX (ENVELOPE + server THREAD),
// runs the Model-D bucketing engine to derive DM/MPDM/list conversations, and
// serves message bodies (MIME-parsed) for loadHistory. No realtime, no send yet
// (IDLE = Phase 3; SMTP send/markRead mutations = Phase 4). Capabilities reported:
// threads / fileUpload / deleteMessage / messageSubjects.
#pragma once

#include "backend/backend.h"
#include "backend/imap/imap_auth.h"
#include "backend/imap/imap_bucketing.h"
#include "backend/imap/imap_compose.h" // OutgoingAttachment
#include "rpl/event_stream.h"
#include "rpl/variable.h"

#include <QHash>
#include <QSet>
#include <functional>
#include <vector>

class QTimer;

namespace imap {

class ImapClient;
class BimiResolver;
class SmtpClient;

class Backend : public ::Backend {
public:
    explicit Backend(const Credentials &creds);
    ~Backend() override;

    // --- Lifecycle ---
    rpl::producer<AuthState> authState() const override;
    Capabilities             capabilities() const override;
    void                     connectRealtime() override;
    void                     disconnectRealtime() override;

    // --- Snapshot loads ---
    rpl::producer<UserId>                    loadMe() override;
    rpl::producer<std::vector<Conversation>> loadConversations() override;
    rpl::producer<std::vector<User>>         loadUsers() override;
    rpl::producer<bool>                      loadPresence(UserId) override;
    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString>) override;
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override;

    // --- Commands ---
    void sendMessage(ConversationId, OutgoingMessage) override;      // Phase 4 (stub)
    void editMessage(ConversationId, Ts, TextWithEntities) override; // not supported
    void deleteMessage(ConversationId, Ts) override;
    bool channelsAreLabels() const override { return true; } // channels = labels/folders
    void labelMessage(
        ConversationId                     sourceConv,
        Ts                                 ts,
        ConversationId                     targetChannel,
        std::function<void(bool, QString)> done
    ) override;
    void addReaction(ConversationId, Ts, QString) override {}    // n/a
    void removeReaction(ConversationId, Ts, QString) override {} // n/a
    void markRead(ConversationId, Ts) override;

    // --- Search / emoji / files ---
    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override;
    rpl::producer<QHash<QString, QString>>   loadEmojiList() override;
    void                                     uploadFiles(
                                            ConversationId,
                                            const QStringList &,
                                            const QString &,
                                            std::function<void(bool, QString)> done
                                        ) override; // Phase 4 (stub)
    void downloadFile(
        const QString &, std::function<void(QByteArray)>, std::function<void(QString)>
    ) override; // Phase 6 (stub)

    rpl::producer<Event> events() const override;

private:
    void whenReady(std::function<void()> fn);
    void flushReady();
    // Run `fn` once the initial INBOX scan has populated _index / _folderMailbox
    // (or immediately if it already has / the connection failed). loadHistory
    // waits on this so opening a conversation before the scan finishes doesn't
    // read a half-built index and wrongly report an empty history.
    void whenScanned(std::function<void()> fn);
    void markScanned(); // set _scanned + flush _scanWaiters (idempotent)

    // Authenticate `c` with whatever the credentials specify: classic LOGIN, or
    // XOAUTH2 when authMethod is OAuth (refreshing the access token first if it
    // has expired). Used for both the command and IDLE connections.
    void               loginClient(ImapClient *c);
    // Refresh the OAuth access token if it is missing/expired, then run `then`.
    // For password auth (or when nothing needs refreshing) runs `then` at once.
    void               ensureFreshToken(std::function<void()> then);
    [[nodiscard]] bool usingOAuth() const;

    // Full conversation scan: LIST → folder/label channels, then INBOX →
    // participant-bucketed DMs/MPDMs, combined. Builds _index + _folderMailbox.
    void scan(std::function<void(std::vector<Conversation>)> done);
    void scanInboxThen(
        std::vector<Conversation>                      folderChannels,
        std::function<void(std::vector<Conversation>)> done
    );
    // Fetch BODY[] for `refs` and emit a MessagePage. The page (MIME + HTML
    // parsing) is built on a worker thread; when `collapse` is true each ref is a
    // channel thread-root with a bold subject title + reply count/latest-reply.
    // Defined in the .cpp (only used there).
    template <typename Consumer>
    void fetchBodiesAndEmit(
        const QList<MsgRef>           &refs,
        bool                           collapse,
        const QHash<QString, int>     &replyCountOf,
        const QHash<QString, QString> &latestReplyOf,
        const QString                 &olderCursor,
        Consumer                       consumer
    );

    // Older-history paging: how to find a conversation's messages in its mailbox
    // (UID SEARCH criteria), for the cases we page (DMs + folder channels).
    struct Pagination {
        QString    mailbox;
        QByteArray criteria; // IMAP SEARCH criteria selecting the conv's messages
        bool       supported = false;
    };
    Pagination paginationFor(const QString &convId) const;

    // Shared send path for plain sends + file uploads (SMTP submit + APPEND + echo).
    void submitMail(
        ConversationId                            conv,
        const QString                            &body,
        std::optional<Ts>                         threadRoot,
        const QString                            &subject,
        const QList<OutgoingAttachment>          &attachments,
        std::function<void(bool ok, QString err)> done
    );

    // BIMI avatars: request brand-logo resolution for the domains of `users`, and
    // on success upgrade matching users' avatarUrl (fires EvUserChanged).
    void    resolveBimiForUsers(const QHash<QString, User> &users);
    void    onBimiResolved(const QString &domain, const QString &logoUrl);
    // Look up a message's UID within a conversation by its ts (messageId key).
    quint32 uidForTs(const QString &convId, const Ts &ts) const;
    // The mailbox a message's UID is valid in (UIDs are per-mailbox; one
    // conversation can span folders — INBOX + Sent, …). Falls back to "INBOX".
    QString mailboxForTs(const QString &convId, const Ts &ts) const;

    // --- Realtime (IDLE) ---
    void         beginIdle();      // (re)enter IDLE on INBOX + (re)arm the refresh timer
    void         onIdleActivity(); // EXISTS push → stop IDLE, fetch new, resume
    void         fetchNewMail();   // fetch messages with UID >= _idleUidNext → EvMessageNew
    // Build a Conversation for a convId not yet in _index (for EvChannelCreated).
    Conversation conversationFor(
        const QString        &convId,
        ConvKind              kind,
        const QList<QString> &participants,
        const QString        &listId
    ) const;

    Credentials              _creds;
    ImapClient              *_client = nullptr;
    QSet<QString>            _myAddresses; // lowercased: user + aliases
    rpl::variable<AuthState> _authState;
    rpl::event_stream<Event> _events;

    bool                               _ready  = false;
    bool                               _failed = false;
    std::vector<std::function<void()>> _pendingReady;

    // The INBOX scan delivers ~hundreds of users at once. They reach the UI in
    // ONE bulk loadUsers() emission (one cache write + one rebuild) rather than a
    // per-user EvUserChanged storm — that storm froze the UI on startup, since
    // Session persists + rebuilds on every EvUserChanged. _scanWaiters also gates
    // loadHistory (via whenScanned) so a conversation opened mid-scan waits for
    // the index instead of reporting an empty history.
    bool                               _scanned = false;
    std::vector<std::function<void()>> _scanWaiters;

    QHash<QString, ConvData> _index;         // ConversationId.value → data (DM/MPDM/list)
    QHash<QString, QString>  _folderMailbox; // "folder:<name>".value → IMAP mailbox name
    QHash<QString, User>     _users;         // email → User
    QString                  _sentMailbox;   // the \Sent folder (for APPEND), from LIST

    // Realtime: a dedicated second connection that IDLEs on INBOX (the command
    // connection can't IDLE and serve fetches at once). See connectRealtime().
    ImapClient *_idleClient  = nullptr;
    QTimer     *_idleRefresh = nullptr;
    quint32     _idleUidNext = 0; // INBOX UIDNEXT baseline; UID >= this is "new"

    BimiResolver *_bimi = nullptr; // brand-logo (BIMI) avatar resolver
};

} // namespace imap

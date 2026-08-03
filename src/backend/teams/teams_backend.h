// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/backend.h"
#include "backend/teams/graph_client.h"
#include "backend/teams/teams_auth.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"

#include <QHash>
#include <QSet>
#include <functional>
#include <vector>

class QTimer;

namespace teams {

// Microsoft Teams backend over Microsoft Graph (delegated), below the Backend
// seam. Reuses net::HttpQueue via teams::GraphClient.
//
// Implements: auth + token refresh, capabilities, loadMe/loadUser/loadUsers
// (with authenticated avatar photos), conversations (chats + team channels),
// history/threads, send/edit/delete/react (reply-aware), search, presence,
// open-DM, profile read + write (display name/title/phone, photo), self
// presence/status/DND (Presence.ReadWrite / User.ReadWrite). Realtime is
// delta-style polling (no public websocket push exists for Teams messages — see
// §5.4 and the realtime spike): opened conversations are polled for new messages,
// and the conversation list is refreshed periodically so channel/team
// add/rename/delete propagate.
// Still stubbed (TODO): uploadFiles, markRead. Not implemented (no clean delegated
// Graph path that fits the Slack-shaped UI): createChannel, joinChannel,
// scheduleMessage, pinMessage, starConversation — see the note in the .cpp.
class Backend : public ::Backend {
public:
    explicit Backend(const Credentials &creds, const AppConfig &appCfg = appConfig());
    ~Backend() override;

    // --- Lifecycle ---
    rpl::producer<AuthState> authState() const override;
    Capabilities             capabilities() const override;
    void                     connectRealtime() override;
    void                     disconnectRealtime() override;

    // Teams ids are opaque GUIDs with no per-kind prefix; treat every non-empty
    // id as a resolvable user so Session::fetchUserIfNeeded resolves message
    // authors via loadUser. (App/bot senders have an empty author + botName, so
    // they don't trigger a lookup.)
    bool isUserId(UserId) const override;

    QString teamUrl() const override { return _teamUrl; }

    rpl::producer<User> loadUser(UserId) override;

    // --- Snapshot loads ---
    rpl::producer<UserId>                    loadMe() override;
    rpl::producer<std::vector<Conversation>> loadConversations() override;
    rpl::producer<std::vector<User>>         loadUsers() override;
    rpl::producer<bool>                      loadPresence(UserId) override;
    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString> cursor) override;
    rpl::producer<MessagePage>
    loadThread(ConversationId, Ts root, std::optional<QString> cursor) override;

    // --- Commands ---
    void sendMessage(ConversationId, OutgoingMessage) override;
    void editMessage(ConversationId, Ts, TextWithEntities) override;
    void deleteMessage(ConversationId, Ts) override;
    void addReaction(ConversationId, Ts, QString emoji) override;
    void removeReaction(ConversationId, Ts, QString emoji) override;
    void markRead(ConversationId, Ts) override;

    // --- Self presence / status / profile (need Presence.ReadWrite / User.ReadWrite) ---
    void setPresence(bool away, std::function<void(bool ok, QString err)> done) override;
    void setStatus(
        const QString                            &emoji,
        const QString                            &text,
        qint64                                    expirationTs,
        std::function<void(bool ok, QString err)> done
    ) override;
    void setDndSnooze(int minutes, std::function<void(bool ok, QString err)> done) override;
    void
    loadMyProfile(std::function<void(MyProfile)> done) override; // loadPresence: snapshot loads
    void updateProfile(
        const QHash<QString, QString> &fields, std::function<void(bool ok, QString err)> done
    ) override;
    void setPhoto(
        const QString                                                  &filePath,
        std::function<void(bool ok, QString err, QString newAvatarUrl)> done
    ) override;
    void openDm(
        UserId, std::function<void(ConversationId)> onSuccess, std::function<void(QString)> onError
    ) override;

    // (No nativeCommands() override — Teams exposes no slash commands of its own;
    // the default Backend::nativeCommands() returns none. See teams_backend.cpp.)

    // --- Search / emoji / files ---
    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &query) override;
    rpl::producer<QHash<QString, QString>>   loadEmojiList() override;
    void                                     uploadFiles(
                                            ConversationId,
                                            const QStringList                          &filePaths,
                                            const QString                              &initialComment,
                                            std::optional<Ts>                           threadRoot,
                                            std::function<void(bool ok, QString error)> done
                                        ) override;
    void downloadFile(
        const QString                  &url,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError
    ) override;

    // --- Events ---
    rpl::producer<Event> events() const override;

private:
    void setupTokenRefresh();
    void doRefresh(std::function<void(bool)> done);
    // Proactive refresh: a periodic wall-clock check (Qt monotonic timers pause
    // during system suspend, so a single long timer fires late after sleep — a
    // periodic check recovers promptly on resume). Refreshes when the token is
    // within a small margin of expiry so user-facing calls never 401.
    void maybeProactiveRefresh();

    // Realtime via delta-style polling. No public websocket path exists for Teams
    // *messages* (the GA socket.io endpoint is drive/list only; the chatMessage
    // websocket is undocumented + its client SDK was removed — see the realtime
    // spike). So connectRealtime() polls the newest messages of every conversation
    // the user has opened this session and emits EvMessageNew for any that arrived
    // since last seen — near-real-time for the conversation in view. (Limitation:
    // conversations never opened this session, and live thread replies, aren't
    // polled; they refresh on next open / full reload.)
    void pollTracked();
    void trackConversation(const ConversationId &conv, const MessagePage &page);

    // Fetch a user's Graph photo (authenticated — ImageCache can't, it's
    // unauthenticated public-URL only) and hand back a `data:` URI ImageCache can
    // load inline, or an empty string if the user has no photo. Graph exposes no
    // public avatar URL, so this is how Teams avatars reach the avatar pipeline.
    void fetchPhoto(const QString &userId, std::function<void(QString)> cb);

    // Resolve a message's images so the already-shown message gains previews, via
    // one EvMessageChanged once all resolve: (a) inline <img> (Teams hostedContents)
    // → authenticated download → data URI → appended image Files; (b) image file
    // *attachments* (SharePoint contentUrl, which the Graph token can't fetch) →
    // the Graph shares API → a public thumbnail/download URL set on the File.
    // No-op when the message has neither.
    void resolveMessageMedia(const ConversationId &conv, const Message &msg);

    // Graph path of a single message for edit/delete/react. A channel *reply* lives
    // under ".../messages/{root}/replies/{id}" — _replyParent (populated from
    // loadThread) maps a known reply id to its root so those actions address it
    // correctly; an unknown id is treated as a top-level message.
    QString messageItemPath(const ConversationId &conv, const Ts &ts) const;

    Credentials              _creds;
    AppConfig                _app;
    GraphClient             *_client;
    QString                  _teamUrl;
    rpl::variable<AuthState> _authState;
    rpl::event_stream<Event> _events;

    bool                                   _refreshInProgress = false;
    std::vector<std::function<void(bool)>> _refreshWaiters;
    QTimer                                *_proactiveRefreshTimer = nullptr;

    QTimer                 *_pollTimer = nullptr;
    int                     _pollTicks = 0; // for the periodic conv-list refresh
    QSet<QString>           _tracked;       // conv ids opened this session
    QHash<QString, qint64>  _lastSeen;      // conv id → newest message date seen/emitted
    QHash<QString, QString> _replyParent;   // channel reply id → its thread root id
};

} // namespace teams

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/backend.h"
#include "rpl/variable.h"
#include "rpl/event_stream.h"
#include "shared_realtime.h"
#include "slack_auth.h"
#include "web_api_client.h"

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QSet>
#include <QTimer>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace slack {

class SocketModeRealtime;
class SessionRealtime;

// Real Slack backend using the public API + Socket Mode.
// The app-level Socket Mode socket is supplied by the refcounted slack::
// SharedRealtime: it exists iff ≥1 Slack workspace is live, and is null when no
// xapp token is configured (then realtime is a no-op). appCfg and
// creds.refreshToken drive transparent token refresh when token rotation is on.
class PublicBackend : public Backend {
public:
    // appCfg defaults to the compiled-in Slack app credentials; tests pass their
    // own. refreshUrl overrides the oauth.v2.access endpoint (empty = Slack's
    // default; tests only).
    explicit PublicBackend(
        const Credentials &creds,
        const AppConfig   &appCfg     = appConfig(),
        const QString     &refreshUrl = {}
    );
    ~PublicBackend() override;

    rpl::producer<AuthState> authState() const override;
    Capabilities             capabilities() const override;
    void                     connectRealtime() override;
    void                     disconnectRealtime() override;
    void                     verifyRealtime() override;
    void                     reestablishRealtime() override;
    // Session auth has no Socket Mode, so the open-conversation poll IS the
    // delivery path — poll it every few seconds (spends the user's own rate
    // limits) instead of the 60 s app-token backstop cadence.
    int  foregroundPollGapMs() const override { return _sessionAuth ? 5'000 : 60'000; }
    // Session auth has no push (classic RTM is unusable — Slack caps the legacy
    // socket at ~5 s); the Session drives discovery/roster refresh by polling.
    bool hasRealtimePush() const override { return !_sessionAuth; }

    bool isSyntheticUser(UserId) const override;
    bool isBotId(UserId) const override;
    bool isUserId(UserId) const override;
    bool isUnresolvedUserId(const QString &) const override;

    QString                                  teamUrl() const override { return _teamUrl; }
    rpl::producer<UserId>                    loadMe() override;
    rpl::producer<std::vector<Conversation>> loadConversations() override;
    rpl::producer<std::vector<User>>         loadUsers() override;
    rpl::producer<bool>                      loadPresence(UserId) override;
    rpl::producer<SelfPresence>              loadSelfPresence() override;
    rpl::producer<User>                      loadBotInfo(UserId botId) override;
    rpl::producer<User>                      loadUser(UserId userId) override;
    rpl::producer<Conversation> loadConversationInfo(ConversationId, bool background) override;
    rpl::producer<std::vector<ConvCounts>> loadUnreadCounts() override;
    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString> cursor) override;
    rpl::producer<MessagePage>
    loadThread(ConversationId, Ts root, std::optional<QString> cursor) override;

    void sendMessage(ConversationId, OutgoingMessage) override;
    void editMessage(ConversationId, Ts, TextWithEntities) override;
    void deleteMessage(ConversationId, Ts) override;
    void addReaction(ConversationId, Ts, QString emoji) override;
    void removeReaction(ConversationId, Ts, QString emoji) override;
    void markRead(ConversationId, Ts) override;
    void sendTyping(ConversationId) override;
    void scheduleMessage(ConversationId, OutgoingMessage, qint64 postAt) override;

    void pinMessage(ConversationId, Ts) override;
    void unpinMessage(ConversationId, Ts) override;
    void starConversation(ConversationId, bool star) override;
    void leaveConversation(ConversationId) override;
    void createChannel(
        const QString                      &name,
        bool                                isPrivate,
        std::function<void(ConversationId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override;
    void joinChannel(
        ConversationId                      id,
        std::function<void(ConversationId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override;
    void openDm(
        UserId                              user,
        std::function<void(ConversationId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override;

    void setPresence(bool away, std::function<void(bool ok, QString err)> done = {}) override;
    void setStatus(
        const QString                            &emoji,
        const QString                            &text,
        qint64                                    expirationTs = 0,
        std::function<void(bool ok, QString err)> done         = {}
    ) override;
    void setDndSnooze(int minutes, std::function<void(bool ok, QString err)> done = {}) override;

    void loadMyProfile(std::function<void(MyProfile)> done) override;
    void updateProfile(
        const QHash<QString, QString> &fields, std::function<void(bool ok, QString err)> done = {}
    ) override;
    void setPhoto(
        const QString                                                  &filePath,
        std::function<void(bool ok, QString err, QString newAvatarUrl)> done = {}
    ) override;

    std::vector<SlashCommand>                nativeCommands() const override;
    rpl::producer<std::vector<SlashCommand>> listCommands() override;
    void                                     runCommand(
                                            ConversationId,
                                            const QString                                &command,
                                            const QString                                &text,
                                            std::function<void(bool ok, QString message)> done = {}
                                        ) override;

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &query) override;
    rpl::producer<QHash<QString, QString>>   loadEmojiList() override;
    void                                     uploadFiles(
                                            ConversationId,
                                            const QStringList                          &filePaths,
                                            const QString                              &initialComment,
                                            std::optional<Ts> threadRoot = std::nullopt,
                                            std::function<void(bool ok, QString error)> done = {}
                                        ) override;
    void deleteFile(const QString &fileId) override;
    void downloadFile(
        const QString                  &url,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError = {}
    ) override;

    void loadChannelCanvas(ConversationId, std::function<void(QString, bool)> done) override;
    void loadCanvasContent(
        const QString                    &fileId,
        std::function<void(QString html)> onHtml,
        std::function<void(QString)>      onError = {}
    ) override;
    void loadCanvasImage(
        const QString                  &fileId,
        std::function<void(QByteArray)> onData,
        std::function<void(QString)>    onError = {}
    ) override;
    void createChannelCanvas(
        ConversationId,
        const QString                      &markdown,
        std::function<void(QString fileId)> onSuccess = {},
        std::function<void(QString)>        onError   = {}
    ) override;
    void editCanvas(
        const QString                            &canvasId,
        const std::vector<CanvasChange>          &changes,
        std::function<void(bool ok, QString err)> done = {}
    ) override;

    // Tests only: point the API clients at a local fake server / shrink the
    // send-reconcile backoff.
    void setApiBaseUrlForTests(const QString &url);
    void setSendRetryDelayMsForTests(int ms) { _sendRetryDelayMs = ms; }

private:
    // One in-flight sendMessage with enough context to verify delivery and
    // resend after a connection loss. See sendMessage in the .cpp.
    struct SendState;
    void postMessageAttempt(std::shared_ptr<SendState> st);
    void reconcileSend(std::shared_ptr<SendState> st);

    // chat.delete posted as a write method (off Qt's GET auto-retransmit path).
    // Deletion is idempotent, so an ambiguous transport failure is safe to
    // resend; `attempts` bounds the retries with backoff.
    void deleteMessageAttempt(ConversationId conv, Ts ts, int attempts);

    // files.completeUploadExternal confirms the upload but, unlike
    // chat.postMessage, does NOT return the posted message's ts — so a file
    // send can only confirm + de-ghost via the realtime echo. When that echo
    // is dropped or delayed the optimistic ghost stays grayed out until a
    // history reload heals it. After a successful upload, scan recent history
    // for the own file_share message bearing one of the uploaded file ids and
    // emit its echo, so de-ghosting no longer depends on the websocket (Session
    // dedups the later realtime echo by ts, same as for text sends). A freshly
    // shared message (especially a heavy one) often hasn't surfaced in
    // conversations.history the instant the upload completes, so the scan
    // retries with backoff until the message appears or `attempt` is exhausted.
    void reconcileUpload(
        const ConversationId &conv, const QSet<QString> &fileIds, std::optional<Ts> threadRoot,
        int attempt = 0
    );

    // Re-derive live-huddle state from a freshly-fetched history page. The
    // USLACKBOT "huddle_thread" message carries the authoritative `room`
    // (current has_ended/date_end), so reading the newest one in the most-recent
    // page lets a conversation open/reload self-heal a banner that a missed or
    // mis-parsed realtime end-edit left stuck. Only ever called for the first
    // (cursor-less, newest) page; emits nothing unless a usable huddle room is
    // present, so it can never clobber a live huddle (e.g. if the token can't
    // read `room` at all).
    void reconcileHuddleFromHistory(const ConversationId &conv, const QJsonArray &messages);

    // canvases.edit allows one change per call; sends the queue sequentially.
    void sendNextCanvasChange(
        const QString                            &canvasId,
        std::shared_ptr<std::deque<CanvasChange>> queue,
        std::function<void(bool ok, QString err)> done
    );

public:
    void loadCanvasMeta(
        const QString                                                               &fileId,
        std::function<void(QString title, QString permalink, CanvasMetaState state)> done
    ) override;
    void deleteCanvas(
        const QString &canvasId, std::function<void(bool ok, QString err)> done = {}
    ) override;

    void subscribePresence(std::vector<UserId> userIds) override;

    rpl::producer<Event> events() const override;

protected:
    // AuthError means Slack definitively rejected the credentials (logout);
    // TransientError (network down, Slack 5xx) keeps the session alive and the
    // periodic check retries.
    enum class RefreshResult { Success, TransientError, AuthError };

    // Overridable in tests to intercept the network call.
    virtual void doRefresh(std::function<void(RefreshResult)> done);
    qint64       _tokenExpiresAt = 0;

private:
    void setupTokenRefresh(const Credentials &creds, const AppConfig &appCfg);
    void triggerRefresh(std::function<void(bool)> done);
    void maybeProactiveRefresh();

    QString       _teamId;
    QString       _refreshToken;
    QString       _refreshUrl;
    AppConfig     _appCfg;
    WebApiClient *_api;
    WebApiClient *_historyApi; // dedicated client for loadHistory/loadThread
    WebApiClient *_infoApi;    // low-priority client for background conversations.info sweeps
    // Refcounted app-level Socket Mode socket: created on the first Slack
    // backend, released on the last. _sharedRealtime caches the handle's socket
    // (null when no xapp token is configured). Held only by OAuth workspaces —
    // session-auth workspaces don't use Socket Mode (they poll), so they never
    // acquire the handle. If EVERY workspace is session-auth the refcount stays
    // 0 and the socket is never opened (no shared-app-key contention banner).
    std::unique_ptr<SharedRealtime>  _realtimeHandle;
    SocketModeRealtime              *_sharedRealtime = nullptr; // non-owning (owned by the handle)
    bool                             _sessionAuth = false; // xoxc/cookie workspace (no Socket Mode)
    // `client.counts` is undocumented and only served to a session (xoxc) token —
    // an OAuth token gets it rejected outright. Latched on the first Slack-level
    // rejection so we stop calling a method this workspace provably can't use;
    // transport failures leave it clear, since those say nothing about the method.
    // (Same degrade-gracefully rule as the undocumented commands.list/chat.command.)
    bool                             _countsUnavailable = false;
    // Per-workspace RTM realtime for session auth (null for OAuth workspaces).
    std::unique_ptr<SessionRealtime> _sessionRealtime;
    QTimer                          *_proactiveRefreshTimer = nullptr;

    // Token refresh deduplication
    bool                                   _refreshInProgress = false;
    std::vector<std::function<void(bool)>> _refreshWaiters;

    UserId  _meUserId; // cached from auth.test; used to reconcile lost sends
    QString _teamUrl;  // workspace web base URL from auth.test's `url` field
    int     _sendRetryDelayMs = 1000;

    rpl::variable<AuthState> _authState{AuthState::LoggedIn};
    rpl::event_stream<Event> _events;
};

} // namespace slack

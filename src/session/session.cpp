// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "session.h"
#include "backend/backend.h"
#include "cache/workspace_cache.h"
#include "text/mrkdwn_parser.h"

#include <QDateTime>

Session::Session(std::unique_ptr<Backend> backend, const QString &teamId)
    : _backend(std::move(backend)), _cache(std::make_unique<WorkspaceCache>(teamId)) {}

Session::~Session() = default;

void Session::start() {
    // Serve cached data immediately so the UI has something to show before
    // the network responds.
    {
        auto convs = _cache->loadConversations();
        if (!convs.empty())
            _conversations = std::move(convs);
        auto users = _cache->loadUsers();
        if (!users.empty())
            _users = std::move(users);
        _botUsers = _cache->loadBots();
        _emojiMap = _cache->loadEmojiMap();
    }

    _backend->connectRealtime();

    // Poll rich self-presence so the UI can show how the user appears to
    // others; there is no realtime event for your own connection count.
    refreshSelfPresence();
    QObject::connect(&_selfPresenceTimer, &QTimer::timeout, [this] { refreshSelfPresence(); });
    _selfPresenceTimer.start(60 * 1000);

    // Load conversations; update cache on arrival.
    _backend->loadConversations() |
        rpl::on_next(
            [this](std::vector<Conversation> convs) {
                // Preserve locally-incremented unread/mention counts
                // accumulated since startup (API returns 0 for
                // channels).
                const auto &prev = _conversations.current();
                for (auto &c : convs) {
                    for (const auto &old : prev) {
                        if (old.id != c.id)
                            continue;
                        c.unread       = std::max(c.unread, old.unread);
                        c.mentionCount = std::max(c.mentionCount, old.mentionCount);
                        // Preserve locally-applied star state: the
                        // API may not echo is_starred immediately
                        // after stars.add/stars.remove.
                        if (old.isStarred != c.isStarred && old.isStarred)
                            c.isStarred = old.isStarred;
                        // Preserve locally-set notification level
                        // (no public read API for per-channel prefs).
                        if (old.notifLevel != NotificationLevel::Default &&
                            c.notifLevel == NotificationLevel::Default)
                            c.notifLevel = old.notifLevel;
                        // Mute state also lives in client prefs the public
                        // API can't read — local state wins.
                        if (old.isMuted)
                            c.isMuted = true;
                        break;
                    }
                }
                _cache->saveConversations(convs);
                _conversations = std::move(convs);
                // Emoji load is deferred to here so it doesn't queue ahead of
                // conversations/users. Cache serves emojis until the refresh
                // arrives and the result is written back to cache.
                _backend->loadEmojiList() | rpl::on_next(
                                                [this](QHash<QString, QString> map) {
                                                    _emojiMap = std::move(map);
                                                    _cache->saveEmojiMap(_emojiMap);
                                                },
                                                _lifetime
                                            );
            },
            _lifetime
        );

    // Load users; update cache on arrival.
    _backend->loadUsers() | rpl::on_next(
                                [this](std::vector<User> users) {
                                    _cache->saveUsers(users);
                                    _users = std::move(users);
                                    // Update admin flag now that the full user list is available.
                                    if (!_meUserId.value.isEmpty()) {
                                        if (const User *u = findUser(_meUserId))
                                            _meIsAdmin = u->isAdmin;
                                    }
                                    // Subscribe to real-time presence events for all non-bot users.
                                    std::vector<UserId> ids;
                                    for (const auto &u : _users.current())
                                        if (!u.isBot && !u.isDeactivated)
                                            ids.push_back(u.id);
                                    _backend->subscribePresence(std::move(ids));

                                    // The self-presence call made at start() can race the
                                    // startup token refresh and come back empty; users.list
                                    // landing proves the token works, so re-poll now (ahead of
                                    // the per-DM polls below, which share the request queue).
                                    if (!_selfPresence.current().loaded)
                                        refreshSelfPresence();

                                    // Poll current presence for every DM conversation partner so
                                    // the list shows the right indicator without waiting for the
                                    // first change.
                                    for (const auto &conv : _conversations.current())
                                        if (conv.dmUser && !conv.dmUser->value.isEmpty())
                                            requestPresence(*conv.dmUser);
                                },
                                _lifetime
                            );

    _backend->loadMe() | rpl::on_next(
                             [this](UserId id) {
                                 setMe(std::move(id));
                                 // Users may already be loaded (from cache); pick up admin flag
                                 // immediately.
                                 if (const User *u = findUser(_meUserId))
                                     _meIsAdmin = u->isAdmin;
                             },
                             _lifetime
                         );

    // Wire the backend event firehose through our hub so Session can
    // intercept and patch state before forwarding to the UI.
    _backend->events() |
        rpl::on_next(
            [this](Event e) {
                // Patch in-memory state then forward.
                if (auto *ev = std::get_if<EvPresenceChanged>(&e)) {
                    auto users = _users.current();
                    for (auto &u : users) {
                        if (u.id == ev->user) {
                            u.isActive = ev->active;
                            break;
                        }
                    }
                    _users = std::move(users);
                } else if (auto *ev = std::get_if<EvDndChanged>(&e)) {
                    auto users = _users.current();
                    for (auto &u : users) {
                        if (u.id == ev->user) {
                            u.dndEnabled = ev->dndEnabled;
                            break;
                        }
                    }
                    _users = std::move(users);
                } else if (auto *ev = std::get_if<EvConvMarked>(&e)) {
                    auto convs = _conversations.current();
                    for (auto &c : convs) {
                        if (c.id == ev->conv) {
                            c.lastRead     = ev->lastRead;
                            c.unread       = ev->unread;
                            c.mentionCount = ev->mentionCount;
                            break;
                        }
                    }
                    _conversations = std::move(convs);
                } else if (auto *ev = std::get_if<EvMessageNew>(&e)) {
                    const bool ownMessage =
                        !_meUserId.value.isEmpty() && ev->msg.author == _meUserId;
                    {
                        auto convs = _conversations.current();
                        for (auto &c : convs) {
                            if (c.id != ev->conv)
                                continue;
                            c.latestTs = ev->msg.ts;
                            if (ownMessage || !c.isMember)
                                break;
                            if (ev->conv == _readingConv) {
                                // On screen right now — keep the server-side
                                // read cursor in sync so other clients (and
                                // the next restart) agree it's read.
                                c.lastRead = ev->msg.ts;
                                _backend->markRead(ev->conv, ev->msg.ts);
                                break;
                            }
                            const bool isDm = (c.kind == ConvKind::Im || c.kind == ConvKind::Mpim);
                            const QString &mt =
                                ev->msg.rawText.isEmpty() ? ev->msg.text.text : ev->msg.rawText;
                            const bool isMention = mrkdwnMentions(mt, _meUserId);
                            // Plain channel thread replies don't mark the
                            // channel unread (they live in the Threads view);
                            // mentions and DM replies still count.
                            if (ev->msg.threadRoot && !isDm && !isMention)
                                break;
                            if (!c.isMuted) {
                                c.unread++;
                                if (isDm || isMention)
                                    c.mentionCount++;
                            } else if (!isDm && isMention) {
                                // Muted channels stay quiet except for explicit
                                // mentions, which still badge (official-client
                                // behavior).
                                c.unread++;
                                c.mentionCount++;
                            }
                            break;
                        }
                        _conversations = std::move(convs);
                    }
                    // Remove the matching optimistic copy so the real message
                    // replaces it instead of appearing as a duplicate.
                    if (ownMessage) {
                        auto it = _pendingOptimisticTs.find(ev->conv.value);
                        if (it != _pendingOptimisticTs.end() && !it->isEmpty()) {
                            const QString fakeTs = it->takeFirst();
                            _eventHub.fire(EvMessageDeleted{ev->conv, fakeTs});
                        }
                    }
                }
                _eventHub.fire(std::move(e));
            },
            _lifetime
        );
}

rpl::producer<std::vector<Conversation>> Session::conversations() const {
    return _conversations.value();
}

rpl::producer<std::vector<User>> Session::users() const {
    return _users.value();
}

rpl::producer<Event> Session::events() const {
    return _eventHub.events();
}

rpl::producer<AuthState> Session::authState() const {
    return _backend->authState();
}

const User *Session::findUser(UserId id) const {
    for (const auto &u : _users.current()) {
        if (u.id == id)
            return &u;
    }
    auto it = _botUsers.constFind(id.value);
    if (it != _botUsers.constEnd())
        return &*it;
    return nullptr;
}

void Session::fetchBotIfNeeded(UserId botId) {
    if (botId.value.isEmpty() || !botId.value.startsWith('B'))
        return;
    if (findUser(botId))
        return;
    if (_pendingBotFetches.contains(botId.value))
        return;
    _pendingBotFetches.insert(botId.value);
    _backend->loadBotInfo(botId) | rpl::on_next(
                                       [this, botId](User u) {
                                           _pendingBotFetches.remove(botId.value);
                                           if (!u.id.value.isEmpty()) {
                                               _botUsers[u.id.value] = std::move(u);
                                               _cache->saveBots(_botUsers);
                                               _botInfoHub.fire_copy(botId);
                                           }
                                       },
                                       _lifetime
                                   );
}

rpl::producer<UserId> Session::botInfoLoaded() const {
    return _botInfoHub.events();
}

const Conversation *Session::findConversation(ConversationId id) const {
    for (const auto &c : _conversations.current()) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

const std::vector<User> &Session::currentUsers() const {
    return _users.current();
}

const std::vector<Conversation> &Session::currentConversations() const {
    return _conversations.current();
}

void Session::sendMessage(ConversationId conv, const QString &text, std::optional<Ts> threadRoot) {
    qint64 msec   = QDateTime::currentMSecsSinceEpoch();
    Ts     fakeTs = QString("%1.%2").arg(msec / 1000).arg((msec % 1000) * 1000, 6, 10, QChar('0'));

    Message optimistic;
    optimistic.ts         = fakeTs;
    optimistic.author     = _meUserId;
    optimistic.text       = MrkdwnParser::parse(text);
    optimistic.rawText    = text;
    optimistic.threadRoot = threadRoot;

    _eventHub.fire(EvMessageNew{conv, optimistic});

    _pendingOptimisticTs[conv.value].append(fakeTs);

    OutgoingMessage out;
    out.text       = optimistic.text;
    out.rawText    = text;
    out.threadRoot = threadRoot;
    _backend->sendMessage(conv, std::move(out));
}

Backend *Session::backend() const {
    return _backend.get();
}

void Session::editMessage(ConversationId conv, Ts ts, const QString &newText) {
    _backend->editMessage(conv, ts, TextWithEntities{newText, {}});
}

void Session::sendTyping(ConversationId conv) {
    _backend->sendTyping(conv);
}

void Session::scheduleMessage(ConversationId conv, const QString &text, qint64 postAt) {
    OutgoingMessage out;
    out.text = MrkdwnParser::parse(text);
    _backend->scheduleMessage(conv, std::move(out), postAt);
}

void Session::uploadFile(ConversationId conv, const QString &filePath) {
    _backend->uploadFile(conv, filePath);
}

void Session::searchMessages(
    const QString &query, std::function<void(std::vector<SearchResult>)> callback
) {
    _backend->searchMessages(query) |
        rpl::on_next(
            [cb = std::move(callback)](std::vector<SearchResult> results) {
                cb(std::move(results));
            },
            _lifetime
        );
}

rpl::producer<QString> Session::errors() const {
    return _errorHub.events();
}

void Session::downloadFile(
    const QString &url, std::function<void(QByteArray)> onData, std::function<void(QString)> onError
) {
    if (!onError) {
        onError = [this](const QString &err) { _errorHub.fire_copy(err); };
    }
    _backend->downloadFile(url, std::move(onData), std::move(onError));
}

std::vector<Message> Session::cachedMessages(ConversationId conv) const {
    return _cache->loadMessages(conv);
}

void Session::cacheMessages(ConversationId conv, const std::vector<Message> &msgs) {
    _cache->saveMessages(conv, msgs);
}

void Session::saveLastConv(ConversationId conv, const QString &displayName) {
    _cache->saveLastConv(conv, displayName);
}

std::pair<ConversationId, QString> Session::loadLastConv() const {
    return _cache->loadLastConv();
}

void Session::cacheImage(const QString &url, const QByteArray &data) {
    _cache->saveImage(url, data);
}

QByteArray Session::cachedImage(const QString &url) const {
    return _cache->loadImage(url);
}

void Session::requestPresence(UserId userId) {
    _backend->loadPresence(userId) | rpl::on_next(
                                         [this, userId](bool active) {
                                             // Update user cache and fire event so all listeners
                                             // see the new state.
                                             auto users = _users.current();
                                             for (auto &u : users) {
                                                 if (u.id == userId) {
                                                     u.isActive = active;
                                                     break;
                                                 }
                                             }
                                             _users = std::move(users);
                                             _eventHub.fire(EvPresenceChanged{userId, active});
                                         },
                                         _lifetime
                                     );
}

rpl::producer<SelfPresence> Session::selfPresence() const {
    return _selfPresence.value();
}

SelfPresence Session::currentSelfPresence() const {
    return _selfPresence.current();
}

void Session::refreshSelfPresence() {
    _backend->loadSelfPresence() |
        rpl::on_next(
            [this](SelfPresence sp) {
                _selfPresence  = sp;
                // Keep our own User.isActive (the others-see-me flag) in sync
                // and notify listeners the same way requestPresence() does.
                const User *me = findUser(_meUserId);
                if (me && me->isActive != sp.active) {
                    auto users = _users.current();
                    for (auto &u : users) {
                        if (u.id == _meUserId) {
                            u.isActive = sp.active;
                            break;
                        }
                    }
                    _users = std::move(users);
                    _eventHub.fire(EvPresenceChanged{_meUserId, sp.active});
                }
            },
            _lifetime
        );
}

void Session::persistUnreads() {
    _cache->saveConversations(_conversations.current());
}

void Session::setReading(ConversationId conv) {
    _readingConv = conv;
    if (conv.value.isEmpty())
        return;

    // Optimistically zero the badge.
    auto convs = _conversations.current();
    for (auto &c : convs) {
        if (c.id == conv) {
            c.unread       = 0;
            c.mentionCount = 0;
            // Sync the read cursor to Slack so other clients (and the next
            // restart) agree this conversation is read.
            if (!c.latestTs.isEmpty() && c.lastRead < c.latestTs) {
                c.lastRead = c.latestTs;
                _backend->markRead(conv, c.latestTs);
            }
            break;
        }
    }
    _conversations = std::move(convs);
}

void Session::starConversation(ConversationId conv, bool star) {
    auto convs = _conversations.current();
    for (auto &c : convs) {
        if (c.id == conv) {
            c.isStarred = star;
            break;
        }
    }
    _conversations = std::move(convs);
    _backend->starConversation(conv, star);
}

void Session::leaveConversation(ConversationId conv) {
    _backend->leaveConversation(conv);
    // Optimistically remove from the local list so the UI updates immediately.
    auto convs = _conversations.current();
    convs.erase(
        std::remove_if(
            convs.begin(), convs.end(), [&](const Conversation &c) { return c.id == conv; }
        ),
        convs.end()
    );
    _conversations = std::move(convs);
}

void Session::createChannel(
    const QString                      &name,
    bool                                isPrivate,
    std::function<void(ConversationId)> onSuccess,
    std::function<void(QString)>        onError
) {
    _backend->createChannel(
        name,
        isPrivate,
        [this, onSuccess](ConversationId id) {
            // Refresh conversation list so the new channel appears.
            _backend->loadConversations() |
                rpl::on_next(
                    [this](std::vector<Conversation> convs) { _conversations = std::move(convs); },
                    _lifetime
                );
            if (onSuccess)
                onSuccess(id);
        },
        [this, onError](QString err) {
            if (onError)
                onError(err);
            else
                _errorHub.fire_copy(err);
        }
    );
}

void Session::joinChannel(
    ConversationId                      id,
    std::function<void(ConversationId)> onSuccess,
    std::function<void(QString)>        onError
) {
    _backend->joinChannel(
        id,
        [this, onSuccess](ConversationId convId) {
            _backend->loadConversations() |
                rpl::on_next(
                    [this](std::vector<Conversation> convs) { _conversations = std::move(convs); },
                    _lifetime
                );
            if (onSuccess)
                onSuccess(convId);
        },
        [this, onError](QString err) {
            if (onError)
                onError(err);
            else
                _errorHub.fire_copy(err);
        }
    );
}

void Session::setNotificationLevel(ConversationId conv, NotificationLevel level) {
    auto convs = _conversations.current();
    for (auto &c : convs) {
        if (c.id == conv) {
            c.notifLevel = level;
            c.isMuted    = (level == NotificationLevel::Mute);
            break;
        }
    }
    _conversations = std::move(convs);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "session.h"
#include "backend/backend.h"
#include "cache/workspace_cache.h"
#include "text/mrkdwn_parser.h"

#include <QDateTime>

Session::Session(std::unique_ptr<Backend> backend, const QString &teamId)
    : _backend(std::move(backend))
    , _cache(std::make_unique<WorkspaceCache>(teamId))
{}

Session::~Session() = default;

void Session::start() {
    // Serve cached data immediately so the UI has something to show before
    // the network responds.
    {
        auto convs = _cache->loadConversations();
        if (!convs.empty()) _conversations = std::move(convs);
        auto users = _cache->loadUsers();
        if (!users.empty()) _users = std::move(users);
    }

    _backend->connectRealtime();

    _backend->loadMe()
        | rpl::on_next([this](UserId id) {
            setMe(std::move(id));
            // Users may already be loaded (from cache); pick up admin flag immediately.
            if (const User *u = findUser(_meUserId))
                _meIsAdmin = u->isAdmin;
        }, _lifetime);

    // Load custom emoji map once.
    _backend->loadEmojiList()
        | rpl::on_next([this](QHash<QString,QString> map) {
            _emojiMap = std::move(map);
        }, _lifetime);

    // Load conversations; update cache on arrival.
    _backend->loadConversations()
        | rpl::on_next([this](std::vector<Conversation> convs) {
            _cache->saveConversations(convs);
            _conversations = std::move(convs);
        }, _lifetime);

    // Load users; update cache on arrival.
    _backend->loadUsers()
        | rpl::on_next([this](std::vector<User> users) {
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
                if (!u.isBot && !u.isDeactivated) ids.push_back(u.id);
            _backend->subscribePresence(std::move(ids));

            // Poll current presence for every DM conversation partner so the
            // list shows the right indicator without waiting for the first change.
            for (const auto &conv : _conversations.current())
                if (conv.dmUser && !conv.dmUser->value.isEmpty())
                    requestPresence(*conv.dmUser);
        }, _lifetime);

    // Wire the backend event firehose through our hub so Session can
    // intercept and patch state before forwarding to the UI.
    _backend->events()
        | rpl::on_next([this](Event e) {
            // Patch in-memory state then forward.
            if (auto *ev = std::get_if<EvPresenceChanged>(&e)) {
                auto users = _users.current();
                for (auto &u : users) {
                    if (u.id == ev->user) { u.isActive = ev->active; break; }
                }
                _users = std::move(users);
            } else if (auto *ev = std::get_if<EvDndChanged>(&e)) {
                auto users = _users.current();
                for (auto &u : users) {
                    if (u.id == ev->user) { u.dndEnabled = ev->dndEnabled; break; }
                }
                _users = std::move(users);
            } else if (auto *ev = std::get_if<EvConvMarked>(&e)) {
                auto convs = _conversations.current();
                for (auto &c : convs) {
                    if (c.id == ev->conv) {
                        c.lastRead = ev->lastRead;
                        c.unread   = ev->unread;
                        break;
                    }
                }
                _conversations = std::move(convs);
            } else if (auto *ev = std::get_if<EvMessageNew>(&e)) {
                const bool ownMessage = !_meUserId.value.isEmpty()
                                        && ev->msg.author == _meUserId;
                if (!ownMessage && ev->conv != _readingConv) {
                    auto convs = _conversations.current();
                    for (auto &c : convs) {
                        if (c.id == ev->conv) { c.unread++; break; }
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
        }, _lifetime);
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
        if (u.id == id) return &u;
    }
    auto it = _botUsers.constFind(id.value);
    if (it != _botUsers.constEnd()) return &*it;
    return nullptr;
}

void Session::fetchBotIfNeeded(UserId botId) {
    if (botId.value.isEmpty() || !botId.value.startsWith('B')) return;
    if (findUser(botId)) return;
    if (_pendingBotFetches.contains(botId.value)) return;
    _pendingBotFetches.insert(botId.value);
    _backend->loadBotInfo(botId)
        | rpl::on_next([this, botId](User u) {
            _pendingBotFetches.remove(botId.value);
            if (!u.id.value.isEmpty()) {
                _botUsers[u.id.value] = std::move(u);
                _botInfoHub.fire_copy(botId);
            }
        }, _lifetime);
}

rpl::producer<UserId> Session::botInfoLoaded() const {
    return _botInfoHub.events();
}

const Conversation *Session::findConversation(ConversationId id) const {
    for (const auto &c : _conversations.current()) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

const std::vector<User>& Session::currentUsers() const {
    return _users.current();
}

const std::vector<Conversation>& Session::currentConversations() const {
    return _conversations.current();
}

void Session::sendMessage(ConversationId conv, const QString &text,
                           std::optional<Ts> threadRoot) {
    qint64 msec = QDateTime::currentMSecsSinceEpoch();
    Ts fakeTs = QString("%1.%2")
        .arg(msec / 1000)
        .arg((msec % 1000) * 1000, 6, 10, QChar('0'));

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

void Session::searchMessages(const QString &query,
                              std::function<void(std::vector<SearchResult>)> callback) {
    _backend->searchMessages(query)
        | rpl::on_next([cb = std::move(callback)](std::vector<SearchResult> results) {
            cb(std::move(results));
        }, _lifetime);
}

rpl::producer<QString> Session::errors() const {
    return _errorHub.events();
}

void Session::downloadFile(const QString &url,
                            std::function<void(QByteArray)> onData,
                            std::function<void(QString)>    onError) {
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
    _backend->loadPresence(userId)
        | rpl::on_next([this, userId](bool active) {
            // Update user cache and fire event so all listeners see the new state.
            auto users = _users.current();
            for (auto &u : users) {
                if (u.id == userId) { u.isActive = active; break; }
            }
            _users = std::move(users);
            _eventHub.fire(EvPresenceChanged{ userId, active });
        }, _lifetime);
}

void Session::setReading(ConversationId conv) {
    _readingConv = conv;
    if (conv.value.isEmpty()) return;

    // Optimistically zero the badge.
    auto convs = _conversations.current();
    for (auto &c : convs) {
        if (c.id == conv) { c.unread = 0; break; }
    }
    _conversations = std::move(convs);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Per-workspace persistent cache: conversations, users, recent messages per conv,
// and the last-opened conversation. Stored as JSON under AppDataLocation.
// All I/O is synchronous but small (<200 KB) so latency is imperceptible.
#pragma once

#include "backend/domain.h"

#include <QHash>
#include <QJsonDocument>
#include <QString>
#include <utility>
#include <vector>

class WorkspaceCache {
public:
    explicit WorkspaceCache(const QString &teamId);

    void                      saveConversations(const std::vector<Conversation> &convs);
    std::vector<Conversation> loadConversations() const;

    void              saveUsers(const std::vector<User> &users);
    std::vector<User> loadUsers() const;

    void                 saveBots(const QHash<QString, User> &bots);
    QHash<QString, User> loadBots() const;

    // Persists the newest kMaxMessages messages for a conversation.
    void                 saveMessages(const ConversationId &conv, const std::vector<Message> &msgs);
    std::vector<Message> loadMessages(const ConversationId &conv) const;

    void saveLastConv(const ConversationId &conv, const QString &displayName);
    std::pair<ConversationId, QString> loadLastConv() const;

    // The authed user's id (from auth.test). Seeds Session::meUserId() at
    // startup so optimistic sends work even if auth.test races the token
    // refresh and fails on this run.
    void   saveMeUserId(const UserId &id);
    UserId loadMeUserId() const;

    // When the background conversations.info activity sweep last completed
    // (Unix seconds; 0 = never). Used to throttle the sweep across restarts.
    void   saveActivitySweepAt(qint64 unixSecs);
    qint64 loadActivitySweepAt() const;

    void                    saveEmojiMap(const QHash<QString, QString> &map);
    QHash<QString, QString> loadEmojiMap() const;

    // Persist/retrieve raw downloaded thumbnail bytes, keyed by URL.
    // The URL is hashed to a safe filename; no expiry — images are small and stable.
    void       saveImage(const QString &url, const QByteArray &data);
    QByteArray loadImage(const QString &url) const;

private:
    static constexpr int kMaxMessages = 50;

    QString _dir;

    QString convPath() const;
    QString usersPath() const;
    QString botsPath() const;
    QString emojiPath() const;
    QString msgsPath(const ConversationId &conv) const;
    QString metaPath() const;
    QString imgPath(const QString &url) const; // url → hashed filename

    static QByteArray readFile(const QString &path);
    static bool       writeFile(const QString &path, const QByteArray &data);
    static bool       writeJson(const QString &path, const QJsonDocument &doc);
};

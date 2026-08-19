// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Per-workspace persistent cache: conversations, users, recent messages per conv,
// and the last-opened conversation. Stored as JSON under AppDataLocation.
// All I/O is synchronous; the big blobs (users.json and conversations.json can
// reach hundreds of KB) must be written from debounced paths only (Session's
// scheduleSave* timers), never per-event on the main thread.
#pragma once

#include "backend/domain.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <optional>
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

    // Locally-muted threads, each key "convId\trootTs" (see Session::threadMuteKey).
    void        saveMutedThreads(const QStringList &keys);
    QStringList loadMutedThreads() const;

    // Message reminders (Session::_reminders): the server list plus the local
    // enrichment (thread root, snippet, fired flag) the server doesn't store.
    // Persisted so blue tints paint and overdue reminders still fire on a start
    // without network.
    void                         saveReminders(const std::vector<MessageReminder> &reminders);
    std::vector<MessageReminder> loadReminders() const;

    // The reminder previews shadow map (Session::_reminderPreviews), keyed by
    // the Session's reminder key — an opaque string here. Persisted separately
    // from the reminder list precisely so it survives a reminder record that the
    // server snapshot briefly drops and re-adds bare.
    void                            saveReminderPreviews(const QHash<QString, ReminderPreview> &);
    QHash<QString, ReminderPreview> loadReminderPreviews() const;

    // The channel_not_found negative cache (Session::_deadConvIds): conversation
    // ids conversations.info reported as nonexistent for this workspace. Persisted
    // so a restart doesn't re-probe every foreign/dead conversation from scratch.
    void        saveDeadConvIds(const QStringList &ids);
    QStringList loadDeadConvIds() const;

    // Persist/retrieve raw downloaded thumbnail bytes, keyed by URL.
    // The URL is hashed to a safe filename. The blob's mtime doubles as its
    // last-used time (loadImage bumps it) so CacheEvictor can drop the least
    // recently viewed blobs when the cache exceeds the configured cap.
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

    // meta.json parsed once and kept: five loaders read it back-to-back at
    // startup, and every save was a full read-modify-write of the file.
    // WorkspaceCache is the sole writer, so the cached object cannot go stale.
    QJsonObject &metaObject() const;
    void         writeMeta(); // serialize the cached object back to meta.json

    mutable std::optional<QJsonObject> _meta;

    static QByteArray readFile(const QString &path);
    static bool       writeFile(const QString &path, const QByteArray &data);
    static bool       writeJson(const QString &path, const QJsonDocument &doc);
};

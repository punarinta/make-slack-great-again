// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// FakeBackend — in-memory Backend implementation for tests and UI development.
// No network, no tokens; populated with hardcoded fixture data at construction.
#pragma once

#include "backend/backend.h"
#include "rpl/variable.h"
#include "rpl/event_stream.h"

class FakeBackend : public Backend {
public:
    FakeBackend();

    rpl::producer<AuthState> authState() const override;
    Capabilities             capabilities() const override;
    void                     connectRealtime() override;
    void                     disconnectRealtime() override;

    rpl::producer<UserId>                    loadMe() override;
    rpl::producer<std::vector<Conversation>> loadConversations() override;
    rpl::producer<std::vector<User>>         loadUsers() override;
    rpl::producer<bool>                      loadPresence(UserId) override;
    rpl::producer<SelfPresence>              loadSelfPresence() override;
    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString>) override;
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override;

    void sendMessage(ConversationId, OutgoingMessage) override;
    void editMessage(ConversationId, Ts, TextWithEntities) override;
    void deleteMessage(ConversationId, Ts) override;
    void addReaction(ConversationId, Ts, QString) override;
    void removeReaction(ConversationId, Ts, QString) override;
    void markRead(ConversationId, Ts) override;

    void setPresence(bool, std::function<void(bool, QString)> done = {}) override {
        if (done)
            done(true, {});
    }
    void setStatus(
        const QString &, const QString &, qint64 = 0, std::function<void(bool, QString)> done = {}
    ) override {
        if (done)
            done(true, {});
    }
    void setDndSnooze(int, std::function<void(bool, QString)> done = {}) override {
        if (done)
            done(true, {});
    }

    rpl::producer<std::vector<SlashCommand>> listCommands() override;
    void                                     runCommand(
                                            ConversationId,
                                            const QString                     &command,
                                            const QString                     &text,
                                            std::function<void(bool, QString)> done = {}
                                        ) override;

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override;
    rpl::producer<QHash<QString, QString>>   loadEmojiList() override;
    void                                     uploadFiles(
                                            ConversationId,
                                            const QStringList &,
                                            const QString &,
                                            std::function<void(bool, QString)> done = {}
                                        ) override {
        if (done)
            done(true, {});
    }
    void downloadFile(
        const QString &, std::function<void(QByteArray)>, std::function<void(QString)> = {}
    ) override {}

    rpl::producer<Event> events() const override;

    // Test helpers — fire events from outside
    void fireEvent(Event e);

    // Test helper — every runCommand() call is recorded here.
    struct RanCommand {
        ConversationId conv;
        QString        command;
        QString        text;
    };
    std::vector<RanCommand> ranCommands;

private:
    rpl::variable<AuthState>                          _authState;
    rpl::variable<std::vector<Conversation>>          _conversations;
    rpl::variable<std::vector<User>>                  _users;
    std::unordered_map<QString, std::vector<Message>> _history; // conv id → messages
    rpl::event_stream<Event>                          _events;
};

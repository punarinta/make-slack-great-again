// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
//
// An inert Backend for widget and session tests. Header-only (everything
// inline), so it lives once in msga_tests however many suites include it.
#pragma once

#include "backend/backend.h"
#include "backend/domain.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"

namespace msga_test {

// Logged in, capable of whatever `caps` says (nothing by default); every
// producer is an rpl::variable, so it answers synchronously on subscription,
// and every command does nothing. A suite derives its own stub from this and
// overrides only what it records or serves.
struct StubBackendBase : Backend {
    rpl::variable<AuthState>                 _authState{AuthState::LoggedIn};
    rpl::variable<UserId>                    _meId;
    rpl::variable<std::vector<Conversation>> _convs;
    rpl::variable<std::vector<User>>         _users;
    rpl::event_stream<Event>                 _events;
    Capabilities                             caps;

    rpl::producer<AuthState> authState() const override { return _authState.value(); }
    Capabilities             capabilities() const override { return caps; }
    void                     connectRealtime() override {}
    void                     disconnectRealtime() override {}

    rpl::producer<UserId>                    loadMe() override { return _meId.value(); }
    rpl::producer<std::vector<Conversation>> loadConversations() override { return _convs.value(); }
    rpl::producer<std::vector<User>>         loadUsers() override { return _users.value(); }
    rpl::producer<bool> loadPresence(UserId) override { return rpl::variable<bool>(false).value(); }

    rpl::producer<MessagePage> loadHistory(ConversationId, std::optional<QString>) override {
        return rpl::variable<MessagePage>(MessagePage{}).value();
    }
    rpl::producer<MessagePage> loadThread(ConversationId, Ts, std::optional<QString>) override {
        return rpl::variable<MessagePage>(MessagePage{}).value();
    }

    void sendMessage(ConversationId, OutgoingMessage, std::function<void(bool, QString)>) override {
    }
    void editMessage(ConversationId, Ts, OutgoingMessage) override {}
    void deleteMessage(ConversationId, Ts) override {}
    void addReaction(ConversationId, Ts, QString) override {}
    void removeReaction(ConversationId, Ts, QString) override {}
    void markRead(ConversationId, Ts) override {}

    void uploadFiles(
        ConversationId,
        const QStringList &,
        const QString &,
        std::optional<Ts>                  = std::nullopt,
        std::function<void(bool, QString)> = {}
    ) override {}
    void downloadFile(
        const QString &, std::function<void(QByteArray)>, std::function<void(QString)> = {}
    ) override {}

    rpl::producer<std::vector<SearchResult>> searchMessages(const QString &) override {
        return rpl::variable<std::vector<SearchResult>>({}).value();
    }
    rpl::producer<QHash<QString, QString>> loadEmojiList() override {
        return rpl::variable<QHash<QString, QString>>({}).value();
    }

    rpl::producer<Event> events() const override { return _events.events(); }
};

} // namespace msga_test

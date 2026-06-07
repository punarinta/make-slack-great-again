// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// QJsonObject → domain type converters. Only this file touches Slack JSON shapes.
#pragma once

#include "backend/domain.h"
#include <QJsonObject>
#include <QJsonArray>

namespace JsonMappers {

User         toUser(const QJsonObject &);
Conversation toConversation(const QJsonObject &);
Message      toMessage(const QJsonObject &);
Reaction     toReaction(const QJsonObject &);
File         toFile(const QJsonObject &);
Block        toBlock(const QJsonObject &);
Attachment   toAttachment(const QJsonObject &);
SearchResult toSearchResult(const QJsonObject &);

// Batch helpers
std::vector<User>         toUsers(const QJsonArray &);
std::vector<Conversation> toConversations(const QJsonArray &);
// reverseOrder=true for conversations.history (newest-first); false for conversations.replies
// (oldest-first).
std::vector<Message>      toMessages(const QJsonArray &, bool reverseOrder = true);
std::vector<SearchResult> toSearchResults(const QJsonArray &);

} // namespace JsonMappers

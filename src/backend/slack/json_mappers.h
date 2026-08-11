// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// QJsonObject → domain type converters. Only this file touches Slack JSON shapes.
#pragma once

#include "backend/domain.h"
#include <QJsonObject>
#include <QJsonArray>

namespace slack {
namespace JsonMappers {

User         toUser(const QJsonObject &);
// users.getPresence response for the authed user (extra fields only exist there).
SelfPresence toSelfPresence(const QJsonObject &);
Conversation toConversation(const QJsonObject &);
Message      toMessage(const QJsonObject &);
Reaction     toReaction(const QJsonObject &);
File         toFile(const QJsonObject &);
Block        toBlock(const QJsonObject &);
Attachment   toAttachment(const QJsonObject &);
SearchResult toSearchResult(const QJsonObject &);

// A `client.counts` response: per-conversation unread/activity for the whole
// workspace, read out of its parallel `channels` / `mpims` / `ims` arrays.
std::vector<ConvCounts> toConvCounts(const QJsonObject &resp);

// A `subscriptions.thread.getView` response: the Threads overview page. Each
// entry's root_msg is a complete parent message that also carries its channel.
ThreadsViewPage toThreadsViewPage(const QJsonObject &resp);

// Batch helpers
std::vector<User>         toUsers(const QJsonArray &);
std::vector<Conversation> toConversations(const QJsonArray &);
// reverseOrder=true for conversations.history (newest-first); false for conversations.replies
// (oldest-first).
std::vector<Message>      toMessages(const QJsonArray &, bool reverseOrder = true);
std::vector<SearchResult> toSearchResults(const QJsonArray &);
// commands.list "commands" value — observed both as an array of command
// objects and as an object keyed by command name; handles either shape.
std::vector<SlashCommand> toSlashCommands(const QJsonValue &);

// Domain → Slack JSON: canvases.edit "changes" array.
QJsonArray toCanvasChanges(const std::vector<CanvasChange> &);

// Parsed huddle state from a `room` object (a conversations.info channel's
// `room`, or a huddle_thread message event's room).
struct HuddleRoom {
    bool                active = false; // call_family "huddle" and not ended
    QString             link;           // room.huddle_link (preferred join URL)
    std::vector<UserId> participants;   // current participants, or [created_by]
};
HuddleRoom readHuddleRoom(const QJsonObject &room);

// Channel canvas lookup on a conversations.info/list "channel" object, across
// both server shapes: paid teams expose properties.canvas {file_id, is_empty};
// free-team channel canvases appear only as a properties.tabs[] entry of
// type "canvas" with data.file_id (no is_empty — reported as false).
// Returns {fileId, isEmpty}; empty fileId = no canvas.
std::pair<QString, bool> channelCanvas(const QJsonObject &channel);

} // namespace JsonMappers
} // namespace slack

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QJsonObject>
#include <QString>
#include <utility>

namespace teams {

// A Teams channel is addressed by (teamId, channelId), but ConversationId is one
// opaque scalar — so encode a channel as "teamId|channelId" and a chat as its
// bare chat id. '|' never appears in a Graph chat/channel id or a tenant GUID, so
// presence of '|' distinguishes a channel conversation from a chat. The UI treats
// the whole thing as opaque; only the Teams backend decodes it.
QString                     channelConvId(const QString &teamId, const QString &channelId);
bool                        isChannelConvId(const QString &convId);
std::pair<QString, QString> splitChannelConvId(const QString &convId); // {teamId, channelId}

// Map a Slack-style emoji name (the UI's reaction vocabulary, e.g. "thumbsup") to
// a Graph reactionType. Graph also accepts unicode/custom values, so unknown
// names pass through unchanged.
QString graphReactionType(const QString &emojiName);

// An inline image referenced by an <img> in a Teams message body. The url is
// typically a hostedContents endpoint needing authenticated download; width/height
// are 0 when the tag omits them.
struct InlineImage {
    QString url;
    int     width  = 0;
    int     height = 0;
};
// Extract the <img> references from a Teams HTML message body (htmlToText drops
// them from the text). The backend resolves each to a data URI and surfaces them
// as image File previews.
std::vector<InlineImage> extractInlineImages(const QString &html);

namespace JsonMappers {

// Microsoft Graph JSON → normalized domain structs.

// A directory user (/me or /users/{id}).
User toUser(const QJsonObject &graphUser);
// A conversationMember inside a chat's expanded members[] (carries userId/displayName/email).
User toMember(const QJsonObject &member);

// Graph message `body` (HTML or plain text) → TextWithEntities. Handles the
// common inline tags (b/strong, i/em, u, s/del, code, pre, blockquote, a[href])
// and HTML entity decoding; unknown tags pass their inner text through.
TextWithEntities htmlToText(const QString &html);

// A chatMessage (channel or chat). Caller should skip messageType != "message"
// (system events) before calling. Fills ts/date/author(or botName)/text/
// reactions/edited/threadRoot.
Message toMessage(const QJsonObject &m);

// A chat (/me/chats item, members expanded). meId picks the "other" participant
// for a 1:1 DM name and dmUser.
Conversation toChatConversation(const QJsonObject &chat, const QString &meId);
// A channel (/teams/{id}/channels item). teamId/teamName carry the parent team.
Conversation
toChannelConversation(const QJsonObject &channel, const QString &teamId, const QString &teamName);

// A /search/query hit's `resource` (a chatMessage). Derives the conversation from
// channelIdentity (channel) or chatId (chat); convName is left empty (resolved by
// the UI from the conv id).
SearchResult toSearchResult(const QJsonObject &chatMessageResource);

// Map a Graph presence `availability` to the binary active/away the UI shows.
bool presenceActive(const QString &availability);

// The signed-in user's editable profile from GET /me.
MyProfile toMyProfile(const QJsonObject &me);

} // namespace JsonMappers
} // namespace teams

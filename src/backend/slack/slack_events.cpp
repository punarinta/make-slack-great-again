// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/slack/slack_events.h"

#include "backend/slack/json_mappers.h"

namespace slack {

std::optional<Event> normalizeSlackEvent(const QJsonObject &ev) {
    const auto type    = ev.value("type").toString();
    const auto subtype = ev.value("subtype").toString();

    if (type == "message") {
        if (subtype == "message_deleted") {
            // previous_message carries the deleted message; toMessage derives
            // threadRoot (set only when it was a reply) so the channel list can
            // drop the root's reply count.
            const auto prev = JsonMappers::toMessage(ev.value("previous_message").toObject());
            return EvMessageDeleted{
                ConversationId{ev.value("channel").toString()},
                ev.value("deleted_ts").toString(),
                prev.threadRoot
            };
        }
        if (subtype == "message_changed" || subtype == "message_replied") {
            return EvMessageChanged{
                ConversationId{ev.value("channel").toString()},
                JsonMappers::toMessage(ev.value("message").toObject())
            };
        }
        // Plain message or bot_message
        return EvMessageNew{
            ConversationId{ev.value("channel").toString()}, JsonMappers::toMessage(ev)
        };
    }

    if (type == "reaction_added") {
        const auto item = ev.value("item").toObject();
        return EvReactionAdded{
            ConversationId{item.value("channel").toString()},
            item.value("ts").toString(),
            ev.value("reaction").toString(),
            UserId{ev.value("user").toString()}
        };
    }

    if (type == "reaction_removed") {
        const auto item = ev.value("item").toObject();
        return EvReactionRemoved{
            ConversationId{item.value("channel").toString()},
            item.value("ts").toString(),
            ev.value("reaction").toString(),
            UserId{ev.value("user").toString()}
        };
    }

    // channel_marked, group_marked, im_marked, mpim_marked all have the same shape
    if (type == "channel_marked" || type == "group_marked" || type == "im_marked" ||
        type == "mpim_marked") {
        return EvConvMarked{
            ConversationId{ev.value("channel").toString()},
            ev.value("ts").toString(),
            ev.value("unread_count_display").toInt(),
            ev.value("mention_count_display").toInt()
        };
    }

    // user_typing is an RTM-only event (no Events API equivalent), so over Socket
    // Mode this branch is dead — but SessionRealtime rides RTM, where Slack DOES
    // send user_typing, so here it lights up the typing UI for real.
    if (type == "user_typing") {
        return EvTyping{
            ConversationId{ev.value("channel").toString()}, UserId{ev.value("user").toString()}
        };
    }

    if (type == "presence_change") {
        return EvPresenceChanged{
            UserId{ev.value("user").toString()}, ev.value("presence").toString() == "active"
        };
    }

    if (type == "dnd_updated_user") {
        return EvDndChanged{
            UserId{ev.value("user").toString()},
            ev.value("dnd_status").toObject().value("dnd_enabled").toBool()
        };
    }

    if (type == "channel_created") {
        return EvChannelCreated{JsonMappers::toConversation(ev.value("channel").toObject())};
    }

    if (type == "member_joined_channel") {
        return EvMemberJoined{
            ConversationId{ev.value("channel").toString()}, UserId{ev.value("user").toString()}
        };
    }

    // A member updated their profile (incl. avatar). user_change carries the
    // full user object — same shape as users.list — so toUser parses it
    // directly. (user_profile_changed carries only id+profile and would zero
    // out is_admin/is_bot/etc., so we don't map it.)
    if (type == "user_change") {
        return EvUserChanged{JsonMappers::toUser(ev.value("user").toObject())};
    }

    return std::nullopt;
}

std::optional<Event> huddleEventFor(const QJsonObject &ev) {
    if (ev.value("type").toString() != "message")
        return std::nullopt;

    const auto subtype = ev.value("subtype").toString();

    // A huddle starting: USLACKBOT posts a "huddle_thread" message carrying the
    // live `room`. The subtype itself proves it's a huddle, so "ongoing" is just
    // "no end timestamp" (participants may not be populated at the announce
    // moment); a roomless announce still counts as a start.
    QJsonObject room;
    QString     channel  = ev.value("channel").toString();
    bool        isHuddle = false;
    if (subtype == "huddle_thread") {
        room     = ev.value("room").toObject();
        isHuddle = true;
    } else if (subtype == "message_changed") {
        // A huddle ending/changing arrives as an edit of the huddle_thread
        // message (its room gains a date_end).
        const auto inner = ev.value("message").toObject();
        if (inner.value("subtype").toString() == "huddle_thread") {
            room     = inner.value("room").toObject();
            isHuddle = true;
        }
    }
    if (!isHuddle)
        return std::nullopt;

    const auto h = JsonMappers::readHuddleRoom(room);
    return EvHuddleChanged{ConversationId{channel}, h.active, h.link, h.participants};
}

} // namespace slack

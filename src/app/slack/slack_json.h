// Slack Web API JSON → the model, table-driven where it
// can be. Pure functions over the Store: references to users are interned
// (Store::internUser), so a message never waits for the roster.
#pragma once

#include "app/model/store.h"
#include "base/json.h"

#include <string>
#include <string_view>
#include <vector>

namespace slack::mapjson {

// USLACKBOT (Slackbot) / USLACK (the "Slack" workspace notifier): absent
// from users.list, is_bot=false, no observable presence.
bool                isSlackSystemUser(std::string_view id);
// A users.list / users.info member. realName / profileName: real_name and
// display_name (enterprise directories fill display_name with a login
// slug); displayName as for full names (the Store re-resolves it); avatar:
// profile.image_72.
model::User         toUser(const json::Value &u);
// Slack's "Names" preference. A user's display_real_names_override
// (users.prefs.get, client.userBoot "prefs"): 1 full names, -1 display
// names, 0 the workspace's default; kNoPref when absent. The workspace's
// display_real_names (team.prefs.get, userBoot team prefs): 1 full names,
// 0 display names, kNoPref when absent.
constexpr int       kNoPref = 2;
int                 namesOverride(const json::Value &prefs);
int                 namesDefault(const json::Value &prefs);
// What they make together: 1 full names, 0 display names, kNoPref while
// not decided (the override, or for 0 the default, not known).
int                 realNamesFrom(int override, int teamDefault);
// A conversations.list / client.userBoot / im.list / conversations.info
// channel or IM. Unread falls back to 1 when latest > last_read (Slack often
// reports unread_count 0 for channels).
model::Conversation toConversation(const json::Value &c, model::Store &store);
// A message from conversations.history / .replies / chat.postMessage /
// search: text, user or bot identity, files, attachments and unfurls,
// reactions, thread fields, edited, subtype. A message_changed event maps
// its nested "message". Messages whose blocks say more than `text` (bot
// sections, headers, context lines, tables) get mrkdwn built from the
// blocks instead of the fallback text.
model::Message      toMessage(const json::Value &m, model::Store &store);

// Block Kit → mrkdwn (rich_text, section, header, context, table). Image
// blocks are not text (see toBlocks).
std::string               blocksToMrkdwn(const json::Value &blocks);
// The blocks as structure (model::Block) when any is a header, divider,
// image or table — what is drawn as such; else none (the mrkdwn is enough).
// Text blocks become Text blocks of their mrkdwn; actions/inputs are skipped.
std::vector<model::Block> toBlocks(const json::Value &blocks);

// A conversation's live huddle from a "room" object:
// false when it is no huddle (call_family != "huddle").
// A room with neither has_ended nor a date_end is live, even with nobody
// in it; nobody listed means its host (created_by).
struct HuddleRoom {
    bool                        active = false;
    std::string                 link; // huddle_link
    std::vector<model::UserRef> participants;
};
bool toHuddleRoom(const json::Value &room, model::Store &store, HuddleRoom &out);
// The newest huddle_thread message's room in a history page
// (the first page's check), false when there is none or it is no huddle.
bool newestHuddleRoom(const json::Value &messages, model::Store &store, HuddleRoom &out);

// One client.counts entry (channels, mpims and ims alike).
struct Counts {
    std::string id;
    model::Ts   latest = 0, lastRead = 0;
    uint32_t    unread = 0, mentions = 0; // unread: a 1 stands in for has_unreads
};
std::vector<Counts> toCounts(const json::Value &resp);

// stars.list items that are conversations (not starred messages or files).
void starredConversationIds(const json::Value &items, std::vector<std::string> &out);

// An app's or bot's picture from an "icons" object: image_72, else 48, else
// 36 ("" for none).
std::string firstIcon(const json::Value &icons);
// Epoch seconds Slack sends as a number or as a string ("1767225600").
int64_t     epochSecs(const json::Value &v);

// One thread of subscriptions.thread.getView (the Threads feed): where it
// is, how far I read it, and its replies (latest_replies and
// unread_replies, deduplicated, oldest first, threadTs set) with each
// reply's parent_user_id == me. False for one to skip: unsubscribed, or in a
// conversation the Store doesn't have.
struct FeedThread {
    model::ConvRef              conv = model::kNoConv;
    model::Ts                   root = 0, lastRead = 0;
    std::vector<model::Message> replies;
    std::vector<char>           parentIsMe; // per reply
};
bool toFeedThread(const json::Value &t, model::Store &store, FeedThread &out);

// One subscription of subscriptions.thread.getTimestamps (the feed where
// getView is refused, issue #118): which thread, its newest message and my
// read cursor; no messages. False for one in a conversation the Store
// doesn't have.
struct ThreadSub {
    model::ConvRef conv = model::kNoConv;
    model::Ts      root = 0, latest = 0, lastRead = 0;
};
bool toThreadSub(const json::Value &t, model::Store &store, ThreadSub &out);

} // namespace slack::mapjson

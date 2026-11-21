// Pure functions of model::Store, no ui:: dependency — so the sidebar's
// row/section rules are testable without a live Sidebar widget.
#pragma once

#include "model/store.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace shell {

// The notification window: a message older than this never notifies, and a
// conversation idle for longer neither badges nor counts.
inline constexpr int64_t kMaxNotifyAgeSecs = 30 * 86400; // 30 days

// conv id → epoch secs, when each conversation was last opened or seen
// unread (keeps it listed for the relevant-days window).
using VisitStamps = std::unordered_map<std::string, int64_t>;

// Liveness filter, shared with the quick switcher.
// - the service never prunes a DM, so one whose peer was deactivated
//   or never resolves to a name is dead
// - a placeholder peer (not loaded yet) is let through
bool deadDm(const model::Store &store, const model::Conversation &c);

bool               sidebarMuted(model::NotifyLevel effectiveLevel);
model::NotifyLevel sidebarLevel(const model::Conversation &c, model::NotifyLevel defaultLevel);
// The one rule for bold rows and the unreads-only filter.
bool               sidebarPaintsUnread(
    const model::Conversation &c, model::NotifyLevel defaultLevel, bool highlightMentionsOnly
);
// The row's badge number: 0 muted or idle past kMaxNotifyAgeSecs, else a DM's
// unread count (its mentions in an agent session: Session counts those as
// mentions) or a channel's mentions.
int sidebarRedCount(
    const model::Conversation &c,
    model::NotifyLevel         defaultLevel,
    bool                       agentSessions,
    int64_t                    nowSecs
);
bool sidebarIsApp(const model::Conversation &c, const model::Store &store);
// A Slack Connect conversation, shared with another org.
bool sidebarIsExternal(const model::Conversation &c, const model::Store &store);
// An unread DM or group DM from a VIP.
// - temporary, like Slack's "VIP unreads": back to its section once read
// - a group DM needs its newest message loaded and a VIP's;
//   a VIP merely being in the group didn't send the unread
bool sidebarIsVip(
    const model::Conversation &c, const model::Store &store, model::NotifyLevel defaultLevel
);
// Relevant: unread, open, visited or active within the window, or a channel
// nothing is known about yet. Agent sessions are always listed.
bool sidebarRelevant(
    model::ConvRef      conv,
    const model::Store &store,
    model::ConvRef      selected,
    bool                agentSessions,
    int64_t             nowSecs,
    int                 relevantDays,
    const VisitStamps  &visited
);

// Precedence, first match wins:
// - Vip outranks all (a VIP may also be starred or an app)
// - Drafts next: an unsent draft lifts it out of Starred and custom sections too
// - then Starred, Apps, External, Direct/Channels
enum class SidebarKind : uint8_t { Starred, Channels, Direct, Apps, External, Vip, Drafts };
SidebarKind sidebarClassify(
    model::ConvRef      ref,
    const model::Store &store,
    model::NotifyLevel  defaultLevel,
    bool                vipsCapability,
    bool                externalSectionEnabled
);
// drafted: it has an unsent draft (buildSidebarLists finds them all in one
// pass, not a draft lookup per conversation).
SidebarKind sidebarClassify(
    model::ConvRef      ref,
    const model::Store &store,
    model::NotifyLevel  defaultLevel,
    bool                vipsCapability,
    bool                externalSectionEnabled,
    bool                drafted
);

// The rebuild-vs-refresh call on a Meta change: under unreads-only a row
// comes or goes when its bold state flips.
// - Starred and custom sections stay put unless their own scope flag is on
// - Drafts never flips (a draft is never hidden)
struct RowFlipInput {
    bool unreadsOnly                        = false;
    bool isSelectedConv                     = false;
    bool starred                            = false;
    bool isInCustomSection                  = false;
    bool isInDrafts                         = false;
    bool unreadsOnlyAppliesToStarred        = false;
    bool unreadsOnlyAppliesToCustomSections = false;
    bool rowExists                          = false;
    bool rowWasUnread                       = false;
    bool wouldPaintUnread                   = false;
    bool isMember                           = false;
};
bool sidebarFilterFlip(const RowFlipInput &in);

// Bucketed by sidebarClassify(), server order inside each.
// - hiddenChannels/hiddenExternal: dropped by relevance/visibility, shown as "N more";
//   a hidden Connect DM isn't counted at all
// - vipUnread and drafts are never hidden
struct SidebarLists {
    std::vector<model::ConvRef> starred, channels, direct, apps;
    std::vector<model::ConvRef> external, vipUnread, drafts, hiddenChannels, hiddenExternal;
};

struct SidebarListFilters {
    int                relevantDays    = 14;
    bool               showAgentsApps  = true;
    bool               unreadsOnly     = false;
    bool               externalSection = true; // off: Connect chats stay in Channels / DMs
    bool               unreadsOnlyAppliesToStarred = false;
    bool               highlightMentionsOnly       = true;
    model::NotifyLevel defaultLevel                = model::NotifyLevel::All;
};

// inCustom: by ConvRef, already placed in one of the user's own sections
// (Store::sections) and so skipped here -- the caller computes that, since
// it also needs it to render those sections' own rows.
SidebarLists buildSidebarLists(
    const model::Store       &store,
    const SidebarListFilters &filters,
    model::ConvRef            selected,
    bool                      agentSessions,
    bool                      vipsCapability,
    int64_t                   nowSecs,
    const VisitStamps        &visited,
    bool                      showAllChannels,
    const std::vector<char>  &inCustom
);

// The workspace's share of the tray dot, launcher badge and rail tile dot.
// - `important`: unread DMs + mentions (agent workspace: only its answers, what needs you)
// - `unread`: other unread activity in "All new posts" channels (`fallback`: the global level)
// - muted, "Nothing", left, closed, and idle past kMaxNotifyAgeSecs count for neither
struct Attention {
    int  important = 0;
    bool unread    = false;
    int  dot() const { return important > 0 ? 2 : unread ? 1 : 0; } // 0, 1 blue, 2 red
    bool operator==(const Attention &) const = default;
};
Attention
workspaceAttention(const model::Store &store, model::NotifyLevel fallback, int64_t nowSecs);

// The launcher badge and tray dot's share across every open workspace: each
// workspace's own dot is its own Attention, this is just their sum -- a
// muted workspace still shows its tile's dot, but doesn't count toward
// these (the caller excludes it, so it's not in `atts`).
Attention sumAttention(const std::vector<Attention> &atts);

} // namespace shell

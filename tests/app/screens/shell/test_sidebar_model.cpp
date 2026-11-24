#include "app/model/store.h"
#include "screens/shell/sidebar_model.h"
#include "support/test.h"

using namespace model;
using namespace shell;

namespace {

ConvRef addChannel(Store &s, const char *id, bool starred = false, uint32_t unread = 0) {
    Conversation c;
    c.id      = id;
    c.name    = id;
    c.kind    = ConvKind::Channel;
    c.member  = true;
    c.starred = starred;
    c.unread  = unread;
    return s.addConversation(std::move(c));
}

ConvRef addDm(Store &s, const char *id, UserRef peer, uint32_t unread = 0) {
    Conversation c;
    c.id     = id;
    c.kind   = ConvKind::Dm;
    c.dmUser = peer;
    c.member = true;
    c.unread = unread;
    return s.addConversation(std::move(c));
}

const std::vector<char> kNoCustom; // no conversation is in a custom section

} // namespace

TEST("sidebar_model: deadDm is true only for a resolved, deactivated or id-labelled peer") {
    Store s;
    User  live;
    live.id = "U1", live.name = "mira";
    UserRef u = s.addUser(live);
    ConvRef d = addDm(s, "D1", u);
    CHECK_FALSE(deadDm(s, s.conversation(d)));

    User gone;
    gone.id = "U2", gone.deleted = true;
    UserRef deletedUser = s.addUser(gone);
    ConvRef d2          = addDm(s, "D2", deletedUser);
    CHECK(deadDm(s, s.conversation(d2)));

    // A placeholder (not yet resolved) peer is let through, not treated dead.
    UserRef placeholder = s.internUser("U9");
    ConvRef d3          = addDm(s, "D3", placeholder);
    CHECK_FALSE(deadDm(s, s.conversation(d3)));
}

TEST("sidebar_model: paintsUnread follows the mute/level/highlight rules") {
    Conversation c;
    c.unread = 3, c.mentions = 0, c.kind = ConvKind::Channel;
    // Default level, highlight-mentions-only off: any unread paints.
    CHECK(sidebarPaintsUnread(c, NotifyLevel::All, false));
    // Mentions-only level, highlight off, a channel with no mentions: quiet.
    c.notify = NotifyLevel::Mentions;
    CHECK_FALSE(sidebarPaintsUnread(c, NotifyLevel::All, false));
    // highlightMentionsOnly on: the mentions-only channel paints anyway.
    CHECK(sidebarPaintsUnread(c, NotifyLevel::All, true));
    // Muted always wins.
    c.muted = true;
    CHECK_FALSE(sidebarPaintsUnread(c, NotifyLevel::All, true));
}

TEST("sidebar_model: relevant — unread, selected, visited and activity windows") {
    Store       s;
    ConvRef     idle = addChannel(s, "C1");
    VisitStamps visited;
    // Nothing known: a channel defaults relevant (not a DM), a DM does not.
    CHECK(sidebarRelevant(idle, s, kNoConv, false, 1'000'000, 14, visited));

    User peer;
    peer.id    = "U1";
    UserRef u  = s.addUser(peer);
    ConvRef dm = addDm(s, "D1", u);
    CHECK_FALSE(sidebarRelevant(dm, s, kNoConv, false, 1'000'000, 14, visited));
    // Selecting it makes it relevant regardless.
    CHECK(sidebarRelevant(dm, s, dm, false, 1'000'000, 14, visited));
    // A recent visit stamp inside the window also does.
    visited[s.conversation(dm).id] = 1'000'000 - 1000;
    CHECK(sidebarRelevant(dm, s, kNoConv, false, 1'000'000, 14, visited));
    // An agent workspace lists every session regardless of the window.
    CHECK(sidebarRelevant(dm, s, kNoConv, true, 1'000'000, 14, VisitStamps{}));
}

TEST("sidebar_model: filterFlip only fires for a bold-state change under unreads-only") {
    // No row yet, unreads-only, would paint unread, a member: comes in.
    CHECK(sidebarFilterFlip({.unreadsOnly = true, .wouldPaintUnread = true, .isMember = true}));
    // No row yet, would not paint unread: stays out, no flip.
    CHECK_FALSE(sidebarFilterFlip({.unreadsOnly = true, .isMember = true}));
    // A row exists, its painted state didn't change: no flip.
    CHECK_FALSE(sidebarFilterFlip(
        {.unreadsOnly = true, .rowExists = true, .rowWasUnread = true, .wouldPaintUnread = true}
    ));
    // A row exists and its painted state flipped: rebuild.
    CHECK(sidebarFilterFlip({.unreadsOnly = true, .rowExists = true, .rowWasUnread = true}));
    // Starred or a custom section, their own scope off: never flips.
    CHECK_FALSE(sidebarFilterFlip(
        {.unreadsOnly = true, .starred = true, .rowExists = true, .rowWasUnread = true}
    ));
    CHECK_FALSE(sidebarFilterFlip(
        {.unreadsOnly = true, .isInCustomSection = true, .rowExists = true, .rowWasUnread = true}
    ));
    // Their own scope on: flips like any other row.
    CHECK(sidebarFilterFlip(
        {.unreadsOnly                 = true,
         .starred                     = true,
         .unreadsOnlyAppliesToStarred = true,
         .rowExists                   = true,
         .rowWasUnread                = true}
    ));
    CHECK(sidebarFilterFlip(
        {.unreadsOnly                        = true,
         .isInCustomSection                  = true,
         .unreadsOnlyAppliesToCustomSections = true,
         .rowExists                          = true,
         .rowWasUnread                       = true}
    ));
    // Drafts never flips, whatever the scopes say.
    CHECK_FALSE(sidebarFilterFlip(
        {.unreadsOnly                        = true,
         .isInDrafts                         = true,
         .unreadsOnlyAppliesToStarred        = true,
         .unreadsOnlyAppliesToCustomSections = true,
         .rowExists                          = true,
         .rowWasUnread                       = true}
    ));
    // The selected conversation never flips out.
    CHECK_FALSE(sidebarFilterFlip(
        {.unreadsOnly = true, .isSelectedConv = true, .rowExists = true, .rowWasUnread = true}
    ));
    // Filter off: never flips.
    CHECK_FALSE(sidebarFilterFlip({.rowExists = true, .rowWasUnread = true}));
}

TEST("sidebar_model: isExternal — Connect-shared, a stranger DM peer, or a stranger in a group") {
    Store s;
    User  stranger;
    stranger.id = "U1", stranger.stranger = true;
    UserRef strangerUser = s.addUser(stranger);
    User    colleague;
    colleague.id          = "U2";
    UserRef colleagueUser = s.addUser(colleague);

    Conversation shared;
    shared.extShared = true;
    CHECK(sidebarIsExternal(shared, s));

    ConvRef dm = addDm(s, "D1", strangerUser);
    CHECK(sidebarIsExternal(s.conversation(dm), s));
    ConvRef internalDm = addDm(s, "D2", colleagueUser);
    CHECK_FALSE(sidebarIsExternal(s.conversation(internalDm), s));

    Conversation group;
    group.kind    = ConvKind::Group;
    group.members = {colleagueUser, strangerUser};
    CHECK(sidebarIsExternal(group, s));
    group.members = {colleagueUser};
    CHECK_FALSE(sidebarIsExternal(group, s));
}

TEST("sidebar_model: isVip — an unread DM from a VIP, muted or read never counts") {
    Store s;
    User  vip;
    vip.id = "U1", vip.vip = true;
    UserRef vipUser = s.addUser(vip);

    ConvRef dm = addDm(s, "D1", vipUser, 1);
    CHECK(sidebarIsVip(s.conversation(dm), s, NotifyLevel::All));
    s.conversation(dm).unread = 0;
    CHECK_FALSE(sidebarIsVip(s.conversation(dm), s, NotifyLevel::All)); // read: not VIP anymore
    s.conversation(dm).unread = 1;
    s.conversation(dm).muted  = true;
    CHECK_FALSE(sidebarIsVip(s.conversation(dm), s, NotifyLevel::All)); // muted: never VIP
}

TEST("sidebar_model: isVip — a group DM counts only its own newest, unread, loaded message") {
    Store s;
    User  vip;
    vip.id = "U1", vip.vip = true;
    UserRef vipUser = s.addUser(vip);
    User    other;
    other.id          = "U2";
    UserRef otherUser = s.addUser(other);

    Conversation g;
    g.kind     = ConvKind::Group;
    g.unread   = 1;
    g.latest   = 100;
    g.lastRead = 50;
    g.members  = {vipUser, otherUser};
    Message m;
    m.ts   = 100;
    m.user = vipUser;
    g.messages.push_back(std::move(m));
    ConvRef group = s.addConversation(std::move(g));
    CHECK(sidebarIsVip(s.conversation(group), s, NotifyLevel::All));

    // The loaded "last" message isn't actually the newest one: stale, no VIP.
    s.conversation(group).latest = 200;
    CHECK_FALSE(sidebarIsVip(s.conversation(group), s, NotifyLevel::All));
    s.conversation(group).latest = 100;

    // Its author already read: no longer unread by that message.
    s.conversation(group).lastRead = 100;
    CHECK_FALSE(sidebarIsVip(s.conversation(group), s, NotifyLevel::All));
    s.conversation(group).lastRead = 50;

    // The newest message is from a non-VIP member: not VIP, even though a
    // VIP is in the group.
    s.conversation(group).messages.back().user = otherUser;
    CHECK_FALSE(sidebarIsVip(s.conversation(group), s, NotifyLevel::All));

    // No messages loaded at all: nothing to judge by.
    s.conversation(group).messages.clear();
    CHECK_FALSE(sidebarIsVip(s.conversation(group), s, NotifyLevel::All));
}

TEST("sidebar_model: classify — VIP outranks Drafts/Starred/Apps, External only when enabled") {
    Store s;
    User  vip;
    vip.id = "U1", vip.vip = true;
    UserRef vipUser = s.addUser(vip);

    ConvRef dm                 = addDm(s, "D1", vipUser, 1);
    s.conversation(dm).starred = true; // starred too: VIP still wins
    CHECK(sidebarClassify(dm, s, NotifyLevel::All, true, true) == SidebarKind::Vip);
    // The vips capability off: falls through to Starred.
    CHECK(sidebarClassify(dm, s, NotifyLevel::All, false, true) == SidebarKind::Starred);

    ConvRef ext                   = addChannel(s, "C1");
    s.conversation(ext).extShared = true;
    CHECK(sidebarClassify(ext, s, NotifyLevel::All, false, true) == SidebarKind::External);
    // The external section off: it lands in Channels like any other.
    CHECK(sidebarClassify(ext, s, NotifyLevel::All, false, false) == SidebarKind::Channels);

    // A bot DM is Apps, ahead of External (a bot can be a Connect stranger
    // too) but behind Starred/VIP.
    User bot;
    bot.id = "U2", bot.bot = true, bot.stranger = true;
    UserRef botUser = s.addUser(bot);
    ConvRef botDm   = addDm(s, "D2", botUser);
    CHECK(sidebarClassify(botDm, s, NotifyLevel::All, false, true) == SidebarKind::Apps);
    // An ordinary DM, nothing special about it: Direct.
    User plain;
    plain.id          = "U3";
    UserRef plainUser = s.addUser(plain);
    ConvRef plainDm   = addDm(s, "D3", plainUser);
    CHECK(sidebarClassify(plainDm, s, NotifyLevel::All, false, true) == SidebarKind::Direct);
    // A channel with none of the above: Channels.
    ConvRef plainChannel = addChannel(s, "C3");
    CHECK(sidebarClassify(plainChannel, s, NotifyLevel::All, false, true) == SidebarKind::Channels);

    // An unsent draft lifts a conversation to Drafts, out of Starred too --
    // but a VIP still outranks it.
    ConvRef drafted = addChannel(s, "C2", true);
    s.setDraft(drafted, 0, "hello", {});
    CHECK(sidebarClassify(drafted, s, NotifyLevel::All, false, true) == SidebarKind::Drafts);
    s.setDraft(dm, 0, "hello", {});
    CHECK(sidebarClassify(dm, s, NotifyLevel::All, true, true) == SidebarKind::Vip);
}

TEST("sidebar_model: buildSidebarLists partitions starred, channels, DMs and apps") {
    Store s;
    User  bot;
    bot.id = "B1", bot.bot = true;
    UserRef botUser = s.addUser(bot);
    User    human;
    human.id          = "U1";
    UserRef humanUser = s.addUser(human);

    ConvRef starred = addChannel(s, "C1", true);
    // Under unreads-only, with no unread, not selected, not visited: hidden.
    ConvRef channel = addChannel(s, "C2");
    ConvRef dm      = addDm(s, "D1", humanUser, 1); // unread: stays listed
    ConvRef app     = addDm(s, "D2", botUser, 1);   // unread: stays listed

    SidebarListFilters f;
    f.unreadsOnly = true;
    VisitStamps  visited;
    SidebarLists out =
        buildSidebarLists(s, f, kNoConv, false, false, 1'000'000, visited, false, kNoCustom);

    CHECK(out.starred.size() == 1 && out.starred[0] == starred);
    CHECK(out.direct.size() == 1 && out.direct[0] == dm);
    CHECK(out.apps.size() == 1 && out.apps[0] == app);
    // The idle, unselected, un-visited channel is hidden, not listed.
    bool hidden = false;
    for (ConvRef c : out.hiddenChannels)
        hidden = hidden || c == channel;
    CHECK(hidden);
    for (ConvRef c : out.channels)
        CHECK(c != channel);

    // showAllChannels folds the hidden ones into the Channels section.
    SidebarLists shown =
        buildSidebarLists(s, f, kNoConv, false, false, 1'000'000, visited, true, kNoCustom);
    CHECK(shown.hiddenChannels.empty());
    bool found = false;
    for (ConvRef c : shown.channels)
        found = found || c == channel;
    CHECK(found);
}

TEST("sidebar_model: buildSidebarLists — VIP unreads, External connections, custom exclusion") {
    Store s;
    User  vip;
    vip.id = "U1", vip.vip = true;
    UserRef vipUser = s.addUser(vip);
    User    stranger;
    stranger.id = "U2", stranger.stranger = true;
    UserRef strangerUser = s.addUser(stranger);

    ConvRef vipDm = addDm(s, "D1", vipUser, 1);
    ConvRef extDm = addDm(s, "D2", strangerUser, 1); // unread: relevant, so listed
    ConvRef own   = addChannel(s, "C1");             // placed in a custom section by the caller

    SidebarListFilters f;
    f.externalSection = true;
    std::vector<char> inCustom(s.conversationCount(), 0);
    inCustom[own] = 1;
    VisitStamps  visited;
    SidebarLists out =
        buildSidebarLists(s, f, kNoConv, false, true, 1'000'000, visited, false, inCustom);

    CHECK(out.vipUnread.size() == 1 && out.vipUnread[0] == vipDm);
    CHECK(out.external.size() == 1 && out.external[0] == extDm);
    for (const auto *section : {&out.starred, &out.channels, &out.direct, &out.apps})
        for (ConvRef c : *section)
            CHECK(c != own); // already in its custom section, not bucketed here

    // vipsCapability off: the VIP DM falls through to the ordinary DM list.
    SidebarLists noVips =
        buildSidebarLists(s, f, kNoConv, false, false, 1'000'000, visited, false, inCustom);
    CHECK(noVips.vipUnread.empty());
    bool foundInDirect = false;
    for (ConvRef c : noVips.direct)
        foundInDirect = foundInDirect || c == vipDm;
    CHECK(foundInDirect);
}

TEST("sidebar_model: buildSidebarLists — Drafts is never hidden; Starred's own scope") {
    Store   s;
    ConvRef drafted = addChannel(s, "C1", true); // starred too: a draft still wins
    s.setDraft(drafted, 0, "hello", {});
    ConvRef starredRead = addChannel(s, "C2", true); // starred, no unread
    s.setDraft(starredRead, 1, "a reply", {});       // a thread's draft doesn't count

    SidebarListFilters f;
    f.unreadsOnly = true;
    VisitStamps  visited;
    SidebarLists out =
        buildSidebarLists(s, f, kNoConv, false, false, 1'000'000, visited, false, kNoCustom);
    CHECK(out.drafts.size() == 1 && out.drafts[0] == drafted);
    // The draft stays out of Starred; a read, non-draft starred row is kept
    // regardless of unreads-only, since its own scope is off by default.
    CHECK(out.starred.size() == 1 && out.starred[0] == starredRead);

    // unreadsOnlyAppliesToStarred: a read starred row is hidden too.
    f.unreadsOnlyAppliesToStarred = true;
    SidebarLists scoped =
        buildSidebarLists(s, f, kNoConv, false, false, 1'000'000, visited, false, kNoCustom);
    CHECK(scoped.starred.empty());
}

TEST("sidebar_model: workspaceAttention sums mentions/unread, skips muted and stale") {
    Store   s;
    ConvRef a                    = addChannel(s, "C1");
    s.conversation(a).mentions   = 2;
    s.conversation(a).unread     = 5;
    ConvRef muted                = addChannel(s, "C2");
    s.conversation(muted).unread = 9;
    s.conversation(muted).muted  = true;

    Attention att = workspaceAttention(s, NotifyLevel::All, 1'000'000);
    CHECK(att.important == 2); // mentions only, for a non-DM under "All"
    CHECK(att.unread);         // unread beyond mentions, on "All"
    CHECK(att.dot() == 2);     // important > 0 -> red
}

TEST("sidebar_model: sumAttention — important adds, unread ORs, empty is quiet") {
    CHECK(sumAttention({}).dot() == 0);
    CHECK(sumAttention({{0, false}, {0, false}}).dot() == 0);

    const Attention sum = sumAttention({{2, false}, {0, true}, {3, false}});
    CHECK(sum.important == 5);
    CHECK(sum.unread);
    CHECK(sum.dot() == 2); // any important -> red, whatever a muted-workspace exclusion left

    const Attention onlyUnread = sumAttention({{0, false}, {0, true}});
    CHECK(onlyUnread.important == 0);
    CHECK(onlyUnread.unread);
    CHECK(onlyUnread.dot() == 1); // no mentions anywhere, but one workspace has unread
}

TEST("sidebar_model: appBadge — DMs and mentions count; the dot only for \"All unread\"") {
    Store   s;
    ConvRef quiet                = addChannel(s, "C1");
    s.conversation(quiet).unread = 4;
    s.conversation(quiet).notify = NotifyLevel::Mentions; // mentions only, no mention

    // Not on the tray's blue dot (a mentions-only channel), but bold in the
    // sidebar by default, so "All unread messages" dots the app icon.
    Attention att = workspaceAttention(s, NotifyLevel::All, 1'000'000);
    CHECK(att.important == 0 && !att.unread && att.anyUnread);
    CHECK(appBadge(att, false) == (AppBadge{0, false})); // "DMs and mentions only"
    CHECK(appBadge(att, true) == (AppBadge{0, true}));
    // The sidebar doesn't highlight it: neither does the badge.
    CHECK_FALSE(workspaceAttention(s, NotifyLevel::All, 1'000'000, false).anyUnread);
    // Muted counts for nothing.
    s.conversation(quiet).muted = true;
    CHECK_FALSE(workspaceAttention(s, NotifyLevel::All, 1'000'000).anyUnread);
    s.conversation(quiet).muted = false;

    // A mention: the number wins over the dot.
    s.conversation(quiet).mentions = 1;
    att                            = workspaceAttention(s, NotifyLevel::All, 1'000'000);
    CHECK(appBadge(att, true) == (AppBadge{1, false}));
    CHECK(appBadge(att, false) == (AppBadge{1, false}));

    // Summed across workspaces, like the tray's.
    const Attention sum = sumAttention({{0, false, false}, {0, false, true}});
    CHECK(sum.anyUnread && appBadge(sum, true).dot);
    CHECK_FALSE(appBadge(sumAttention({}), true).dot);
}

TEST("sidebar_model: redCount — muted/stale are 0, a DM's unread vs. a channel's mentions") {
    Store         s;
    const int64_t now = 1'000'000;

    ConvRef muted               = addChannel(s, "C1", false, 5);
    s.conversation(muted).muted = true;
    CHECK(sidebarRedCount(s.conversation(muted), NotifyLevel::All, false, now) == 0);

    ConvRef stale                = addChannel(s, "C2", false, 5);
    s.conversation(stale).latest = Ts(now - kMaxNotifyAgeSecs - 1);
    CHECK(sidebarRedCount(s.conversation(stale), NotifyLevel::All, false, now) == 0);

    UserRef u                   = s.addUser({});
    ConvRef dm                  = addDm(s, "D1", u, 7);
    s.conversation(dm).mentions = 2;
    // Not an agent session: a DM's badge is every unread message.
    CHECK(sidebarRedCount(s.conversation(dm), NotifyLevel::All, false, now) == 7);
    // An agent session only counts what needs me -- its mentions.
    CHECK(sidebarRedCount(s.conversation(dm), NotifyLevel::All, true, now) == 2);

    ConvRef chan                  = addChannel(s, "C3", false, 9);
    s.conversation(chan).mentions = 3;
    CHECK(sidebarRedCount(s.conversation(chan), NotifyLevel::All, false, now) == 3);
}

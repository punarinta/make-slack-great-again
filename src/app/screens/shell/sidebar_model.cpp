#include "screens/shell/sidebar_model.h"

#include <algorithm>

using model::ConvKind;
using model::ConvRef;
using model::tsSecs;

namespace shell {

namespace {

bool looksLikeUserId(std::string_view s) {
    if (s.size() < 9 || (s[0] != 'U' && s[0] != 'W'))
        return false;
    for (char ch : s.substr(1))
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
            return false;
    return true;
}

} // namespace

bool deadDm(const model::Store &store, const model::Conversation &c) {
    if (c.kind != ConvKind::Dm || c.dmUser == model::kNoUser)
        return false;
    const model::User &u = store.user(c.dmUser);
    return !u.placeholder &&
           (u.deleted || u.displayName == "deactivateduser" || looksLikeUserId(u.label()));
}

bool sidebarMuted(model::NotifyLevel effectiveLevel) {
    return effectiveLevel == model::NotifyLevel::Nothing;
}

model::NotifyLevel sidebarLevel(const model::Conversation &c, model::NotifyLevel defaultLevel) {
    return c.effectiveNotify(defaultLevel);
}

bool sidebarPaintsUnread(
    const model::Conversation &c, model::NotifyLevel defaultLevel, bool highlightMentionsOnly
) {
    const model::NotifyLevel level = sidebarLevel(c, defaultLevel);
    if (c.unread == 0 || sidebarMuted(level))
        return false;
    return !(
        level == model::NotifyLevel::Mentions && !highlightMentionsOnly && !c.isDirect() &&
        c.mentions == 0
    );
}

int sidebarRedCount(
    const model::Conversation &c,
    model::NotifyLevel         defaultLevel,
    bool                       agentSessions,
    int64_t                    nowSecs
) {
    if (sidebarMuted(sidebarLevel(c, defaultLevel)) ||
        (c.latest && tsSecs(c.latest) < nowSecs - kMaxNotifyAgeSecs))
        return 0;
    return c.isDirect() && !agentSessions ? int(c.unread) : int(c.mentions);
}

bool sidebarIsApp(const model::Conversation &c, const model::Store &store) {
    return c.kind == ConvKind::Dm && c.dmUser != model::kNoUser && store.user(c.dmUser).bot;
}

bool sidebarIsExternal(const model::Conversation &c, const model::Store &store) {
    if (c.extShared)
        return true;
    if (c.kind == ConvKind::Dm)
        return c.dmUser != model::kNoUser && store.user(c.dmUser).stranger;
    if (c.kind == ConvKind::Group)
        return std::any_of(c.members.begin(), c.members.end(), [&](model::UserRef u) {
            return store.user(u).stranger;
        });
    return false;
}

bool sidebarIsVip(
    const model::Conversation &c, const model::Store &store, model::NotifyLevel defaultLevel
) {
    if (c.unread == 0 || sidebarMuted(sidebarLevel(c, defaultLevel)) || !c.isDirect())
        return false;
    if (c.kind == ConvKind::Dm)
        return c.dmUser != model::kNoUser && store.user(c.dmUser).vip;
    if (c.messages.empty())
        return false;
    const model::Message &last = c.messages.back();
    return last.ts == c.latest && last.ts > c.lastRead && last.user != model::kNoUser &&
           store.user(last.user).vip;
}

SidebarKind sidebarClassify(
    ConvRef             ref,
    const model::Store &store,
    model::NotifyLevel  defaultLevel,
    bool                vipsCapability,
    bool                externalSectionEnabled
) {
    const auto &c = store.conversation(ref);
    if (vipsCapability && sidebarIsVip(c, store, defaultLevel))
        return SidebarKind::Vip;
    if (store.draft(ref, 0) != nullptr)
        return SidebarKind::Drafts;
    if (c.starred)
        return SidebarKind::Starred;
    if (sidebarIsApp(c, store))
        return SidebarKind::Apps;
    if (externalSectionEnabled && sidebarIsExternal(c, store))
        return SidebarKind::External;
    return c.isDirect() ? SidebarKind::Direct : SidebarKind::Channels;
}

bool sidebarRelevant(
    ConvRef             conv,
    const model::Store &store,
    ConvRef             selected,
    bool                agentSessions,
    int64_t             nowSecs,
    int                 relevantDays,
    const VisitStamps  &visited
) {
    const auto &c = store.conversation(conv);
    if (c.unread > 0 || conv == selected || agentSessions)
        return true;
    const int64_t cutoff = nowSecs - int64_t(std::max(1, relevantDays)) * 86400;
    const auto    v      = visited.find(c.id);
    if (v != visited.end() && v->second >= cutoff)
        return true;
    const model::Ts activity = std::max(c.latest, c.lastRead);
    if (activity)
        return tsSecs(activity) >= cutoff;
    return !c.isDirect();
}

bool sidebarFilterFlip(const RowFlipInput &in) {
    return in.unreadsOnly && !in.isSelectedConv &&
           !(in.starred && !in.isInCustomSection && !in.unreadsOnlyAppliesToStarred) &&
           !(in.isInCustomSection && !in.unreadsOnlyAppliesToCustomSections) && !in.isInDrafts &&
           (in.rowExists ? in.rowWasUnread != in.wouldPaintUnread
                         : in.isMember && in.wouldPaintUnread);
}

SidebarLists buildSidebarLists(
    const model::Store       &store,
    const SidebarListFilters &filters,
    ConvRef                   selected,
    bool                      agentSessions,
    bool                      vipsCapability,
    int64_t                   nowSecs,
    const VisitStamps        &visited,
    bool                      showAllChannels,
    const std::vector<char>  &inCustom
) {
    SidebarLists out;
    const auto   listed = [&](ConvRef c) {
        return !filters.unreadsOnly || c == selected ||
               sidebarPaintsUnread(
                   store.conversation(c), filters.defaultLevel, filters.highlightMentionsOnly
               );
    };
    const auto relevantAt = [&](ConvRef c) {
        return sidebarRelevant(
            c, store, selected, agentSessions, nowSecs, filters.relevantDays, visited
        );
    };
    for (ConvRef c = 0; c < store.conversationCount(); ++c) {
        const auto &cv = store.conversation(c);
        if (!cv.member || deadDm(store, cv) || (c < inCustom.size() && inCustom[c]))
            continue; // left channels, closed or dead DMs, or already in a custom section
        switch (sidebarClassify(
            c, store, filters.defaultLevel, vipsCapability, filters.externalSection
        )) {
        case SidebarKind::Drafts:
            out.drafts.push_back(c); // a draft is never hidden by the filters
            break;
        case SidebarKind::Starred:
            if (!filters.unreadsOnly || !filters.unreadsOnlyAppliesToStarred || listed(c))
                out.starred.push_back(c);
            break;
        case SidebarKind::Apps:
            if (listed(c) && (filters.showAgentsApps || c == selected))
                out.apps.push_back(c);
            break;
        case SidebarKind::Vip:
            out.vipUnread.push_back(c); // unread already required; always shown
            break;
        case SidebarKind::External:
            if (relevantAt(c) && listed(c))
                out.external.push_back(c);
            // Its own hidden list (hiddenExternal, not hiddenChannels): a
            // Connect DM doesn't count toward "N more", only a channel does.
            else if (!cv.isDirect())
                out.hiddenExternal.push_back(c);
            break;
        case SidebarKind::Direct:
            if (relevantAt(c) && listed(c))
                out.direct.push_back(c);
            break;
        case SidebarKind::Channels:
            if (relevantAt(c) && listed(c))
                out.channels.push_back(c);
            else
                out.hiddenChannels.push_back(
                    c
                ); // a channel only: a DM that drops just isn't listed
            break;
        }
    }
    if (showAllChannels) {
        out.channels.insert(
            out.channels.end(), out.hiddenChannels.begin(), out.hiddenChannels.end()
        );
        out.external.insert(
            out.external.end(), out.hiddenExternal.begin(), out.hiddenExternal.end()
        );
        out.hiddenChannels.clear();
        out.hiddenExternal.clear();
    }
    return out;
}

Attention workspaceAttention(const model::Store &st, model::NotifyLevel fallback, int64_t nowSecs) {
    Attention a;
    for (ConvRef c = 0; c < st.conversationCount(); ++c) {
        const auto &cv = st.conversation(c);
        if (!cv.member || (cv.latest && tsSecs(cv.latest) < nowSecs - kMaxNotifyAgeSecs))
            continue; // left or closed; or all of it older than the notification window
        const model::NotifyLevel level = cv.effectiveNotify(fallback);
        if (level == model::NotifyLevel::Nothing)
            continue;
        if (st.answersAreMentions) // an agent session: only what needs you
            a.important += int(cv.mentions);
        else if (cv.isDirect())
            a.important += int(cv.unread);
        else {
            a.important += int(cv.mentions);
            a.unread = a.unread || (level == model::NotifyLevel::All && cv.unread > cv.mentions);
        }
    }
    return a;
}

Attention sumAttention(const std::vector<Attention> &atts) {
    Attention sum;
    for (const Attention &a : atts) {
        sum.important += a.important;
        sum.unread = sum.unread || a.unread;
    }
    return sum;
}

} // namespace shell

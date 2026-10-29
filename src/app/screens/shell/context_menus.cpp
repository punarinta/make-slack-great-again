#include "screens/shell/context_menus.h"

#include "base/i18n.h"
#include "base/str.h"
#include "gfx/icons_generated.h"
#include "screens/shell/shell.h"
#include "screens/shell/shell_dialogs.h"

#include <algorithm>
#include <utility>

using gfx::Icon;
using i18n::tr;
using model::ConvKind;
using model::ConvRef;
using model::kNoConv;
using model::NotifyLevel;
using model::UserRef;

namespace shell {

namespace {

// One row per item: its label in a channel's menu and, where different, in
// a direct conversation's (the alt label), and its icon (icons show only on
// the notification levels).
constexpr uint16_t    kNone   = ui::Button::kNoIcon;
constexpr ui::MenuDef kDefs[] = {
    {Menus::kStar, kNone, N_("Star channel"), N_("Star conversation"), nullptr},
    {Menus::kUnstar, kNone, N_("Unstar channel"), N_("Unstar conversation"), nullptr},
    {Menus::kNotifyAll, uint16_t(Icon::Bell), N_("All new posts"), nullptr, nullptr},
    {Menus::kNotifyMentions, uint16_t(Icon::Bell), N_("Just mentions"), nullptr, nullptr},
    {Menus::kNotifyMute, uint16_t(Icon::BellOff), N_("Mute and hide"), nullptr, nullptr},
    {Menus::kMute, kNone, N_("Mute"), nullptr, nullptr},
    {Menus::kUnmute, kNone, N_("Unmute"), nullptr, nullptr},
    {Menus::kRename, kNone, N_("Name conversation…"), nullptr, nullptr},
    {Menus::kLeave, kNone, N_("Leave channel"), N_("Leave conversation"), nullptr},
    {Menus::kStopSession, kNone, N_("Stop"), nullptr, nullptr},
    {Menus::kNewSection, kNone, N_("New section…"), nullptr, nullptr},
    {Menus::kRemoveFromSection, kNone, N_("Remove from %1"), nullptr, nullptr},
    {Menus::kWorkspaceAdmin, kNone, N_("Workspace admin"), nullptr, nullptr},
    {Menus::kChangeIcon, kNone, N_("Change icon…"), nullptr, nullptr},
    {Menus::kMuteWorkspace, kNone, N_("Mute"), nullptr, nullptr},
    {Menus::kUnmuteWorkspace, kNone, N_("Unmute"), nullptr, nullptr},
    {Menus::kSignOut, kNone, N_("Log out"), nullptr, nullptr},
    {Menus::kFindSession, kNone, N_("Find a session"), nullptr, nullptr},
    {Menus::kCreateSession, kNone, N_("Create a session"), nullptr, nullptr},
    {Menus::kCreateUnsafeSession, kNone, N_("Create an unsafe session"), nullptr, nullptr},
    {Menus::kEditTeammate, kNone, N_("Edit teammate…"), nullptr, nullptr},
    {Menus::kRestoreTeammate, kNone, N_("Restore default"), nullptr, nullptr},
    {Menus::kRemoveTeammate, kNone, N_("Remove teammate…"), nullptr, nullptr},
    {Menus::kCreateSection, kNone, N_("Create a section…"), nullptr, nullptr},
    {Menus::kRenameSection, kNone, N_("Rename section…"), nullptr, nullptr},
    {Menus::kDeleteSection, kNone, N_("Delete section…"), nullptr, nullptr},
    {Menus::kSectionUp, kNone, N_("Move section up"), nullptr, nullptr},
    {Menus::kSectionDown, kNone, N_("Move section down"), nullptr, nullptr},
    {Menus::kSortDefault, kNone, N_("Default"), nullptr, nullptr},
    {Menus::kSortAlpha, kNone, N_("Alphabetically"), nullptr, nullptr},
    {Menus::kSortRecent, kNone, N_("By most recent"), nullptr, nullptr},
    {Menus::kUnreadsFirst, kNone, N_("Unreads on top"), nullptr, nullptr},
};

ui::MenuItem &add(std::vector<ui::MenuItem> &out, int id, bool direct = false, bool on = true) {
    ui::MenuItem &m = ui::addMenuItem(out, kDefs, id, direct, on);
    m.danger = id == Menus::kLeave || id == Menus::kSignOut || id == Menus::kRemoveTeammate ||
               id == Menus::kDeleteSection;
    return m;
}

// The notification section: a header and the three levels, the effective
// one checked (Default ticks Settings' level). "Mute and hide" is the
// conversation's mute.
void notifySection(std::vector<ui::MenuItem> &out, NotifyLevel effective) {
    out.push_back(ui::MenuItem::headerItem(tr("Notify you about…")));
    const int level = effective == NotifyLevel::Nothing    ? Menus::kNotifyMute
                      : effective == NotifyLevel::Mentions ? Menus::kNotifyMentions
                                                           : Menus::kNotifyAll;
    for (int id : {int(Menus::kNotifyAll), int(Menus::kNotifyMentions), int(Menus::kNotifyMute)})
        add(out, id).checked = id == level;
}

// A section as a menu shows it: its emoji, then its name.
std::string sectionLabel(const model::Store &st, const model::Store::Section &s) {
    const std::string glyph = s.emoji.empty() ? std::string() : st.emojiFor(s.emoji).unicode;
    return glyph.empty() ? s.name : str::concat({glyph, "  ", s.name});
}

} // namespace

// Under the star: "Move to section" (each section, the current one ticked,
// then "New section…") and "Remove from <section>" while in one.
void Menus::sectionMoves(std::vector<ui::MenuItem> &items, const model::Conversation &cv) const {
    if (!_ctx.backend.capabilities().customSections)
        return;
    const model::Store          &st  = _ctx.store();
    const model::Store::Section *cur = st.sectionOf(cv.id);
    ui::MenuItem                 move;
    move.label = tr("Move to section");
    for (size_t i = 0; i < st.sections().size(); ++i) {
        ui::MenuItem m;
        m.id      = kMoveToSection + int(i);
        m.label   = sectionLabel(st, st.sections()[i]);
        m.checked = cur == &st.sections()[i];
        move.sub.push_back(std::move(m));
    }
    ui::addMenuSeparator(move.sub);
    add(move.sub, kNewSection);
    items.push_back(std::move(move));
    if (cur)
        add(items, kRemoveFromSection).label = i18n::arg(tr("Remove from %1"), cur->name);
}

Menus::Menus(Shell &shell, screens::Context &ctx, ui::Window &win)
    : _shell(shell), _ctx(ctx), _win(win) {}

std::vector<ui::MenuItem> Menus::chatItems(ConvRef c) const {
    std::vector<ui::MenuItem> items;
    if (c >= _ctx.store().conversationCount())
        return items;
    const model::Conversation &cv     = _ctx.store().conversation(c);
    const bool                 direct = cv.isDirect();
    if (cv.kind == ConvKind::Group) // showMpdmContextMenu
        add(items, kRename).label =
            cv.localName.empty() ? tr("Name conversation…") : tr("Rename conversation…");
    ui::addMenuSeparator(items);
    add(items, cv.starred ? kUnstar : kStar, direct);
    sectionMoves(items, cv);
    ui::addMenuSeparator(items);
    if (cv.kind == ConvKind::Dm) { // showDmContextMenu
        add(items, cv.muted ? kUnmute : kMute);
        if (_ctx.backend.isAgentSession(c)) {
            // Claude Code: stop a running turn; the session stays in Claude
            // Code when it leaves the list.
            if (_ctx.backend.canStopSession(c))
                add(items, kStopSession);
            add(items, kRename).label = tr("Rename session…");
            ui::addMenuSeparator(items);
            add(items, kLeave).label = tr("Remove from msga");
        }
        return items;
    }
    notifySection(items, _shell.sidebar().level(cv));
    ui::addMenuSeparator(items);
    add(items, kLeave, direct);
    return items;
}

std::vector<ui::MenuItem> Menus::workspaceItems() const {
    return workspaceItems(Workspace{});
}

std::vector<ui::MenuItem> Menus::workspaceItems(const Workspace &w) const {
    const model::Store       &st = _ctx.store;
    std::vector<ui::MenuItem> items;
    // When the workspace's live session knows you are an admin (any
    // running workspace, open or in the background) and has its URL.
    if (const model::Store *ws = w.store    ? w.store
                                 : w.active ? &st
                                            : nullptr;
        ws && ws->me != model::kNoUser && (ws->user(ws->me).admin || ws->user(ws->me).owner) &&
        !ws->workspaceUrl.empty())
        add(items, kWorkspaceAdmin, false, bool(_ctx.openUrl));
    add(items, kChangeIcon, false, bool(hooks.changeWorkspaceIcon));
    add(items, (w.active ? st.workspaceMuted : w.muted) ? kUnmuteWorkspace : kMuteWorkspace);
    const std::string &name = w.active ? st.workspaceName : w.name;
    add(items, kSignOut, false, bool(hooks.signOut)).label =
        name.empty() ? std::string(tr("Log out")) : i18n::arg(tr("Log out from %1"), name);
    return items;
}

std::vector<ui::MenuItem> Menus::sessionItems() const {
    std::vector<ui::MenuItem> items;
    for (int id : {int(kFindSession), int(kCreateSession), int(kCreateUnsafeSession)})
        add(items, id);
    return items;
}

std::vector<ui::MenuItem> Menus::teammateItems(const model::Backend::AgentRole &mate) const {
    std::vector<ui::MenuItem> items;
    add(items, kEditTeammate);
    if (mate.builtIn && mate.edited)
        add(items, kRestoreTeammate);
    if (!mate.builtIn) {
        ui::addMenuSeparator(items);
        add(items, kRemoveTeammate);
    }
    return items;
}

void Menus::runTeammate(int id, const std::string &role) {
    switch (id) {
    case kEditTeammate:
        _shell.editTeammate(role);
        break;
    case kRestoreTeammate:
        _shell.restoreTeammate(role);
        break;
    case kRemoveTeammate:
        _shell.removeTeammate(role);
        break;
    }
}

std::vector<ui::MenuItem> Menus::sectionItems(const std::string &section, SectionSort sort) const {
    std::vector<ui::MenuItem> items;
    if (_ctx.backend.capabilities().customSections) {
        add(items, kCreateSection);
        const auto &all = _ctx.store().sections();
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].id == section) {
                add(items, kRenameSection);
                ui::addMenuSeparator(items);
                add(items, kSectionUp, false, i > 0);
                add(items, kSectionDown, false, i + 1 < all.size());
                ui::addMenuSeparator(items);
                add(items, kDeleteSection);
            }
        ui::addMenuSeparator(items);
    }
    // The sort: one of the orders ticked, and unreads on top on or off.
    items.push_back(ui::MenuItem::headerItem(tr("Sort")));
    add(items, kSortDefault).checked = sort.by == SectionSort::By::Default;
    add(items, kSortAlpha).checked   = sort.by == SectionSort::By::Alpha;
    add(items, kSortRecent).checked  = sort.by == SectionSort::By::Recent;
    ui::addMenuSeparator(items);
    add(items, kUnreadsFirst).checked = sort.unreadsFirst;
    return items;
}

SectionSort Menus::sortAfter(int id, SectionSort sort) {
    switch (id) {
    case kSortDefault:
        sort.by = SectionSort::By::Default;
        break;
    case kSortAlpha:
        sort.by = SectionSort::By::Alpha;
        break;
    case kSortRecent:
        sort.by = SectionSort::By::Recent;
        break;
    case kUnreadsFirst:
        sort.unreadsFirst = !sort.unreadsFirst;
        break;
    }
    return sort;
}

void Menus::runSection(int id, const std::string &section) {
    switch (id) {
    case kCreateSection:
        showSectionDialog(_ctx, _win, {}, kNoConv);
        break;
    case kRenameSection:
        showSectionDialog(_ctx, _win, section, kNoConv);
        break;
    case kDeleteSection:
        showDeleteSectionDialog(_ctx, _win, section);
        break;
    case kSectionUp:
    case kSectionDown:
        _ctx.backend.moveSection(section, id == kSectionUp ? -1 : 1);
        break;
    }
}

void Menus::showSection(
    const std::string               &section,
    SectionSort                      sort,
    ui::PointF                       at,
    std::function<void(SectionSort)> onSort
) {
    ui::Menu::popupAt(
        _win,
        at,
        sectionItems(section, sort),
        [this, section, sort, onSort = std::move(onSort)](int id) {
            if (id >= kSortDefault && id <= kUnreadsFirst) {
                if (onSort)
                    onSort(sortAfter(id, sort));
            } else if (id)
                runSection(id, section);
        }
    );
}

void Menus::run(int id, uint32_t target) {
    if (id >= kMoveToSection) { // ── "Move to section" → one of them ──
        const auto &all = _ctx.store().sections();
        if (target < _ctx.store().conversationCount() && size_t(id - kMoveToSection) < all.size())
            _ctx.backend.moveToSection(target, all[size_t(id - kMoveToSection)].id);
        return;
    }
    if (id >= kFindSession) { // ── an agent workspace's "+" ──
        if (id == kFindSession)
            _shell.openSessionFinder();
        else if (id == kCreateSession || id == kCreateUnsafeSession)
            _shell.startAgentSession(id == kCreateUnsafeSession);
        return;
    }
    model::Store   &st = _ctx.store;
    model::Backend &be = _ctx.backend;
    if (id < kWorkspaceAdmin) { // ── conversations ──
        if (target >= st.conversationCount())
            return;
        switch (id) {
        case kStar:
        case kUnstar:
            be.setStarred(target, id == kStar);
            break;
        case kNotifyAll:
        case kNotifyMentions:
            if (st.conversation(target).muted)
                be.setMuted(target, false);
            be.setNotifyLevel(target, id == kNotifyAll ? NotifyLevel::All : NotifyLevel::Mentions);
            break;
        case kNotifyMute:
        case kMute:
        case kUnmute:
            be.setMuted(target, id != kUnmute);
            break;
        case kRename:
            _shell.renameConversation(target);
            break;
        case kLeave: {
            // Leaving the open conversation: go to its neighbour first.
            if (_shell.current() == target) {
                const std::vector<ConvRef> order = _shell.sidebar().order();
                auto                       it    = std::find(order.begin(), order.end(), target);
                ConvRef                    next  = kNoConv;
                if (it != order.end() && it + 1 != order.end())
                    next = *(it + 1);
                else if (it != order.end() && it != order.begin())
                    next = *(it - 1);
                if (next != kNoConv)
                    _shell.open(next);
            }
            be.leave(target);
            break;
        }
        case kStopSession:
            be.stopSession(target);
            break;
        case kNewSection:
            showSectionDialog(_ctx, _win, {}, target);
            break;
        case kRemoveFromSection:
            be.moveToSection(target, {});
            break;
        }
    } else {
        // The menu's workspace (reset: the next menu may be the active one's).
        const Workspace ws = std::exchange(_ws, Workspace{});
        switch (id) { // ── the workspace ──
        case kWorkspaceAdmin:
            if (_ctx.openUrl)
                _ctx.openUrl((ws.store ? *ws.store : st).workspaceLink() + "admin/settings");
            break;
        case kChangeIcon:
            if (hooks.changeWorkspaceIcon)
                hooks.changeWorkspaceIcon(ws.key);
            break;
        case kMuteWorkspace:
        case kUnmuteWorkspace:
            if (ws.active && !hooks.muteWorkspace) // without accounts (the demo)
                st.workspaceMuted = id == kMuteWorkspace;
            if (hooks.muteWorkspace) // kept with the workspace's record
                hooks.muteWorkspace(ws.key, id == kMuteWorkspace);
            break;
        case kSignOut:
            if (hooks.signOut)
                hooks.signOut(ws.key);
            break;
        }
    }
}

ui::Menu *Menus::show(std::vector<ui::MenuItem> items, uint32_t target, ui::PointF at) {
    if (items.empty())
        return nullptr;
    return ui::Menu::popupAt(_win, at, std::move(items), [this, target](int id) {
        if (id)
            run(id, target);
    });
}

void Menus::showChat(ConvRef c, ui::PointF at) {
    show(chatItems(c), c, at);
}

void Menus::showWorkspace(ui::PointF at) {
    showWorkspace(Workspace{}, at);
}

void Menus::showWorkspace(const Workspace &w, ui::PointF at) {
    _ws = w;
    if (ui::Menu *m = show(workspaceItems(w), 0, at))
        m->setMinWidth(140); // at least 140 px, wider for longer labels
}

void Menus::showSessions(ui::PointF at) {
    if (!_ctx.backend.capabilities().agentSessions)
        return;
    if (ui::Menu *m = show(sessionItems(), 0, at))
        m->setMinWidth(140); // at least 140 px, wider for longer labels
}

void Menus::showTeammate(const std::string &role, ui::PointF at) {
    for (const auto &mate : _ctx.backend.agentRoles())
        if (mate.id == role) {
            ui::Menu::popupAt(_win, at, teammateItems(mate), [this, role](int id) {
                if (id)
                    runTeammate(id, role);
            });
            return;
        }
}

} // namespace shell

// The shell's context menus — sidebar conversations and section headers, an
// agent workspace's session "+" and teammates, and the workspace tile on the
// rail — and what their items do.
//
// Items, order and wording are fixed per menu (channel, group DM, DM,
// teammate, workspace and agent session). Every
// state change goes through the Backend (or the
// Store for app-local state), copies through plat's clipboard. The menus are
// data first (…Items) and actions second (run), so tests check both without
// a pointer.
#pragma once

#include "screens/common/context.h"
#include "ui/ui.h"

#include <functional>
#include <string>
#include <vector>

namespace shell {

class Shell;

class Menus {
public:
    // A workspace on the rail. Its menu acts on it by key; `active` is the
    // one the Store holds (its name and mute state are read from there).
    // `store`: its Store while it runs (active or not), else null.
    struct Workspace {
        std::string         key, name;
        bool                muted  = false;
        bool                active = true;
        const model::Store *store  = nullptr;
    };
    // What the accounts controller does (unset: the items show disabled).
    // `key` is the workspace's; "" = the active one.
    struct Hooks {
        std::function<void(const std::string &key)>             changeWorkspaceIcon;
        std::function<void(const std::string &key)>             signOut;
        std::function<void(const std::string &key, bool muted)> muteWorkspace; // saved
    };

    enum Id : int {
        // Conversations (target: ConvRef)
        kStar = 20,
        kUnstar,
        kNotifyAll,
        kNotifyMentions,
        kNotifyMute, // "Mute and hide"
        kMute,       // DMs
        kUnmute,
        kRename, // group DMs, agent sessions
        kLeave,
        kStopSession,
        kNewSection,        // "Move to section" → "New section…"
        kRemoveFromSection, // "Remove from <section>"
        // The workspace (no target)
        kWorkspaceAdmin = 40,
        kChangeIcon,
        kMuteWorkspace,
        kUnmuteWorkspace,
        kSignOut,
        // An agent workspace's Sessions "+" / "Add sessions" (no target)
        kFindSession = 60,
        kCreateSession,
        kCreateUnsafeSession,
        // A teammate's row in the Team section (target: its role id)
        kEditTeammate = 70,
        kRestoreTeammate,
        kRemoveTeammate,
        // A section header in the sidebar (target: the section's id, "" for
        // one of the built-in sections)
        kCreateSection = 80,
        kRenameSection,
        kDeleteSection,
        kSectionUp,
        kSectionDown,
        // "Move to section" → the n-th section (target: ConvRef)
        kMoveToSection = 200,
    };

    Menus(Shell &shell, screens::Context &ctx, ui::Window &win);
    Hooks hooks;

    std::vector<ui::MenuItem> chatItems(model::ConvRef c) const;
    // The active workspace's menu, or `w`'s (a rail tile's).
    std::vector<ui::MenuItem> workspaceItems() const;
    std::vector<ui::MenuItem> workspaceItems(const Workspace &w) const;
    // An agent session's "+" menu, and a teammate's menu.
    std::vector<ui::MenuItem> sessionItems() const;
    std::vector<ui::MenuItem> teammateItems(const model::Backend::AgentRole &mate) const;
    // A sidebar section header's menu: `section` is a custom section's id,
    // "" a built-in section (Channels, Direct messages…).
    std::vector<ui::MenuItem> sectionItems(const std::string &section) const;
    void                      runSection(int id, const std::string &section);
    // `target` is the ConvRef the item's menu was built for (0: workspace).
    void                      run(int id, uint32_t target);
    void                      runTeammate(int id, const std::string &role);

    void showChat(model::ConvRef c, ui::PointF at);
    void showWorkspace(ui::PointF at);
    void showWorkspace(const Workspace &w, ui::PointF at);
    void showSessions(ui::PointF at);
    void showTeammate(const std::string &role, ui::PointF at);
    void showSection(const std::string &section, ui::PointF at);

private:
    ui::Menu *show(std::vector<ui::MenuItem> items, uint32_t target, ui::PointF at);
    void      sectionMoves(std::vector<ui::MenuItem> &items, const model::Conversation &cv) const;

    Shell            &_shell;
    screens::Context &_ctx;
    ui::Window       &_win;
    Workspace         _ws; // the workspace menu's (key "" = the active one)
};

} // namespace shell

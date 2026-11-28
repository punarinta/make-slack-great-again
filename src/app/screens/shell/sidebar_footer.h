// The sidebar's footer: my avatar (40 px rounded
// square with the presence dot; a click or right click opens "Manage
// profile" / "Manage status"), and on the right the presence toggle —
// visible (circle-user-round) or hidden (hat-glasses), flipped at once and
// settled on what the service reports, its tooltip saying why I look away.
// A service with zen mode (Claude Code, Capabilities::zenMode) and nothing
// to be away from shows the zen toggle there instead: the leaf while tool
// calls are hidden, the eye while everything shows. While background jobs
// run (model::jobs(): downloads, copies, …) a turning cog sits left of the
// toggle; hovering it lists them ("%n background task(s) running").
// Below them, behind a separator, "Show workspaces" (panel-left), ticked
// while the workspace rail shows; unticking it gives a small screen the
// rail's room for the conversation.
#pragma once

#include "app/model/jobs.h"
#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

#include <functional>

namespace shell {

class Sidebar;
class PresenceToggle;
class GhostButton;
class TaskSpinner;

class SidebarFooter : public ui::View {
public:
    SidebarFooter(Sidebar &sb, screens::Context &ctx, Avatars &avatars);
    ~SidebarFooter() override;

    void refresh(); // my avatar, presence and the toggle from the Store/backend

    // The avatar menu's items; unset ones show disabled.
    std::function<void()>        onManageProfile, onManageStatus;
    // The zen toggle was clicked; `on` is the new state (already shown).
    std::function<void(bool on)> onZenToggled;
    void                         setZenOn(bool on);
    bool                         zenOn() const { return _zenOn; }
    void                         toggleZen();
    ui::Clickable               *zenToggle() const;

    // The rail item was picked; `shown` is the new state.
    std::function<void(bool shown)> onRailToggled;
    void                            setRailShown(bool shown) { _railShown = shown; }
    bool                            railShown() const { return _railShown; }
    void                            toggleRail();

    // The presence toggle's change failed: the message for the error banner.
    std::function<void(const std::string &message)> onError;

    void           showAvatarMenu();
    void           togglePresence();
    bool           showsHidden() const; // the toggle's icon: hat-glasses
    ui::View      *avatarButton() const { return _avatarBtn; }
    ui::Clickable *toggle() const; // the presence toggle

    // The background-task cog (hidden while no job runs) and its hover list
    // (null unless shown).
    ui::View  *tasksIndicator() const;
    bool       tasksTurning() const; // the cog is animating
    ui::Popup *tasksPopup() const { return _tasksPopup; }
    void       showTasksPopup();
    void       hideTasksPopup();

private:
    void jobsChanged();

    screens::Context       &_ctx;
    Avatars                &_avatars;
    ui::Clickable          *_avatarBtn    = nullptr;
    Avatar                 *_avatar       = nullptr;
    PresenceToggle         *_toggle       = nullptr;
    GhostButton            *_zen          = nullptr;
    TaskSpinner            *_tasks        = nullptr;
    ui::Popup              *_tasksPopup   = nullptr;
    model::Jobs::ObserverId _jobsObserver = 0;
    bool                    _zenOn        = false;
    bool                    _railShown    = true;
    plat::TimerId           _confirm      = 0; // settle back if the service never answers
};

} // namespace shell

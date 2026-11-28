// A teammate's page in an agent workspace (Backend::agentRoles),
// opened from the sidebar's Team section: who it is ("Edit
// teammate…"), its sessions (a click opens one) and the folder a new session
// with it starts in ("Change folder": the recent folders, "Browse…").
// Writing to a teammate starts that session: the shell keeps its composer
// under this page and hands the text to Backend::startAgentSession with the
// teammate's role and folder(). It sits in the content area in the message
// list's place.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "screens/shell/session_dialogs.h"
#include "screens/shell/settings.h"
#include "ui/ui.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shell {

class TeammatePage : public ui::View {
public:
    // saveSettings persists `settings` after a folder pick.
    TeammatePage(
        screens::Context     &ctx,
        Avatars              &avatars,
        Settings             &settings,
        std::function<void()> saveSettings
    );
    ~TeammatePage() override;

    // Shows `mate` (the list follows the sessions live while the page is up).
    void                             open(const model::Backend::AgentRole &mate);
    void                             clear(); // forget the teammate (workspace switch)
    const model::Backend::AgentRole &teammate() const { return _mate; }

    // Where a new session starts: the folder last used with this teammate,
    // else the last one used for any session, else home.
    const std::string &folder() const { return _folder; }
    // Why no session can start in folder() right now ("" = one can): the
    // composer under the page says it, locked, like a read-only chat's.
    const std::string &blocker() const { return _blocker; }

    std::function<void(model::ConvRef)>      onOpenSession;
    std::function<void(const std::string &)> onEdit;
    std::function<void()>                    onFolderChanged; // folder() or blocker()

    // The folder menu (recent folders, "Browse…") and a pick in it (tests).
    void                      showFolderMenu();
    void                      pickFolder(const std::string &dir);
    std::vector<ui::MenuItem> folderMenuItems(std::vector<std::string> *paths) const;
    BrowseList               &list() { return *_list; }
    ui::Label                &folderLabel() { return *_folderLabel; }

    void paint(gfx::Painter &p) override;
    void visibilityChanged(bool on) override;

private:
    // fresh: from the top (a teammate opened); else the list keeps its scroll.
    void                                     rebuild(bool fresh = false);
    void                                     rebuildSoon();
    void                                     setFolder(const std::string &dir);
    void                                     chooseFolder();
    // Whether a path is a folder sessions can start in (another machine's: any absolute one).
    std::function<bool(const std::string &)> folderCheck() const;

    screens::Context         &_ctx;
    Avatars                  &_avatars;
    Settings                 &_settings;
    std::function<void()>     _saveSettings;
    model::Backend::AgentRole _mate;
    std::string               _folder, _blocker;
    Avatar                   *_avatar      = nullptr;
    ui::Label                *_name        = nullptr;
    ui::Label                *_description = nullptr;
    ui::Label                *_empty       = nullptr;
    ui::Label                *_folderLabel = nullptr;
    ui::Button               *_folderBtn   = nullptr;
    BrowseList               *_list        = nullptr;
    model::Store::ObserverId  _observer    = 0;
    plat::TimerId             _timer       = 0;
    std::shared_ptr<int>      _alive       = std::make_shared<int>(0);
};

} // namespace shell

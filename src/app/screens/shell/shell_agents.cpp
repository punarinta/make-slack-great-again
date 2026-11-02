// The shell's agent-workspace flows (Claude Code):
// the Sessions "+" menu's actions (openSessionFinder, startAgentSession), the
// teammate page with its composer (openTeammate, applyTeammateComposer,
// startSessionWithTeammate) and the team's dialogs (editTeammate,
// removeTeammate). Shell members kept apart from shell.cpp.
#include "app/mrkdwn/markdown.h"
#include <algorithm>
#include "base/i18n.h"
#include "base/log.h"
#include "base/str.h"
#include "screens/common/file_dialogs.h"
#include "screens/shell/canvas_page.h"
#include "screens/shell/context_menus.h"
#include "screens/shell/header.h"
#include "screens/shell/huddle_banner.h"
#include "screens/shell/message_search.h"
#include "screens/shell/recent_folders.h"
#include "screens/shell/session_dialogs.h"
#include "screens/shell/shell.h"
#include "screens/shell/teammate_page.h"
#include "screens/shell/typing_indicator.h"

using namespace ui;
using i18n::tr;
using model::ConvRef;
using model::kNoConv;
using AgentRole = model::Backend::AgentRole;

namespace shell {

namespace {

const AgentRole *findRole(const std::vector<AgentRole> &team, const std::string &id) {
    for (const AgentRole &r : team)
        if (r.id == id)
            return &r;
    return nullptr;
}

} // namespace

void Shell::buildTeammatePage(View *stack) {
    _teammatePage =
        stack->add<TeammatePage>(_ctx, _avatars, _settings, [this] { saveSettingsSoon(); });
    _teammatePage->setVisible(false);
    _teammatePage->onOpenSession   = [this](ConvRef c) { open(c); };
    _teammatePage->onEdit          = [this](const std::string &id) { editTeammate(id); };
    _teammatePage->onFolderChanged = [this] {
        // ↑'s history for the folder: asked now, so a first look reads it
        // ahead on a worker rather than at the first ↑.
        _ctx.backend.folderPromptHistory(_teammatePage->folder());
        applyTeammateComposer();
        if (teammateOpen()) // a folder picked: ready to write
            _composer->edit().focus();
    };
    _sidebar->onSessionMenu = [this](PointF at) { _menus->showSessions(at); };
    _sidebar->onTeammate    = [this](const std::string &role) { openTeammate(role); };
    _sidebar->onAddTeammate = [this] { editTeammate({}); };
}

bool Shell::teammateOpen() const {
    return _teammatePage && _teammatePage->visible();
}

// ── The Sessions "+" ────────────────────────────────────────────────────────

void Shell::openSessionFinder() {
    if (!_ctx.backend.capabilities().agentSessions)
        return;
    std::weak_ptr<int> alive = _agentAlive;
    showSessionFinder(
        _ctx,
        _win,
        _avatars,
        [this, alive](const std::string &id) {
            if (alive.expired())
                return;
            const ConvRef conv = _ctx.backend.addFoundSession(id);
            if (conv == kNoConv) {
                showError(tr("That session is gone: Claude Code no longer has it."));
                return;
            }
            // Listed by the backend's announcement; select once that has landed.
            _ctx.app.platform().post([this, alive, conv] {
                if (!alive.expired())
                    open(conv);
            });
        },
        [this, alive] {
            _ctx.app.platform().post([this, alive] {
                if (!alive.expired())
                    startAgentSession(false);
            });
        }
    );
}

// Pick the directory the new session works in, and open its conversation
// (listed when done runs). The session itself starts with the first message.
void Shell::startAgentSession(bool skipPermissionChecks) {
    if (!_ctx.backend.capabilities().agentSessions)
        return;
    plat::FileDialogDesc d;
    d.mode       = plat::FileDialogDesc::Mode::PickFolder;
    d.title      = skipPermissionChecks ? tr("Start session in a directory without permission "
                                             "checks…")
                                        : tr("Start session in a directory…");
    d.initialDir = _settings.claudeLastDir.empty()
                       ? _ctx.app.platform().standardDir(plat::StandardDir::Home)
                       : _settings.claudeLastDir;
    std::weak_ptr<int> alive = _agentAlive;
    screens::fileDialog(
        _ctx, std::move(d), [this, alive, skipPermissionChecks](std::vector<std::string> paths) {
            if (alive.expired() || paths.empty() || paths.front().empty())
                return;
            const std::string dir   = paths.front();
            _settings.claudeLastDir = dir;
            saveSettingsSoon();
            // The generalist; a specialist is written to on its page.
            _ctx.backend.startAgentSession(
                dir,
                skipPermissionChecks,
                {},
                [this, alive, dir](ConvRef conv, const std::string &err) {
                    if (alive.expired())
                        return;
                    if (conv == kNoConv) {
                        showError(err);
                        return;
                    }
                    recent_folders::bump(_settings, dir);
                    saveSettingsSoon();
                    open(conv); // focuses the composer
                }
            );
        }
    );
}

// ── The teammate page ───────────────────────────────────────────────────────

void Shell::openTeammate(const std::string &role) {
    if (!_teammatePage || !_signedIn)
        return;
    const auto       team = _ctx.backend.agentRoles();
    const AgentRole *mate = findRole(team, role);
    if (!mate)
        return;
    // Already on this teammate's page: back to it, the input as it is.
    const bool same = teammateOpen() && _teammatePage->teammate().id == role;
    if (!same)
        leaveTeammate(); // another teammate's: its draft is kept
    leavePages();
    // Unlike the overview pages the composer stays: writing to a teammate
    // starts a session with it. The conversation's chrome goes.
    leaveConversationChrome(true);
    _sidebar->selectTeammate(role);
    _teammatePage->setVisible(true);
    _teammatePage->open(*mate);
    _composer->setVisible(true);
    _composer->onSendRequest = [this] { return startSessionWithTeammate(); };
    applyTeammateComposer();
    if (!same)
        for (const TeammateDraft &d : _teammateDrafts)
            if (d.role == role) {
                if (!d.html.empty())
                    _composer->edit().insertHtml(d.html);
                _composer->addAttachments(d.files);
            }
    _composer->edit().focus();
}

// Leaving the page (a conversation opens, the workspace goes): what was typed
// to the teammate stays for the next visit, as a chat's draft does.
void Shell::leaveTeammate() {
    if (!teammateOpen())
        return;
    const std::string role = _teammatePage->teammate().id;
    _teammateDrafts.erase(
        std::remove_if(
            _teammateDrafts.begin(),
            _teammateDrafts.end(),
            [&](const TeammateDraft &d) { return d.role == role; }
        ),
        _teammateDrafts.end()
    );
    if (!_composer->edit().empty() || !_composer->attachments().empty())
        _teammateDrafts.push_back(
            {role,
             _composer->edit().empty() ? std::string() : _composer->edit().html(),
             _composer->attachments()}
        );
    _composer->edit().clear();
    _composer->clearAttachments();
    _composer->onSendRequest = nullptr;
    _composer->setPlaceholder({});
    _composer->setEnabled(true);
    _teammatePage->setVisible(false);
    _sidebar->selectTeammate({});
}

void Shell::applyTeammateComposer() {
    if (!teammateOpen())
        return;
    const std::string &blocker = _teammatePage->blocker();
    _composer->setEnabled(blocker.empty());
    _composer->setPlaceholder(
        blocker.empty() ? i18n::arg(tr("Message %1"), _teammatePage->teammate().name) : blocker
    );
}

// The composer's send on the page: a session with the teammate in its folder,
// the text (and files) its first message.
bool Shell::startSessionWithTeammate() {
    if (!teammateOpen())
        return false;
    const std::string        text(str::trim(mrkdwn::convertOutgoing(_composer->mrkdwn())));
    std::vector<std::string> files = _composer->attachments();
    if (text.empty() && files.empty())
        return false;
    const std::string role  = _teammatePage->teammate().id;
    const std::string dir   = _teammatePage->folder();
    _settings.claudeLastDir = dir;
    saveSettingsSoon();
    _composer->edit().clear();
    _composer->clearAttachments();
    std::weak_ptr<int> alive = _agentAlive;
    _ctx.backend.startAgentSession(
        dir, false, role, [this, alive, dir, text, files](ConvRef conv, const std::string &err) {
            if (alive.expired())
                return;
            if (conv == kNoConv) {
                showError(err);
                applyTeammateComposer();
                return;
            }
            recent_folders::bump(_settings, dir);
            saveSettingsSoon();
            // Listed by now (the backend announces a session before this):
            // open it, then the text is its first message.
            open(conv);
            if (files.empty())
                _ctx.backend.send(conv, text, 0, nullptr);
            else
                _ctx.backend.sendWithFiles(conv, text, 0, files, nullptr);
        }
    );
    return true;
}

// A message forwarded to a teammate: its page, the
// text after what was already typed there and the files added, left for the
// user to pick a folder and send.
void Shell::prefillTeammate(
    const std::string &role, std::string text, std::vector<std::string> paths
) {
    openTeammate(role);
    if (!teammateOpen() || _teammatePage->teammate().id != role)
        return; // off the team meanwhile
    if (!text.empty()) {
        if (!_composer->edit().empty())
            text = str::concat({_composer->mrkdwn(), "\n", text});
        loadMrkdwn(_composer->edit(), _ctx.store(), text);
    }
    _composer->attachFiles(paths); // skips the ones attached already
}

void Shell::refreshTeammates() {
    _sidebar->rebuild();
    if (!teammateOpen())
        return;
    const auto team = _ctx.backend.capabilities().agentSessions ? _ctx.backend.agentRoles()
                                                                : std::vector<AgentRole>{};
    // The page shows the teammate as it is now — or, once it's off the team,
    // the Generalist.
    if (const AgentRole *r = findRole(team, _teammatePage->teammate().id)) {
        _teammatePage->open(*r);
        applyTeammateComposer();
    } else if (!team.empty()) {
        openTeammate(team.front().id);
    } else {
        leaveTeammate();
        _welcome->setVisible(true);
    }
}

// ── The team ────────────────────────────────────────────────────────────────

void Shell::editTeammate(const std::string &id) {
    AgentRole role;
    if (id.empty()) {
        role.glyph = "pen-tool"; // a starting point, changed in the dialog
        role.color = 0x0e8c9a;
    } else {
        const auto       team = _ctx.backend.agentRoles();
        const AgentRole *r    = findRole(team, id);
        if (!r)
            return;
        role = *r;
    }
    std::weak_ptr<int> alive = _agentAlive;
    showTeammateDialog(_win, role, [this, alive, id](const AgentRole &edited, bool restore) {
        if (alive.expired())
            return;
        if (restore) {
            _ctx.backend.restoreAgentRole(id);
            refreshTeammates();
            return;
        }
        std::string       error;
        const std::string saved = _ctx.backend.saveAgentRole(edited, &error);
        if (saved.empty()) {
            showError(error);
            return;
        }
        refreshTeammates();
        if (id.empty())
            openTeammate(saved); // a new teammate: straight to its page, ready to write to
    });
}

void Shell::restoreTeammate(const std::string &id) {
    _ctx.backend.restoreAgentRole(id);
    refreshTeammates();
}

void Shell::removeTeammate(const std::string &id) {
    const auto       team = _ctx.backend.agentRoles();
    const AgentRole *r    = findRole(team, id);
    if (!r || r->name.empty())
        return;
    std::weak_ptr<int> alive = _agentAlive;
    showRemoveTeammateDialog(_win, r->name, [this, alive, id] {
        if (alive.expired())
            return;
        _ctx.backend.removeAgentRole(id);
        refreshTeammates();
    });
}

} // namespace shell

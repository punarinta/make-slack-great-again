// Signing in and out (adding a workspace, connecting Slack or Claude Code,
// importing sessions, signing out, switching): owns the workspace store and
// every signed-in workspace's backend and Store. They all run —
// the open one behind the screens' BackendSlot and StoreSlot, the others
// in the background, still polling, counting unreads and notifying (the
// shell's attachWorkspace). Switching only swaps what the screens show.
#pragma once

#include "app/auth/workspaces.h"
#include "app/model/null_backend.h"
#include "app/slack/credentials.h"
#include "app/slack/session.h"
#include "net/net.h"
#include "screens/common/context.h"
#include "ui/ui.h"

#include <memory>
#include <string>
#include <vector>

namespace claude {
class Links;
}
namespace net {
class Client;
}
namespace slack {
class OAuthFlow;
class SlackBackend;
class SocketMode;
class TokenDeriver;
} // namespace slack
namespace shell {

class Shell;
struct Settings;

class Accounts {
public:
    Accounts(
        screens::Context     &ctx,
        Shell                &shell,
        ui::Window           &win,
        Settings             &settings,
        std::function<void()> saveSettings,
        model::Backend       &none, // the screens' backend while signed out
        net::Client          &client,
        std::string           storePath
    );
    ~Accounts();

    // Opens the active workspace, or the signed-out page.
    void start();
    // The rail's "+" / "Log in to workspace": the service menu at `anchor`.
    void promptAdd(ui::PointF anchor);
    // Slack's sign-in (the session-import dialog first).
    void connectSlack();
    // The Claude Code workspace: nothing to sign in to — the CLI must be
    // there and logged in.
    void connectClaudeCode();
    // OAuth with app keys ("Use app keys (OAuth) instead").
    void loginWithAppKeys();
    // A msga:// URL from the OS (the OAuth callback). True if consumed.
    bool handleUrl(std::string_view url);
    // plat's NetworkChanged: the shared Socket Mode socket reconnects when
    // the network comes back.
    void networkChanged(bool online);
    // "Log out from <workspace>": the active one, or `key`'s ("" = active).
    void signOut();
    void signOut(const std::string &key);
    // The rail: a tile clicked, a drag-reorder
    // dropped (saved), a workspace's Mute/Unmute (saved with its record).
    void switchTo(const std::string &key);
    void reorder(const std::vector<std::string> &keys);
    void setMuted(const std::string &key, bool muted);
    // Settings → "Import Slack session…" (the dialog without the OAuth link
    // wired) and "Convert them to session".
    void importSession();
    void convertToSession();
    int  oauthSlackWorkspaces() const;
    // Settings → Claude Code → "Runs on": where the Claude Code workspace's
    // sessions run (false: there's no such workspace) — its SSH destination,
    // "" for this computer. setClaudeHost saves it with the workspace and
    // starts the workspace over there.
    bool claudeHost(std::string *destination, std::string *msga) const;
    void setClaudeHost(const std::string &destination, const std::string &msga);

    auth::WorkspaceStore &store() { return _store; }
    // Agent thread links: every running workspace is handed to them (the
    // shell's robot, menus and chips read and drive them).
    claude::Links        &links() { return *_links; }

private:
    struct Running; // a signed-in workspace's backend and Store, open or not
    void     addSessionWorkspaces(std::vector<slack::Credentials> creds);
    // The session-import dialog, with its "Use app keys (OAuth)" link or not.
    void     sessionDialog(bool appKeysLink);
    void     refreshRail(); // the store's workspaces to the shell
    void     activate(const std::string &key);
    // The open workspace's last chat opens again, once its
    // Store lists it; false when it doesn't (yet).
    bool     restoreLast(Running &r);
    void     showSignedOut();
    void     imageAuth(); // RemoteImages' per-URL workspace credentials
    Running *find(const std::string &key) const;
    Running *bySerial(uint64_t serial) const;
    // Starts `key` if it isn't running (its cache, then connect); null when
    // its record can't be opened.
    Running *ensure(const std::string &key);
    // Stops one (the open one leaves the screens first) and forgets it.
    void     drop(Running *r, bool keepCache);
    // Detaches a running workspace and stops its work: timers, requests,
    // its cache (written first with keepCache), its backend.
    void     shutdown(Running &r, bool keepCache);
    // A newly signed-in workspace: saved, made active, (re)started, opened.
    void     adoptRecord(auth::WorkspaceRecord rec);
    void     restart(const std::string &key); // new credentials: a fresh backend
    void     startNext();                     // the background ones, one at a time
    void     connect(uint64_t serial);
    void     authLost(uint64_t serial, const std::string &error);
    void     fetchIcon(Running &r);
    std::shared_ptr<slack::SocketMode> socketMode(); // null: session mode / no xapp
    void                               migrateNext();
    void migrateDone(std::vector<slack::Credentials> converted, const std::string &error);

    // "Convert them to session": the OAuth workspaces, resolved to their
    // workspace URLs one by one, then their tokens derived from the cookie.
    struct Migration {
        std::string                     cookie, lastError;
        std::vector<slack::Credentials> items;
        size_t                          index = 0;
        std::vector<slack::TeamSession> resolved;
        net::RequestId                  req = 0;
    };
    std::unique_ptr<Migration> _migration;

    screens::Context                     &_ctx;
    Shell                                &_shell;
    ui::Window                           &_win;
    Settings                             &_settings;
    std::function<void()>                 _saveSettings;
    net::Client                          &_client;
    auth::WorkspaceStore                  _store;
    model::Backend                       &_none;  // main's: outlives the shell
    model::Store                         &_blank; // the screens' Store while signed out (main's)
    std::vector<std::unique_ptr<Running>> _running;
    Running                              *_active = nullptr; // the one the screens show
    std::string                           _activeKey;
    std::vector<std::string>              _pending; // not started yet (startNext)
    uint64_t                              _serial = 0, _staggerTimer = 0;
    std::unique_ptr<slack::OAuthFlow>     _oauth;
    std::unique_ptr<slack::TokenDeriver>  _remint;
    std::weak_ptr<slack::SocketMode>      _socketMode; // held by the Slack backends
    std::unique_ptr<claude::Links>        _links;
};

} // namespace shell

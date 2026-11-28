// Earlier versions' settings and workspace list (legacy.h).
//
// Runs once: when <configDir>/settings.json (resp. workspaces.json) does not
// exist yet, it is written from the old INI/plist/registry settings store
// (base/old_settings.h). The old store is only read — a rollback to an earlier
// version finds it as it was. Credentials are not copied anywhere: they are
// read and written in the entries earlier versions used (base/secret.h).
//
// Old keys with nothing to map to here are left behind:
// notifications/defaultMigrated (applied, not kept), the Teams and IMAP
// workspaces and credentials/teamsClientId (services dropped),
// imap/domainIcons. zenMode/<workspace key> becomes Settings::zenWorkspaces.
#pragma once

#include "app/auth/workspaces.h"
#include "base/old_settings.h"

#include <string>
#include <vector>

namespace plat {
class App;
}

namespace shell {
struct Settings;
}

namespace legacy {

// `old` is the "msga"/"msga" settings store, `appStore` the "msga"/"MSGA"
// one (composer/lastAttachDir only); `dataDir` the data directory, where
// the custom tray icon was copied; `workspaces` the imported ones with their
// auth (an OAuth one keeps the app-keys connection default it had).
void importOldSettings(
    const oldsettings::Map                   &old,
    const oldsettings::Map                   &appStore,
    const std::string                        &dataDir,
    const std::vector<auth::WorkspaceRecord> &workspaces,
    shell::Settings                          *s
);

// The stored Slack and Claude Code workspaces in their order, and the one
// that was open. A record's auth is empty when it is in the secret store under
// the old key (WorkspaceStore reads it there); the pre-2026 layouts kept
// plain tokens in the settings, which come back as a blob to store.
std::vector<auth::WorkspaceRecord> oldWorkspaces(const oldsettings::Map &old, std::string *active);

// The custom workspace pictures installed earlier (workspace/<key>/
// customIcon), copied under the current names into `iconDir`.
void importOldWorkspaceIcons(const oldsettings::Map &old, const std::string &iconDir);

// The settings and workspaces part of legacy::importOldData (see the top of
// this file). Cheap when both files exist: two stat calls.
void importOldSettingsAndWorkspaces(
    plat::App &app, const std::string &settingsPath, const std::string &workspacesPath
);

} // namespace legacy

// The upgrade from earlier versions' data — a self-contained module, meant to
// be deleted once most users have upgraded. The rest of the app does not depend
// on it: main.cpp makes the one call below, in an #ifdef MSGA_LEGACY_IMPORT.
//
// What it brings along, once:
//   old_settings_import.h  the old settings store's settings and workspace list
//                          (settings.json, workspaces.json, workspace icons,
//                          the zen toggles)
//   old_cache_import.h     each workspace's cache (mutes, notification levels,
//                          group-DM names, muted threads, AI transcripts, the
//                          roster for a warm first start, …)
// The old files are only read: a rollback to an earlier version finds them as
// they were. Credentials are not migrated at all — they stay in the entries
// earlier versions used (base/secret.h, base/old_settings.h; both stay).
//
// To remove the module:
//   1. delete src/app/legacy/
//   2. delete the MSGA_LEGACY_IMPORT block in src/app/CMakeLists.txt
//   3. delete the two `#ifdef MSGA_LEGACY_IMPORT` blocks in
//      src/app/screens/shell/main.cpp (the include and the call)
// Leftovers are harmless: <configDir>/legacy-imported (the done list) and
// Settings::zenWorkspaces' import source.
#pragma once

#include <string>

namespace plat {
class App;
}

namespace legacy {

// The first start's import, then each workspace's old cache the first time
// it is seen. `settingsPath` / `workspacesPath` are the current files
// (shell::Settings::defaultPath, auth::WorkspaceStore::defaultPath). Cheap
// once done: a few stat calls and one small read. Synchronous, before the
// UI exists (the workspaces' caches must be in place before they open).
void importOldData(
    plat::App &app, const std::string &settingsPath, const std::string &workspacesPath
);

} // namespace legacy

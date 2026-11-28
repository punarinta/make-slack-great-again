#include "app/legacy/legacy.h"

#include "app/identity.h"
#include "app/legacy/old_cache_import.h"
#include "app/legacy/old_settings_import.h"
#include "base/file.h"

namespace legacy {

void importOldData(
    plat::App &app, const std::string &settingsPath, const std::string &workspacesPath
) {
    importOldSettingsAndWorkspaces(app, settingsPath, workspacesPath);
    const std::string config = identity::configDir(app);
    if (!config.empty())
        importOldCaches(app, workspacesPath, file::join(config, "legacy-imported"));
}

} // namespace legacy

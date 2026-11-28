// "Import from local Slack": the session of the Slack desktop app installed
// on this computer — the `d` cookie decrypted from its Chromium cookie
// database, and the signed-in workspaces from its local storage. It is
// offered on Linux only (Chromium "v10"/"v11" cookie encryption,
// the v11 key from the Secret Service); elsewhere the button is hidden.
#pragma once

#include "app/slack/session.h"

#include <functional>
#include <string>
#include <vector>

namespace plat {
class App;
}

namespace slack {

struct LocalImport {
    std::string              cookie;
    std::vector<TeamSession> teams;
    // On failure: not_installed, locked, decrypt_failed, no_cookie,
    // unsupported_platform.
    std::string              error;
    bool                     ok() const { return !cookie.empty() && error.empty(); }
};

bool        localImportSupported();
LocalImport importLocalSession();
// importLocalSession on a worker (up to three D-Bus calls of 1.5 s, SQLite,
// a LevelDB scan, PBKDF2): `done` runs on the UI thread (session.cpp).
void        importLocalSessionAsync(plat::App &app, std::function<void(LocalImport)> done);

} // namespace slack

// Linux pieces that are neither display-server nor D-Bus specific: the
// single-instance channel (a Unix socket per user and key), per-user URL
// scheme registration (.desktop handler + mimeapps.list), and the XDG
// standard directories. Shared by the Wayland and X11 apps via ServicesApp.
#pragma once

#include "core/backends.h"

namespace plat::linux_instance {

// Keeps the primary instance's listening socket (and its fd watch) alive.
class Server {
public:
    virtual ~Server() = default;
};

// True: we are primary; *server listens and emits InstanceActivated/OpenUrls
// on `app`. False: another instance owns `key`; `args` (plus our cwd and
// XDG_ACTIVATION_TOKEN/DESKTOP_STARTUP_ID) were handed to it.
bool claim(
    BackendApp                     &app,
    std::string_view                key,
    const std::vector<std::string> &args,
    std::unique_ptr<Server>        *server
);

// Per-user registration of `scheme:` URLs for this executable. Remembered in
// the process so forwarded args with that scheme also produce OpenUrls.
bool registerUrlScheme(BackendApp &app, std::string_view scheme);

std::string standardDir(StandardDir d);

} // namespace plat::linux_instance

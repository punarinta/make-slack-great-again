// Backend factories. Each returns null and sets *error when the backend is
// compiled in but cannot start (no compositor, no X server, …).
#pragma once

#include "plat/plat.h"

namespace plat {

#if defined(PLAT_PORT)
// The one backend of a port build (src/plat/src/port): the C port contract.
std::unique_ptr<App> createPortApp(std::string *error);
#elif defined(PLAT_TEST_HOOKS)
std::unique_ptr<App> createHeadlessApp(std::string *error);
#endif
#if defined(PLAT_HAS_WAYLAND)
std::unique_ptr<App> createWaylandApp(std::string *error);
#endif
#if defined(PLAT_HAS_X11)
std::unique_ptr<App> createX11App(std::string *error);
#endif
#if defined(_WIN32)
std::unique_ptr<App> createWin32App(std::string *error);
#endif
#if defined(__APPLE__)
std::unique_ptr<App> createCocoaApp(std::string *error);
#endif

// Lets a backend's Window emit through its App (App::emit is protected).
class BackendApp : public App {
public:
    using App::emit;
};

} // namespace plat

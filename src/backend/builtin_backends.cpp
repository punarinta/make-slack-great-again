// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// The one list of backends compiled into this build. Each optional backend is a
// CMake switch (MSGA_BACKEND_*) that adds its sources AND defines the macro
// below, so the call and the code come and go together. Explicit on purpose:
// a self-registering static object would be silently dropped by a static link.
#include "backend/backend_registry.h"

#include "backend/slack/slack_module.h"
#if defined(MSGA_BACKEND_TEAMS)
#include "backend/teams/teams_module.h"
#endif
#if defined(MSGA_BACKEND_IMAP)
#include "backend/imap/imap_module.h"
#endif
#if defined(MSGA_BACKEND_CLAUDE_CODE)
#include "backend/claude_code/claude_code_module.h"
#endif
#if defined(MSGA_DEMO)
#include "backend/demo/demo_module.h"
#endif

void registerBuiltinBackends() {
    // Registration only fills a descriptor — no backend object, socket, timer or
    // file watcher exists until a workspace for that service is loaded.
    slack::registerBackend(); // always built
#if defined(MSGA_BACKEND_TEAMS)
    teams::registerBackend();
#endif
#if defined(MSGA_BACKEND_IMAP)
    imap::registerBackend();
#endif
#if defined(MSGA_BACKEND_CLAUDE_CODE)
    claude_code::registerBackend();
#endif
#if defined(MSGA_DEMO)
    demo::registerBackend();
#endif
}

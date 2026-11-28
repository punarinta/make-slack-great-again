// NullBackend — no workspace signed in: the Store stays empty and every call
// is a no-op. The shell runs on it while it shows the "Log in to workspace"
// page (the logged-out state).
#pragma once

#include "app/model/backend.h"

namespace model {

class NullBackend : public Backend {
public:
    using Backend::Backend;

    void connect(Done) override {}
    void loadHistory(ConvRef, Ts, Done) override {}
    void loadThread(ConvRef, Ts, Done) override {}
    void send(ConvRef, std::string, Ts, Done) override {}
    void markRead(ConvRef, Ts) override {}
};

} // namespace model

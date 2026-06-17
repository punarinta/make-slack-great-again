// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

namespace slack {

class SocketModeRealtime;

// RAII handle to the single app-level Socket Mode socket. Slack delivers every
// installation's events over one socket (and round-robins across sockets of the
// same app), so the socket must be shared by all Slack backends. It is created
// lazily on the FIRST live handle and destroyed when the LAST one is released —
// so "≥1 Slack workspace ⇒ exactly one socket; zero ⇒ no socket" holds by
// refcounting, with no special-case teardown anywhere else.
//
// socket() is null when no xapp token is configured (then the backend simply
// runs without realtime, exactly as before). A Telegram-only user, owning no
// Slack backend, never constructs a handle and so never opens a Slack socket.
class SharedRealtime {
public:
    SharedRealtime();
    ~SharedRealtime();
    SharedRealtime(const SharedRealtime &)            = delete;
    SharedRealtime &operator=(const SharedRealtime &) = delete;

    [[nodiscard]] SocketModeRealtime *socket() const { return _socket; }

private:
    SocketModeRealtime *_socket = nullptr; // the process-global, refcounted socket
};

} // namespace slack

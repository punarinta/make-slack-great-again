// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QString>
#include <deque>
#include <functional>
#include <optional>

// A place the user can navigate back/forward to: a conversation in a
// (possibly background) workspace.
struct NavLocation {
    QString        teamId;
    ConversationId conv;

    bool valid() const { return !teamId.isEmpty() && !conv.value.isEmpty(); }
    bool operator==(const NavLocation &) const = default;
};

// Back/forward chat-navigation history with text-editor undo/redo semantics:
// going back keeps the forward stack intact, but opening a conversation by a
// direct user action discards everything in front of the current location.
class NavHistory {
public:
    static constexpr int kMaxDepth = 32;

    // Entries can go stale (workspace logged out, channel left); goBack /
    // goForward silently drop entries the validator rejects.
    using Validator = std::function<bool(const NavLocation &)>;

    // A conversation was opened by a direct user action (list click, search
    // result, dialog, notification): push the previous location onto the back
    // stack and clear the forward stack.
    void recordOpen(const NavLocation &loc) {
        if (_current == loc)
            return;
        if (_current.valid())
            push(_back, _current);
        _forward.clear();
        _current = loc;
    }

    // Sync the current location without touching the stacks — used while a
    // back/forward jump is being applied to the UI.
    void               setCurrent(const NavLocation &loc) { _current = loc; }
    const NavLocation &current() const { return _current; }

    std::optional<NavLocation> goBack(const Validator &valid) { return go(_back, _forward, valid); }
    std::optional<NavLocation> goForward(const Validator &valid) {
        return go(_forward, _back, valid);
    }

    bool canGoBack() const { return !_back.empty(); }
    bool canGoForward() const { return !_forward.empty(); }

    // Drop every trace of a logged-out workspace.
    void purgeTeam(const QString &teamId) {
        const auto stale = [&](const NavLocation &l) { return l.teamId == teamId; };
        std::erase_if(_back, stale);
        std::erase_if(_forward, stale);
        if (_current.teamId == teamId)
            _current = {};
    }

private:
    static void push(std::deque<NavLocation> &d, const NavLocation &loc) {
        if (!d.empty() && d.back() == loc)
            return;
        d.push_back(loc);
        while (int(d.size()) > kMaxDepth)
            d.pop_front();
    }

    std::optional<NavLocation>
    go(std::deque<NavLocation> &from, std::deque<NavLocation> &to, const Validator &valid) {
        while (!from.empty()) {
            const NavLocation loc = from.back();
            from.pop_back();
            if (!valid(loc) || loc == _current)
                continue; // stale entry — drop it and keep looking
            if (_current.valid())
                push(to, _current);
            _current = loc;
            return loc;
        }
        return std::nullopt;
    }

    NavLocation             _current;
    std::deque<NavLocation> _back, _forward;
};

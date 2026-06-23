// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "auth_strategy_factory.h"

#include "auth/auth_strategy.h"
#include "backend/slack/oauth_flow.h"
#include "backend/teams/oauth_flow.h"
#include "backend/teams/teams_auth.h"

namespace auth {

std::unique_ptr<AuthStrategy> makeAuthStrategy(Service service, QObject *parent) {
    switch (service) {
    case Service::Slack:
        // The only place the Slack auth flow appears above the seam: the case
        // that builds it. slack::OAuthFlow reads its own compiled-in app-config.
        return std::make_unique<slack::OAuthFlow>(slack::appConfig(), parent);
    case Service::Teams:
        // Microsoft Teams: Auth Code + PKCE (public client) over Microsoft identity.
        return std::make_unique<teams::OAuthFlow>(teams::appConfig(), parent);
    }
    return nullptr;
}

std::vector<Service> registeredAuthServices() {
    // Order = the order the picker offers them.
    return {Service::Slack, Service::Teams};
}

} // namespace auth

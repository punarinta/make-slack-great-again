// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "auth_strategy_factory.h"

#include "auth/auth_strategy.h"
#include "backend/backend_registry.h"

namespace auth {

std::unique_ptr<AuthStrategy> makeAuthStrategy(const Service &service, QObject *parent) {
    const auto *d = backends::find(service);
    if (!d || !d->makeAuthStrategy)
        return nullptr;
    return d->makeAuthStrategy(parent);
}

std::vector<Service> registeredAuthServices() {
    return backends::pickerServices();
}

} // namespace auth

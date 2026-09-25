// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend_registry.h"

#include <algorithm>

namespace backends {
namespace {

// Function-local so nothing is constructed before first use. A handful of
// entries at most — a linear scan beats a hash here.
std::vector<BackendDescriptor> &registry() {
    static std::vector<BackendDescriptor> r;
    return r;
}

} // namespace

void registerBackend(BackendDescriptor d) {
    auto &r  = registry();
    auto  it = std::find_if(r.begin(), r.end(), [&](const BackendDescriptor &e) {
        return e.service == d.service;
    });
    if (it != r.end())
        *it = std::move(d);
    else
        r.push_back(std::move(d));
}

const BackendDescriptor *find(const Service &service) {
    for (const auto &d : registry())
        if (d.service == service)
            return &d;
    return nullptr;
}

bool isRegistered(const Service &service) {
    return find(service) != nullptr;
}

QString displayName(const Service &service) {
    const auto *d = find(service);
    return d ? d->displayName : service.token;
}

std::vector<Service> pickerServices() {
    std::vector<const BackendDescriptor *> offered;
    for (const auto &d : registry())
        if (d.pickerOrder >= 0 && d.makeAuthStrategy)
            offered.push_back(&d);
    std::stable_sort(offered.begin(), offered.end(), [](const auto *a, const auto *b) {
        return a->pickerOrder < b->pickerOrder;
    });
    std::vector<Service> out;
    out.reserve(offered.size());
    for (const auto *d : offered)
        out.push_back(d->service);
    return out;
}

void clearForTests() {
    registry().clear();
}

} // namespace backends

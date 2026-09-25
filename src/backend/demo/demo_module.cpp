// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "demo_module.h"

#include "backend/backend_registry.h"
#include "backend/demo/demo_backend.h"
#include "backend/demo/demo_fixture.h"
#include "backend/demo/demo_mode.h"

#include <QDebug>

namespace demo {

void registerBackend() {
    backends::registerBackend({
        .service     = kService,
        .displayName = QStringLiteral("Demo"),
        .pickerOrder = -1, // seeded by `--demo`; nothing to sign in to, never offered
        // The auth blob is the fixture directory (seedWorkspace wrote it).
        .makeBackend = [](const TokenStore::WorkspaceRecord &rec) -> std::unique_ptr<Backend> {
            QString error;
            auto    fx = loadFixture(QString::fromUtf8(rec.auth), &error);
            if (!fx) {
                qWarning() << "demo fixture:" << error;
                return nullptr;
            }
            return std::make_unique<DemoBackend>(std::move(*fx));
        },
        .makeAuthStrategy = {},
    });
}

} // namespace demo

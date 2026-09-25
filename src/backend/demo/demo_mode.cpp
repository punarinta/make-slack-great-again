// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "demo_mode.h"

#include "auth/token_store.h"
#include "backend/demo/demo_fixture.h"
#include "backend/demo/demo_services.h"
#include "cache/workspace_cache.h"
#include "llm/llm_provider.h"
#include "llm/llm_service.h"
#include "network/gif_search.h"
#include "ui/context_menu/context_menu.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace demo {

QString fixtureDirFromArgs(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--demo") && i + 1 < argc)
            return QString::fromLocal8Bit(argv[i + 1]);
        if (arg.startsWith(QLatin1String("--demo=")))
            return arg.mid(7);
    }
    return {};
}

bool isolateState(QString *error) {
#if !defined(Q_OS_LINUX)
    if (error)
        *error = QStringLiteral("demo mode redirects HOME/XDG_* and is Linux-only for now");
    return false;
#else
    const QString state = QDir::temp().filePath(QStringLiteral("msga-demo-state"));
    QDir          dir(state);
    if (dir.exists() && !dir.removeRecursively()) {
        if (error)
            *error = QStringLiteral("cannot wipe %1").arg(state);
        return false;
    }
    for (const char *sub : {"config", "data", "cache"})
        if (!dir.mkpath(QLatin1String(sub))) {
            if (error)
                *error = QStringLiteral("cannot create %1/%2").arg(state, QLatin1String(sub));
            return false;
        }
    // Read by QSettings / QStandardPaths on first use, and by SingleInstance's
    // socket name (QDir::home().dirName()) — all of which happen after this.
    qputenv("HOME", state.toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", dir.filePath(QStringLiteral("config")).toLocal8Bit());
    qputenv("XDG_DATA_HOME", dir.filePath(QStringLiteral("data")).toLocal8Bit());
    qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toLocal8Bit());
    return true;
#endif
}

bool seedWorkspace(const QString &fixtureDir, QString *error) {
    const auto fx = loadFixture(fixtureDir, error);
    if (!fx)
        return false;

    TokenStore::WorkspaceRecord rec;
    rec.key         = WorkspaceKey{kService, fx->workspaceId};
    rec.displayName = fx->workspaceName;
    rec.iconUrl     = fx->workspaceIcon;
    rec.auth        = fx->dir.toUtf8(); // makeBackend reloads the fixture from here
    TokenStore::saveWorkspace(rec);
    TokenStore::setActiveWorkspace(rec.key);

    if (!fx->startConversation.value.isEmpty()) {
        QString name;
        for (const auto &c : fx->conversations)
            if (c.id == fx->startConversation)
                name = c.name;
        WorkspaceCache(rec.key.toString()).saveLastConv(fx->startConversation, name);
    }

    QSettings s(QStringLiteral("msga"), QStringLiteral("msga"));
    s.setValue(QStringLiteral("updates/autoCheck"), false); // no network, no update bar

    // Stand-in GIPHY + AI endpoint on localhost, alive for the process.
    static FakeServices *services = nullptr;
    if (!services) {
        services = new FakeServices(*fx, QCoreApplication::instance());
        if (!services->start(error))
            return false;
    }
    net::GifSearch::setDemoEndpoint(services->baseUrl());
    ContextMenu::setFlatPopups(true); // Xvfb has no compositor: translucent halos go black

    auto &llm = LlmService::instance();
    if (!llm.isAvailable()) {
        LlmProviderConfig cfg = LlmProviderConfig::newCustom();
        cfg.name              = QStringLiteral("Lumen AI");
        cfg.baseUrl           = services->baseUrl() + QStringLiteral("/v1");
        cfg.model             = QStringLiteral("lumen-1");
        if (auto *p = llm.addCustom(cfg, QStringLiteral("demo")))
            llm.setDefaultProviderId(p->id());
    }
    return true;
}

} // namespace demo

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QString>
#include <QStringList>

namespace TokenStore {

struct Credentials {
    QString xoxp;
    QString teamId;
    QString teamName;
    QString iconUrl;    // 88px workspace icon URL, may be empty
};

// ── Multi-workspace ───────────────────────────────────────────────────────────
void        saveWorkspace(const Credentials &c);
Credentials loadWorkspace(const QString &teamId);
void        removeWorkspace(const QString &teamId);
QStringList workspaceIds();
bool        hasAnyWorkspace();
QString     activeWorkspaceId();
void        setActiveWorkspace(const QString &teamId);

// ── Legacy wrappers (operate on active workspace) ─────────────────────────────
bool        hasToken();            // = hasAnyWorkspace()
Credentials load();               // = loadWorkspace(activeWorkspaceId())
void        save(const Credentials &);  // = saveWorkspace + setActive
void        clear();              // = removeWorkspace(activeWorkspaceId())

// ── App registration (credentials compiled in via credentials.cmake) ──────────
struct AppConfig {
    QString clientId;
    QString clientSecret;
    QString xapp;
};
AppConfig loadApp();

} // namespace TokenStore

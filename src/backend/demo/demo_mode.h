// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// `msga --demo <fixture dir>`: run the app against the fake workspace described
// by demo/fixture.json — for recording the README demo and for UI work without
// a live account. Debug builds only (MSGA_DEMO); see demo/README.md.
#pragma once

#include "backend/domain.h"

#include <QString>

namespace demo {

// This backend's service. The token is stored in workspace handles — never change it.
inline const Service kService{QStringLiteral("demo")};

// The directory (or .json file) given to --demo / --demo=…; empty when absent.
QString fixtureDirFromArgs(int argc, char **argv);

// Before QApplication: point HOME and the XDG_* dirs at a throwaway state dir
// (wiped on every start) so the demo never reads or writes the user's real
// config, caches or single-instance socket, and every run starts identical.
// Linux only — returns false with *error set elsewhere.
bool isolateState(QString *error);

// After QApplication: load the fixture, register it as the only workspace, and
// make the app open on the fixture's start conversation.
bool seedWorkspace(const QString &fixtureDir, QString *error);

} // namespace demo

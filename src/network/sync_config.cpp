// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include "sync_config.h"

#include <QDebug>
#include <QSettings>

namespace net {

namespace {

SyncMode modeFromString(const QString &raw, bool *ok = nullptr) {
    const QString m = raw.trimmed().toLower();
    if (ok)
        *ok = true;
    if (m == QLatin1String("minimal"))
        return SyncMode::Minimal;
    if (m == QLatin1String("conservative") || m == QLatin1String("slow"))
        return SyncMode::Conservative;
    if (m == QLatin1String("normal") || m == QLatin1String("fast"))
        return SyncMode::Normal;
    if (ok)
        *ok = false;
    return SyncMode::Normal;
}

} // namespace

SyncConfig &SyncConfig::instance() {
    static SyncConfig inst;
    return inst;
}

SyncConfig::SyncConfig(QObject *parent) : QObject(parent) {
    if (qEnvironmentVariableIsSet("MSGA_SYNC_MODE")) {
        _mode = modeFromString(qEnvironmentVariable("MSGA_SYNC_MODE"));
        return;
    }
    if (qEnvironmentVariableIsSet("MSGA_SLOW_SYNC")) {
        _mode = qEnvironmentVariableIntValue("MSGA_SLOW_SYNC") ? SyncMode::Conservative
                                                               : SyncMode::Normal;
        return;
    }
    QSettings s("msga", "msga");
    bool      ok   = false;
    const auto mode = modeFromString(s.value("sync/mode").toString(), &ok);
    if (ok)
        _mode = mode;
    else if (s.value("sync/conservative", false).toBool()) // legacy key
        _mode = SyncMode::Conservative;
}

QString SyncConfig::modeName() const {
    switch (_mode) {
    case SyncMode::Minimal:
        return QStringLiteral("minimal");
    case SyncMode::Conservative:
        return QStringLiteral("conservative (slow-sync)");
    case SyncMode::Normal:
        break;
    }
    return QStringLiteral("normal");
}

void SyncConfig::setSyncMode(SyncMode mode) {
    if (_mode == mode)
        return;
    _mode = mode;
    qInfo().noquote() << "[SyncConfig] sync mode changed to" << modeName();
    emit syncModeChanged(_mode);
}

void SyncConfig::persist() const {
    QSettings s("msga", "msga");
    s.setValue("sync/mode", _mode == SyncMode::Minimal        ? "minimal"
                            : _mode == SyncMode::Conservative ? "conservative"
                                                              : "normal");
    s.remove("sync/conservative"); // superseded by sync/mode
}

void SyncConfig::parseArgs(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        bool          matched = true;
        if (arg == "--minimal-sync")
            setSyncMode(SyncMode::Minimal);
        else if (arg == "--slow-sync" || arg == "--conservative-sync" || arg == "-s")
            setSyncMode(SyncMode::Conservative);
        else if (arg == "--no-slow-sync" || arg == "--fast-sync")
            setSyncMode(SyncMode::Normal);
        else if (arg.startsWith("--sync-mode=")) {
            bool ok = false;
            const auto m = modeFromString(arg.section('=', 1), &ok);
            if (ok)
                setSyncMode(m);
            matched = ok;
        } else
            matched = false;
        if (matched)
            persist();
    }
}

void SyncConfig::setAppActive(bool active) {
    if (_appActive == active)
        return;
    _appActive = active;
    emit appActiveChanged(_appActive);
}

int SyncConfig::foregroundPollGapMs(bool isSessionAuth) const {
    if (!isSessionAuth)
        return 60'000;
    switch (_mode) {
    case SyncMode::Minimal:
        return _appActive ? 30'000 : 120'000;
    case SyncMode::Conservative:
        return _appActive ? 10'000 : 60'000;
    case SyncMode::Normal:
        break;
    }
    return _appActive ? 5'000 : 30'000;
}

int SyncConfig::countsPollGapMs() const {
    switch (_mode) {
    case SyncMode::Minimal:
        return 600'000; // blocked at the transport anyway
    case SyncMode::Conservative:
        return _appActive ? 25'000 : 60'000;
    case SyncMode::Normal:
        break;
    }
    return _appActive ? 10'000 : 30'000;
}

int SyncConfig::threadsPollGapMs() const {
    switch (_mode) {
    case SyncMode::Minimal:
        return 600'000; // blocked at the transport anyway
    case SyncMode::Conservative:
        return _appActive ? 45'000 : 120'000;
    case SyncMode::Normal:
        break;
    }
    return _appActive ? 20'000 : 60'000;
}

int SyncConfig::rosterReloadGapMs() const {
    switch (_mode) {
    case SyncMode::Minimal:
        return _appActive ? 900'000 : 1'800'000;
    case SyncMode::Conservative:
        return _appActive ? 180'000 : 600'000;
    case SyncMode::Normal:
        break;
    }
    return _appActive ? 60'000 : 300'000;
}

int SyncConfig::presencePollGapMs() const {
    switch (_mode) {
    case SyncMode::Minimal:
        return 3'600'000; // presence is not polled in minimal mode
    case SyncMode::Conservative:
        return _appActive ? 180'000 : 600'000;
    case SyncMode::Normal:
        break;
    }
    return _appActive ? 60'000 : 180'000;
}

int SyncConfig::backgroundPollGapMs() const {
    switch (_mode) {
    case SyncMode::Minimal:
        return 3'600'000; // background rotation is off in minimal mode
    case SyncMode::Conservative:
        return _appActive ? 300'000 : 900'000;
    case SyncMode::Normal:
        break;
    }
    return _appActive ? 120'000 : 300'000;
}

int SyncConfig::maxDiffPollsPerTick() const {
    switch (_mode) {
    case SyncMode::Minimal:
        return 1;
    case SyncMode::Conservative:
        return 3;
    case SyncMode::Normal:
        break;
    }
    return 8;
}

int SyncConfig::maxSweepTargets() const {
    switch (_mode) {
    case SyncMode::Minimal:
        return 0; // no sweeps at all
    case SyncMode::Conservative:
        return 12;
    case SyncMode::Normal:
        break;
    }
    return 25;
}

int SyncConfig::presenceHotCount() const {
    switch (_mode) {
    case SyncMode::Minimal:
        return 0;
    case SyncMode::Conservative:
        return 5;
    case SyncMode::Normal:
        break;
    }
    return 12;
}

int SyncConfig::missingDmUsersLimit() const {
    switch (_mode) {
    case SyncMode::Minimal:
        return 5;
    case SyncMode::Conservative:
        return 10;
    case SyncMode::Normal:
        break;
    }
    return 50;
}

} // namespace net

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#pragma once

#include <QObject>
#include <QString>

namespace net {

enum class SyncMode {
    Normal,
    Conservative, // slow-sync: paced requests, capped sweeps, gentle polling
    // Minimal: the smallest API footprint that still shows the open chat and
    // lets you reply. Everything else — roster, presence, unread counts, stars,
    // emoji, threads feed, background sweeps — is switched off, and a hard
    // allowlist at the transport guarantees nothing else goes on the wire. For
    // workspaces whose admins revoke sessions on API activity (Enterprise Grid).
    Minimal,
};

class SyncConfig : public QObject {
    Q_OBJECT
public:
    static SyncConfig &instance();

    void     setSyncMode(SyncMode mode);
    SyncMode syncMode() const { return _mode; }
    // True for Conservative AND Minimal — Minimal is Conservative plus more, so
    // every conservative cap applies to it as well.
    bool     isConservative() const { return _mode != SyncMode::Normal; }
    bool     isMinimal() const { return _mode == SyncMode::Minimal; }
    QString  modeName() const;

    // --slow-sync / --minimal-sync / --sync-mode=… (persisted to QSettings so the
    // choice survives the restart that follows adding an account).
    void parseArgs(int argc, char *argv[]);

    void setAppActive(bool active);
    bool isAppActive() const { return _appActive; }

    // Adaptive intervals (ms) and caps.
    int foregroundPollGapMs(bool isSessionAuth) const;
    int countsPollGapMs() const;
    int threadsPollGapMs() const;
    int rosterReloadGapMs() const;
    int presencePollGapMs() const;
    int backgroundPollGapMs() const;
    int maxDiffPollsPerTick() const;
    int maxSweepTargets() const;
    int presenceHotCount() const;
    int missingDmUsersLimit() const;

signals:
    void syncModeChanged(SyncMode mode);
    void appActiveChanged(bool active);

private:
    explicit SyncConfig(QObject *parent = nullptr);
    void persist() const;

    SyncMode _mode      = SyncMode::Normal;
    bool     _appActive = true;
};

} // namespace net

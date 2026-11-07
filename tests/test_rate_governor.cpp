// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include "network/http_queue.h"
#include "network/rate_governor.h"
#include "network/sync_config.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("msga-test");
    app.setOrganizationName("msga-test");

    // Redirect QSettings into a throwaway dir BEFORE any test runs. SyncConfig
    // persists the parsed --slow-sync choice, and without this the suite writes
    // sync/conservative into the developer's real ~/.config/msga/msga.conf and
    // silently puts their actual app into slow-sync mode.
    QTemporaryDir tempDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tempDir.path());

    return Catch::Session().run(argc, argv);
}

TEST_CASE("SyncConfig: mode and CLI argument parsing", "[sync_config]") {
    auto &config = net::SyncConfig::instance();

    // Default mode test
    config.setSyncMode(net::SyncMode::Normal);
    CHECK_FALSE(config.isConservative());
    CHECK(config.syncMode() == net::SyncMode::Normal);

    // Explicit switch to conservative
    config.setSyncMode(net::SyncMode::Conservative);
    CHECK(config.isConservative());
    CHECK(config.syncMode() == net::SyncMode::Conservative);

    // CLI args: --no-slow-sync
    char arg0[] = "msga";
    char arg1[] = "--no-slow-sync";
    char *argv_normal[] = {arg0, arg1, nullptr};
    config.parseArgs(2, argv_normal);
    CHECK_FALSE(config.isConservative());
    {
        QSettings s("msga", "msga");
        CHECK(s.value("sync/mode").toString().toStdString() == "normal");
    }

    // CLI args: --slow-sync
    char arg2[] = "--slow-sync";
    char *argv_slow[] = {arg0, arg2, nullptr};
    config.parseArgs(2, argv_slow);
    CHECK(config.isConservative());
    {
        QSettings s("msga", "msga");
        CHECK(s.value("sync/mode").toString().toStdString() == "conservative");
    }

    // CLI args: --conservative-sync
    config.setSyncMode(net::SyncMode::Normal);
    char arg3[] = "--conservative-sync";
    char *argv_conservative[] = {arg0, arg3, nullptr};
    config.parseArgs(2, argv_conservative);
    CHECK(config.isConservative());

    // CLI args: --sync-mode=slow
    config.setSyncMode(net::SyncMode::Normal);
    char arg4[] = "--sync-mode=slow";
    char *argv_sync_mode[] = {arg0, arg4, nullptr};
    config.parseArgs(2, argv_sync_mode);
    CHECK(config.isConservative());

    // Reset to Normal for other tests
    config.setSyncMode(net::SyncMode::Normal);
}

TEST_CASE("SyncConfig: adaptive cadences for active vs inactive app", "[sync_config]") {
    auto &config = net::SyncConfig::instance();

    // Normal mode: active
    config.setSyncMode(net::SyncMode::Normal);
    config.setAppActive(true);
    CHECK(config.foregroundPollGapMs(/*isSessionAuth=*/true) == 5'000);
    CHECK(config.countsPollGapMs() == 10'000);
    CHECK(config.threadsPollGapMs() == 20'000);
    CHECK(config.maxDiffPollsPerTick() == 8);
    CHECK(config.maxSweepTargets() == 25);

    // Normal mode: inactive
    config.setAppActive(false);
    CHECK(config.foregroundPollGapMs(/*isSessionAuth=*/true) == 30'000);
    CHECK(config.countsPollGapMs() == 30'000);
    CHECK(config.threadsPollGapMs() == 60'000);

    // Conservative mode: active
    config.setSyncMode(net::SyncMode::Conservative);
    config.setAppActive(true);
    CHECK(config.foregroundPollGapMs(/*isSessionAuth=*/true) == 10'000);
    CHECK(config.countsPollGapMs() == 25'000);
    CHECK(config.threadsPollGapMs() == 45'000);
    CHECK(config.maxDiffPollsPerTick() == 3);
    CHECK(config.maxSweepTargets() == 12);
    CHECK(config.presenceHotCount() == 5);
    CHECK(config.missingDmUsersLimit() == 10);

    // Conservative mode: inactive
    config.setAppActive(false);
    CHECK(config.foregroundPollGapMs(/*isSessionAuth=*/true) == 60'000);
    CHECK(config.countsPollGapMs() == 60'000);
    CHECK(config.threadsPollGapMs() == 120'000);

    // Clean up
    config.setSyncMode(net::SyncMode::Normal);
    config.setAppActive(true);
}

TEST_CASE("RateGovernor: concurrency and pacing", "[rate_governor]") {
    net::RateGovernor gov;
    gov.setMaxInflight(1);
    gov.setMinDispatchIntervalMs(500);
    gov.setMaxRequestsPerMinute(10);

    // Initially eligible
    CHECK(gov.delayUntilEligibleMs(net::HttpQueue::Priority::Normal) == 0);

    // When a request is dispatched, inflight = 1
    gov.recordDispatched(net::HttpQueue::Priority::Normal);

    // Concurrency limit reached -> delay > 0
    CHECK(gov.delayUntilEligibleMs(net::HttpQueue::Priority::Normal) > 0);

    // Request finishes
    gov.recordFinished(net::HttpQueue::Priority::Normal);

    // Now inflight = 0, but min dispatch interval (500ms) is still active
    CHECK(gov.delayUntilEligibleMs(net::HttpQueue::Priority::Normal) > 0);
}

TEST_CASE("RateGovernor: conservative mode presets", "[rate_governor]") {
    net::RateGovernor gov;
    gov.setConservativeMode(true);
    CHECK(gov.isConservativeMode());
    CHECK(gov.maxInflight() == 1);
    CHECK(gov.minDispatchIntervalMs() == 800);
    CHECK(gov.maxRequestsPerMinute() == 25);

    gov.setConservativeMode(false);
    CHECK_FALSE(gov.isConservativeMode());
    CHECK(gov.maxInflight() == 2);
    CHECK(gov.minDispatchIntervalMs() == 150);
    CHECK(gov.maxRequestsPerMinute() == 70);
}

TEST_CASE("RateGovernor: rolling window rate ceiling", "[rate_governor]") {
    net::RateGovernor gov;
    gov.setMaxInflight(100);
    gov.setMinDispatchIntervalMs(0);
    gov.setMaxRequestsPerMinute(3);

    gov.recordDispatched(net::HttpQueue::Priority::Normal);
    gov.recordFinished(net::HttpQueue::Priority::Normal);
    CHECK(gov.delayUntilEligibleMs(net::HttpQueue::Priority::Normal) == 0);

    gov.recordDispatched(net::HttpQueue::Priority::Normal);
    gov.recordFinished(net::HttpQueue::Priority::Normal);
    CHECK(gov.delayUntilEligibleMs(net::HttpQueue::Priority::Normal) == 0);

    gov.recordDispatched(net::HttpQueue::Priority::Normal);
    gov.recordFinished(net::HttpQueue::Priority::Normal);

    // 3 requests dispatched, ceiling is 3 -> must wait
    CHECK(gov.delayUntilEligibleMs(net::HttpQueue::Priority::Normal) > 0);
}


TEST_CASE("SyncConfig: minimal mode is a stricter superset of conservative", "[sync_config]") {
    auto &config = net::SyncConfig::instance();

    char arg0[]  = "msga";
    char arg1[]  = "--minimal-sync";
    char *argv[] = {arg0, arg1, nullptr};
    config.parseArgs(2, argv);
    CHECK(config.isMinimal());
    CHECK(config.isConservative()); // every conservative cap applies too
    {
        QSettings s("msga", "msga");
        CHECK(s.value("sync/mode").toString().toStdString() == "minimal");
    }

    config.setAppActive(true);
    CHECK(config.foregroundPollGapMs(/*isSessionAuth=*/true) == 30'000);
    CHECK(config.maxSweepTargets() == 0);
    CHECK(config.presenceHotCount() == 0);
    CHECK(config.maxDiffPollsPerTick() == 1);

    char arg2[]  = "--sync-mode=normal";
    char *argv2[] = {arg0, arg2, nullptr};
    config.parseArgs(2, argv2);
    CHECK_FALSE(config.isMinimal());
    CHECK_FALSE(config.isConservative());
}

TEST_CASE("RateGovernor: minimal preset", "[rate_governor]") {
    net::RateGovernor gov;
    gov.setMinimalMode();
    CHECK(gov.maxInflight() == 1);
    CHECK(gov.minDispatchIntervalMs() == 3000);
    CHECK(gov.maxRequestsPerMinute() == 15);
}

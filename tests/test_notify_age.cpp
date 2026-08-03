// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include <catch2/catch_test_macros.hpp>

#include <QDateTime>

#include "backend/domain.h"

// Covers the pure notification-freshness policy (tooOldToNotify). It gates every
// notification surface at once — OS notification, sound, tray/dock tint,
// workspace counters, chat counters — so the thing worth nailing down is the
// fail-open behaviour: only a *positively known* old time suppresses anything.

namespace {

qint64 microsDaysAgo(double days) {
    const qint64 secs = QDateTime::currentSecsSinceEpoch() - qint64(days * 86400);
    return secs * 1000000;
}

QString slackTsDaysAgo(double days) {
    return QString::number(microsDaysAgo(days) / 1000000) + QStringLiteral(".000100");
}

Conversation convWithLatest(const QString &latestTs) {
    Conversation c;
    c.id       = ConversationId{"C1"};
    c.kind     = ConvKind::PublicChannel;
    c.isMember = true;
    c.latestTs = latestTs;
    return c;
}

} // namespace

TEST_CASE("Event age is measured against kMaxNotifyAgeDays") {
    CHECK_FALSE(tooOldToNotify(microsDaysAgo(0)));
    CHECK_FALSE(tooOldToNotify(microsDaysAgo(kMaxNotifyAgeDays - 1)));
    CHECK(tooOldToNotify(microsDaysAgo(kMaxNotifyAgeDays + 1)));
    CHECK(tooOldToNotify(microsDaysAgo(365)));
}

TEST_CASE("An unknown event time notifies (fail-open)") {
    CHECK_FALSE(tooOldToNotify(qint64(0)));
    CHECK_FALSE(tooOldToNotify(qint64(-1)));
}

TEST_CASE("A future-dated event notifies") {
    CHECK_FALSE(tooOldToNotify(microsDaysAgo(-1)));
}

TEST_CASE("Conversation staleness follows its latest message") {
    CHECK_FALSE(tooOldToNotify(convWithLatest(slackTsDaysAgo(1))));
    CHECK_FALSE(tooOldToNotify(convWithLatest(slackTsDaysAgo(kMaxNotifyAgeDays - 1))));
    CHECK(tooOldToNotify(convWithLatest(slackTsDaysAgo(kMaxNotifyAgeDays + 1))));
}

TEST_CASE("A conversation with no usable clock notifies (fail-open)") {
    SECTION("no cursor at all") {
        CHECK_FALSE(tooOldToNotify(convWithLatest(QString())));
    }
    SECTION("IMAP Message-ID header parses to nothing") {
        CHECK_FALSE(tooOldToNotify(convWithLatest(QStringLiteral("<abc123@example.com>"))));
    }
    SECTION("Teams message id is not a decimal clock") {
        // Teams ids are epoch *milliseconds*; read as seconds they land far in
        // the future, which must never read as stale.
        CHECK_FALSE(tooOldToNotify(convWithLatest(QStringLiteral("1615971548136"))));
    }
}

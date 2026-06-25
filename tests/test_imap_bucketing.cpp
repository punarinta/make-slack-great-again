// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Unit tests for the Model-D bucketing engine (thread → conversation grouping).
#include <catch2/catch_test_macros.hpp>

#include "backend/imap/imap_bucketing.h"

using namespace imap;

namespace {
MimeAddress addr(const QString &name, const QString &email) {
    return {name, email.toLower()};
}

MsgRef
mk(quint32                   uid,
   const QString            &msgId,
   const QString            &inReplyTo,
   const MimeAddress        &from,
   const QList<MimeAddress> &to,
   const QString            &subject,
   qint64                    epochSecs,
   bool                      seen) {
    MsgRef m;
    m.uid           = uid;
    m.env.messageId = msgId;
    m.env.inReplyTo = inReplyTo;
    m.env.from      = {from};
    m.env.to        = to;
    m.env.subject   = subject;
    m.env.date      = QDateTime::fromSecsSinceEpoch(epochSecs);
    m.seen          = seen;
    return m;
}

const MimeAddress kMe = addr("Me", "me@x.com");
} // namespace

TEST_CASE("bucket: 1:1 thread becomes a DM", "[imap][bucket]") {
    Bucketer      b({"me@x.com"}, "me@x.com");
    QList<MsgRef> msgs = {
        mk(1, "<a@x>", "", addr("Alice", "alice@y.com"), {kMe}, "Lunch?", 1000, false),
        mk(2, "<b@x>", "<a@x>", kMe, {addr("Alice", "alice@y.com")}, "Re: Lunch?", 1100, true),
    };
    // server THREAD groups them as one thread
    const auto r = b.run(msgs, {{1, 2}});
    REQUIRE(r.conversations.size() == 1);
    const auto &c = r.conversations[0];
    CHECK(c.kind == ConvKind::Im);
    REQUIRE(c.dmUser.has_value());
    CHECK(c.dmUser->value == "alice@y.com");
    CHECK(c.name == "Alice");
    CHECK(c.id.value == "dm:alice@y.com");
    CHECK(c.unread == 1); // one unseen message
    REQUIRE(r.byId.contains("dm:alice@y.com"));
    CHECK(r.byId["dm:alice@y.com"].messages.size() == 2);
    // thread linkage: reply points at the root
    CHECK(r.byId["dm:alice@y.com"].threadRootOf["<b@x>"] == "<a@x>");
    CHECK(r.byId["dm:alice@y.com"].replyCountOf["<a@x>"] == 1);
}

TEST_CASE("bucket: several participants become an MPDM", "[imap][bucket]") {
    Bucketer      b({"me@x.com"}, "me@x.com");
    QList<MsgRef> msgs = {
        mk(1,
           "<a@x>",
           "",
           addr("Alice", "alice@y.com"),
           {kMe, addr("Bob", "bob@z.com")},
           "Plan",
           2000,
           false),
    };
    const auto r = b.run(msgs, {{1}});
    REQUIRE(r.conversations.size() == 1);
    const auto &c = r.conversations[0];
    CHECK(c.kind == ConvKind::Mpim);
    CHECK(c.members.size() == 2);                      // alice + bob, me excluded
    CHECK(c.id.value == "mpim:alice@y.com,bob@z.com"); // sorted emails
}

TEST_CASE("bucket: a thread that gains a participant moves to the union MPDM", "[imap][bucket]") {
    // me+Alice initially, then Bob is added on the reply — the whole thread is
    // one MPDM keyed by the union (matches the §3 rule).
    Bucketer      b({"me@x.com"}, "me@x.com");
    QList<MsgRef> msgs = {
        mk(1, "<a@x>", "", addr("Alice", "alice@y.com"), {kMe}, "Hi", 3000, true),
        mk(2,
           "<b@x>",
           "<a@x>",
           addr("Alice", "alice@y.com"),
           {kMe, addr("Bob", "bob@z.com")},
           "Re: Hi",
           3100,
           false),
    };
    const auto r = b.run(msgs, {{1, 2}});
    REQUIRE(r.conversations.size() == 1);
    CHECK(r.conversations[0].kind == ConvKind::Mpim);
    CHECK(r.conversations[0].members.size() == 2);
}

TEST_CASE("bucket: distinct people make distinct DMs, sorted by recency", "[imap][bucket]") {
    Bucketer      b({"me@x.com"}, "me@x.com");
    QList<MsgRef> msgs = {
        mk(1, "<a@x>", "", addr("Alice", "alice@y.com"), {kMe}, "old", 1000, true),
        mk(2, "<c@x>", "", addr("Carol", "carol@w.com"), {kMe}, "new", 5000, false),
    };
    const auto r = b.run(msgs, {{1}, {2}});
    REQUIRE(r.conversations.size() == 2);
    // most-recent-active first
    CHECK(r.conversations[0].id.value == "dm:carol@w.com");
    CHECK(r.conversations[1].id.value == "dm:alice@y.com");
}

TEST_CASE("bucket: List-Id routes to a list channel", "[imap][bucket]") {
    Bucketer b({"me@x.com"}, "me@x.com");
    MsgRef   m   = mk(1, "<a@x>", "", addr("Sender", "s@list.com"), {kMe}, "post", 1000, false);
    m.listId     = "dev.list.example.com";
    const auto r = b.run({m}, {{1}});
    REQUIRE(r.conversations.size() == 1);
    CHECK(r.conversations[0].kind == ConvKind::PublicChannel);
    CHECK(r.conversations[0].id.value == "list:dev.list.example.com");
}

TEST_CASE("bucket: fallback threading via In-Reply-To when no server THREAD", "[imap][bucket]") {
    Bucketer      b({"me@x.com"}, "me@x.com");
    QList<MsgRef> msgs = {
        mk(1, "<a@x>", "", addr("Alice", "alice@y.com"), {kMe}, "Hi", 1000, true),
        mk(2, "<b@x>", "<a@x>", kMe, {addr("Alice", "alice@y.com")}, "Re", 1100, false),
    };
    const auto r = b.run(msgs); // no server threads → union-find fallback
    REQUIRE(r.conversations.size() == 1);
    CHECK(r.byId["dm:alice@y.com"].messages.size() == 2);
    CHECK(r.byId["dm:alice@y.com"].replyCountOf["<a@x>"] == 1);
}

// ── replyRecipients: Reply-All To/Cc derivation ─────────────────────────────

TEST_CASE("replyRecipients: To = sender, Cc = other recipients minus me", "[imap][reply]") {
    Envelope e;
    e.from       = {addr("Alice", "alice@y.com")};
    e.to         = {kMe, addr("Bob", "bob@z.com")};
    e.cc         = {addr("Carol", "carol@w.com")};
    const auto r = Bucketing::replyRecipients(e, {"me@x.com"});
    CHECK(r.to == QStringList{"alice@y.com"});                // the sender
    CHECK(r.cc == (QStringList{"bob@z.com", "carol@w.com"})); // the rest, me removed
}

TEST_CASE("replyRecipients: Reply-To overrides From", "[imap][reply]") {
    Envelope e;
    e.from       = {addr("Alice", "alice@y.com")};
    e.replyTo    = {addr("Support", "support@y.com")};
    e.to         = {kMe};
    const auto r = Bucketing::replyRecipients(e, {"me@x.com"});
    CHECK(r.to == QStringList{"support@y.com"});
    CHECK(r.cc.isEmpty()); // only me was on To → nothing left to Cc
}

TEST_CASE("replyRecipients: never duplicates an address across To and Cc", "[imap][reply]") {
    Envelope e;
    e.from       = {addr("Alice", "alice@y.com")};
    e.to         = {addr("Alice", "alice@y.com"), kMe}; // sender also listed in To
    const auto r = Bucketing::replyRecipients(e, {"me@x.com"});
    CHECK(r.to == QStringList{"alice@y.com"});
    CHECK(r.cc.isEmpty()); // alice already in To, me removed
}

TEST_CASE(
    "replyRecipients: replying after my own message addresses the group in To", "[imap][reply]"
) {
    Envelope e; // I sent it: From = me, recipients = the group
    e.from       = {kMe};
    e.to         = {addr("Alice", "alice@y.com"), addr("Bob", "bob@z.com")};
    const auto r = Bucketing::replyRecipients(e, {"me@x.com"});
    CHECK(r.to == (QStringList{"alice@y.com", "bob@z.com"})); // participants (sorted)
    CHECK(r.cc.isEmpty());                                    // not duplicated into Cc
}

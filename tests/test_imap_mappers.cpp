// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Unit tests for IMAP ENVELOPE / FLAGS / THREAD mapping (S-expression parsing).
#include <catch2/catch_test_macros.hpp>

#include "backend/imap/imap_mappers.h"

using namespace imap;

TEST_CASE("parseFetch: real ENVELOPE with FLAGS + INTERNALDATE", "[imap][mappers]") {
    // Shape captured from a live Dovecot server (Phase 0 spike output).
    const QByteArray line  = "1118 FETCH (UID 7307 FLAGS (\\Answered \\Seen) "
                             "INTERNALDATE \"22-Jun-2026 15:16:16 -0400\" "
                             "ENVELOPE (\"Mon, 22 Jun 2026 13:15:29 -0600\" \"Access Token\" "
                             "((\"Kent\" NIL \"kent\" \"worklingua.com\")) "
                             "((\"Kent\" NIL \"kent\" \"worklingua.com\")) "
                             "((\"Kent\" NIL \"kent\" \"worklingua.com\")) "
                             "((\"Vladimir Osipov\" NIL \"vladimir\" \"lingolette.com\")) "
                             "NIL NIL NIL \"<CALxyDaMbjk4@mail.gmail.com>\"))";
    const auto       items = Mappers::parseFetch({line});
    REQUIRE(items.size() == 1);
    const auto &it = items[0];
    CHECK(it.uid == 7307u);
    CHECK(it.seen());
    CHECK(it.hasEnvelope);
    CHECK(it.envelope.subject == "Access Token");
    CHECK(it.envelope.messageId == "CALxyDaMbjk4@mail.gmail.com");
    REQUIRE(it.envelope.from.size() == 1);
    CHECK(it.envelope.from[0].name == "Kent");
    CHECK(it.envelope.from[0].email == "kent@worklingua.com");
    REQUIRE(it.envelope.to.size() == 1);
    CHECK(it.envelope.to[0].email == "vladimir@lingolette.com");
    CHECK(it.envelope.cc.isEmpty()); // NIL
    CHECK(it.envelope.date.isValid());
    CHECK(it.internalDate.isValid());
}

TEST_CASE("parseFetch: RFC 2047 encoded subject as a literal", "[imap][mappers]") {
    const QByteArray subj = "=?UTF-8?Q?Puzzle_=F0=9F=A7=A9?=";
    QByteArray       line =
        "* 5 FETCH (UID 9 ENVELOPE (NIL {" + QByteArray::number(subj.size()) + "}\r\n" + subj +
        " ((\"L\" NIL \"messages-noreply\" \"linkedin.com\")) NIL NIL NIL NIL NIL NIL \"<a@b>\"))";
    const auto items = Mappers::parseFetch({line});
    REQUIRE(items.size() == 1);
    CHECK(items[0].envelope.subject.startsWith("Puzzle"));
    REQUIRE(items[0].envelope.from.size() == 1);
    CHECK(items[0].envelope.from.first().email == "messages-noreply@linkedin.com");
}

TEST_CASE("parseFetch: BODY[] literal captured for MIME parsing", "[imap][mappers]") {
    const QByteArray body = "Subject: x\r\n\r\nhi";
    const QByteArray line =
        "* 1 FETCH (UID 3 BODY[] {" + QByteArray::number(body.size()) + "}\r\n" + body + ")";
    const auto items = Mappers::parseFetch({line});
    REQUIRE(items.size() == 1);
    CHECK(items[0].hasBody);
    CHECK(items[0].rawBody == body);
}

TEST_CASE("parseThread: flattens nested groups to root-first UID lists", "[imap][mappers]") {
    const auto g = Mappers::parseThread("THREAD (97)(125 127)((488 (480)(499)(500)))");
    REQUIRE(g.size() == 3);
    CHECK(g[0] == QList<quint32>{97});
    CHECK(g[1] == QList<quint32>{125, 127});
    // nested tree flattens depth-first, root (488) first
    CHECK(g[2] == QList<quint32>{488, 480, 499, 500});
}

TEST_CASE("parseFetch: ignores BODYSTRUCTURE it doesn't need", "[imap][mappers]") {
    // A BODYSTRUCTURE value (nested list) must be parsed-and-skipped without
    // desyncing the key/value walk.
    const QByteArray line =
        "1 FETCH (UID 4 BODYSTRUCTURE ((\"text\" \"plain\" (\"charset\" \"utf-8\") NIL NIL "
        "\"7bit\" 65 1 NIL NIL NIL NIL) \"alternative\") ENVELOPE (NIL \"after\" NIL NIL NIL NIL "
        "NIL NIL NIL NIL))";
    const auto items = Mappers::parseFetch({line});
    REQUIRE(items.size() == 1);
    CHECK(items[0].uid == 4u);
    CHECK(items[0].envelope.subject == "after"); // walk stayed aligned past BODYSTRUCTURE
}

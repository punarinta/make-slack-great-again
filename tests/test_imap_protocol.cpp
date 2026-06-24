// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Unit tests for the pure IMAP protocol layer: response framing + light parsers.
#include <catch2/catch_test_macros.hpp>

#include "backend/imap/imap_protocol.h"

using namespace imap;

namespace {
QByteArray wire(const QByteArray &s) {
    QByteArray out = s;
    out.replace("\n", "\r\n");
    return out;
}
QList<QByteArray> drain(ResponseFramer &f) {
    QList<QByteArray> lines;
    QByteArray        l;
    while (f.nextLine(l))
        lines.append(l);
    return lines;
}
} // namespace

// ── ResponseFramer ───────────────────────────────────────────────────────────

TEST_CASE("framer: simple CRLF lines", "[imap][framer]") {
    ResponseFramer f;
    f.append(wire("* OK ready\nA1 OK done\n"));
    auto lines = drain(f);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "* OK ready");
    CHECK(lines[1] == "A1 OK done");
}

TEST_CASE("framer: partial data across appends", "[imap][framer]") {
    ResponseFramer f;
    f.append("* OK par");
    CHECK(drain(f).isEmpty()); // no complete line yet
    f.append(wire("tial\n"));
    auto lines = drain(f);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "* OK partial");
}

TEST_CASE("framer: literal with embedded CRLFs is absorbed into one line", "[imap][framer]") {
    // A FETCH whose literal body contains a CRLF — built byte-exact so the {12}
    // count matches "line1\r\nline2" (5 + 2 + 5).
    ResponseFramer f;
    QByteArray     data;
    data += "* 1 FETCH (UID 5 BODY[] {12}\r\n";
    data += "line1\r\nline2"; // exactly 12 literal bytes
    data += ")\r\n";
    data += "A2 OK done\r\n";
    f.append(data);
    auto lines = drain(f);
    REQUIRE(lines.size() == 2);
    // the literal's internal CRLF must NOT split the logical line
    CHECK(lines[0].contains("line1\r\nline2"));
    CHECK(lines[0].startsWith("* 1 FETCH"));
    CHECK(lines[1] == "A2 OK done");
}

TEST_CASE("framer: incomplete literal waits for the rest", "[imap][framer]") {
    ResponseFramer f;
    f.append(wire("* 1 FETCH (BODY[] {10}\nshort"));
    CHECK(drain(f).isEmpty()); // only 5 of 10 literal bytes present
    f.append("12345");
    f.append(wire(")\n"));
    auto lines = drain(f);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].contains("short12345"));
}

TEST_CASE("framer: non-synchronizing literal {n+}", "[imap][framer]") {
    ResponseFramer f;
    f.append(wire("* 1 FETCH (X {3+}\nabc)\n"));
    auto lines = drain(f);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].contains("abc"));
}

// ── LIST ─────────────────────────────────────────────────────────────────────

TEST_CASE("parseList: special-use flags, delimiter, quoted names", "[imap][list]") {
    const QList<QByteArray> u = {
        "LIST (\\HasNoChildren \\Trash) \"/\" Trash",
        "LIST (\\HasNoChildren \\Sent) \"/\" Sent",
        "LIST (\\HasNoChildren) \"/\" INBOX",
        "LIST (\\HasChildren \\Noselect) \"/\" \"[Gmail]\"",
        "LIST (\\HasNoChildren \\All) \"/\" \"[Gmail]/All Mail\"",
    };
    const auto m = Proto::parseList(u);
    REQUIRE(m.size() == 5);
    CHECK(m[0].name == "Trash");
    CHECK(m[0].hasFlag("\\Trash"));
    CHECK(m[0].delimiter == QLatin1Char('/'));
    CHECK(m[1].hasFlag("\\Sent"));
    CHECK(m[2].name == "INBOX");
    CHECK_FALSE(m[3].selectable);           // \Noselect
    CHECK(m[4].name == "[Gmail]/All Mail"); // quoted name with a space
    CHECK(m[4].hasFlag("\\All"));
}

// ── SEARCH ─────────────────────────────────────────────────────────────────────

TEST_CASE("parseSearch: UID list", "[imap][search]") {
    const auto uids = Proto::parseSearch({"SEARCH 97 117 125 7309", "x"});
    REQUIRE(uids.size() == 4);
    CHECK(uids.first() == 97u);
    CHECK(uids.last() == 7309u);
    CHECK(Proto::parseSearch({"SEARCH"}).isEmpty()); // empty mailbox
}

// ── SELECT ─────────────────────────────────────────────────────────────────────

TEST_CASE("parseSelect: cursors + read-write", "[imap][select]") {
    const QList<QByteArray> u = {
        "FLAGS (\\Answered \\Flagged \\Seen)",
        "1119 EXISTS",
        "0 RECENT",
        "OK [UNSEEN 1112] First unseen.",
        "OK [UIDVALIDITY 1696016395] UIDs valid",
        "OK [UIDNEXT 7310] Predicted next UID",
    };
    const auto r = Proto::parseSelect(u, "OK [READ-WRITE] Select completed");
    CHECK(r.exists == 1119);
    CHECK(r.recent == 0);
    CHECK(r.uidValidity == 1696016395u);
    CHECK(r.uidNext == 7310u);
    CHECK(r.readWrite);
    CHECK(r.flags.contains("\\Seen"));

    const auto ro = Proto::parseSelect({"5 EXISTS"}, "OK [READ-ONLY] done");
    CHECK_FALSE(ro.readWrite);
}

// ── tokenize ────────────────────────────────────────────────────────────────

TEST_CASE("tokenize: atoms, quoted strings, parenthesised groups", "[imap][tokenize]") {
    const auto t = Proto::tokenize("(\\A \\B) \"/\" \"a name\"");
    REQUIRE(t.size() == 3);
    CHECK(t[0] == "(\\A \\B)"); // group kept whole
    CHECK(t[1] == "/");         // quotes stripped
    CHECK(t[2] == "a name");
}

TEST_CASE("quote: escapes backslash and quote", "[imap][quote]") {
    CHECK(Proto::quote("plain") == "\"plain\"");
    CHECK(Proto::quote("a\"b") == "\"a\\\"b\"");
    CHECK(Proto::quote("a\\b") == "\"a\\\\b\"");
}

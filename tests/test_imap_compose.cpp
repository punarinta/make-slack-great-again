// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Unit tests for the outgoing MIME message builder (Phase 4 send path).
#include <catch2/catch_test_macros.hpp>

#include "backend/imap/imap_compose.h"

using namespace imap;

namespace {
ComposeParams base() {
    ComposeParams p;
    p.fromEmail   = "vladimir@lingolette.com";
    p.to          = {"alice@example.com"};
    p.subject     = "Lunch?";
    p.bodyText    = "Are you free tomorrow?";
    p.messageId   = "abc123@lingolette.com";
    p.dateRfc2822 = "Tue, 23 Jun 2026 12:00:00 +0000";
    return p;
}
QString decodeB64Body(const QByteArray &msg) {
    const int hdrEnd = msg.indexOf("\r\n\r\n");
    return QString::fromUtf8(QByteArray::fromBase64(msg.mid(hdrEnd + 4)));
}
} // namespace

TEST_CASE("compose: headers + base64 plain body", "[imap][compose]") {
    const QByteArray m = buildMimeMessage(base());
    CHECK(m.contains("From: vladimir@lingolette.com\r\n"));
    CHECK(m.contains("To: alice@example.com\r\n"));
    CHECK(m.contains("Subject: Lunch?\r\n"));
    CHECK(m.contains("Message-ID: <abc123@lingolette.com>\r\n"));
    CHECK(m.contains("Date: Tue, 23 Jun 2026 12:00:00 +0000\r\n"));
    CHECK(m.contains("Content-Type: text/plain; charset=utf-8\r\n"));
    CHECK(m.contains("Content-Transfer-Encoding: base64\r\n"));
    CHECK(decodeB64Body(m).trimmed() == "Are you free tomorrow?");
}

TEST_CASE("compose: reply threading headers", "[imap][compose]") {
    ComposeParams p    = base();
    p.subject          = "Re: Lunch?";
    p.inReplyTo        = "abc123@lingolette.com";
    p.references       = {"root@x.com", "abc123@lingolette.com"};
    const QByteArray m = buildMimeMessage(p);
    CHECK(m.contains("In-Reply-To: <abc123@lingolette.com>\r\n"));
    CHECK(m.contains("References: <root@x.com> <abc123@lingolette.com>\r\n"));
}

TEST_CASE("compose: RFC 2047-encodes a non-ASCII subject", "[imap][compose]") {
    ComposeParams p    = base();
    p.subject          = QString::fromUtf8("Caf\xC3\xA9");
    const QByteArray m = buildMimeMessage(p);
    CHECK(m.contains("Subject: =?UTF-8?B?")); // encoded-word, not raw UTF-8
}

TEST_CASE("compose: multipart/mixed with an attachment", "[imap][compose]") {
    ComposeParams p    = base();
    p.boundary         = "BOUND123";
    p.attachments      = {{"r.pdf", "application/pdf", QByteArray("PDFDATA")}};
    const QByteArray m = buildMimeMessage(p);
    CHECK(m.contains("Content-Type: multipart/mixed; boundary=\"BOUND123\"\r\n"));
    CHECK(m.contains("--BOUND123\r\n"));
    CHECK(m.contains("Content-Disposition: attachment; filename=\"r.pdf\"\r\n"));
    CHECK(m.contains(QByteArray("PDFDATA").toBase64()));
    CHECK(m.endsWith("--BOUND123--\r\n"));
}

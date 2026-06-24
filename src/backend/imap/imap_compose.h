// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Build an RFC 5322 / MIME message for SMTP submission (imap-backend-plan Phase 4).
// Pure (deterministic given its inputs — date/message-id/boundary are passed in,
// not generated here), so it unit-tests in isolation. Plain-text body (base64) +
// optional attachments as multipart/mixed; subjects/names RFC 2047-encoded.
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace imap {

struct OutgoingAttachment {
    QString    filename;
    QString    mimeType; // e.g. "application/pdf"; defaults to application/octet-stream
    QByteArray content;
};

struct ComposeParams {
    QString                   fromName;
    QString                   fromEmail;
    QStringList               to;
    QStringList               cc;
    QString                   subject;
    QString                   bodyText;    // plain UTF-8
    QString                   inReplyTo;   // Message-ID (no angle brackets) or empty
    QStringList               references;  // Message-IDs (no angle brackets)
    QString                   messageId;   // this message's id (no angle brackets)
    QString                   dateRfc2822; // pre-formatted Date header value
    QString                   boundary;    // multipart boundary (only used with attachments)
    QList<OutgoingAttachment> attachments;
};

// Assemble the full message (CRLF line endings, dot-stuffing is the SMTP layer's job).
QByteArray buildMimeMessage(const ComposeParams &p);

// RFC 2047-encode a header value if it contains non-ASCII (else returned as-is).
QByteArray encodeHeaderWord(const QString &s);

} // namespace imap

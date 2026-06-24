// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// IMAP response → structured data mappers (below the Backend seam). Parses the
// S-expression bodies of FETCH (ENVELOPE/FLAGS/INTERNALDATE) and THREAD into
// plain structs. Reuses the MIME parser's RFC 2047 + addr helpers (subjects and
// address display-names in ENVELOPE are likewise RFC 2047-encoded). Pure (no
// socket/QObject), so unit-testable in isolation.
#pragma once

#include "backend/imap/mime_parser.h" // MimeAddress + Mime helpers

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

namespace imap {

// Parsed IMAP ENVELOPE (RFC 3501 §7.4.2): the message's addressing + identity.
struct Envelope {
    QDateTime          date;    // the Date header, parsed
    QString            subject; // RFC 2047-decoded
    QList<MimeAddress> from;
    QList<MimeAddress> sender;
    QList<MimeAddress> replyTo;
    QList<MimeAddress> to;
    QList<MimeAddress> cc;
    QList<MimeAddress> bcc;
    QString            inReplyTo; // message-id, angle brackets stripped
    QString            messageId; // message-id, angle brackets stripped
};

// One message as returned by a FETCH (the parts we request: UID/FLAGS/
// INTERNALDATE/ENVELOPE, plus the raw BODY[] literal when fetched).
struct FetchItem {
    quint32     uid = 0;
    QStringList flags;        // \Seen, \Answered, $label, …
    QDateTime   internalDate; // server arrival time (fallback when ENVELOPE date is bad)
    bool        hasEnvelope = false;
    Envelope    envelope;
    bool        hasBody = false;
    QByteArray  rawBody; // BODY[] contents (a full RFC 822 message) when requested

    bool seen() const {
        for (const auto &f : flags)
            if (f.compare(QStringLiteral("\\Seen"), Qt::CaseInsensitive) == 0)
                return true;
        return false;
    }
};

namespace Mappers {

// Parse the raw "* n FETCH (...)" lines (literals inlined by ResponseFramer).
QList<FetchItem> parseFetch(const QList<QByteArray> &fetchLines);

// Parse a "THREAD (...)(...)" line into flattened thread groups: each inner list
// is one thread as an ordered UID list, root first (the tree is flattened
// depth-first, matching Slack's root-plus-replies model).
QList<QList<quint32>> parseThread(const QByteArray &threadLine);

} // namespace Mappers
} // namespace imap

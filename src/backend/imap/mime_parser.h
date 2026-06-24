// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Hand-rolled MIME / RFC 5322 parser for the IMAP backend (below the seam).
// Turns a raw fetched message into displayable text + attachment metadata.
// Scoped to the common shapes (imap-backend-plan §2): text/plain, text/html,
// multipart/{alternative,mixed,related}, nested multiparts; base64 +
// quoted-printable transfer encodings; RFC 2047 encoded-words in headers;
// charset → UTF-8 via QStringDecoder. Defensive by design — malformed mail
// degrades, never throws. No Qt GUI / network deps (unit-testable in isolation).
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace imap {

// One parsed address from a From/To/Cc/Reply-To header (RFC 5322, common forms).
struct MimeAddress {
    QString name;  // display name, RFC 2047-decoded (may be empty)
    QString email; // addr-spec "local@domain", lowercased
    bool    operator==(const MimeAddress &) const = default;
};

// A non-inline part surfaced as an attachment (or an inline image by Content-ID).
struct MimeAttachment {
    QString    filename;
    QString    mimeType;  // e.g. "application/pdf", "image/png"
    QString    contentId; // for cid: refs; angle brackets stripped; empty if none
    bool       isInline = false;
    qint64     size     = 0; // decoded byte count
    QByteArray content;      // decoded bytes
    bool       operator==(const MimeAttachment &) const = default;
};

// The fully parsed message: headers of interest + decoded body alternatives +
// attachments. Both textPlain and textHtml may be present (multipart/alternative)
// — the renderer prefers plain, falling back to converted HTML (§2).
struct ParsedMessage {
    QString            messageId;  // angle brackets stripped
    QString            inReplyTo;  // angle brackets stripped
    QStringList        references; // message-ids, brackets stripped, in order
    QString            subject;    // RFC 2047-decoded
    QString            listId;     // List-Id value (the id token) if present
    QList<MimeAddress> from;
    QList<MimeAddress> to;
    QList<MimeAddress> cc;
    QList<MimeAddress> replyTo;
    QDateTime          date; // parsed Date header

    QString               textPlain;
    QString               textHtml;
    QList<MimeAttachment> attachments;

    // All non-self addresses (to ∪ cc ∪ from), deduped — the raw material the
    // bucketing engine groups conversations by (§3). "me" filtering happens in
    // the backend, which knows the account's own aliases.
    QList<MimeAddress> participants() const;
};

// Stateless parsing helpers — also reused by the IMAP ENVELOPE mapper (subjects
// and address display-names there are likewise RFC 2047-encoded).
namespace Mime {

ParsedMessage parse(const QByteArray &rawMessage);

// RFC 2047 encoded-words ("=?utf-8?B?…?=" / "=?utf-8?Q?…?="), with the
// adjacent-encoded-word whitespace-collapse rule. Plain text passes through.
QString decodeEncodedWords(const QByteArray &headerValue);

// Decode bytes labelled with `charset` to a QString (UTF-8 fallback).
QString decodeText(const QByteArray &bytes, const QString &charset);

QByteArray decodeQuotedPrintable(const QByteArray &in); // body QP (soft breaks)
QByteArray decodeBase64(const QByteArray &in);          // whitespace-tolerant

// RFC 5322 address-list: "A <a@x>, \"B\" <b@y>, c@z". Common forms only;
// the raw addr-spec is kept as a fallback. Display names are RFC 2047-decoded.
QList<MimeAddress> parseAddressList(const QString &value);

// Strip surrounding <...> from a Message-ID / In-Reply-To token.
QString stripAngles(const QString &id);

} // namespace Mime
} // namespace imap

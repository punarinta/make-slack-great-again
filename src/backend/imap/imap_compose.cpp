// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_compose.h"

namespace imap {

namespace {

bool isAscii(const QString &s) {
    for (QChar c : s)
        if (c.unicode() > 126 || c.unicode() < 32)
            return false;
    return true;
}

// Wrap base64 to 76-char lines (RFC 2045).
QByteArray base64Wrapped(const QByteArray &raw) {
    const QByteArray b64 = raw.toBase64();
    QByteArray       out;
    for (int i = 0; i < b64.size(); i += 76) {
        out += b64.mid(i, 76);
        out += "\r\n";
    }
    return out;
}

QByteArray addr(const QString &name, const QString &email) {
    if (name.trimmed().isEmpty())
        return email.toUtf8();
    return encodeHeaderWord(name) + " <" + email.toUtf8() + ">";
}

QByteArray joinAddrs(const QStringList &emails) {
    QByteArray out;
    for (const QString &e : emails) {
        if (!out.isEmpty())
            out += ", ";
        out += e.toUtf8();
    }
    return out;
}

} // namespace

QByteArray encodeHeaderWord(const QString &s) {
    if (isAscii(s)) {
        // Quote if it contains specials that would confuse a display-name context.
        return s.toUtf8();
    }
    return "=?UTF-8?B?" + s.toUtf8().toBase64() + "?=";
}

QByteArray buildMimeMessage(const ComposeParams &p) {
    QByteArray m;
    auto       hdr = [&](const char *name, const QByteArray &value) {
        if (!value.isEmpty())
            m += QByteArray(name) + ": " + value + "\r\n";
    };

    hdr("From", addr(p.fromName, p.fromEmail));
    hdr("To", joinAddrs(p.to));
    hdr("Cc", joinAddrs(p.cc));
    hdr("Subject", encodeHeaderWord(p.subject));
    hdr("Date", p.dateRfc2822.toUtf8());
    if (!p.messageId.isEmpty())
        hdr("Message-ID", "<" + p.messageId.toUtf8() + ">");
    if (!p.inReplyTo.isEmpty())
        hdr("In-Reply-To", "<" + p.inReplyTo.toUtf8() + ">");
    if (!p.references.isEmpty()) {
        QByteArray refs;
        for (const QString &r : p.references) {
            if (!refs.isEmpty())
                refs += " ";
            refs += "<" + r.toUtf8() + ">";
        }
        hdr("References", refs);
    }
    m += "MIME-Version: 1.0\r\n";

    const QByteArray body = base64Wrapped(p.bodyText.toUtf8());

    if (p.attachments.isEmpty()) {
        m += "Content-Type: text/plain; charset=utf-8\r\n";
        m += "Content-Transfer-Encoding: base64\r\n";
        m += "\r\n";
        m += body;
    } else {
        const QByteArray b = p.boundary.toUtf8();
        m += "Content-Type: multipart/mixed; boundary=\"" + b + "\"\r\n\r\n";
        m += "--" + b + "\r\n";
        m += "Content-Type: text/plain; charset=utf-8\r\n";
        m += "Content-Transfer-Encoding: base64\r\n\r\n";
        m += body;
        m += "\r\n";
        for (const OutgoingAttachment &a : p.attachments) {
            const QByteArray mime =
                a.mimeType.isEmpty() ? "application/octet-stream" : a.mimeType.toUtf8();
            m += "--" + b + "\r\n";
            m += "Content-Type: " + mime + "; name=\"" + a.filename.toUtf8() + "\"\r\n";
            m += "Content-Disposition: attachment; filename=\"" + a.filename.toUtf8() + "\"\r\n";
            m += "Content-Transfer-Encoding: base64\r\n\r\n";
            m += base64Wrapped(a.content);
            m += "\r\n";
        }
        m += "--" + b + "--\r\n";
    }
    return m;
}

} // namespace imap

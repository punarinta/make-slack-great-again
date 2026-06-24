// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/mime_parser.h"

#include <QRegularExpression>
#include <QStringDecoder>

namespace imap {

namespace {

// ── Low-level header helpers ─────────────────────────────────────────────────

// Split a raw message/part into (header-block, body). Tolerates CRLF and bare
// LF. Returns header bytes (without the separating blank line) and body bytes.
std::pair<QByteArray, QByteArray> splitHeaderBody(const QByteArray &raw) {
    int       sep    = raw.indexOf("\r\n\r\n");
    int       sepLen = 4;
    const int lf     = raw.indexOf("\n\n");
    if (sep < 0 || (lf >= 0 && lf < sep)) {
        sep    = lf;
        sepLen = 2;
    }
    if (sep < 0)
        return {raw, QByteArray()}; // all headers, no body
    return {raw.left(sep), raw.mid(sep + sepLen)};
}

// Unfold (RFC 5322 §2.2.3) and split into name→value. Names lowercased. First
// occurrence wins for the simple map; callers needing all values of a repeated
// header (References) get them re-joined since we keep the first which for our
// headers is sufficient — References is a single (possibly folded) header.
QMap<QString, QString> parseHeaders(const QByteArray &headerBlock) {
    QMap<QString, QString>  out;
    const QList<QByteArray> rawLines = headerBlock.split('\n');
    QByteArray              cur;
    auto                    flush = [&] {
        if (cur.isEmpty())
            return;
        const int colon = cur.indexOf(':');
        if (colon > 0) {
            QString name = QString::fromLatin1(cur.left(colon)).trimmed().toLower();
            QString value = QString::fromLatin1(cur.mid(colon + 1)).trimmed();
            if (!out.contains(name)) // first wins
                out.insert(name, value);
        }
        cur.clear();
    };
    for (QByteArray line : rawLines) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            continue;
        if (line[0] == ' ' || line[0] == '\t') { // continuation (folded)
            cur += ' ';
            cur += line.trimmed();
        } else {
            flush();
            cur = line;
        }
    }
    flush();
    return out;
}

// Parse a structured header value "main; key=val; key2="quoted val"" into the
// leading token + a lowercased-key parameter map.
struct StructuredValue {
    QString                main; // lowercased
    QMap<QString, QString> params;
};
StructuredValue parseStructured(const QString &value) {
    StructuredValue   sv;
    const QStringList parts = [&] {
        // split on ';' but not inside quotes
        QStringList out;
        QString     cur;
        bool        inq = false;
        for (QChar c : value) {
            if (c == '"')
                inq = !inq;
            if (c == ';' && !inq) {
                out << cur;
                cur.clear();
            } else
                cur += c;
        }
        out << cur;
        return out;
    }();
    for (int i = 0; i < parts.size(); ++i) {
        const QString p = parts[i].trimmed();
        if (i == 0) {
            sv.main = p.toLower();
            continue;
        }
        const int eq = p.indexOf('=');
        if (eq < 0)
            continue;
        QString k = p.left(eq).trimmed().toLower();
        QString v = p.mid(eq + 1).trimmed();
        if (v.startsWith('"') && v.endsWith('"') && v.size() >= 2)
            v = v.mid(1, v.size() - 2);
        sv.params.insert(k, v);
    }
    return sv;
}

// ── A recursively-parsed MIME entity ─────────────────────────────────────────
struct Part {
    QMap<QString, QString> headers;
    QString                type    = "text"; // RFC 2045 default
    QString                subtype = "plain";
    QMap<QString, QString> ctParams;
    QString                cte;         // content-transfer-encoding (lowercased)
    QString                disposition; // "inline" / "attachment"
    QString                dispFilename;
    QString                contentId;
    QByteArray             body;     // raw (undecoded) body for leaves
    QList<Part>            children; // for multipart
    bool                   isMultipart = false;
};

QByteArray decodeBody(const Part &p) {
    if (p.cte == "base64")
        return Mime::decodeBase64(p.body);
    if (p.cte == "quoted-printable")
        return Mime::decodeQuotedPrintable(p.body);
    return p.body; // 7bit / 8bit / binary / none
}

// Split a multipart body into its child entity byte-blobs given the boundary.
QList<QByteArray> splitMultipart(const QByteArray &body, const QByteArray &boundary) {
    QList<QByteArray> parts;
    const QByteArray  delim    = "--" + boundary;
    int               pos      = 0;
    bool              started  = false;
    int               partFrom = -1;
    while (pos < body.size()) {
        int nl = body.indexOf('\n', pos);
        if (nl < 0)
            nl = body.size();
        QByteArray line = body.mid(pos, nl - pos);
        if (line.endsWith('\r'))
            line.chop(1);
        const bool isDelim = line == delim || line == delim + "--";
        if (isDelim) {
            if (started && partFrom >= 0) {
                // part runs from partFrom up to the line before this delim
                int end = pos;
                // trim the trailing CRLF that belongs to the delimiter line
                parts.append(body.mid(partFrom, end - partFrom));
            }
            if (line == delim + "--")
                break; // closing delimiter
            started  = true;
            partFrom = (nl < body.size()) ? nl + 1 : body.size();
        }
        pos = (nl < body.size()) ? nl + 1 : body.size();
    }
    return parts;
}

Part parsePart(const QByteArray &raw) {
    Part p;
    auto [hdr, body] = splitHeaderBody(raw);
    p.headers        = parseHeaders(hdr);
    p.body           = body;

    if (p.headers.contains("content-type")) {
        const StructuredValue ct = parseStructured(p.headers.value("content-type"));
        const int             sl = ct.main.indexOf('/');
        if (sl > 0) {
            p.type    = ct.main.left(sl);
            p.subtype = ct.main.mid(sl + 1);
        } else if (!ct.main.isEmpty()) {
            p.type = ct.main;
        }
        p.ctParams = ct.params;
    }
    p.cte = p.headers.value("content-transfer-encoding").toLower().trimmed();
    if (p.headers.contains("content-disposition")) {
        const StructuredValue cd = parseStructured(p.headers.value("content-disposition"));
        p.disposition            = cd.main;
        p.dispFilename           = cd.params.value("filename");
    }
    p.contentId = Mime::stripAngles(p.headers.value("content-id"));

    if (p.type == "multipart" && p.ctParams.contains("boundary")) {
        p.isMultipart             = true;
        const QByteArray boundary = p.ctParams.value("boundary").toUtf8();
        for (const QByteArray &child : splitMultipart(p.body, boundary))
            p.children.append(parsePart(child));
        p.body.clear(); // children own the content
    }
    return p;
}

// Walk the tree, filling text alternatives and attachments.
void collect(const Part &p, ParsedMessage &msg) {
    if (p.isMultipart) {
        for (const Part &c : p.children)
            collect(c, msg);
        return;
    }
    const bool isAttachment =
        p.disposition == "attachment" || (!p.dispFilename.isEmpty() && p.type != "text") ||
        (p.ctParams.contains("name") && p.type != "text" && p.disposition != "inline");

    if (p.type == "text" && !isAttachment) {
        const QString charset = p.ctParams.value("charset", "utf-8");
        const QString text    = Mime::decodeText(decodeBody(p), charset);
        if (p.subtype == "html") {
            if (msg.textHtml.isEmpty())
                msg.textHtml = text;
        } else { // plain (or any other text/*)
            if (msg.textPlain.isEmpty())
                msg.textPlain = text;
        }
        return;
    }

    // Otherwise it's an attachment (or inline image referenced by cid).
    MimeAttachment a;
    a.filename  = !p.dispFilename.isEmpty() ? p.dispFilename : p.ctParams.value("name");
    a.mimeType  = p.type + "/" + p.subtype;
    a.contentId = p.contentId;
    a.isInline  = p.disposition == "inline" || !p.contentId.isEmpty();
    a.content   = decodeBody(p);
    a.size      = a.content.size();
    msg.attachments.append(a);
}

} // namespace

// ── Public helpers ───────────────────────────────────────────────────────────

QString Mime::stripAngles(const QString &id) {
    QString s = id.trimmed();
    if (s.startsWith('<') && s.endsWith('>') && s.size() >= 2)
        s = s.mid(1, s.size() - 2);
    return s.trimmed();
}

QByteArray Mime::decodeBase64(const QByteArray &in) {
    QByteArray clean;
    clean.reserve(in.size());
    for (char c : in)
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
            clean.append(c);
    return QByteArray::fromBase64(clean);
}

QByteArray Mime::decodeQuotedPrintable(const QByteArray &in) {
    QByteArray out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '=' && i + 1 < in.size()) {
            // soft line break: "=\r\n" or "=\n"
            if (in[i + 1] == '\r' && i + 2 < in.size() && in[i + 2] == '\n') {
                i += 2;
                continue;
            }
            if (in[i + 1] == '\n') {
                i += 1;
                continue;
            }
            if (i + 2 < in.size()) {
                bool      ok = false;
                const int v  = in.mid(i + 1, 2).toInt(&ok, 16);
                if (ok) {
                    out.append(static_cast<char>(v));
                    i += 2;
                    continue;
                }
            }
        }
        out.append(c);
    }
    return out;
}

QString Mime::decodeText(const QByteArray &bytes, const QString &charset) {
    QStringDecoder dec(charset.isEmpty() ? "UTF-8" : charset.toUtf8().constData());
    if (!dec.isValid())
        dec = QStringDecoder("UTF-8");
    QString s = dec.decode(bytes);
    if (dec.hasError()) {
        // Fall back to Latin-1, which never errors and is the de-facto default
        // for unlabelled / mislabelled 8-bit mail.
        s = QString::fromLatin1(bytes);
    }
    return s;
}

QString Mime::decodeEncodedWords(const QByteArray &headerValue) {
    const QString in = QString::fromLatin1(headerValue);
    QString       out;
    int           i           = 0;
    bool          prevWasWord = false;
    while (i < in.size()) {
        const int start = in.indexOf("=?", i);
        if (start < 0) {
            out += in.mid(i);
            break;
        }
        // Emit the gap before the encoded word. Per RFC 2047, whitespace between
        // two adjacent encoded words is not displayed.
        const QString gap = in.mid(i, start - i);
        if (!(prevWasWord && gap.trimmed().isEmpty()))
            out += gap;

        // Parse =?charset?enc?text?=
        const int q1  = in.indexOf('?', start + 2);
        const int q2  = (q1 >= 0) ? in.indexOf('?', q1 + 1) : -1;
        const int end = (q2 >= 0) ? in.indexOf("?=", q2 + 1) : -1;
        if (q1 < 0 || q2 < 0 || end < 0) {
            out += in.mid(start, 2); // not a valid word; emit "=?" literally
            i           = start + 2;
            prevWasWord = false;
            continue;
        }
        const QString    charset = in.mid(start + 2, q1 - (start + 2));
        const QString    enc     = in.mid(q1 + 1, q2 - (q1 + 1)).toUpper();
        const QByteArray text    = in.mid(q2 + 1, end - (q2 + 1)).toLatin1();
        QByteArray       raw;
        if (enc == "B") {
            raw = decodeBase64(text);
        } else if (enc == "Q") {
            // RFC 2047 Q: like QP but '_' is a space.
            QByteArray t = text;
            t.replace('_', ' ');
            raw = decodeQuotedPrintable(t);
        } else {
            raw = text;
        }
        out += decodeText(raw, charset);
        i           = end + 2;
        prevWasWord = true;
    }
    return out;
}

QList<MimeAddress> Mime::parseAddressList(const QString &value) {
    QList<MimeAddress> out;
    // Split on commas not inside quotes or angle brackets.
    QStringList        items;
    {
        QString cur;
        bool    inq = false, inangle = false;
        for (QChar c : value) {
            if (c == '"')
                inq = !inq;
            else if (c == '<')
                inangle = true;
            else if (c == '>')
                inangle = false;
            if (c == ',' && !inq && !inangle) {
                items << cur;
                cur.clear();
            } else
                cur += c;
        }
        if (!cur.trimmed().isEmpty())
            items << cur;
    }
    for (QString item : items) {
        item = item.trimmed();
        if (item.isEmpty())
            continue;
        MimeAddress a;
        const int   lt = item.lastIndexOf('<');
        const int   gt = item.lastIndexOf('>');
        if (lt >= 0 && gt > lt) {
            a.email      = item.mid(lt + 1, gt - lt - 1).trimmed().toLower();
            QString name = item.left(lt).trimmed();
            if (name.startsWith('"') && name.endsWith('"') && name.size() >= 2)
                name = name.mid(1, name.size() - 2);
            a.name = decodeEncodedWords(name.toLatin1()).trimmed();
        } else {
            a.email = item.toLower();
        }
        if (!a.email.isEmpty())
            out.append(a);
    }
    return out;
}

// ── Top-level parse ──────────────────────────────────────────────────────────

QList<MimeAddress> ParsedMessage::participants() const {
    QList<MimeAddress> all;
    auto               addAll = [&](const QList<MimeAddress> &src) {
        for (const auto &a : src) {
            bool seen = false;
            for (const auto &e : all)
                if (e.email == a.email) {
                    seen = true;
                    break;
                }
            if (!seen)
                all.append(a);
        }
    };
    addAll(from);
    addAll(to);
    addAll(cc);
    return all;
}

ParsedMessage Mime::parse(const QByteArray &rawMessage) {
    ParsedMessage msg;
    const Part    root = parsePart(rawMessage);

    msg.messageId = stripAngles(root.headers.value("message-id"));
    msg.inReplyTo = stripAngles(root.headers.value("in-reply-to"));
    msg.subject   = decodeEncodedWords(root.headers.value("subject").toLatin1());
    msg.from      = parseAddressList(root.headers.value("from"));
    msg.to        = parseAddressList(root.headers.value("to"));
    msg.cc        = parseAddressList(root.headers.value("cc"));
    msg.replyTo   = parseAddressList(root.headers.value("reply-to"));

    // References: whitespace-separated <id> tokens.
    const QString refs = root.headers.value("references");
    for (const QString &tok : refs.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
        const QString id = stripAngles(tok);
        if (!id.isEmpty())
            msg.references.append(id);
    }

    // List-Id: "Friendly Name <list.id.example.com>" → keep the bracketed token,
    // else the whole trimmed value.
    if (root.headers.contains("list-id")) {
        // "Friendly Name <list.id.example.com>" → the bracketed token; else the
        // whole trimmed value (some lists omit the description).
        const QString li = root.headers.value("list-id");
        const int     lt = li.indexOf('<');
        const int     gt = li.indexOf('>', lt + 1);
        msg.listId = (lt >= 0 && gt > lt) ? li.mid(lt + 1, gt - lt - 1).trimmed() : li.trimmed();
    }

    msg.date = QDateTime::fromString(root.headers.value("date"), Qt::RFC2822Date);

    collect(root, msg);
    return msg;
}

} // namespace imap

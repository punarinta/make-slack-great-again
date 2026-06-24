// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_mappers.h"

namespace imap {

namespace {

// ── Minimal IMAP S-expression parser ─────────────────────────────────────────
// A node is either NIL, a string (atom / quoted / literal), or a list. Quoted
// strings unescape \" \\; literals "{n}\r\n<bytes>" capture exactly n raw bytes.
struct Node {
    enum Type { Nil, Str, List } type = Nil;
    QByteArray  str;
    QList<Node> items;

    bool isNil() const { return type == Nil; }
    bool isStr() const { return type == Str; }
    bool isList() const { return type == List; }
};

class SexpParser {
public:
    explicit SexpParser(const QByteArray &d, int pos = 0) : _d(d), _i(pos) {}

    int pos() const { return _i; }

    void skipSpace() {
        while (_i < _d.size() && (_d[_i] == ' ' || _d[_i] == '\t'))
            ++_i;
    }

    // Parse one value; returns false at end / on ')'.
    bool parseValue(Node &out) {
        skipSpace();
        if (_i >= _d.size() || _d[_i] == ')')
            return false;
        const char c = _d[_i];
        if (c == '(') {
            ++_i; // consume '('
            out.type = Node::List;
            out.items.clear();
            for (;;) {
                skipSpace();
                if (_i < _d.size() && _d[_i] == ')') {
                    ++_i;
                    break;
                }
                Node child;
                if (!parseValue(child))
                    break;
                out.items.append(child);
            }
            return true;
        }
        if (c == '"') {
            ++_i;
            QByteArray s;
            while (_i < _d.size() && _d[_i] != '"') {
                if (_d[_i] == '\\' && _i + 1 < _d.size())
                    ++_i;
                s.append(_d[_i]);
                ++_i;
            }
            if (_i < _d.size())
                ++_i; // closing quote
            out.type = Node::Str;
            out.str  = s;
            return true;
        }
        if (c == '{') {
            const int close = _d.indexOf('}', _i);
            if (close < 0) {
                _i = _d.size();
                return false;
            }
            const int n = _d.mid(_i + 1, close - _i - 1).toInt();
            _i          = close + 1;
            // skip the CRLF (or LF) that follows the literal octet count
            if (_i < _d.size() && _d[_i] == '\r')
                ++_i;
            if (_i < _d.size() && _d[_i] == '\n')
                ++_i;
            out.type = Node::Str;
            out.str  = _d.mid(_i, n);
            _i += n;
            return true;
        }
        // atom: until space / ( / ) / "
        const int start = _i;
        while (_i < _d.size() && _d[_i] != ' ' && _d[_i] != '\t' && _d[_i] != '(' &&
               _d[_i] != ')' && _d[_i] != '"')
            ++_i;
        const QByteArray atom = _d.mid(start, _i - start);
        if (atom.compare("NIL", Qt::CaseInsensitive) == 0) {
            out.type = Node::Nil;
        } else {
            out.type = Node::Str;
            out.str  = atom;
        }
        return true;
    }

private:
    const QByteArray &_d;
    int               _i;
};

// One ENVELOPE address: (name adl mailbox host) → MimeAddress.
MimeAddress addrFromNode(const Node &n) {
    MimeAddress a;
    if (!n.isList() || n.items.size() < 4)
        return a;
    const Node &name    = n.items[0];
    const Node &mailbox = n.items[2];
    const Node &host    = n.items[3];
    if (name.isStr())
        a.name = Mime::decodeEncodedWords(name.str).trimmed();
    if (mailbox.isStr() && host.isStr())
        a.email = QString::fromUtf8(mailbox.str + "@" + host.str).toLower();
    else if (mailbox.isStr())
        a.email = QString::fromUtf8(mailbox.str).toLower();
    return a;
}

QList<MimeAddress> addrListFromNode(const Node &n) {
    QList<MimeAddress> out;
    if (!n.isList())
        return out; // NIL → empty
    for (const Node &item : n.items) {
        const MimeAddress a = addrFromNode(item);
        if (!a.email.isEmpty() || !a.name.isEmpty())
            out.append(a);
    }
    return out;
}

// RFC 5322 Date parsing, tolerant of the deviations real mail ships with — most
// commonly a trailing zone comment ("… +0000 (UTC)") or an obsolete alphabetic
// zone, both of which make a bare Qt::RFC2822Date parse fail (→ epoch/1970).
QDateTime parseMessageDate(const QByteArray &raw) {
    QString s = QString::fromUtf8(raw).trimmed();
    if (s.isEmpty())
        return {};
    // Drop a trailing "(comment)" — e.g. "(UTC)", "(PST)", "(GMT+02:00)".
    const int paren = s.indexOf('(');
    if (paren >= 0)
        s = s.left(paren).trimmed();
    if (QDateTime dt = QDateTime::fromString(s, Qt::RFC2822Date); dt.isValid())
        return dt;
    // Map an obsolete alphabetic zone in the last token to a numeric offset.
    static const QHash<QString, QString> zones = {
        {"UT", "+0000"},
        {"GMT", "+0000"},
        {"UTC", "+0000"},
        {"Z", "+0000"},
        {"EST", "-0500"},
        {"EDT", "-0400"},
        {"CST", "-0600"},
        {"CDT", "-0500"},
        {"MST", "-0700"},
        {"MDT", "-0600"},
        {"PST", "-0800"},
        {"PDT", "-0700"},
    };
    if (const int sp = s.lastIndexOf(' '); sp > 0) {
        const QString z = s.mid(sp + 1).toUpper();
        if (zones.contains(z))
            if (QDateTime dt =
                    QDateTime::fromString(s.left(sp + 1) + zones.value(z), Qt::RFC2822Date);
                dt.isValid())
                return dt;
    }
    // Retry without a leading weekday ("Wed, 7 May 2025 …" → "7 May 2025 …").
    if (const int comma = s.indexOf(','); comma >= 0 && comma <= 4)
        if (QDateTime dt = QDateTime::fromString(s.mid(comma + 1).trimmed(), Qt::RFC2822Date);
            dt.isValid())
            return dt;
    return {};
}

Envelope envelopeFromNode(const Node &n) {
    Envelope e;
    if (!n.isList() || n.items.size() < 10)
        return e;
    if (n.items[0].isStr())
        e.date = parseMessageDate(n.items[0].str);
    if (n.items[1].isStr())
        e.subject = Mime::decodeEncodedWords(n.items[1].str);
    e.from    = addrListFromNode(n.items[2]);
    e.sender  = addrListFromNode(n.items[3]);
    e.replyTo = addrListFromNode(n.items[4]);
    e.to      = addrListFromNode(n.items[5]);
    e.cc      = addrListFromNode(n.items[6]);
    e.bcc     = addrListFromNode(n.items[7]);
    if (n.items[8].isStr())
        e.inReplyTo = Mime::stripAngles(QString::fromUtf8(n.items[8].str));
    if (n.items[9].isStr())
        e.messageId = Mime::stripAngles(QString::fromUtf8(n.items[9].str));
    return e;
}

// INTERNALDATE format: "22-Jun-2026 14:38:11 -0400".
QDateTime parseInternalDate(const QByteArray &s) {
    QDateTime dt =
        QDateTime::fromString(QString::fromUtf8(s), QStringLiteral("dd-MMM-yyyy HH:mm:ss t"));
    if (!dt.isValid())
        dt = QDateTime::fromString(
            QString::fromUtf8(s).left(20), QStringLiteral("dd-MMM-yyyy HH:mm:ss")
        );
    return dt;
}

void flattenThread(const Node &n, QList<quint32> &out) {
    if (n.isStr()) {
        bool          ok = false;
        const quint32 v  = n.str.toUInt(&ok);
        if (ok)
            out.append(v);
    } else if (n.isList()) {
        for (const Node &c : n.items)
            flattenThread(c, out);
    }
}

} // namespace

namespace Mappers {

QList<FetchItem> parseFetch(const QList<QByteArray> &fetchLines) {
    QList<FetchItem> out;
    for (const QByteArray &line : fetchLines) {
        const int fk = line.indexOf(" FETCH ");
        if (fk < 0)
            continue;
        const int op = line.indexOf('(', fk);
        if (op < 0)
            continue;
        SexpParser parser(line, op);
        Node       attrs;
        if (!parser.parseValue(attrs) || !attrs.isList())
            continue;

        FetchItem item;
        // attrs.items is a flat key/value sequence: UID 7306 FLAGS (...) ...
        for (int i = 0; i + 1 < attrs.items.size(); i += 2) {
            const Node &k = attrs.items[i];
            const Node &v = attrs.items[i + 1];
            if (!k.isStr())
                continue;
            const QByteArray key = k.str.toUpper();
            if (key == "UID" && v.isStr()) {
                item.uid = v.str.toUInt();
            } else if (key == "FLAGS" && v.isList()) {
                for (const Node &f : v.items)
                    if (f.isStr())
                        item.flags << QString::fromUtf8(f.str);
            } else if (key == "INTERNALDATE" && v.isStr()) {
                item.internalDate = parseInternalDate(v.str);
            } else if (key == "ENVELOPE" && v.isList()) {
                item.hasEnvelope = true;
                item.envelope    = envelopeFromNode(v);
            } else if ((key == "BODY[]" || key == "RFC822" || key == "BODY[TEXT]") && v.isStr()) {
                item.hasBody = true;
                item.rawBody = v.str;
            }
        }
        if (item.uid != 0 || item.hasEnvelope || item.hasBody)
            out.append(item);
    }
    return out;
}

QList<QList<quint32>> parseThread(const QByteArray &threadLine) {
    QList<QList<quint32>> groups;
    int                   start = threadLine.indexOf("THREAD");
    if (start < 0)
        return groups;
    start += 6; // past "THREAD"
    SexpParser parser(threadLine, start);
    for (;;) {
        Node group;
        if (!parser.parseValue(group))
            break;
        QList<quint32> uids;
        flattenThread(group, uids);
        if (!uids.isEmpty())
            groups.append(uids);
    }
    return groups;
}

} // namespace Mappers
} // namespace imap

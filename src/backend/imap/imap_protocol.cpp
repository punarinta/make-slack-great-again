// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_protocol.h"

namespace imap {

// ── ResponseFramer ───────────────────────────────────────────────────────────

bool ResponseFramer::nextLine(QByteArray &out) {
    int from = 0;
    for (;;) {
        const int crlf = _buf.indexOf("\r\n", from);
        if (crlf < 0)
            return false; // no complete line yet
        const QByteArray cand = _buf.left(crlf);
        // Trailing literal "{n}" / "{n+}" → the next n bytes are literal data
        // (possibly with CRLFs); keep scanning past them for the real EOL.
        int              n    = 0;
        bool             lit  = false;
        const int        br   = cand.lastIndexOf('{');
        if (br >= 0 && cand.endsWith('}')) {
            QByteArray inner = cand.mid(br + 1, cand.size() - br - 2);
            if (inner.endsWith('+'))
                inner.chop(1);
            bool ok = false;
            n       = inner.toInt(&ok);
            lit     = ok && n >= 0;
        }
        if (lit) {
            const int need = crlf + 2 + n;
            if (_buf.size() < need)
                return false; // literal not fully arrived
            from = need;
            continue;
        }
        out = _buf.left(crlf);
        _buf.remove(0, crlf + 2);
        return true;
    }
}

namespace Proto {

QByteArray quote(const QByteArray &s) {
    QByteArray e = s;
    e.replace("\\", "\\\\");
    e.replace("\"", "\\\"");
    return "\"" + e + "\"";
}

QList<QByteArray> tokenize(const QByteArray &line) {
    QList<QByteArray> out;
    int               i = 0;
    const int         n = line.size();
    while (i < n) {
        const char c = line[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '"') { // quoted string
            QByteArray tok;
            ++i;
            while (i < n && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < n)
                    ++i; // skip escape, take next literally
                tok.append(line[i]);
                ++i;
            }
            ++i; // closing quote
            out.append(tok);
        } else if (c == '(') { // parenthesised group — keep whole (incl. nesting)
            int       depth = 0;
            const int start = i;
            bool      inq   = false;
            while (i < n) {
                const char d = line[i];
                if (d == '"')
                    inq = !inq;
                else if (d == '(' && !inq)
                    ++depth;
                else if (d == ')' && !inq) {
                    --depth;
                    if (depth == 0) {
                        ++i;
                        break;
                    }
                }
                ++i;
            }
            out.append(line.mid(start, i - start));
        } else { // atom
            const int start = i;
            while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '(' && line[i] != '"')
                ++i;
            out.append(line.mid(start, i - start));
        }
    }
    return out;
}

QList<Mailbox> parseList(const QList<QByteArray> &untagged) {
    QList<Mailbox> out;
    for (const QByteArray &u : untagged) {
        // Accept "LIST" and "LSUB"/"XLIST" shapes.
        if (!(u.startsWith("LIST") || u.startsWith("XLIST")))
            continue;
        const int sp = u.indexOf(' ');
        if (sp < 0)
            continue;
        QByteArray rest = u.mid(sp + 1).trimmed();

        Mailbox m;
        // Flags: the leading "(...)" group.
        if (rest.startsWith('(')) {
            const int cp = rest.indexOf(')');
            if (cp > 0) {
                const QByteArray flagStr = rest.mid(1, cp - 1);
                for (const QByteArray &f : flagStr.split(' '))
                    if (!f.trimmed().isEmpty())
                        m.flags << QString::fromUtf8(f.trimmed());
                rest = rest.mid(cp + 1).trimmed();
            }
        }
        // Remainder: <delimiter> <name>.
        const QList<QByteArray> toks = tokenize(rest);
        if (toks.size() >= 2) {
            const QByteArray delim = toks[0];
            if (delim != "NIL" && !delim.isEmpty())
                m.delimiter = QLatin1Char(delim[0]);
            m.name = QString::fromUtf8(toks[1]);
        } else if (toks.size() == 1) {
            m.name = QString::fromUtf8(toks[0]);
        }
        m.selectable = !m.hasFlag(QStringLiteral("\\Noselect"));
        if (!m.name.isEmpty())
            out.append(m);
    }
    return out;
}

QList<quint32> parseSearch(const QList<QByteArray> &untagged) {
    QList<quint32> out;
    for (const QByteArray &u : untagged) {
        if (!u.startsWith("SEARCH"))
            continue;
        const QByteArray rest = u.mid(QByteArray("SEARCH").size()).simplified();
        for (const QByteArray &tok : rest.split(' ')) {
            bool          ok = false;
            const quint32 v  = tok.toUInt(&ok);
            if (ok)
                out.append(v);
        }
    }
    return out;
}

namespace {
// Extract the integer after a "[KEY " marker, e.g. "[UIDVALIDITY 12345]".
quint32 bracketUint(const QByteArray &line, const QByteArray &key) {
    const int k = line.indexOf("[" + key + " ");
    if (k < 0)
        return 0;
    const int start = k + 1 + key.size() + 1;
    const int end   = line.indexOf(']', start);
    if (end < 0)
        return 0;
    return line.mid(start, end - start).trimmed().toUInt();
}
} // namespace

SelectResult parseSelect(const QList<QByteArray> &untagged, const QByteArray &status) {
    SelectResult r;
    for (const QByteArray &u : untagged) {
        if (u.endsWith("EXISTS")) {
            r.exists = u.left(u.indexOf(' ')).toInt();
        } else if (u.endsWith("RECENT")) {
            r.recent = u.left(u.indexOf(' ')).toInt();
        } else if (u.startsWith("FLAGS (")) {
            const int op = u.indexOf('('), cp = u.indexOf(')');
            if (op >= 0 && cp > op)
                for (const QByteArray &f : u.mid(op + 1, cp - op - 1).split(' '))
                    if (!f.trimmed().isEmpty())
                        r.flags << QString::fromUtf8(f.trimmed());
        } else if (u.contains("[UIDVALIDITY ")) {
            r.uidValidity = bracketUint(u, "UIDVALIDITY");
        } else if (u.contains("[UIDNEXT ")) {
            r.uidNext = bracketUint(u, "UIDNEXT");
        }
    }
    if (status.contains("[READ-ONLY]"))
        r.readWrite = false;
    return r;
}

} // namespace Proto
} // namespace imap

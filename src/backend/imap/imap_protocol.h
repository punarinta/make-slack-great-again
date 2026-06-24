// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Pure IMAP4rev1 protocol helpers — framing + light response parsing, with no
// socket/QObject/network dependency, so they unit-test in isolation (the
// transport lives in imap_client.{h,cpp}). Mirrors the net::HttpQueue (mechanics)
// vs slack::WebApiClient (semantics) split. The heavy S-expression parsing of
// ENVELOPE/BODYSTRUCTURE/THREAD is a later increment (imap_mappers); this layer
// covers framing + the simple line responses (CAPABILITY/LIST/SELECT/SEARCH).
#pragma once

#include <QByteArray>
#include <QChar>
#include <QList>
#include <QString>
#include <QStringList>

namespace imap {

// One mailbox from a LIST response. `flags` carries RFC 6154 special-use markers
// (\Sent \Trash \Junk \Drafts \Archive \All) the Model-D classifier relies on.
struct Mailbox {
    QString     name;
    QChar       delimiter = QLatin1Char('/');
    QStringList flags;
    bool        selectable = true; // false when \Noselect

    bool hasFlag(const QString &f) const {
        for (const auto &x : flags)
            if (x.compare(f, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    }
    bool operator==(const Mailbox &) const = default;
};

// SELECT/EXAMINE result — the cursors the backend needs (EXISTS for counts,
// UIDVALIDITY/UIDNEXT for stable paging).
struct SelectResult {
    int         exists      = 0;
    int         recent      = 0;
    quint32     uidValidity = 0;
    quint32     uidNext     = 0;
    QStringList flags;
    bool        readWrite                              = true;
    bool        operator==(const SelectResult &) const = default;
};

// A completed command's outcome: tagged OK vs NO/BAD, the trailing status text,
// and the raw untagged `* …` lines collected while it ran (literals inlined).
struct Response {
    bool              ok = false;
    QByteArray        status; // text after "OK "/"NO "/"BAD "
    QList<QByteArray> untagged;
};

// Pulls complete *logical* IMAP response lines from a byte stream, transparently
// absorbing `{n}` literals (which may themselves contain CRLFs) so each returned
// line is a complete unit. Feed bytes with append(); drain with nextLine().
class ResponseFramer {
public:
    void append(const QByteArray &data) { _buf += data; }
    bool nextLine(QByteArray &out);
    void clear() { _buf.clear(); }
    bool empty() const { return _buf.isEmpty(); }

private:
    QByteArray _buf;
};

namespace Proto {

// Quote an IMAP astring (mailbox name / LOGIN arg), escaping " and \.
QByteArray quote(const QByteArray &s);

// Light response parsers over the untagged lines (and status, for SELECT).
QList<Mailbox> parseList(const QList<QByteArray> &untagged);
QList<quint32> parseSearch(const QList<QByteArray> &untagged);
SelectResult   parseSelect(const QList<QByteArray> &untagged, const QByteArray &status);

// Tokenize an IMAP line into atoms / quoted-strings (quotes respected, escapes
// unwrapped, parenthesised groups kept whole). Exposed for tests + reuse.
QList<QByteArray> tokenize(const QByteArray &line);

} // namespace Proto
} // namespace imap

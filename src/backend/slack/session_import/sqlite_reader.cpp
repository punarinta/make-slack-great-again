// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Format reference: https://www.sqlite.org/fileformat2.html (sections cited below).
#include "backend/slack/session_import/sqlite_reader.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <cstring>
#include <optional>

namespace slack::session {
namespace {

using Error = SqliteTable::Error;

constexpr qint64 kMaxFileSize = 512LL * 1024 * 1024; // a cookie DB is KBs to a few MB
constexpr int    kMaxDepth    = 64;                  // real b-trees are a handful deep

// The file bytes plus a sticky "corrupt" flag, like QDataStream's status: any
// out-of-bounds or inconsistent read returns 0 and marks the file bad, loops stop
// as soon as it is, and the caller checks ok() once at the end.
class Db {
public:
    explicit Db(QByteArray bytes) : _b(std::move(bytes)) {}

    bool ok() const { return _ok; }

    // §1.3 database header. Returns false when this is not an SQLite 3 file.
    bool readHeader() {
        static constexpr char kMagic[] = "SQLite format 3";
        if (_b.size() < 100 || std::memcmp(_b.constData(), kMagic, sizeof kMagic) != 0)
            return false;
        const quint32 ps = u16(16);
        _pageSize        = ps == 1 ? 65536 : ps;
        _usable          = _pageSize - u8(20); // minus the per-page reserved bytes
        _encoding        = u32(56);            // 0 only in a brand-new empty file: UTF-8
        _wal             = u8(18) == 2 || u8(19) == 2;
        if (_pageSize < 512 || _pageSize > 65536 || (_pageSize & (_pageSize - 1)) != 0 ||
            _usable < 480 || _encoding > 3)
            _ok = false;
        else
            _pageCount = quint32(_b.size() / _pageSize);
        return true;
    }

    bool isWal() const { return _wal; }

    // Calls fn(rowid, payload) for every row of the table b-tree rooted at `root`.
    template <typename Fn>
    void walkTable(quint32 root, Fn &&fn) {
        QSet<quint32> seen;
        walk(root, 0, seen, fn);
    }

    // §2.1 record format → one value per column.
    QVariantList decodeRecord(const QByteArray &rec) {
        int          pos = 0;
        const qint64 hdr = varint(rec, pos);
        if (hdr < pos || hdr > rec.size())
            return fail<QVariantList>();
        QList<qint64> types;
        while (_ok && pos < hdr)
            types.append(varint(rec, pos));
        if (pos != hdr)
            return fail<QVariantList>();
        QVariantList out;
        qint64       body = hdr;
        for (const qint64 t : types) {
            const qint64 len = serialLength(t);
            if (!_ok || len > rec.size() - body)
                return fail<QVariantList>();
            out.append(decodeValue(t, rec.constData() + body, int(len)));
            body += len;
        }
        return out;
    }

private:
    template <typename T = quint32>
    T fail() {
        _ok = false;
        return T{};
    }

    quint8 u8(qint64 off) { return off >= 0 && off < _b.size() ? quint8(_b[off]) : fail<quint8>(); }
    quint32 u16(qint64 off) { return (quint32(u8(off)) << 8) | u8(off + 1); }
    quint32 u32(qint64 off) { return (u16(off) << 16) | u16(off + 2); }

    // §1.6 varint: big-endian 7-bit groups, up to 9 bytes, the 9th contributing all 8.
    template <typename Byte>
    qint64 readVarint(Byte &&next) {
        quint64 v = 0;
        for (int i = 0; i < 8; ++i) {
            const quint8 c = next();
            v              = (v << 7) | (c & 0x7f);
            if (!(c & 0x80))
                return qint64(v);
        }
        return qint64((v << 8) | next());
    }
    qint64 varint(qint64 &off) {
        return readVarint([&] { return u8(off++); });
    }
    qint64 varint(const QByteArray &buf, int &pos) {
        return readVarint([&] { return pos < buf.size() ? quint8(buf[pos++]) : fail<quint8>(); });
    }

    // §1.6 b-tree pages: 0x05 = interior table page, 0x0d = leaf table page. Page 1
    // carries the 100-byte database header before its b-tree header.
    template <typename Fn>
    void walk(quint32 page, int depth, QSet<quint32> &seen, Fn &fn) {
        // A cycle, an absurd depth or a page past the end only happens in a damaged file.
        if (!_ok || depth > kMaxDepth || seen.contains(page) || page < 1 || page > _pageCount) {
            _ok = false;
            return;
        }
        seen.insert(page);
        const qint64  base  = qint64(page - 1) * _pageSize;
        const qint64  hdr   = base + (page == 1 ? 100 : 0);
        const quint8  type  = u8(hdr);
        const quint32 n     = u16(hdr + 3);
        const bool    inner = type == 0x05;
        if (!inner && type != 0x0d) {
            _ok = false;
            return;
        }
        const qint64 ptrs = hdr + (inner ? 12 : 8);
        for (quint32 i = 0; _ok && i < n; ++i) {
            const qint64 cell = base + u16(ptrs + 2 * i);
            if (cell < ptrs || cell >= base + _usable) {
                _ok = false;
                return;
            }
            if (inner) {
                walk(u32(cell), depth + 1, seen, fn); // left child; the rowid key isn't needed
            } else {
                qint64           off   = cell;
                const qint64     size  = varint(off);
                const qint64     rowid = varint(off);
                const QByteArray rec   = payload(off, size);
                if (_ok)
                    fn(rowid, rec);
            }
        }
        if (inner)
            walk(u32(hdr + 8), depth + 1, seen, fn); // right-most child
    }

    // §1.6 table leaf cell payload: up to X bytes inline, the rest on a chain of
    // overflow pages (4-byte next-page number, then usable-4 bytes of content).
    QByteArray payload(qint64 off, qint64 size) {
        if (!_ok || size < 0 || size > _b.size()) // can't be bigger than the whole file
            return fail<QByteArray>();
        const qint64 u     = _usable;
        const qint64 x     = u - 35;
        qint64       local = size;
        if (size > x) {
            const qint64 m = ((u - 12) * 32 / 255) - 23;
            const qint64 k = m + ((size - m) % (u - 4));
            local          = k <= x ? k : m;
        }
        if (off + local > _b.size())
            return fail<QByteArray>();
        QByteArray out = _b.mid(off, local);
        if (local == size)
            return out;
        quint32 next = u32(off + local);
        quint32 hops = 0;
        while (_ok && out.size() < size) {
            if (next < 1 || next > _pageCount || ++hops > _pageCount)
                return fail<QByteArray>();
            const qint64 p     = qint64(next - 1) * _pageSize;
            const qint64 chunk = qMin<qint64>(u - 4, size - out.size());
            if (p + 4 + chunk > _b.size())
                return fail<QByteArray>();
            next = u32(p);
            out.append(_b.constData() + p + 4, chunk);
        }
        return out;
    }

    // §2.1 serial type → content length in bytes (10 and 11 are reserved).
    qint64 serialLength(qint64 t) {
        static constexpr int kFixed[] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0};
        if (t >= 0 && t <= 9)
            return kFixed[t];
        if (t >= 12)
            return (t - (t & 1 ? 13 : 12)) / 2;
        return fail<qint64>();
    }

    QVariant decodeValue(qint64 t, const char *p, int len) const {
        switch (t) {
        case 0:
            return {};
        case 8:
            return qint64(0);
        case 9:
            return qint64(1);
        case 7: {
            quint64 bits = 0;
            for (int i = 0; i < 8; ++i)
                bits = (bits << 8) | quint8(p[i]);
            double d;
            std::memcpy(&d, &bits, sizeof d);
            return d;
        }
        default:
            break;
        }
        if (t <= 6) { // big-endian two's-complement integer of 1..8 bytes
            quint64 v = quint8(p[0]) & 0x80 ? ~quint64(0) : 0;
            for (int i = 0; i < len; ++i)
                v = (v << 8) | quint8(p[i]);
            return qint64(v);
        }
        if (t & 1) { // TEXT, in the database's encoding (1 UTF-8, 2 UTF-16le, 3 UTF-16be)
            if (_encoding <= 1)
                return QString::fromUtf8(p, len);
            QByteArray units(p, len & ~1);
            if (_encoding == 3)
                for (int i = 0; i + 1 < units.size(); i += 2)
                    std::swap(units[i], units[i + 1]);
            return QString::fromUtf16(
                reinterpret_cast<const char16_t *>(units.constData()), units.size() / 2
            );
        }
        return QByteArray(p, len); // BLOB
    }

    QByteArray _b;
    quint32    _pageSize  = 0;
    quint32    _usable    = 0;
    quint32    _encoding  = 1;
    quint32    _pageCount = 0;
    bool       _wal       = false;
    bool       _ok        = true;
};

// A writer is mid-transaction: a rollback journal that still starts with its magic
// (Chromium truncates it to zero bytes after each commit), or a WAL file holding
// frames not yet copied back into the database.
bool writerActive(const QString &path, bool wal) {
    if (wal)
        return QFileInfo(path + QStringLiteral("-wal")).size() > 0;
    QFile j(path + QStringLiteral("-journal"));
    if (!j.open(QIODevice::ReadOnly))
        return false;
    static constexpr unsigned char kJournalMagic[] = {
        0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7
    };
    const QByteArray head = j.read(sizeof kJournalMagic);
    return head.size() == int(sizeof kJournalMagic) &&
           std::memcmp(head.constData(), kJournalMagic, sizeof kJournalMagic) == 0;
}

// Splits a CREATE TABLE body at top-level commas, skipping quoted text and
// parenthesised sub-expressions (DEFAULT 'a,b', CHECK(x IN (1,2)), ...).
QStringList splitDefinitions(const QString &body) {
    QStringList out;
    QString     cur;
    int         depth = 0;
    QChar       close; // non-null while inside a quoted run
    for (const QChar c : body) {
        cur += c;
        if (!close.isNull()) {
            if (c == close)
                close = QChar();
        } else if (c == u'\'' || c == u'"' || c == u'`') {
            close = c;
        } else if (c == u'[') {
            close = u']';
        } else if (c == u'(') {
            ++depth;
        } else if (c == u')') {
            --depth;
        } else if (c == u',' && depth == 0) {
            cur.chop(1);
            out.append(cur.trimmed());
            cur.clear();
        }
    }
    if (!cur.trimmed().isEmpty())
        out.append(cur.trimmed());
    return out;
}

// The leading identifier of a column definition, unquoted.
QString firstIdentifier(const QString &def) {
    if (def.isEmpty())
        return {};
    const QChar open = def[0];
    if (open == u'"' || open == u'`' || open == u'[') {
        const int end = def.indexOf(open == u'[' ? QChar(u']') : open, 1);
        return end < 0 ? QString() : def.mid(1, end - 1);
    }
    int end = 0;
    while (end < def.size() && !def[end].isSpace() && def[end] != u'(')
        ++end;
    return def.left(end);
}

struct Schema {
    QStringList columns;
    int  rowidAlias   = -1; // an INTEGER PRIMARY KEY column stores NULL: its value is the rowid
    bool withoutRowid = false;
};

std::optional<Schema> parseCreateTable(const QString &sql) {
    const int open  = sql.indexOf(u'(');
    const int close = sql.lastIndexOf(u')');
    if (open < 0 || close <= open)
        return std::nullopt;
    Schema s;
    s.withoutRowid = sql.mid(close + 1).simplified().contains(
        QStringLiteral("WITHOUT ROWID"), Qt::CaseInsensitive
    );
    static const QStringList kTableConstraints = {
        QStringLiteral("CONSTRAINT"),
        QStringLiteral("PRIMARY"),
        QStringLiteral("UNIQUE"),
        QStringLiteral("CHECK"),
        QStringLiteral("FOREIGN"),
    };
    for (const QString &def : splitDefinitions(sql.mid(open + 1, close - open - 1))) {
        const QString name   = firstIdentifier(def);
        const bool    quoted = def.startsWith(u'"') || def.startsWith(u'`') || def.startsWith(u'[');
        if (name.isEmpty())
            return std::nullopt;
        if (!quoted && kTableConstraints.contains(name, Qt::CaseInsensitive))
            continue;
        const QString rest = def.mid(quoted ? name.size() + 2 : name.size()).simplified();
        if (rest.startsWith(QStringLiteral("INTEGER PRIMARY KEY"), Qt::CaseInsensitive) &&
            !rest.contains(QStringLiteral("DESC"), Qt::CaseInsensitive))
            s.rowidAlias = int(s.columns.size());
        s.columns.append(name);
    }
    return s;
}

} // namespace

SqliteTable readSqliteTable(const QString &path, const QString &table) {
    SqliteTable result;
    QFile       f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxFileSize) {
        result.error = Error::Open;
        return result;
    }
    Db db(f.readAll());
    f.close();
    if (!db.readHeader()) {
        result.error = Error::NotSqlite;
        return result;
    }
    if (!db.ok()) {
        result.error = Error::Corrupt;
        return result;
    }
    if (writerActive(path, db.isWal())) {
        result.error = Error::Busy;
        return result;
    }
    // §2.6 the schema table is rooted at page 1: (type, name, tbl_name, rootpage, sql).
    quint32 root = 0;
    QString sql;
    db.walkTable(1, [&](qint64, const QByteArray &rec) {
        const QVariantList v = db.decodeRecord(rec);
        if (root == 0 && v.size() >= 5 && v[0].toString() == QLatin1String("table") &&
            v[1].toString().compare(table, Qt::CaseInsensitive) == 0) {
            root = quint32(v[3].toLongLong());
            sql  = v[4].toString();
        }
    });
    if (!db.ok()) {
        result.error = Error::Corrupt;
        return result;
    }
    if (root == 0) { // also a virtual table, which has no b-tree of its own
        result.error = Error::NoTable;
        return result;
    }
    const std::optional<Schema> schema = parseCreateTable(sql);
    if (!schema) {
        result.error = Error::Corrupt;
        return result;
    }
    if (schema->withoutRowid) {
        result.error = Error::Unsupported;
        return result;
    }
    const int cols = int(schema->columns.size());
    db.walkTable(root, [&](qint64 rowid, const QByteArray &rec) {
        QVariantList v = db.decodeRecord(rec);
        v.resize(cols);
        if (schema->rowidAlias >= 0 && !v[schema->rowidAlias].isValid())
            v[schema->rowidAlias] = rowid;
        result.rows.append(std::move(v));
    });
    if (!db.ok()) {
        result.rows.clear();
        result.error = Error::Corrupt;
        return result;
    }
    result.columns = schema->columns;
    return result;
}

} // namespace slack::session

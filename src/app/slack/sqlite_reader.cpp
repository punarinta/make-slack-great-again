// Read-only SQLite 3 file reader (see sqlite_reader.h).
//
// Format reference: https://www.sqlite.org/fileformat2.html (sections cited
// below).
#include "app/slack/sqlite_reader.h"

#include "base/file.h"
#include "base/str.h"
#include "base/utf8.h"

#include <cstring>
#include <optional>

namespace slack {

namespace {

using Error = SqliteTable::Error;

constexpr int64_t kMaxFileSize = 512LL * 1024 * 1024; // a cookie DB is KBs to a few MB
constexpr int     kMaxDepth    = 64;                  // real b-trees are a handful deep

// The file bytes plus a sticky "corrupt" flag: any out-of-bounds or
// inconsistent read returns 0 and marks the file bad, loops stop as soon as
// it is, and the caller checks ok() once at the end.
class Db {
public:
    explicit Db(std::string bytes) : _b(std::move(bytes)) {}

    bool ok() const { return _ok; }

    // §1.3 database header. False when this is not an SQLite 3 file.
    bool readHeader() {
        static constexpr char kMagic[] = "SQLite format 3";
        if (_b.size() < 100 || std::memcmp(_b.data(), kMagic, sizeof kMagic) != 0)
            return false;
        const uint32_t ps = u16(16);
        _pageSize         = ps == 1 ? 65536 : ps;
        _usable           = _pageSize - u8(20); // minus the per-page reserved bytes
        _encoding         = u32(56);            // 0 only in a brand-new empty file: UTF-8
        _wal              = u8(18) == 2 || u8(19) == 2;
        if (_pageSize < 512 || _pageSize > 65536 || (_pageSize & (_pageSize - 1)) != 0 ||
            _usable < 480 || _encoding > 3)
            _ok = false;
        else
            _pageCount = uint32_t(int64_t(_b.size()) / _pageSize);
        return true;
    }

    bool isWal() const { return _wal; }

    // Calls fn(rowid, payload) for every row of the table b-tree at `root`.
    template <typename Fn>
    void walkTable(uint32_t root, Fn &&fn) {
        std::vector<uint32_t> seen;
        walk(root, 0, seen, fn);
    }

    // §2.1 record format → one value per column.
    std::vector<Cell> decodeRecord(const std::string &rec) {
        int           pos = 0;
        const int64_t hdr = varint(rec, pos);
        if (hdr < pos || hdr > int64_t(rec.size()))
            return fail<std::vector<Cell>>();
        std::vector<int64_t> types;
        while (_ok && pos < hdr)
            types.push_back(varint(rec, pos));
        if (pos != hdr)
            return fail<std::vector<Cell>>();
        std::vector<Cell> out;
        int64_t           body = hdr;
        for (const int64_t t : types) {
            const int64_t len = serialLength(t);
            if (!_ok || len > int64_t(rec.size()) - body)
                return fail<std::vector<Cell>>();
            out.push_back(decodeValue(t, rec.data() + body, int(len)));
            body += len;
        }
        return out;
    }

private:
    template <typename T = uint32_t>
    T fail() {
        _ok = false;
        return T{};
    }

    uint8_t u8(int64_t off) {
        return off >= 0 && off < int64_t(_b.size()) ? uint8_t(_b[off]) : fail<uint8_t>();
    }
    uint32_t u16(int64_t off) { return (uint32_t(u8(off)) << 8) | u8(off + 1); }
    uint32_t u32(int64_t off) { return (u16(off) << 16) | u16(off + 2); }

    // §1.6 varint: big-endian 7-bit groups, up to 9 bytes, the 9th all 8 bits.
    template <typename Byte>
    int64_t readVarint(Byte &&next) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            const uint8_t c = next();
            v               = (v << 7) | (c & 0x7f);
            if (!(c & 0x80))
                return int64_t(v);
        }
        return int64_t((v << 8) | next());
    }
    int64_t varint(int64_t &off) {
        return readVarint([&] { return u8(off++); });
    }
    int64_t varint(const std::string &buf, int &pos) {
        return readVarint([&] {
            return pos < int(buf.size()) ? uint8_t(buf[pos++]) : fail<uint8_t>();
        });
    }

    // §1.6 b-tree pages: 0x05 interior table, 0x0d leaf table. Page 1 carries
    // the 100-byte database header before its b-tree header.
    template <typename Fn>
    void walk(uint32_t page, int depth, std::vector<uint32_t> &seen, Fn &fn) {
        // A cycle, an absurd depth or a page past the end: only a damaged file.
        bool cycle = false;
        for (uint32_t p : seen)
            cycle = cycle || p == page;
        if (!_ok || depth > kMaxDepth || cycle || page < 1 || page > _pageCount) {
            _ok = false;
            return;
        }
        seen.push_back(page);
        const int64_t  base  = int64_t(page - 1) * _pageSize;
        const int64_t  hdr   = base + (page == 1 ? 100 : 0);
        const uint8_t  type  = u8(hdr);
        const uint32_t n     = u16(hdr + 3);
        const bool     inner = type == 0x05;
        if (!inner && type != 0x0d) {
            _ok = false;
            return;
        }
        const int64_t ptrs = hdr + (inner ? 12 : 8);
        for (uint32_t i = 0; _ok && i < n; ++i) {
            const int64_t cell = base + u16(ptrs + 2 * i);
            if (cell < ptrs || cell >= base + _usable) {
                _ok = false;
                return;
            }
            if (inner) {
                walk(u32(cell), depth + 1, seen, fn); // left child; the rowid key isn't needed
            } else {
                int64_t           off   = cell;
                const int64_t     size  = varint(off);
                const int64_t     rowid = varint(off);
                const std::string rec   = payload(off, size);
                if (_ok)
                    fn(rowid, rec);
            }
        }
        if (inner)
            walk(u32(hdr + 8), depth + 1, seen, fn); // right-most child
    }

    // §1.6 table leaf cell payload: up to X bytes inline, the rest on a chain
    // of overflow pages (4-byte next-page number, then usable-4 of content).
    std::string payload(int64_t off, int64_t size) {
        if (!_ok || size < 0 || size > int64_t(_b.size())) // can't exceed the whole file
            return fail<std::string>();
        const int64_t u     = _usable;
        const int64_t x     = u - 35;
        int64_t       local = size;
        if (size > x) {
            const int64_t m = ((u - 12) * 32 / 255) - 23;
            const int64_t k = m + ((size - m) % (u - 4));
            local           = k <= x ? k : m;
        }
        if (off + local > int64_t(_b.size()))
            return fail<std::string>();
        std::string out = _b.substr(off, local);
        if (local == size)
            return out;
        uint32_t next = u32(off + local);
        uint32_t hops = 0;
        while (_ok && int64_t(out.size()) < size) {
            if (next < 1 || next > _pageCount || ++hops > _pageCount)
                return fail<std::string>();
            const int64_t p     = int64_t(next - 1) * _pageSize;
            const int64_t chunk = std::min<int64_t>(u - 4, size - int64_t(out.size()));
            if (p + 4 + chunk > int64_t(_b.size()))
                return fail<std::string>();
            next = u32(p);
            out.append(_b.data() + p + 4, chunk);
        }
        return out;
    }

    // §2.1 serial type → content length in bytes (10 and 11 are reserved).
    int64_t serialLength(int64_t t) {
        static constexpr int kFixed[] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0};
        if (t >= 0 && t <= 9)
            return kFixed[t];
        if (t >= 12)
            return (t - (t & 1 ? 13 : 12)) / 2;
        return fail<int64_t>();
    }

    Cell decodeValue(int64_t t, const char *p, int len) const {
        Cell cell;
        switch (t) {
        case 0:
            return cell; // NULL
        case 8:
            cell.v = int64_t(0);
            return cell;
        case 9:
            cell.v = int64_t(1);
            return cell;
        case 7: {
            uint64_t bits = 0;
            for (int i = 0; i < 8; ++i)
                bits = (bits << 8) | uint8_t(p[i]);
            double d;
            std::memcpy(&d, &bits, sizeof d);
            cell.v = d;
            return cell;
        }
        default:
            break;
        }
        if (t <= 6) { // big-endian two's-complement integer of 1..8 bytes
            uint64_t v = uint8_t(p[0]) & 0x80 ? ~uint64_t(0) : 0;
            for (int i = 0; i < len; ++i)
                v = (v << 8) | uint8_t(p[i]);
            cell.v = int64_t(v);
            return cell;
        }
        if (t & 1) { // TEXT, in the database encoding (1 UTF-8, 2 UTF-16le, 3 UTF-16be)
            if (_encoding <= 1) {
                cell.v = std::string(p, len);
            } else {
                // The cookie store is UTF-8; transcode UTF-16 roughly (host
                // keys and cookie names are ASCII) for completeness.
                std::string s;
                for (int i = 0; i + 1 < len; i += 2) {
                    const uint8_t  a = uint8_t(p[i]), b = uint8_t(p[i + 1]);
                    const uint16_t u16v =
                        _encoding == 3 ? (uint16_t(a) << 8 | b) : (uint16_t(b) << 8 | a);
                    utf8::append(s, u16v);
                }
                cell.v = std::move(s);
            }
            return cell;
        }
        cell.v      = std::string(p, len); // BLOB
        cell.isBlob = true;
        return cell;
    }

    std::string _b;
    uint32_t    _pageSize  = 0;
    uint32_t    _usable    = 0;
    uint32_t    _encoding  = 1;
    uint32_t    _pageCount = 0;
    bool        _wal       = false;
    bool        _ok        = true;
};

// A writer is mid-transaction: a rollback journal that still starts with its
// magic (Chromium truncates it to zero bytes after each commit), or a WAL
// file holding frames not yet copied back.
bool writerActive(const std::string &path, bool wal) {
    if (wal)
        return file::size(path + "-wal") > 0;
    std::string head;
    if (!file::readAll(path + "-journal", &head))
        return false;
    static constexpr unsigned char kJournalMagic[] = {
        0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7
    };
    return head.size() >= sizeof kJournalMagic &&
           std::memcmp(head.data(), kJournalMagic, sizeof kJournalMagic) == 0;
}

// Splits a CREATE TABLE body at top-level commas, skipping quoted text and
// parenthesised sub-expressions (DEFAULT 'a,b', CHECK(x IN (1,2)), …).
std::vector<std::string> splitDefinitions(std::string_view body) {
    std::vector<std::string> out;
    std::string              cur;
    int                      depth = 0;
    char                     close = 0; // non-zero while inside a quoted run
    for (const char c : body) {
        cur += c;
        if (close) {
            if (c == close)
                close = 0;
        } else if (c == '\'' || c == '"' || c == '`') {
            close = c;
        } else if (c == '[') {
            close = ']';
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        } else if (c == ',' && depth == 0) {
            cur.pop_back();
            out.push_back(std::string(str::trim(cur)));
            cur.clear();
        }
    }
    if (!str::trim(cur).empty())
        out.push_back(std::string(str::trim(cur)));
    return out;
}

// The leading identifier of a column definition, unquoted.
std::string firstIdentifier(const std::string &def) {
    if (def.empty())
        return {};
    const char open = def[0];
    if (open == '"' || open == '`' || open == '[') {
        const char   want = open == '[' ? ']' : open;
        const size_t end  = def.find(want, 1);
        return end == std::string::npos ? std::string() : def.substr(1, end - 1);
    }
    size_t end = 0;
    while (end < def.size() && def[end] != ' ' && def[end] != '\t' && def[end] != '\n' &&
           def[end] != '\r' && def[end] != '(')
        ++end;
    return def.substr(0, end);
}

struct Schema {
    std::vector<std::string> columns;
    bool                     withoutRowid = false;
    int                      rowidAlias = -1; // INTEGER PRIMARY KEY column stores NULL = the rowid
};

std::optional<Schema> parseCreateTable(const std::string &sql) {
    const size_t open  = sql.find('(');
    const size_t close = sql.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open)
        return std::nullopt;
    Schema      s;
    std::string tail = str::asciiLower(std::string(str::trim(sql.substr(close + 1))));
    s.withoutRowid   = tail.find("without rowid") != std::string::npos;
    static const char *const kTableConstraints[] = {
        "constraint", "primary", "unique", "check", "foreign"
    };
    for (const std::string &def : splitDefinitions(sql.substr(open + 1, close - open - 1))) {
        const std::string name = firstIdentifier(def);
        if (name.empty())
            return std::nullopt;
        const bool quoted = def[0] == '"' || def[0] == '`' || def[0] == '[';
        if (!quoted) {
            const std::string lname        = str::asciiLower(name);
            bool              isConstraint = false;
            for (const char *k : kTableConstraints)
                isConstraint = isConstraint || lname == k;
            if (isConstraint)
                continue;
        }
        std::string rest = str::asciiLower(
            std::string(str::trim(def.substr(quoted ? name.size() + 2 : name.size())))
        );
        if (str::startsWith(rest, "integer primary key") && rest.find("desc") == std::string::npos)
            s.rowidAlias = int(s.columns.size());
        s.columns.push_back(name);
    }
    return s;
}

} // namespace

std::string Cell::text() const {
    return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string();
}

int SqliteTable::columnIndex(std::string_view name) const {
    for (size_t i = 0; i < columns.size(); ++i)
        if (str::asciiLower(columns[i]) == str::asciiLower(name))
            return int(i);
    return -1;
}

SqliteTable readSqliteTable(const std::string &path, const std::string &table) {
    SqliteTable result;
    if (file::size(path) > kMaxFileSize) {
        result.error = Error::Open;
        return result;
    }
    std::string bytes;
    if (!file::readAll(path, &bytes)) {
        result.error = Error::Open;
        return result;
    }
    Db db(std::move(bytes));
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
    uint32_t    root = 0;
    std::string sql;
    db.walkTable(1, [&](int64_t, const std::string &rec) {
        const std::vector<Cell> v = db.decodeRecord(rec);
        if (root == 0 && v.size() >= 5 && v[0].text() == "table" &&
            str::asciiLower(v[1].text()) == str::asciiLower(table)) {
            root = uint32_t(v[3].integer());
            sql  = v[4].text();
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
    db.walkTable(root, [&](int64_t rowid, const std::string &rec) {
        std::vector<Cell> v = db.decodeRecord(rec);
        v.resize(cols);
        if (schema->rowidAlias >= 0 && v[schema->rowidAlias].isNull())
            v[schema->rowidAlias].v = rowid;
        result.rows.push_back(std::move(v));
    });
    if (!db.ok()) {
        result.rows.clear();
        result.error = Error::Corrupt;
        return result;
    }
    result.columns = schema->columns;
    return result;
}

} // namespace slack

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Tests for slack::session::readSqliteTable — the read-only SQLite file reader the
// Linux session import uses for Chromium's cookie DB instead of linking SQLite.
// Every database here is written by the REAL SQLite (Qt's QSQLITE driver, linked
// into the test binary only), so the reader is checked against the actual format,
// not against its own understanding of it:
//   - Chromium's cookies schema: column order from CREATE TABLE, value types
//   - Multi-level b-trees (interior pages), overflow chains, page sizes 512..64K
//   - Every integer width, REAL, NULL, UTF-16le/be text
//   - Schema quirks: quoted names, table constraints, ALTER TABLE ADD COLUMN,
//     INTEGER PRIMARY KEY rowid alias, WITHOUT ROWID
//   - A writer mid-transaction (hot journal, pending WAL) reads as Busy
//   - Truncated / bit-flipped files never crash
#include <catch2/catch_test_macros.hpp>

#include "backend/slack/session_import/sqlite_reader.h"
#include "test_main.h"

#include <QCoreApplication>
#include <QFile>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <limits>

using slack::session::readSqliteTable;
using slack::session::SqliteTable;
using Error = SqliteTable::Error;

namespace {

// A throwaway database file written through the real SQLite. The connection
// stays open until close() / destruction, which the WAL test relies on.
class RealDb {
public:
    explicit RealDb(const QString &path) : _conn(QUuid::createUuid().toString()), _path(path) {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), _conn);
        db.setDatabaseName(path);
        REQUIRE(db.open());
    }
    ~RealDb() { close(); }

    void exec(const QString &sql) {
        QSqlQuery q(QSqlDatabase::database(_conn));
        INFO(sql.toStdString());
        REQUIRE(q.exec(sql));
    }
    // Prepared insert, so blobs and exact integer types go in as bound values.
    void insert(const QString &sql, const QVariantList &values) {
        QSqlQuery q(QSqlDatabase::database(_conn));
        REQUIRE(q.prepare(sql));
        for (const QVariant &v : values)
            q.addBindValue(v);
        INFO(q.lastError().text().toStdString());
        REQUIRE(q.exec());
    }
    void close() {
        if (_conn.isEmpty())
            return;
        QSqlDatabase::database(_conn).close();
        QSqlDatabase::removeDatabase(_conn);
        _conn.clear();
    }
    QString path() const { return _path; }

private:
    QString _conn;
    QString _path;
};

QByteArray pseudoRandomBytes(int n, quint32 seed) {
    QRandomGenerator rng(seed);
    QByteArray       b(n, Qt::Uninitialized);
    for (char &c : b)
        c = char(rng.bounded(256));
    return b;
}

// Chromium's cookies table as of Chrome 1xx (column order matters: the reader
// must take it from CREATE TABLE, not assume one).
constexpr auto kCookiesSchema =
    "CREATE TABLE cookies(creation_utc INTEGER NOT NULL,host_key TEXT NOT NULL,"
    "top_frame_site_key TEXT NOT NULL,name TEXT NOT NULL,value TEXT NOT NULL,"
    "encrypted_value BLOB NOT NULL,path TEXT NOT NULL,expires_utc INTEGER NOT NULL,"
    "is_secure INTEGER NOT NULL,is_httponly INTEGER NOT NULL,last_access_utc INTEGER NOT NULL,"
    "has_expires INTEGER NOT NULL,is_persistent INTEGER NOT NULL,priority INTEGER NOT NULL,"
    "samesite INTEGER NOT NULL,source_scheme INTEGER NOT NULL,source_port INTEGER NOT NULL,"
    "last_update_utc INTEGER NOT NULL,source_type INTEGER NOT NULL,"
    "has_cross_site_ancestor INTEGER NOT NULL,"
    "UNIQUE (host_key, top_frame_site_key, has_cross_site_ancestor, name, path, "
    "source_scheme, source_port))";

void insertCookie(RealDb &db, const QString &host, const QString &name, const QByteArray &enc) {
    db.insert(
        QStringLiteral("INSERT INTO cookies VALUES(?,?,'',?,'',?,'/',0,1,1,0,1,1,1,0,2,443,0,0,0)"),
        {qint64(13370000000000000), host, name, enc}
    );
}

} // namespace

MSGA_TEST_MAIN(argc, argv) {
    QCoreApplication app(argc, argv); // QSQLITE is a plugin: needs the app's library paths
    return msga_test::runCatch(argc, argv);
}

// ── Chromium cookie store ─────────────────────────────────────────────────────

TEST_CASE(
    "SqliteReader: reads Chromium's cookies table like the importer does", "[sqlite][cookies]"
) {
    QTemporaryDir dir;
    RealDb        db(dir.filePath("Cookies"));
    db.exec(QString::fromLatin1(kCookiesSchema));
    db.exec("CREATE TABLE meta(key LONGVARCHAR NOT NULL UNIQUE PRIMARY KEY, value LONGVARCHAR)");
    const QByteArray dEnc = QByteArray("v11") + pseudoRandomBytes(200, 1);
    insertCookie(db, ".example.com", "d", QByteArray("v11x"));
    insertCookie(db, ".slack.com", "b", QByteArray("v11y"));
    insertCookie(db, ".slack.com", "d", dEnc);
    db.close();

    const SqliteTable t = readSqliteTable(db.path(), QStringLiteral("cookies"));
    REQUIRE(t.error == Error::None);
    REQUIRE(t.columns.size() == 20);
    CHECK(t.columns.first() == "creation_utc");
    CHECK(
        t.columns.last() == "has_cross_site_ancestor"
    ); // the UNIQUE(...) constraint is not a column
    const qsizetype host = t.columns.indexOf("host_key"), name = t.columns.indexOf("name"),
                    enc = t.columns.indexOf("encrypted_value");
    REQUIRE(t.rows.size() == 3);
    CHECK(t.rows[2][host].toString() == ".slack.com");
    CHECK(t.rows[2][name].toString() == "d");
    CHECK(t.rows[2][enc].toByteArray() == dEnc);
    CHECK(t.rows[2][0].toLongLong() == 13370000000000000LL);
    CHECK(t.rows[2][t.columns.indexOf("source_port")].toLongLong() == 443);
}

TEST_CASE(
    "SqliteReader: table name matches case-insensitively; a missing one is NoTable", "[sqlite]"
) {
    QTemporaryDir dir;
    RealDb        db(dir.filePath("t.db"));
    db.exec("CREATE TABLE Cookies(a)");
    db.exec("INSERT INTO Cookies VALUES(1)");
    db.close();
    CHECK(readSqliteTable(db.path(), "cookies").rows.size() == 1);
    CHECK(readSqliteTable(db.path(), "nope").error == Error::NoTable);
}

// ── B-tree shapes ─────────────────────────────────────────────────────────────

TEST_CASE("SqliteReader: every row of a multi-level b-tree, in rowid order", "[sqlite][btree]") {
    for (const int pageSize : {512, 1024, 4096, 65536}) {
        INFO("page_size " << pageSize);
        QTemporaryDir dir;
        RealDb        db(dir.filePath("t.db"));
        db.exec(QStringLiteral("PRAGMA page_size=%1").arg(pageSize));
        db.exec("CREATE TABLE t(n INTEGER, s TEXT)");
        db.exec("BEGIN");
        const int rows =
            pageSize == 65536 ? 20000 : 3000; // deep enough for interior pages either way
        for (int i = 0; i < rows; ++i)
            db.insert(
                "INSERT INTO t VALUES(?,?)", {i, QStringLiteral("row %1 ").arg(i).repeated(3)}
            );
        db.exec("COMMIT");
        db.exec("DELETE FROM t WHERE n % 7 = 3"); // freeblocks + freelist pages must be skipped
        db.close();

        const SqliteTable t = readSqliteTable(db.path(), "t");
        REQUIRE(t.error == Error::None);
        int expect = 0, seen = 0;
        for (const QVariantList &row : t.rows) {
            while (expect % 7 == 3)
                ++expect;
            REQUIRE(row[0].toLongLong() == expect);
            REQUIRE(row[1].toString() == QStringLiteral("row %1 ").arg(expect).repeated(3));
            ++expect;
            ++seen;
        }
        CHECK(seen == rows - (rows + 3) / 7);
    }
}

TEST_CASE("SqliteReader: values spilling onto overflow page chains", "[sqlite][overflow]") {
    for (const int pageSize : {512, 4096}) {
        INFO("page_size " << pageSize);
        QTemporaryDir dir;
        RealDb        db(dir.filePath("t.db"));
        db.exec(QStringLiteral("PRAGMA page_size=%1").arg(pageSize));
        db.exec("CREATE TABLE t(a BLOB, b TEXT)");
        QList<QByteArray> blobs;
        // Around every local/overflow boundary for this page size, plus a long chain.
        for (const int n :
             {0,
              1,
              pageSize / 4,
              pageSize - 36,
              pageSize - 35,
              pageSize - 34,
              pageSize,
              pageSize * 3 + 17,
              200000})
            blobs.append(pseudoRandomBytes(n, quint32(n + pageSize)));
        for (const QByteArray &b : blobs)
            db.insert("INSERT INTO t VALUES(?,?)", {b, QString::fromLatin1(b.toHex().left(3000))});
        db.close();

        const SqliteTable t = readSqliteTable(db.path(), "t");
        REQUIRE(t.error == Error::None);
        REQUIRE(t.rows.size() == blobs.size());
        for (int i = 0; i < blobs.size(); ++i) {
            CHECK(t.rows[i][0].toByteArray() == blobs[i]);
            CHECK(t.rows[i][1].toString() == QString::fromLatin1(blobs[i].toHex().left(3000)));
        }
    }
}

// ── Values ────────────────────────────────────────────────────────────────────

TEST_CASE("SqliteReader: every integer width, REAL and NULL", "[sqlite][values]") {
    const QList<qint64> ints = {
        0,
        1,
        -1,
        2,
        127,
        128,
        -128,
        -129,
        32767,
        32768,
        -32768,
        (1LL << 23) - 1,
        -(1LL << 23),
        (1LL << 31) - 1,
        -(1LL << 31),
        (1LL << 47) - 1,
        -(1LL << 47),
        1LL << 47,
        std::numeric_limits<qint64>::max(),
        std::numeric_limits<qint64>::min(),
    };
    QTemporaryDir dir;
    RealDb        db(dir.filePath("t.db"));
    db.exec("CREATE TABLE t(v)");
    for (const qint64 v : ints)
        db.insert("INSERT INTO t VALUES(?)", {v});
    db.exec("INSERT INTO t VALUES(3.25)");
    db.exec("INSERT INTO t VALUES(-1e300)");
    db.exec("INSERT INTO t VALUES(NULL)");
    db.close();

    const SqliteTable t = readSqliteTable(db.path(), "t");
    REQUIRE(t.error == Error::None);
    REQUIRE(t.rows.size() == ints.size() + 3);
    for (int i = 0; i < ints.size(); ++i) {
        INFO(ints[i]);
        CHECK(t.rows[i][0].typeId() == QMetaType::LongLong);
        CHECK(t.rows[i][0].toLongLong() == ints[i]);
    }
    CHECK(t.rows[ints.size()][0].toDouble() == 3.25);
    CHECK(t.rows[ints.size() + 1][0].toDouble() == -1e300);
    CHECK_FALSE(t.rows[ints.size() + 2][0].isValid());
}

TEST_CASE("SqliteReader: text in UTF-8, UTF-16le and UTF-16be databases", "[sqlite][values]") {
    const QString text = QStringLiteral("Привет, 日本語 🍺 ascii");
    for (const char *enc : {"UTF-8", "UTF-16le", "UTF-16be"}) {
        INFO(enc);
        QTemporaryDir dir;
        RealDb        db(dir.filePath("t.db"));
        db.exec(QStringLiteral("PRAGMA encoding='%1'").arg(QLatin1String(enc)));
        db.exec("CREATE TABLE t(s)");
        db.insert("INSERT INTO t VALUES(?)", {text});
        db.close();
        const SqliteTable t = readSqliteTable(db.path(), "t");
        REQUIRE(t.error == Error::None);
        CHECK(t.rows.value(0).value(0).toString() == text);
    }
}

// ── Schema quirks ─────────────────────────────────────────────────────────────

TEST_CASE("SqliteReader: column names from awkward CREATE TABLE syntax", "[sqlite][schema]") {
    QTemporaryDir dir;
    RealDb        db(dir.filePath("t.db"));
    db.exec(
        "CREATE TABLE t(\"weird, name\" TEXT DEFAULT 'a,b', [sq bracket] INT, `tick` "
        "CHECK(tick IN (1,2)), plain, CONSTRAINT pk PRIMARY KEY (plain))"
    );
    db.exec("INSERT INTO t VALUES('x', 5, 2, 'p')");
    db.close();
    const SqliteTable t = readSqliteTable(db.path(), "t");
    REQUIRE(t.error == Error::None);
    CHECK(t.columns == QStringList{"weird, name", "sq bracket", "tick", "plain"});
    CHECK(t.rows.value(0) == QVariantList{"x", qint64(5), qint64(2), "p"});
}

TEST_CASE(
    "SqliteReader: ALTER TABLE ADD COLUMN — old rows read the new column as NULL",
    "[sqlite][schema]"
) {
    QTemporaryDir dir;
    RealDb        db(dir.filePath("t.db"));
    db.exec("CREATE TABLE t(a)");
    db.exec("INSERT INTO t VALUES(1)");
    db.exec("ALTER TABLE t ADD COLUMN b TEXT");
    db.exec("INSERT INTO t VALUES(2, 'two')");
    db.close();
    const SqliteTable t = readSqliteTable(db.path(), "t");
    REQUIRE(t.error == Error::None);
    CHECK(t.columns == QStringList{"a", "b"});
    REQUIRE(t.rows.size() == 2);
    CHECK_FALSE(t.rows[0][1].isValid());
    CHECK(t.rows[1][1].toString() == "two");
}

TEST_CASE("SqliteReader: an INTEGER PRIMARY KEY column reads as the rowid", "[sqlite][schema]") {
    QTemporaryDir dir;
    RealDb        db(dir.filePath("t.db"));
    db.exec("CREATE TABLE t(id INTEGER PRIMARY KEY, v)");
    db.exec("INSERT INTO t VALUES(42, 'x')");
    db.exec("INSERT INTO t VALUES(7, 'y')");
    db.close();
    const SqliteTable t = readSqliteTable(db.path(), "t");
    REQUIRE(t.error == Error::None);
    REQUIRE(t.rows.size() == 2);
    CHECK(t.rows[0][0].toLongLong() == 7); // rowid order
    CHECK(t.rows[1][0].toLongLong() == 42);
    CHECK(t.rows[1][1].toString() == "x");
}

TEST_CASE("SqliteReader: a WITHOUT ROWID table is reported, not misread", "[sqlite][schema]") {
    QTemporaryDir dir;
    RealDb        db(dir.filePath("t.db"));
    db.exec("CREATE TABLE t(k TEXT PRIMARY KEY, v) WITHOUT ROWID");
    db.exec("INSERT INTO t VALUES('a', 1)");
    db.close();
    CHECK(readSqliteTable(db.path(), "t").error == Error::Unsupported);
}

// ── A writer mid-transaction ──────────────────────────────────────────────────

TEST_CASE(
    "SqliteReader: a hot rollback journal means Busy; an empty one does not", "[sqlite][journal]"
) {
    QTemporaryDir dir;
    RealDb        db(dir.filePath("Cookies"));
    db.exec("CREATE TABLE t(a)");
    db.exec("INSERT INTO t VALUES(1)");
    db.close();

    QFile journal(db.path() + "-journal");
    REQUIRE(journal.open(QIODevice::WriteOnly)); // Chromium leaves a truncated, empty journal
    journal.close();
    CHECK(readSqliteTable(db.path(), "t").error == Error::None);

    REQUIRE(journal.open(QIODevice::WriteOnly));
    journal.write(QByteArray::fromHex("d9d505f920a163d7") + QByteArray(504, '\0'));
    journal.close();
    CHECK(readSqliteTable(db.path(), "t").error == Error::Busy);
}

TEST_CASE("SqliteReader: WAL frames not yet checkpointed mean Busy", "[sqlite][journal]") {
    QTemporaryDir dir;
    RealDb        db(dir.filePath("t.db"));
    db.exec("PRAGMA journal_mode=WAL");
    db.exec("PRAGMA wal_autocheckpoint=0");
    db.exec("CREATE TABLE t(a)");
    db.exec("INSERT INTO t VALUES(1)");
    // The connection is still open: the rows live only in t.db-wal so far.
    CHECK(readSqliteTable(db.path(), "t").error == Error::Busy);
    db.close(); // the last connection checkpoints and removes the WAL
    const SqliteTable t = readSqliteTable(db.path(), "t");
    REQUIRE(t.error == Error::None);
    CHECK(t.rows.size() == 1);
}

// ── Bad input ─────────────────────────────────────────────────────────────────

TEST_CASE("SqliteReader: missing and non-SQLite files", "[sqlite][bad]") {
    QTemporaryDir dir;
    CHECK(readSqliteTable(dir.filePath("absent"), "t").error == Error::Open);
    QFile f(dir.filePath("junk"));
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(pseudoRandomBytes(4096, 7));
    f.close();
    CHECK(readSqliteTable(f.fileName(), "t").error == Error::NotSqlite);
}

TEST_CASE("SqliteReader: truncated and bit-flipped files never crash", "[sqlite][bad]") {
    QTemporaryDir dir;
    RealDb        db(dir.filePath("good.db"));
    db.exec("PRAGMA page_size=512");
    db.exec(QString::fromLatin1(kCookiesSchema));
    for (int i = 0; i < 60; ++i)
        insertCookie(
            db, QStringLiteral(".host%1.com").arg(i), "d", pseudoRandomBytes(i * 37, quint32(i))
        );
    db.close();
    QFile good(db.path());
    REQUIRE(good.open(QIODevice::ReadOnly));
    const QByteArray bytes = good.readAll();
    good.close();

    const QString    bad = dir.filePath("bad.db");
    QRandomGenerator rng(12345);
    auto             tryBytes = [&](const QByteArray &b) {
        QFile f(bad);
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(b);
        f.close();
        const SqliteTable t = readSqliteTable(bad, "cookies"); // must return, whatever it says
        if (t.error != Error::None)
            CHECK(t.rows.isEmpty());
    };
    for (qsizetype cut = 0; cut < bytes.size(); cut += 97)
        tryBytes(bytes.left(cut));
    for (int round = 0; round < 400; ++round) {
        QByteArray b = bytes;
        for (int flips = 1 + int(rng.bounded(8)); flips > 0; --flips)
            b[qsizetype(rng.bounded(quint32(b.size())))] = char(rng.bounded(256));
        tryBytes(b);
    }
}

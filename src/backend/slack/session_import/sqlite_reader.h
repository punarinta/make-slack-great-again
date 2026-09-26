// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Minimal read-only reader for the SQLite 3 file format — just enough to pull the
// rows of one table out of Chromium's cookie store, so the session import does not
// have to link SQLite (Qt's QSQLITE plugin was 1.4 MB of the static Linux binary).
//
// Scope: rowid tables only (no WITHOUT ROWID, no indexes, no SQL — the caller
// filters rows itself). The file is read into memory once and every access is
// bounds-checked, so a corrupt or half-written file yields an error, never a crash.
// It never writes, and never replays a journal: a file with a hot rollback journal
// or un-checkpointed WAL content (a writer mid-transaction) is reported as Busy
// rather than read in a possibly stale or torn state.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace slack::session {

struct SqliteTable {
    enum class Error {
        None,
        Open,        // file missing / unreadable / too large
        NotSqlite,   // not an SQLite 3 database
        Corrupt,     // structure out of bounds or inconsistent
        Busy,        // a writer is mid-transaction (hot journal / WAL pending)
        NoTable,     // the database has no such table
        Unsupported, // e.g. a WITHOUT ROWID table
    };

    Error               error = Error::None;
    QStringList         columns; // declaration order, from the table's CREATE TABLE
    // One entry per row, values in `columns` order: NULL → invalid QVariant,
    // INTEGER → qint64, REAL → double, TEXT → QString, BLOB → QByteArray. Columns
    // added by a later ALTER TABLE that an old row predates read as NULL.
    QList<QVariantList> rows;
};

// Reads every row of `table` (name matched case-insensitively, as SQLite does).
[[nodiscard]] SqliteTable readSqliteTable(const QString &path, const QString &table);

} // namespace slack::session

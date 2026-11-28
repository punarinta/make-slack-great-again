// Minimal read-only reader for the SQLite 3 file format — just enough to pull
// the rows of one table out of Chromium's cookie store, so the local import
// does not have to link SQLite (a full SQLite is over a megabyte; we have
// none at all).
//
// Scope: rowid tables only (no WITHOUT ROWID, no indexes, no SQL — the caller
// filters rows itself). The file is read into memory once and every access is
// bounds-checked, so a corrupt or half-written file yields an error, never a
// crash. It never writes, and never replays a journal: a file with a hot
// rollback journal or un-checkpointed WAL content (a writer mid-transaction)
// is reported as Busy rather than read in a possibly torn state.
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace slack {

// One cell value. monostate = NULL; Blob and Text are distinguished so the
// caller can take encrypted_value as raw bytes.
struct Cell {
    std::variant<std::monostate, int64_t, double, std::string> v;
    bool                                                       isBlob = false;

    bool    isNull() const { return std::holds_alternative<std::monostate>(v); }
    int64_t integer() const {
        return std::holds_alternative<int64_t>(v) ? std::get<int64_t>(v) : 0;
    }
    std::string text() const; // TEXT/BLOB as a string; "" otherwise
};

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

    Error                          error = Error::None;
    std::vector<std::string>       columns; // declaration order, from CREATE TABLE
    std::vector<std::vector<Cell>> rows;    // values in `columns` order

    // Index of a column (case-insensitive, as SQLite), or -1.
    int columnIndex(std::string_view name) const;
};

// Reads every row of `table` (name matched case-insensitively, as SQLite).
SqliteTable readSqliteTable(const std::string &path, const std::string &table);

} // namespace slack

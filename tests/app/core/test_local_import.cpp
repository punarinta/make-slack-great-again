// "Import from local Slack" (Linux): the SQLite reader and the cookie
// decrypt, against a fake Slack profile built in the test's tmp HOME. The
// test never reads the real ~/.config/Slack — runAll points HOME at a
// throwaway dir and D-Bus at nothing, so no keyring is touched either.
//
// The encrypted cookies are fixed AES-128-CBC(v10) vectors computed offline
// with the key PBKDF2-HMAC-SHA1("peanuts","saltysalt",1,16) and IV = 16
// spaces, so the test needs no AES of its own — only python3's sqlite3 to
// write a real Cookies database (it skips when python3 is missing).
#include "app/slack/local_import.h"
#include "app/slack/session.h"
#include "app/slack/sqlite_reader.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/process.h"
#include "base/str.h"
#include "support/fake_slack_server.h"
#include "support/test.h"

#include <cstdlib>
#include <string>

namespace {

// plaintext "xoxd-aaaabbbbccccddddeeee-localtest"
constexpr char kV10Hex[] = "b6d35467251409ba7d21ec5f589b8be97bf353499f39174b54f4b9c1a9b90c92bcbbe9a"
                           "28db59702d2fadaba47bbf9b1";
// plaintext SHA256(".slack.com") || "xoxd-prefixed-9999-value" (Chromium >= 24)
constexpr char kV24Hex[] = "34cc1c321001cbd03eb0a32947b5884a9d41f7cdef55a6c01c47b36fc1f513697644733"
                           "0398ece5a4bfe90cd92fcec17"
                           "35f59f76a186edc1473d739db66e5142";

std::string home() {
    const char *h = std::getenv("HOME");
    return h ? h : "";
}

bool havePython() {
    return !base::findExecutable("python3").empty();
}

// Runs a short python3 program (args after it), waits for it, returns the
// exit status (-1 if it could not start).
int runPython(const std::string &script, const std::vector<std::string> &args) {
    const std::string path = home() + "/gen.py";
    if (!file::writeAtomic(path, script))
        return -1;
    std::vector<std::string> argv = {path};
    for (const auto &a : args)
        argv.push_back(a);
    base::Process p;
    if (!p.start(base::findExecutable("python3"), argv))
        return -1;
    while (p.running()) // the script is tiny and exits in well under a second
        ;
    return p.exitStatus();
}

// Writes a real SQLite Cookies DB at `dbPath` with the `d` cookie's
// encrypted_value set to the bytes of `encHex`. Returns false to skip.
bool writeCookieDb(const std::string &dbPath, const char *encHex) {
    static const char *const kScript = R"PY(
import sqlite3, sys, os, binascii
db = sys.argv[1]
# Chromium stores the "v10" version tag before the AES-CBC ciphertext.
enc = b'v10' + binascii.unhexlify(sys.argv[2])
os.makedirs(os.path.dirname(db), exist_ok=True)
if os.path.exists(db):
    os.remove(db)
con = sqlite3.connect(db)
con.execute("PRAGMA journal_mode=DELETE")
con.execute("CREATE TABLE cookies (creation_utc INTEGER PRIMARY KEY, host_key TEXT, "
            "name TEXT, value TEXT, encrypted_value BLOB, path TEXT)")
con.execute("INSERT INTO cookies VALUES (1,'decoy.example.com','d','',?, '/')", (b'nope',))
con.execute("INSERT INTO cookies VALUES (2,'.slack.com','d','',?, '/')", (enc,))
con.execute("INSERT INTO cookies VALUES (3,'.slack.com','other','x',?, '/')", (b'',))
con.commit(); con.close()
)PY";
    return runPython(kScript, {dbPath, encHex}) == 0;
}

void writeLevelDb(const std::string &configDir) {
    const std::string dir = configDir + "/Local Storage/leveldb";
    file::makeDirs(dir);
    // A leveldb .log is binary; teamsFromHosts just scans the bytes for hosts.
    std::string blob = "\x01\x02localConfig_v2\x00";
    blob += "https://myteam.slack.com/ ... another.slack.com ... edgeapi.slack.com";
    blob += std::string("\x00\x00", 2) + "files.slack.com";
    file::writeAtomic(file::join(dir, "000003.log"), blob);
}

bool teamsHave(const std::vector<slack::TeamSession> &teams, std::string_view host) {
    for (const auto &t : teams)
        if (t.workspaceUrl.find(host) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST("local import: sqlite reader reads cookie rows") {
    if (!havePython())
        return (void)std::fprintf(stderr, "  skip: no python3\n");
    const std::string db = home() + "/sqlite-probe/Cookies";
    REQUIRE(writeCookieDb(db, kV10Hex));
    const slack::SqliteTable t = slack::readSqliteTable(db, "COOKIES"); // case-insensitive
    CHECK(t.error == slack::SqliteTable::Error::None);
    CHECK(t.columnIndex("host_key") >= 0);
    CHECK(t.columnIndex("encrypted_value") >= 0);
    REQUIRE(t.rows.size() == 3);
    // The creation_utc INTEGER PRIMARY KEY aliases the rowid.
    CHECK(t.rows[1][t.columnIndex("creation_utc")].integer() == 2);
    CHECK(t.rows[1][t.columnIndex("host_key")].text() == ".slack.com");
    CHECK(t.rows[1][t.columnIndex("encrypted_value")].isBlob);
    // A missing table is reported, not crashed into.
    CHECK(slack::readSqliteTable(db, "nope").error == slack::SqliteTable::Error::NoTable);
    CHECK(
        slack::readSqliteTable(home() + "/does-not-exist", "cookies").error ==
        slack::SqliteTable::Error::Open
    );
}

TEST("local import: not installed in an empty HOME") {
    CHECK(slack::localImportSupported()); // this build has the importer
    // HOME is the test's throwaway dir; no Slack profile under it yet.
    const slack::LocalImport r = slack::importLocalSession();
    CHECK(r.error == "not_installed");
    CHECK(r.cookie.empty());
}

TEST("local import: the async form answers on the UI thread, later") {
    bool               called = false;
    slack::LocalImport r;
    slack::importLocalSessionAsync(fakeslack::app(), [&](slack::LocalImport x) {
        r      = std::move(x);
        called = true;
    });
    CHECK_FALSE(called); // never inside the call
    REQUIRE(fakeslack::pumpUntil([&] { return called; }, 5000));
    CHECK(r.error == "not_installed");
}

TEST("local import: decrypt a v10 cookie and list workspaces") {
    if (!havePython())
        return (void)std::fprintf(stderr, "  skip: no python3\n");
    const std::string config = home() + "/.config/Slack";
    REQUIRE(writeCookieDb(config + "/Network/Cookies", kV10Hex));
    writeLevelDb(config);
    const slack::LocalImport r = slack::importLocalSession();
    CHECK_STR(r.error, "");
    CHECK_STR(r.cookie, "xoxd-aaaabbbbccccddddeeee-localtest");
    CHECK(r.ok());
    CHECK(teamsHave(r.teams, "myteam.slack.com"));
    CHECK(teamsHave(r.teams, "another.slack.com"));
    CHECK_FALSE(teamsHave(r.teams, "edgeapi")); // Slack infra, filtered out
    CHECK_FALSE(teamsHave(r.teams, "files"));
}

TEST("local import: strips the Chromium v24 SHA-256 host prefix") {
    if (!havePython())
        return (void)std::fprintf(stderr, "  skip: no python3\n");
    const std::string config = home() + "/.config/Slack";
    // Overwrite the cookie DB (Network/Cookies preferred over Cookies).
    REQUIRE(writeCookieDb(config + "/Network/Cookies", kV24Hex));
    const slack::LocalImport r = slack::importLocalSession();
    CHECK_STR(r.error, "");
    CHECK_STR(r.cookie, "xoxd-prefixed-9999-value");
}

TEST("local import: a non-slack cookie is no_cookie") {
    if (!havePython())
        return (void)std::fprintf(stderr, "  skip: no python3\n");
    const std::string        config  = home() + "/.config/Slack";
    // A cookie DB whose only `d` cookie is on a foreign host.
    static const char *const kScript = R"PY(
import sqlite3, sys, os
db = sys.argv[1]
os.makedirs(os.path.dirname(db), exist_ok=True)
if os.path.exists(db): os.remove(db)
con = sqlite3.connect(db)
con.execute("CREATE TABLE cookies (creation_utc INTEGER PRIMARY KEY, host_key TEXT, "
            "name TEXT, value TEXT, encrypted_value BLOB, path TEXT)")
con.execute("INSERT INTO cookies VALUES (1,'.example.com','d','',?, '/')", (b'v10xxxx',))
con.commit(); con.close()
)PY";
    REQUIRE(runPython(kScript, {config + "/Network/Cookies"}) == 0);
    const slack::LocalImport r = slack::importLocalSession();
    CHECK_STR(r.error, "no_cookie");
}

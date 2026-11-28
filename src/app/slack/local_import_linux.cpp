// "Import from local Slack", Linux. Reads and decrypts the `d` cookie from
// the Slack desktop app's Chromium cookie store and lists its signed-in
// workspaces from local storage. Best effort and version/OS specific: every
// failure path sets LocalImport::error so the UI falls back to manual paste.
//
// Chromium cookie encryption: the value is "v10"/"v11" + AES-128-CBC
// ciphertext, IV = 16 spaces, key = PBKDF2-HMAC-SHA1(password, "saltysalt",
// 1, 16). "v10" uses the fixed password "peanuts"; "v11" uses the password
// Chromium stored in the Secret Service (looked up over D-Bus). Newer
// Chromium (cookie meta version >= 24) prepends SHA-256(host_key) to the
// plaintext; we strip it when it matches.
#include "app/slack/local_import.h"
#include "app/slack/session.h"
#include "app/slack/sqlite_reader.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/process.h"
#include "base/str.h"
#include "net/aes.h"

#include <cstdlib>
#include <string>
#include <vector>

#if defined(MSGA_HAS_DBUS)
#include <dbus/dbus.h>
#endif

namespace slack {

namespace {

// Where the Slack desktop app may keep its Chromium profile, most common
// first: native, Flatpak, Snap.
std::vector<std::string> slackConfigDirs() {
    const char       *xdg    = std::getenv("XDG_CONFIG_HOME");
    const std::string home   = base::homeDir();
    const std::string config = xdg && *xdg ? xdg : str::concat({home, "/.config"});
    return {
        str::concat({config, "/Slack"}),
        str::concat({home, "/.var/app/com.slack.Slack/config/Slack"}), // Flatpak
        str::concat({home, "/snap/slack/current/.config/Slack"}),      // Snap
    };
}

// The cookie DB moved under Network/ in newer Chromium; try both.
std::string findCookieDb(const std::string &configDir) {
    for (const char *rel : {"/Network/Cookies", "/Cookies"}) {
        const std::string p = configDir + rel;
        if (file::exists(p))
            return p;
    }
    return {};
}

std::string deriveKey(std::string_view password) {
    return crypto::pbkdf2Sha1(password, "saltysalt", 1, 16);
}

// AES-128-CBC with the fixed 16-space IV. Empty on any failure (padding is
// removed by the decryptor).
std::string aesCbcDecrypt(std::string_view key, std::string_view ciphertext) {
    std::string out;
    if (!net::aes128CbcDecrypt(key, std::string(16, ' '), ciphertext, &out))
        return {};
    return out;
}

#if defined(MSGA_HAS_DBUS)

// Reads one byte array out of a message iterator (Secret Service hands the
// password as ay inside the GetSecret struct).
std::string readByteArray(DBusMessageIter *it) {
    std::string     out;
    DBusMessageIter arr;
    dbus_message_iter_recurse(it, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_BYTE) {
        uint8_t b = 0;
        dbus_message_iter_get_basic(&arr, &b);
        out += char(b);
        dbus_message_iter_next(&arr);
    }
    return out;
}

// Best-effort Secret Service lookup of Slack's cookie-encryption password
// (the "v11" key). Works when the login keyring is already unlocked (the
// usual case in an active session); we never drive an unlock prompt, and
// every call has a short timeout so a button click never hangs. Empty on
// failure.
std::string secretServicePassword() {
    constexpr int kTimeoutMs = 1500;
    const char   *kService   = "org.freedesktop.secrets";
    const char   *kRoot      = "/org/freedesktop/secrets";
    const char   *kServiceIf = "org.freedesktop.Secret.Service";

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!bus) {
        dbus_error_free(&err);
        return {};
    }
    // Our own connection: don't let a D-Bus disconnect abort the whole app.
    dbus_connection_set_exit_on_disconnect(bus, FALSE);
    std::string result;

    auto cleanup = [&] {
        dbus_connection_close(bus);
        dbus_connection_unref(bus);
        dbus_error_free(&err);
    };

    // OpenSession("plain", variant "") → (variant output, object path session).
    std::string sessionPath;
    {
        DBusMessage *msg = dbus_message_new_method_call(kService, kRoot, kServiceIf, "OpenSession");
        if (!msg) {
            cleanup();
            return {};
        }
        DBusMessageIter args, variant;
        dbus_message_iter_init_append(msg, &args);
        const char *algo = "plain";
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &algo);
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "s", &variant);
        const char *empty = "";
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &empty);
        dbus_message_iter_close_container(&args, &variant);
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, msg, kTimeoutMs, &err);
        dbus_message_unref(msg);
        if (!reply) {
            cleanup();
            return {};
        }
        DBusMessageIter it;
        if (dbus_message_iter_init(reply, &it)) {
            // skip the variant output, read the object path
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT)
                dbus_message_iter_next(&it);
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
                const char *p = nullptr;
                dbus_message_iter_get_basic(&it, &p);
                if (p)
                    sessionPath = p;
            }
        }
        dbus_message_unref(reply);
    }
    if (sessionPath.empty()) {
        cleanup();
        return {};
    }

    // SearchItems({application: "Slack"}) → (unlocked[], locked[]).
    std::string itemPath;
    {
        DBusMessage *msg = dbus_message_new_method_call(kService, kRoot, kServiceIf, "SearchItems");
        if (!msg) {
            cleanup();
            return {};
        }
        DBusMessageIter args, dict, entry;
        dbus_message_iter_init_append(msg, &args);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{ss}", &dict);
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char *k = "application", *v = "Slack";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &v);
        dbus_message_iter_close_container(&dict, &entry);
        dbus_message_iter_close_container(&args, &dict);
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, msg, kTimeoutMs, &err);
        dbus_message_unref(msg);
        if (!reply) {
            cleanup();
            return {};
        }
        DBusMessageIter it;
        if (dbus_message_iter_init(reply, &it) &&
            dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
            DBusMessageIter arr;
            dbus_message_iter_recurse(&it, &arr); // the unlocked[] array
            if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_OBJECT_PATH) {
                const char *p = nullptr;
                dbus_message_iter_get_basic(&arr, &p);
                if (p)
                    itemPath = p;
            }
        }
        dbus_message_unref(reply);
    }
    if (itemPath.empty()) {
        cleanup(); // nothing unlocked (locked keyring) or no Slack entry
        return {};
    }

    // GetSecret(session) on the item → struct (objpath, ay params, ay value, s type).
    {
        DBusMessage *msg = dbus_message_new_method_call(
            kService, itemPath.c_str(), "org.freedesktop.Secret.Item", "GetSecret"
        );
        if (!msg) {
            cleanup();
            return {};
        }
        DBusMessageIter args;
        dbus_message_iter_init_append(msg, &args);
        const char *sp = sessionPath.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sp);
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, msg, kTimeoutMs, &err);
        dbus_message_unref(msg);
        if (!reply) {
            cleanup();
            return {};
        }
        DBusMessageIter it, st;
        if (dbus_message_iter_init(reply, &it) &&
            dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRUCT) {
            dbus_message_iter_recurse(&it, &st);
            dbus_message_iter_next(&st); // skip session object path
            dbus_message_iter_next(&st); // skip params (ay)
            if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_ARRAY)
                result = readByteArray(&st); // the value (ay)
        }
        dbus_message_unref(reply);
    }
    cleanup();
    return result;
}

#else  // no libdbus: only v10 cookies can be decrypted
std::string secretServicePassword() {
    return {};
}
#endif // MSGA_HAS_DBUS

// Decrypt one Chromium encrypted_value for host `hostKey`. v11 tries the
// keyring password first, then falls back to "peanuts" (v10 / no keyring).
std::string decryptCookie(std::string_view enc, std::string_view hostKey) {
    if (enc.size() < 3)
        return {};
    const std::string_view prefix = enc.substr(0, 3);
    const std::string_view body   = enc.substr(3);

    std::vector<std::string> passwords;
    if (prefix == "v11") {
        std::string kr = secretServicePassword();
        if (!kr.empty())
            passwords.push_back(std::move(kr));
    }
    passwords.push_back("peanuts"); // v10 and the universal fallback

    for (const std::string &pw : passwords) {
        std::string plain = aesCbcDecrypt(deriveKey(pw), body);
        if (plain.empty())
            continue;
        // Chromium >= 24 prepends SHA-256(host_key) to the plaintext; strip it
        // when the 32-byte prefix matches, else fall back to the xoxd- check.
        if (plain.size() > 32) {
            const auto h = crypto::sha256(hostKey);
            if (std::string_view(plain.data(), 32) == crypto::bytes(h))
                plain.erase(0, 32);
            else if (!str::startsWith(plain, "xoxd-"))
                plain.erase(0, 32);
        }
        if (str::startsWith(plain, "xoxd-"))
            return plain;
    }
    return {};
}

// The workspace hosts in the web client's localStorage leveldb (*.ldb/*.log),
// passed to teamsFromHosts (which filters Slack's own infra subdomains).
std::vector<TeamSession> discoverWorkspaces(const std::string &configDir) {
    const std::string           dir = configDir + "/Local Storage/leveldb";
    std::vector<file::DirEntry> entries;
    if (!file::listDir(dir, &entries))
        return {};
    std::vector<std::string> blobs;
    for (const auto &e : entries) {
        if (e.isDir)
            continue;
        if (!str::endsWith(e.name, ".ldb") && !str::endsWith(e.name, ".log"))
            continue;
        std::string blob;
        if (file::readAll(file::join(dir, e.name), &blob))
            blobs.push_back(std::move(blob));
    }
    return teamsFromHosts(blobs);
}

} // namespace

bool localImportSupported() {
    return true;
}

LocalImport importLocalSession() {
    LocalImport result;

    std::string configDir, cookieDb;
    for (const std::string &dir : slackConfigDirs()) {
        const std::string db = findCookieDb(dir);
        if (!db.empty()) {
            configDir = dir;
            cookieDb  = db;
            break;
        }
    }
    if (cookieDb.empty()) {
        result.error = "not_installed";
        return result;
    }

    // Read the encrypted `d` cookie straight from the file (read-only, no lock
    // taken, so Slack's own DB handle is never disturbed). A Slack mid-write
    // reads as Busy and, like an unreadable file, is reported as "locked".
    const SqliteTable cookies = readSqliteTable(cookieDb, "cookies");
    if (cookies.error != SqliteTable::Error::None && cookies.error != SqliteTable::Error::NoTable) {
        result.error = "locked";
        return result;
    }
    const int   host  = cookies.columnIndex("host_key");
    const int   name  = cookies.columnIndex("name");
    const int   value = cookies.columnIndex("encrypted_value");
    std::string enc, hostKey;
    if (host >= 0 && name >= 0 && value >= 0) {
        for (const auto &row : cookies.rows) {
            if (row[name].text() == "d" &&
                row[host].text().find("slack.com") != std::string::npos) {
                enc     = row[value].text();
                hostKey = row[host].text();
                break;
            }
        }
    }
    if (enc.empty()) {
        result.error = "no_cookie";
        return result;
    }

    const std::string cookie = decryptCookie(enc, hostKey);
    if (cookie.empty()) {
        result.error = "decrypt_failed";
        return result;
    }

    result.cookie = cookie;
    result.teams  = discoverWorkspaces(configDir);
    return result;
}

} // namespace slack

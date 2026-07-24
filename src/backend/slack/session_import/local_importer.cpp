// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/slack/session_import/local_importer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

namespace slack::session {

// ─────────────────────────────────────────────────────────────────────────────
// Real Linux implementation, compiled only when the deps were found at configure
// time (OpenSSL for AES/PBKDF2, Qt6::Sql for the Chromium cookie DB). Everything
// else — macOS, Windows, or a Linux build without those deps — uses the stub at
// the bottom, so the app always builds and simply offers guided manual paste.
// ─────────────────────────────────────────────────────────────────────────────
#if defined(MSGA_SLACK_SESSION_IMPORT)

} // namespace slack::session

#include <QByteArray>
#include <QCryptographicHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

#include <openssl/evp.h>

#if defined(MSGA_HAS_DBUS)
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#endif

namespace slack::session {
namespace {

// Directories the Slack desktop app may keep its Chromium profile in, most common
// first: native, Flatpak, Snap.
QStringList slackConfigDirs() {
    const QString home = QDir::homePath();
    const QString xdg  = qEnvironmentVariable("XDG_CONFIG_HOME", home + "/.config");
    return {
        xdg + "/Slack",
        home + "/.var/app/com.slack.Slack/config/Slack", // Flatpak
        home + "/snap/slack/current/.config/Slack",      // Snap
    };
}

// The cookie DB moved under Network/ in newer Chromium; try both.
QString findCookieDb(const QString &configDir) {
    for (const QString &rel : {QStringLiteral("Network/Cookies"), QStringLiteral("Cookies")}) {
        const QString p = configDir + "/" + rel;
        if (QFileInfo::exists(p))
            return p;
    }
    return {};
}

// PBKDF2-HMAC-SHA1(password, "saltysalt", 1 iter, 16 bytes) — Chromium's fixed KDF.
QByteArray deriveKey(const QByteArray &password) {
    QByteArray       key(16, Qt::Uninitialized);
    const QByteArray salt = "saltysalt";
    PKCS5_PBKDF2_HMAC_SHA1(
        password.constData(),
        password.size(),
        reinterpret_cast<const unsigned char *>(salt.constData()),
        salt.size(),
        1,
        key.size(),
        reinterpret_cast<unsigned char *>(key.data())
    );
    return key;
}

// AES-128-CBC decrypt with a fixed 16-space IV (Chromium's convention). Returns
// empty on any failure. PKCS7 padding is removed by the cipher.
QByteArray aesCbcDecrypt(const QByteArray &key, const QByteArray &ciphertext) {
    if (ciphertext.isEmpty() || ciphertext.size() % 16 != 0)
        return {};
    const QByteArray iv(16, ' ');
    EVP_CIPHER_CTX  *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};
    QByteArray out(ciphertext.size() + 16, Qt::Uninitialized);
    int        outLen = 0, finalLen = 0;
    bool       okInit = EVP_DecryptInit_ex(
                      ctx,
                      EVP_aes_128_cbc(),
                      nullptr,
                      reinterpret_cast<const unsigned char *>(key.constData()),
                      reinterpret_cast<const unsigned char *>(iv.constData())
                  ) == 1;
    bool okUpd = okInit && EVP_DecryptUpdate(
                               ctx,
                               reinterpret_cast<unsigned char *>(out.data()),
                               &outLen,
                               reinterpret_cast<const unsigned char *>(ciphertext.constData()),
                               ciphertext.size()
                           ) == 1;
    bool okFin = okUpd && EVP_DecryptFinal_ex(
                              ctx, reinterpret_cast<unsigned char *>(out.data()) + outLen, &finalLen
                          ) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!okFin)
        return {};
    out.truncate(outLen + finalLen);
    return out;
}

#if defined(MSGA_HAS_DBUS)
// Best-effort Secret Service lookup of Slack's cookie-encryption password (the
// "v11" key). Works when the login keyring is already unlocked (the usual case in
// an active desktop session); we do not drive the unlock prompt. Empty on failure.
QByteArray secretServicePassword() {
    const QString service = QStringLiteral("org.freedesktop.secrets");
    const QString path    = QStringLiteral("/org/freedesktop/secrets");
    const QString iface   = QStringLiteral("org.freedesktop.Secret.Service");
    auto          bus     = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return {};

    // OpenSession(algorithm="plain") → a session object path for GetSecret.
    QDBusInterface svc(service, path, iface, bus);
    QDBusMessage   os = svc.call(
        QStringLiteral("OpenSession"),
        QStringLiteral("plain"),
        QVariant::fromValue(QDBusVariant(QString()))
    );
    if (os.type() != QDBusMessage::ReplyMessage || os.arguments().size() < 2)
        return {};
    const auto sessionPath = os.arguments().at(1).value<QDBusObjectPath>();

    // SearchItems({application: "Slack"}) — Chromium stores the key under this attr.
    QMap<QString, QString> attrs{{QStringLiteral("application"), QStringLiteral("Slack")}};
    QDBusMessage search = svc.call(QStringLiteral("SearchItems"), QVariant::fromValue(attrs));
    if (search.type() != QDBusMessage::ReplyMessage || search.arguments().isEmpty())
        return {};
    const auto unlocked = qdbus_cast<QList<QDBusObjectPath>>(search.arguments().at(0));
    if (unlocked.isEmpty())
        return {};

    // GetSecret(session) on the first matching item → struct (session, params,
    // value, contentType); the password is the value field.
    QDBusInterface item(
        service, unlocked.first().path(), QStringLiteral("org.freedesktop.Secret.Item"), bus
    );
    QDBusMessage gs = item.call(QStringLiteral("GetSecret"), QVariant::fromValue(sessionPath));
    if (gs.type() != QDBusMessage::ReplyMessage || gs.arguments().isEmpty())
        return {};
    const auto      arg = gs.arguments().at(0).value<QDBusArgument>();
    QDBusObjectPath sPath;
    QByteArray      params, value;
    QString         contentType;
    arg.beginStructure();
    arg >> sPath >> params >> value >> contentType;
    arg.endStructure();
    return value;
}
#else
QByteArray secretServicePassword() {
    return {};
}
#endif

// Decrypt one Chromium `encrypted_value`. Tries the keyring password (v11) first,
// then the "peanuts" fallback (v10 / no keyring). Strips the 32-byte SHA256 domain
// hash prefix newer Chromium prepends to the plaintext.
QString decryptCookie(const QByteArray &enc) {
    if (enc.size() < 3)
        return {};
    const QByteArray prefix = enc.left(3);
    const QByteArray body   = enc.mid(3);

    QList<QByteArray> passwords;
    if (prefix == "v11") {
        const QByteArray kr = secretServicePassword();
        if (!kr.isEmpty())
            passwords << kr;
    }
    passwords << QByteArray("peanuts"); // v10 and the universal fallback

    for (const QByteArray &pw : passwords) {
        QByteArray plain = aesCbcDecrypt(deriveKey(pw), body);
        if (plain.isEmpty())
            continue;
        // Newer Chromium prepends SHA256(host) (32 bytes) to the value.
        if (!plain.startsWith("xoxd-") && plain.size() > 32)
            plain = plain.mid(32);
        const QString s = QString::fromUtf8(plain);
        if (s.startsWith(QStringLiteral("xoxd-")))
            return s;
    }
    return {};
}

// Grep the leveldb localStorage for the user's workspace hosts (e.g.
// "myteam.slack.com"). Cheap and robust vs. parsing the token JSON: the deriver
// fetches per-workspace tokens from these hosts using the cookie.
QList<TeamSession> discoverWorkspaces(const QString &configDir) {
    QList<TeamSession> teams;
    QSet<QString>      seen;
    const QDir         db(configDir + "/Local Storage/leveldb");
    if (!db.exists())
        return teams;
    static const QRegularExpression host(QStringLiteral("([a-z0-9][a-z0-9-]*)\\.slack\\.com"));
    const auto                      files = db.entryList({"*.ldb", "*.log"}, QDir::Files);
    for (const QString &name : files) {
        QFile f(db.filePath(name));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QString blob = QString::fromLatin1(f.readAll());
        auto          it   = host.globalMatch(blob);
        while (it.hasNext()) {
            const QString sub = it.next().captured(1);
            // Skip Slack's own infra subdomains; keep real team domains.
            if (sub == "app" || sub == "edgeapi" || sub == "files" || sub == "api" ||
                sub == "www" || sub == "a" || sub == "downloads" || seen.contains(sub))
                continue;
            seen.insert(sub);
            TeamSession t;
            t.workspaceUrl = "https://" + sub + ".slack.com";
            teams.append(t);
        }
    }
    return teams;
}

} // namespace

bool localImportSupported() {
    return true;
}

LocalImport importLocalSlackSession() {
    LocalImport result;

    QString configDir, cookieDb;
    for (const QString &dir : slackConfigDirs()) {
        const QString db = findCookieDb(dir);
        if (!db.isEmpty()) {
            configDir = dir;
            cookieDb  = db;
            break;
        }
    }
    if (cookieDb.isEmpty()) {
        result.error = QStringLiteral("not_installed");
        return result;
    }

    // Read the encrypted `d` cookie. Open a private, uniquely-named read-only
    // connection so we never disturb Slack's own DB handle.
    QByteArray enc;
    {
        const QString conn = QStringLiteral("msga_slack_cookies_") + QUuid::createUuid().toString();
        {
            QSqlDatabase sdb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            sdb.setDatabaseName(cookieDb);
            sdb.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY=1"));
            if (!sdb.open()) {
                QSqlDatabase::removeDatabase(conn);
                result.error = QStringLiteral("locked");
                return result;
            }
            QSqlQuery q(sdb);
            q.exec(QStringLiteral("SELECT host_key, encrypted_value FROM cookies WHERE name='d'"));
            while (q.next()) {
                if (q.value(0).toString().contains(QStringLiteral("slack.com"))) {
                    enc = q.value(1).toByteArray();
                    break;
                }
            }
            sdb.close();
        }
        QSqlDatabase::removeDatabase(conn);
    }
    if (enc.isEmpty()) {
        result.error = QStringLiteral("no_cookie");
        return result;
    }

    const QString cookie = decryptCookie(enc);
    if (cookie.isEmpty()) {
        result.error = QStringLiteral("decrypt_failed");
        return result;
    }

    result.cookie = cookie;
    result.teams  = discoverWorkspaces(configDir);
    return result;
}

} // namespace slack::session

#else // !MSGA_SLACK_SESSION_IMPORT — stub for unsupported platforms/builds

bool localImportSupported() {
    return false;
}

LocalImport importLocalSlackSession() {
    LocalImport r;
    r.error = QStringLiteral("unsupported_platform");
    return r;
}

} // namespace slack::session

#endif

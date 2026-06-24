// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_providers.h"

#include <QCryptographicHash>

namespace imap {

ProviderInfo providerForMxHost(const QString &mxHost) {
    const QString h = mxHost.trimmed().toLower();
    ProviderInfo  p;
    auto          set = [&](const QString &name,
                   const QString &imap,
                   const QString &smtp,
                   AuthMethod     auth,
                   const QString &help) {
        p.name               = name;
        p.imapHost           = imap;
        p.smtpHost           = smtp;
        p.auth               = auth;
        p.appPasswordHelpUrl = help;
        p.known              = true;
    };
    if (h.contains("google") || h.contains("googlemail"))
        set("Gmail",
            "imap.gmail.com",
            "smtp.gmail.com",
            AuthMethod::OAuthGoogle,
            "https://myaccount.google.com/apppasswords");
    else if (h.contains("outlook") || h.contains("office365") || h.contains("microsoft"))
        set("Outlook",
            "outlook.office365.com",
            "smtp.office365.com",
            AuthMethod::OAuthMicrosoft,
            "https://account.live.com/proofs/AppPassword");
    else if (h.contains("messagingengine") || h.contains("fastmail"))
        set("Fastmail",
            "imap.fastmail.com",
            "smtp.fastmail.com",
            AuthMethod::Password,
            "https://www.fastmail.help/hc/en-us/articles/360058752854");
    else if (h.contains("yahoodns") || h.contains("yahoo"))
        set("Yahoo",
            "imap.mail.yahoo.com",
            "smtp.mail.yahoo.com",
            AuthMethod::Password,
            "https://help.yahoo.com/kb/SLN15241.html");
    else if (h.contains("zoho"))
        set("Zoho", "imap.zoho.com", "smtp.zoho.com", AuthMethod::Password, {});
    else if (h.contains("icloud") || h.contains("me.com") || h.contains("apple"))
        set("iCloud",
            "imap.mail.me.com",
            "smtp.mail.me.com",
            AuthMethod::Password,
            "https://support.apple.com/en-us/102654");
    return p; // known=false if no branch matched
}

QString gravatarUrl(const QString &email, int size) {
    const QByteArray hash =
        QCryptographicHash::hash(email.trimmed().toLower().toUtf8(), QCryptographicHash::Md5)
            .toHex();
    return QStringLiteral("https://www.gravatar.com/avatar/%1?s=%2&d=404")
        .arg(QString::fromLatin1(hash))
        .arg(size);
}

ProviderInfo detectProvider(const QString &email) {
    const int     at     = email.indexOf('@');
    const QString domain = (at >= 0 ? email.mid(at + 1) : email).trimmed().toLower();

    ProviderInfo p;
    auto         set = [&](const QString &name,
                   const QString &imap,
                   const QString &smtp,
                   AuthMethod     auth,
                   const QString &help) {
        p.name               = name;
        p.imapHost           = imap;
        p.smtpHost           = smtp;
        p.auth               = auth;
        p.appPasswordHelpUrl = help;
        p.known              = true;
    };

    if (domain == "gmail.com" || domain == "googlemail.com") {
        set("Gmail",
            "imap.gmail.com",
            "smtp.gmail.com",
            AuthMethod::OAuthGoogle,
            "https://myaccount.google.com/apppasswords");
    } else if (domain == "outlook.com" || domain == "hotmail.com" || domain == "live.com" ||
               domain == "msn.com") {
        set("Outlook",
            "outlook.office365.com",
            "smtp.office365.com",
            AuthMethod::OAuthMicrosoft,
            "https://account.live.com/proofs/AppPassword");
    } else if (domain == "icloud.com" || domain == "me.com" || domain == "mac.com") {
        set("iCloud",
            "imap.mail.me.com",
            "smtp.mail.me.com",
            AuthMethod::Password,
            "https://support.apple.com/en-us/102654");
    } else if (domain == "fastmail.com" || domain == "fastmail.fm") {
        set("Fastmail",
            "imap.fastmail.com",
            "smtp.fastmail.com",
            AuthMethod::Password,
            "https://www.fastmail.help/hc/en-us/articles/360058752854");
    } else if (domain == "yahoo.com" || domain == "ymail.com") {
        set("Yahoo",
            "imap.mail.yahoo.com",
            "smtp.mail.yahoo.com",
            AuthMethod::Password,
            "https://help.yahoo.com/kb/SLN15241.html");
    } else if (domain == "gmx.com" || domain == "gmx.net") {
        set("GMX", "imap.gmx.com", "mail.gmx.com", AuthMethod::Password, {});
    } else {
        // Unknown domain — guess the conventional hosts; the user can edit them.
        p.name     = domain.isEmpty() ? QStringLiteral("Email") : domain;
        p.imapHost = QStringLiteral("imap.") + domain;
        p.smtpHost = QStringLiteral("smtp.") + domain;
        p.auth     = AuthMethod::Password;
        p.known    = false;
    }
    return p;
}

} // namespace imap

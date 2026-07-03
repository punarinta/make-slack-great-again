// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Email provider detection for the "Email + auto-detect" add-account flow
// (imap-backend-plan §5). Maps an email address → IMAP/SMTP server settings +
// the auth method the provider needs. Known providers carry exact settings and
// an app-password help link; unknown domains get guessed hosts (imap.<domain> /
// smtp.<domain>) that the user can edit. Pure (no Qt GUI/network), unit-testable.
#pragma once

#include <QString>

namespace imap {

enum class AuthMethod {
    Password,       // app-password / account password (Phase 5 — works now)
    OAuthGoogle,    // XOAUTH2 via Google (Phase 5b — deferred; app-password meanwhile)
    OAuthMicrosoft, // XOAUTH2 via Microsoft (Phase 5b — deferred; app-password meanwhile)
};

struct ProviderInfo {
    QString    name;     // human label, e.g. "Gmail", or the bare domain when unknown
    QString    imapHost; // e.g. "imap.gmail.com"
    quint16    imapPort = 993;
    QString    smtpHost; // e.g. "smtp.gmail.com"
    quint16    smtpPort = 587;
    AuthMethod auth     = AuthMethod::Password;
    QString    appPasswordHelpUrl; // where to create an app-password (empty if n/a)
    bool       known = false;      // false → settings were guessed from the domain
};

// Look up provider settings for `email`. Always returns something usable: a known
// provider's exact config, or guessed hosts for an unrecognized domain.
ProviderInfo detectProvider(const QString &email);

// Recognize a hosting provider from one of a domain's MX hostnames — so a custom
// domain hosted on Gmail/Outlook/Fastmail (e.g. MX → aspmx.l.google.com) resolves
// to that provider's real IMAP host + auth method. Returns known=false if the MX
// host isn't a recognized provider.
ProviderInfo providerForMxHost(const QString &mxHost);

// Gravatar URL for an email (the de-facto avatar source for mail). `d=404` so a
// sender with no Gravatar yields a 404 → the UI falls back to the initials
// placeholder rather than a generic silhouette. (Note: this hashes the address to
// gravatar.com — standard for mail clients, but a privacy trade-off.)
QString gravatarUrl(const QString &email, int size = 128);

// True for consumer mailbox providers (gmail.com, yahoo.*, outlook.*, …). Domain-
// level icons (BIMI logo, favicon) must never be applied to users at these
// domains: the provider's brand is not the person's avatar — Yahoo publishes
// BIMI, so without this guard every @yahoo.com peer gets the Yahoo logo.
// Checks parent domains too (mail.yahoo.com → yahoo.com).
bool isFreemailDomain(const QString &domain);

} // namespace imap

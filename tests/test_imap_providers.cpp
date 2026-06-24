// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Unit tests for email provider auto-detection (the add-account flow).
#include <catch2/catch_test_macros.hpp>

#include "backend/imap/imap_providers.h"

using namespace imap;

TEST_CASE("detectProvider: known providers map to exact settings", "[imap][providers]") {
    const auto g = detectProvider("alice@gmail.com");
    CHECK(g.known);
    CHECK(g.name == "Gmail");
    CHECK(g.imapHost == "imap.gmail.com");
    CHECK(g.smtpHost == "smtp.gmail.com");
    CHECK(g.auth == AuthMethod::OAuthGoogle);
    CHECK_FALSE(g.appPasswordHelpUrl.isEmpty());

    const auto o = detectProvider("bob@outlook.com");
    CHECK(o.imapHost == "outlook.office365.com");
    CHECK(o.auth == AuthMethod::OAuthMicrosoft);

    const auto f = detectProvider("c@fastmail.com");
    CHECK(f.imapHost == "imap.fastmail.com");
    CHECK(f.auth == AuthMethod::Password);

    const auto i = detectProvider("d@icloud.com");
    CHECK(i.imapHost == "imap.mail.me.com");
    CHECK(i.auth == AuthMethod::Password);
}

TEST_CASE("detectProvider: unknown domain is guessed and editable", "[imap][providers]") {
    const auto u = detectProvider("vladimir@lingolette.com");
    CHECK_FALSE(u.known); // guessed — UI lets the user edit
    CHECK(u.imapHost == "imap.lingolette.com");
    CHECK(u.smtpHost == "smtp.lingolette.com");
    CHECK(u.auth == AuthMethod::Password);
    CHECK(u.imapPort == 993);
    CHECK(u.smtpPort == 587);
}

TEST_CASE("detectProvider: case-insensitive, handles bare domain", "[imap][providers]") {
    CHECK(detectProvider("X@GMAIL.COM").imapHost == "imap.gmail.com");
    CHECK(detectProvider("gmail.com").imapHost == "imap.gmail.com"); // no local part
}

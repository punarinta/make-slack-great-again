// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Unit tests for the favicon fallback resolver's pure parts: the homepage
// <link rel=icon> scan, the parent-domain hop, and image sniffing. The network
// probe sequencing itself is exercised live (it's a straight chain over these).
#include <catch2/catch_test_macros.hpp>

#include "backend/imap/imap_favicon.h"

#include <QBuffer>
#include <QImage>

using imap::FaviconResolver;

TEST_CASE("iconCandidatesFromHtml: apple-touch-icon outranks plain icons", "[imap][favicon]") {
    const QString html = QStringLiteral(
        "<html><head>"
        "<link rel=\"icon\" href=\"/favicon-32.png\" sizes=\"32x32\">"
        "<link rel=\"apple-touch-icon\" href=\"/apple-180.png\" sizes=\"180x180\">"
        "<link rel=\"shortcut icon\" href=\"favicon.ico\">"
        "</head></html>"
    );
    const auto c = FaviconResolver::iconCandidatesFromHtml(html, QUrl("https://www.example.com/"));
    REQUIRE(c.size() == 3);
    CHECK(c[0] == "https://www.example.com/apple-180.png");
    CHECK(c[1] == "https://www.example.com/favicon-32.png");
    CHECK(c[2] == "https://www.example.com/favicon.ico");
}

TEST_CASE("iconCandidatesFromHtml: larger sizes win within a rel class", "[imap][favicon]") {
    const QString html = QStringLiteral(
        "<link rel=\"icon\" href=\"/s16.png\" sizes=\"16x16\">"
        "<link rel=\"icon\" href=\"/s192.png\" sizes=\"192x192\">"
        "<link rel=\"icon\" href=\"/any.svg\" sizes=\"any\">"
    );
    const auto c = FaviconResolver::iconCandidatesFromHtml(html, QUrl("https://x.com/"));
    REQUIRE(c.size() == 3);
    CHECK(c[0] == "https://x.com/any.svg"); // "any" = 256, beats 192
    CHECK(c[1] == "https://x.com/s192.png");
    CHECK(c[2] == "https://x.com/s16.png");
}

TEST_CASE(
    "iconCandidatesFromHtml: skips mask-icon, data: URLs, non-icon links", "[imap][favicon]"
) {
    const QString html = QStringLiteral(
        "<link rel=\"mask-icon\" href=\"/pinned.svg\" color=\"#000\">"
        "<link rel=\"icon\" href=\"data:image/png;base64,AAAA\">"
        "<link rel=\"stylesheet\" href=\"/style.css\">"
        "<link rel=\"icon\" href=\"/real.png\">"
    );
    const auto c = FaviconResolver::iconCandidatesFromHtml(html, QUrl("https://x.com/"));
    REQUIRE(c.size() == 1);
    CHECK(c[0] == "https://x.com/real.png");
}

TEST_CASE(
    "iconCandidatesFromHtml: unquoted attrs, mixed case, absolute URLs, dedupe", "[imap][favicon]"
) {
    const QString html = QStringLiteral(
        "<LINK REL=ICON HREF=/plain.png>"
        "<link rel='icon' href='https://cdn.example.net/icon.png'>"
        "<link rel=\"icon\" href=\"/plain.png\">" // duplicate of the first
    );
    const auto c = FaviconResolver::iconCandidatesFromHtml(html, QUrl("https://x.com/"));
    REQUIRE(c.size() == 2);
    CHECK(c.contains("https://x.com/plain.png"));
    CHECK(c.contains("https://cdn.example.net/icon.png"));
}

TEST_CASE(
    "iconCandidatesFromHtml: relative hrefs resolve against redirect target", "[imap][favicon]"
) {
    // Probe started at example.com but landed on www.example.com/home/.
    const auto c = FaviconResolver::iconCandidatesFromHtml(
        QStringLiteral("<link rel=\"icon\" href=\"fav.png\">"),
        QUrl("https://www.example.com/home/")
    );
    REQUIRE(c.size() == 1);
    CHECK(c[0] == "https://www.example.com/home/fav.png");
}

TEST_CASE("parentDomain: strips to the last two labels", "[imap][favicon]") {
    CHECK(FaviconResolver::parentDomain("em.news.example.com") == "example.com");
    CHECK(FaviconResolver::parentDomain("mail.example.com") == "example.com");
    CHECK(FaviconResolver::parentDomain("example.com") == "example.com");
    CHECK(FaviconResolver::parentDomain("localhost") == "localhost");
}

TEST_CASE("looksLikeImage: accepts real rasters, rejects HTML/tiny/empty", "[imap][favicon]") {
    auto png = [](int w, int h) {
        QImage img(w, h, QImage::Format_ARGB32);
        img.fill(Qt::red);
        QByteArray bytes;
        QBuffer    buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        return bytes;
    };
    CHECK(FaviconResolver::looksLikeImage(png(32, 32)));
    CHECK_FALSE(FaviconResolver::looksLikeImage(png(1, 1))); // tracker/placeholder
    CHECK_FALSE(FaviconResolver::looksLikeImage(QByteArrayLiteral("<html><body>404")));
    CHECK_FALSE(FaviconResolver::looksLikeImage({}));
}

TEST_CASE("looksLikeImage: sniffs SVG (the ImageCache QSvgRenderer path)", "[imap][favicon]") {
    CHECK(
        FaviconResolver::looksLikeImage(
            QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\"/>")
        )
    );
    CHECK(
        FaviconResolver::looksLikeImage(
            QByteArrayLiteral("<?xml version=\"1.0\"?>\n<svg xmlns=\"a\"></svg>")
        )
    );
    CHECK_FALSE(
        FaviconResolver::looksLikeImage(QByteArrayLiteral("<?xml version=\"1.0\"?><foo/>"))
    );
}

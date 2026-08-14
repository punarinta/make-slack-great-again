// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
#include "text/link_labels.h"
#include <catch2/catch_test_macros.hpp>

static const QString kLongUrl =
    "https://akitravel.citycity.se/flights/rebook/%7B%22pnr%22%3A%22YU4SS7%22%7D";

TEST_CASE("shortened label is detected", "[link_labels]") {
    CHECK(
        LinkLabels::isShortenedUrlLabel(
            QString::fromUtf8("akitravel.citycity.se/flights/…/…"), kLongUrl
        )
    );
    CHECK(
        LinkLabels::isShortenedUrlLabel(
            QString::fromUtf8("akitravel.citycity.se/flights/reb…"), kLongUrl
        )
    );
    // ASCII "..." variant.
    CHECK(LinkLabels::isShortenedUrlLabel("akitravel.citycity.se/flights/...", kLongUrl));
}

TEST_CASE("real text labels are not detected", "[link_labels]") {
    CHECK_FALSE(LinkLabels::isShortenedUrlLabel("see this", kLongUrl));
    // Ellipsis, but not derived from this URL.
    CHECK_FALSE(LinkLabels::isShortenedUrlLabel(QString::fromUtf8("read more…"), kLongUrl));
    // Full label without ellipsis is not "shortened".
    CHECK_FALSE(LinkLabels::isShortenedUrlLabel("akitravel.citycity.se/flights", kLongUrl));
    CHECK_FALSE(LinkLabels::isShortenedUrlLabel(QString::fromUtf8("…"), QString()));
}

TEST_CASE("fragments must appear in order", "[link_labels]") {
    CHECK_FALSE(
        LinkLabels::isShortenedUrlLabel(
            QString::fromUtf8("rebook/…/akitravel.citycity.se"), kLongUrl
        )
    );
}

TEST_CASE("expandedLabel strips scheme and elides the tail", "[link_labels]") {
    CHECK(LinkLabels::expandedLabel("https://example.com/a", 100) == "example.com/a");
    const QString label = LinkLabels::expandedLabel(kLongUrl, 30);
    CHECK(label.size() == 30);
    CHECK(label.startsWith("akitravel.citycity.se/"));
    CHECK(label.endsWith(QChar(0x2026)));
}

TEST_CASE("plainTextWithFullUrls substitutes shortened spans only", "[link_labels]") {
    const QString    shortLabel = QString::fromUtf8("akitravel.citycity.se/flights/…/…");
    TextWithEntities t;
    t.text     = "check " + shortLabel + " and see this";
    t.entities = {
        {EntityType::Link, 6, (int)shortLabel.size(), kLongUrl},
        {EntityType::Link, 6 + (int)shortLabel.size() + 5, 8, "https://other.example/x"},
    };
    CHECK(LinkLabels::plainTextWithFullUrls(t) == "check " + kLongUrl + " and see this");
}

TEST_CASE("plainTextWithFullUrls is a no-op without shortened links", "[link_labels]") {
    TextWithEntities t;
    t.text     = "just words";
    t.entities = {{EntityType::Bold, 0, 4, {}}};
    CHECK(LinkLabels::plainTextWithFullUrls(t) == "just words");
}

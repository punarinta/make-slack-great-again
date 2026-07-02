// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QByteArray>
#include <QUrl>
#include <QUrlQuery>

namespace net {

// QUrlQuery::toString(QUrl::FullyEncoded) encodes per RFC 3986 query rules,
// where a literal '+' is a legal character and stays as-is — but in
// application/x-www-form-urlencoded (and in query strings as Slack parses
// them) a literal '+' means SPACE, so any value containing '+' arrives
// corrupted ("C++" → "C  "). In FullyEncoded output spaces are already %20,
// so every remaining raw '+' is a literal plus; escaping it to %2B is the
// only correction form-encoding needs. Use this for every form body and
// every query string handed to the Slack API — never raw
// toString(QUrl::FullyEncoded).
[[nodiscard]] inline QByteArray formUrlEncode(const QUrlQuery &params) {
    auto out = params.toString(QUrl::FullyEncoded).toUtf8();
    out.replace("+", "%2B");
    return out;
}

} // namespace net

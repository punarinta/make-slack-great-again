// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// HTML email → TextWithEntities (imap-backend-plan Phase 6). Email is mostly
// HTML, so rather than flatten to plain text we map it onto the same entity model
// Slack mrkdwn produces (Bold/Italic/Underline/Strike/Code/Pre/Blockquote/Link),
// which the existing message renderer displays with formatting + clickable links.
// Hand-rolled + defensive (keeps the backend GUI-free; malformed mail degrades,
// never throws). Scoped to common shapes; CSS/scripts/tables are stripped to text.
#pragma once

#include "backend/domain.h" // TextWithEntities

#include <QByteArray>
#include <QString>

namespace imap {

// Convert an HTML email body to text + inline entity spans.
TextWithEntities htmlToEntities(const QString &html);

// Decode HTML character references (&amp; &lt; &#39; &#xHH; &nbsp; …). Exposed
// for reuse + tests.
QString decodeHtmlEntities(const QString &s);

} // namespace imap

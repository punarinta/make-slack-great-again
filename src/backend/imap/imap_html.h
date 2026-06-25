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

// Plain-text body cleanup (the non-HTML branch bypasses htmlToEntities): drop
// zero-width filler and cap consecutive newlines at 3 (≤2 blank lines) so a
// plain-text mail with big vertical gaps doesn't render as a giant void.
//
// Whitespace-only lines must NOT reset the blank-line cap: marketing mail pads
// gaps with lines full of spaces/tabs (or a single space per line), and a naive
// counter resets on each space and lets every newline through, reopening the
// gap. So inline whitespace is buffered and committed only when real content
// follows on the same line — trailing/blank-line whitespace is dropped and the
// newline run keeps counting straight through it.
QString normalizePlainText(const QString &in);

// Decode HTML character references (&amp; &lt; &#39; &#xHH; &nbsp; …). Exposed
// for reuse + tests.
QString decodeHtmlEntities(const QString &s);

} // namespace imap

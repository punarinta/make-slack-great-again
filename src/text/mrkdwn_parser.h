// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Slack mrkdwn → TextWithEntities.
// Does NOT use cmark-gfm — Slack mrkdwn is its own grammar (see PLAN2.md §8).
#pragma once

#include "backend/domain.h"

namespace MrkdwnParser {

// Parse Slack mrkdwn into plain text + entity offsets.
// Entity offsets are into the returned .text (not the original input).
// Marks nest (e.g. *<url|label>* yields a Link entity inside a Bold one);
// nested entities are fully contained within their parent's span.
TextWithEntities parse(const QString &mrkdwn);

// Resolve ONLY Slack's angle-bracket tokens (<@U>, <#C>, <!cmd> incl. <!date^…>,
// <url|label>) and :emoji: shortcodes in an already-structured text run — e.g. a
// rich_text "text" element, whose emphasis comes from its style object rather
// than mrkdwn marks. *, _, ~, ` are left literal. Use this instead of parse()
// when a bot (or Slack's text→rich_text conversion) leaves such tokens embedded
// in a plain text element.
TextWithEntities resolveTokens(const QString &src);

// Decode the HTML entities Slack escapes in every API text field
// (&lt; &gt; &amp;). parse() applies this itself; use directly for fields
// that are displayed without going through the parser (titles, footers…).
QString decodeEntities(QString s);

} // namespace MrkdwnParser

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Slack's composer stores pasted-URL labels already truncated ("host/path/…/…")
// — only the Link entity keeps the full URL. These helpers detect such labels
// so display can rebuild a longer one and copy/export can substitute the full
// URL (the visible "…" text is useless on the clipboard).
#pragma once

#include "backend/domain.h"

namespace LinkLabels {

// True when `label` reads as a truncated rendering of `url`: it contains an
// ellipsis and its fragments appear, in order, in the scheme-less URL.
bool isShortenedUrlLabel(const QString &label, const QString &url);

// Display label rebuilt from the full URL: scheme stripped, tail elided with
// '…' past maxChars.
QString expandedLabel(const QString &url, int maxChars);

// Plain text of `t` with every shortened link label replaced by that link's
// full URL — for clipboard/export paths.
QString plainTextWithFullUrls(const TextWithEntities &t);

} // namespace LinkLabels

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QString>
#include <QStringList>

namespace Emoji {

// Returns the Unicode character(s) for a Slack emoji short-name (no colons).
// Falls back to ":name:" for unknown names.
// Example: fromName("palm_tree") → "🌴"
QString fromName(const QString &name);

// Replaces all :code: patterns in text with their Unicode equivalents.
// Unknown codes are left as-is (":unknown:").
// Example: expandCodes("Hello :palm_tree: world") → "Hello 🌴 world"
QString expandCodes(const QString &text);

// All built-in short names, sorted alphabetically. Used for :code: autocomplete.
const QStringList &allNames();

} // namespace Emoji

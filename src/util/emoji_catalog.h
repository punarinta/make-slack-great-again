// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace Emoji {

// One emoji-picker category tab: a stable id (also the SVG icon base name),
// a human-readable label, and the built-in emoji short-names it contains in
// Slack's canonical sort order. Only names resolvable via Emoji::fromName()
// are included, so the picker never renders a ":name:" fallback.
struct Category {
    QString     id;
    QString     label;
    QStringList names;
};

// Ordered list of standard categories (Smileys & People, Animals & Nature, …).
// Built once; cheap to reference. Custom (workspace) emoji are NOT included —
// the picker appends those from the live Session.
const QVector<Category> &categories();

// True if the base emoji has a per-person Fitzpatrick skin variation, i.e. a
// global skin-tone selection can be applied to it by appending one modifier.
bool supportsSkinTone(const QString &name);

} // namespace Emoji

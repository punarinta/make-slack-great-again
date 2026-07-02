// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Maps Slack short codes to Unicode. The data lives in the generated
// emoji_data.cpp blob; this file only builds the lazy name index over it.
#include "emoji.h"

#include "emoji_catalog.h"
#include "emoji_data.h"

#include <QHash>

namespace Emoji {

namespace {

// name → index into Data::kEntries. Keys are zero-copy slices of the static
// blob, so first use allocates the hash nodes but no string payloads.
const QHash<QString, int> &indexByName() {
    static const QHash<QString, int> kIndex = [] {
        QHash<QString, int> h;
        h.reserve(Data::kEntryCount);
        for (int i = 0; i < Data::kEntryCount; ++i)
            h.insert(Data::entryName(Data::kEntries[i]), i);
        return h;
    }();
    return kIndex;
}

} // namespace

QString fromName(const QString &name) {
    const auto &idx = indexByName();
    const auto  it  = idx.constFind(name);
    return it != idx.constEnd() ? Data::entryValue(Data::kEntries[*it]) : (":" + name + ":");
}

QString expandCodes(const QString &text) {
    if (!text.contains(':'))
        return text;

    const auto &idx = indexByName();
    QString     result;
    result.reserve(text.size());
    int       i   = 0;
    const int len = text.size();
    while (i < len) {
        if (text[i] != ':') {
            result += text[i++];
            continue;
        }
        // Find closing colon
        int j = i + 1;
        while (j < len && text[j] != ':' && text[j] != ' ' && text[j] != '\n')
            ++j;
        if (j < len && text[j] == ':' && j > i + 1) {
            const QString code = text.mid(i + 1, j - i - 1);
            const auto    it   = idx.constFind(code);
            if (it != idx.constEnd()) {
                result += Data::entryValue(Data::kEntries[*it]);
                i = j + 1;
                continue;
            }
        }
        result += text[i++];
    }
    return result;
}

const QStringList &allNames() {
    static const QStringList kNames = [] {
        QStringList names;
        names.reserve(Data::kEntryCount);
        for (int i = 0; i < Data::kEntryCount; ++i)
            names.append(Data::entryName(Data::kEntries[i]));
        names.sort();
        return names;
    }();
    return kNames;
}

// Declared in emoji_catalog.h; defined here to reuse indexByName() instead of
// keeping a second name-keyed container alive.
bool supportsSkinTone(const QString &name) {
    const auto &idx = indexByName();
    const auto  it  = idx.constFind(name);
    return it != idx.constEnd() && (Data::kEntries[*it].flags & Data::kSkinToneFlag);
}

} // namespace Emoji

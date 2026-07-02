// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QString>

// Compact storage for the built-in emoji set, defined in the generated
// emoji_data.cpp (see scripts/gen-emoji-catalog.py). Every short-code name and
// unicode value lives in one UTF-16 blob referenced by offset, so the ~2000
// entries are pure .rodata — no QStringLiteral constructor code in the binary,
// no per-string heap payloads at first use. Consumers (emoji.cpp,
// emoji_catalog.cpp) build their lookup structures lazily from the zero-copy
// entryName()/entryValue() slices below.
namespace Emoji::Data {

// Entry::flags: the emoji has a per-person Fitzpatrick skin variation.
inline constexpr quint8 kSkinToneFlag = 0x01;

// One :short_code: → unicode mapping. The name occupies kStrings[off,
// off + nameLen); its value the units immediately after it.
struct Entry {
    quint32 off;
    quint8  nameLen;
    quint8  valueLen;
    quint8  flags;
};

// One picker category tab: ASCII id/label + the entries it lists (indexes
// into kEntries) in Slack's canonical sort order.
struct Category {
    const char    *id;
    const char    *label;
    const quint16 *entries;
    quint16        count;
};

extern const char16_t kStrings[];
extern const Entry    kEntries[];
extern const int      kEntryCount;
extern const Category kCategories[];
extern const int      kCategoryCount;

// Zero-copy views into kStrings — no allocation, safe to keep (the blob is
// immortal); any mutation by a caller detaches into a deep copy as usual.
inline QString entryName(const Entry &e) {
    return QString::fromRawData(reinterpret_cast<const QChar *>(kStrings + e.off), e.nameLen);
}
inline QString entryValue(const Entry &e) {
    return QString::fromRawData(
        reinterpret_cast<const QChar *>(kStrings + e.off + e.nameLen), e.valueLen
    );
}

} // namespace Emoji::Data

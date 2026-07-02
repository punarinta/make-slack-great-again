// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Picker catalog view over the generated emoji_data.cpp blob.
// supportsSkinTone() is defined in emoji.cpp (it reuses the name index there).
#include "emoji_catalog.h"

#include "emoji_data.h"

namespace Emoji {

const QVector<Category> &categories() {
    static const QVector<Category> kCategories = [] {
        QVector<Category> cats;
        cats.reserve(Data::kCategoryCount);
        for (int c = 0; c < Data::kCategoryCount; ++c) {
            const auto &cd = Data::kCategories[c];
            Category    cat;
            cat.id    = QString::fromLatin1(cd.id);
            cat.label = QString::fromLatin1(cd.label);
            cat.names.reserve(cd.count);
            for (int i = 0; i < cd.count; ++i)
                cat.names.append(Data::entryName(Data::kEntries[cd.entries[i]]));
            cats.append(cat);
        }
        return cats;
    }();
    return kCategories;
}

} // namespace Emoji

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "text/link_labels.h"

#include <QRegularExpression>
#include <algorithm>

namespace LinkLabels {

namespace {

constexpr QChar kEllipsis(0x2026); // '…'

QString schemeless(const QString &url) {
    static const QRegularExpression kScheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*://"));
    QString                         u = url;
    u.remove(kScheme);
    return u;
}

} // namespace

bool isShortenedUrlLabel(const QString &label, const QString &url) {
    if (url.isEmpty())
        return false;
    // Slack uses '…'; normalise a literal "..." to it too. A label whose
    // ellipsis-split fragments all appear in order in the URL is URL-derived
    // either way, so the substitution stays safe on a false positive.
    QString l = label;
    l.replace(QStringLiteral("..."), QString(kEllipsis));
    if (!l.contains(kEllipsis))
        return false;
    const QString     u      = schemeless(url);
    const QStringList pieces = l.split(kEllipsis, Qt::SkipEmptyParts);
    if (pieces.isEmpty() || !u.startsWith(pieces[0]))
        return false;
    int from = pieces[0].size();
    for (int i = 1; i < pieces.size(); ++i) {
        const int at = u.indexOf(pieces[i], from);
        if (at < 0)
            return false;
        from = at + pieces[i].size();
    }
    return true;
}

QString expandedLabel(const QString &url, int maxChars) {
    QString u = schemeless(url);
    if (u.size() > maxChars)
        u = u.left(std::max(1, maxChars - 1)) + kEllipsis;
    return u;
}

QString plainTextWithFullUrls(const TextWithEntities &t) {
    // Links never nest inside each other, but entities are stored
    // parent-before-child, so sort the matches back into text order.
    std::vector<const TextEntity *> links;
    for (const auto &e : t.entities) {
        if (e.type == EntityType::Link && !e.data.isEmpty() &&
            isShortenedUrlLabel(t.text.mid(e.offset, e.length), e.data))
            links.push_back(&e);
    }
    if (links.empty())
        return t.text;
    std::sort(links.begin(), links.end(), [](const TextEntity *a, const TextEntity *b) {
        return a->offset < b->offset;
    });
    QString out;
    out.reserve(t.text.size());
    int pos = 0;
    for (const auto *e : links) {
        if (e->offset < pos)
            continue;
        out += t.text.mid(pos, e->offset - pos);
        out += e->data;
        pos = e->offset + e->length;
    }
    out += t.text.mid(pos);
    return out;
}

} // namespace LinkLabels

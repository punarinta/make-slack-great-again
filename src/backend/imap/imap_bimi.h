// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// BIMI (Brand Indicators for Message Identification) avatar resolution. A sender
// *domain* may publish a brand logo via a `default._bimi.<domain>` DNS TXT record
// (`v=BIMI1; l=<https logo svg>; …`); providers like Gmail/Apple Mail render it.
// This resolves that logo URL per domain (cached, deduped) so brand senders get a
// real avatar; a miss falls back to the domain favicon (imap_favicon), then
// Gravatar/initials. Below the seam, no UI.
#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

namespace imap {

class BimiResolver : public QObject {
    Q_OBJECT
public:
    explicit BimiResolver(QObject *parent = nullptr);

    // Resolve a domain's BIMI logo. Always emits resolved() exactly once per
    // domain (cached); concurrent calls for the same domain coalesce.
    void resolve(const QString &domain);

signals:
    // logoUrl is empty when the domain publishes no usable BIMI logo.
    void resolved(QString domain, QString logoUrl);

private:
    QHash<QString, QString> _cache;    // domain → logo URL ("" = checked, none)
    QSet<QString>           _inflight; // domains with a DNS lookup in progress
};

} // namespace imap

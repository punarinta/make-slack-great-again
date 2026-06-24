// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_bimi.h"

#include <QDnsLookup>

namespace imap {

namespace {
// Extract the `l=` logo URL from a BIMI record value. Only https URLs accepted.
QString bimiLogo(const QString &record) {
    if (!record.contains(QLatin1String("BIMI1"), Qt::CaseInsensitive))
        return {};
    for (QString tok : record.split(QLatin1Char(';'))) {
        tok = tok.trimmed();
        if (tok.startsWith(QLatin1String("l="), Qt::CaseInsensitive)) {
            const QString url = tok.mid(2).trimmed();
            if (url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
                return url;
        }
    }
    return {};
}
} // namespace

BimiResolver::BimiResolver(QObject *parent) : QObject(parent) {}

void BimiResolver::resolve(const QString &domain) {
    const QString d = domain.trimmed().toLower();
    if (d.isEmpty()) {
        emit resolved(domain, {});
        return;
    }
    if (_cache.contains(d)) {
        emit resolved(d, _cache.value(d));
        return;
    }
    if (_inflight.contains(d))
        return; // a lookup is already running; its finish will emit
    _inflight.insert(d);

    auto *dns = new QDnsLookup(QDnsLookup::TXT, QStringLiteral("default._bimi.") + d, this);
    connect(dns, &QDnsLookup::finished, this, [this, d, dns] {
        dns->deleteLater();
        QString url;
        if (dns->error() == QDnsLookup::NoError)
            for (const QDnsTextRecord &r : dns->textRecords()) {
                QByteArray full;
                for (const QByteArray &v : r.values())
                    full += v;
                url = bimiLogo(QString::fromLatin1(full));
                if (!url.isEmpty())
                    break;
            }
        _cache.insert(d, url);
        _inflight.remove(d);
        emit resolved(d, url);
    });
    dns->lookup();
}

} // namespace imap

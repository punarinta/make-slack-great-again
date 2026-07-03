// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Domain favicon avatar resolution — the fallback when a sender domain has no
// BIMI record. Probes the domain's own web icons in order: apple-touch-icon.png
// (high-res, brand-styled), <link rel=icon> candidates scraped from the
// homepage, /favicon.ico; if the whole chain misses and the domain is a
// subdomain (em.example.com — common for newsletter senders), the parent domain
// is retried once. Every candidate is downloaded and validated as a decodable
// image before its URL is handed out, so the UI never trades initials for a
// broken image. Same seam as BimiResolver: resolve(domain) →
// resolved(domain, url), "" meaning no usable icon. Below the seam, no UI.
#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>

namespace imap {

class FaviconResolver : public QObject {
    Q_OBJECT
public:
    explicit FaviconResolver(QObject *parent = nullptr);

    // Resolve a domain's favicon. Always emits resolved() exactly once per
    // domain (cached); concurrent calls for the same domain coalesce. At most
    // a few domains are probed at a time — the rest queue.
    void resolve(const QString &domain);

    // Pure helpers, public for tests.
    // Icon <link>s from homepage HTML, best first (apple-touch-icon beats plain
    // icon, larger `sizes` beats smaller; mask-icon is skipped — it's a
    // monochrome outline), hrefs resolved against baseUrl, data:/exotic schemes
    // dropped, duplicates removed.
    static QStringList iconCandidatesFromHtml(const QString &html, const QUrl &baseUrl);
    // "em.news.example.com" → "example.com"; ≤2-label domains return unchanged.
    // (No public-suffix list — "example.co.uk" → "co.uk" just probes and misses.)
    static QString     parentDomain(const QString &domain);
    // True when the bytes decode as a usable image (≥8px; SVG sniffed by
    // content, matching ImageCache's QSvgRenderer fallback).
    static bool        looksLikeImage(const QByteArray &bytes);

signals:
    // iconUrl is empty when the domain serves no usable icon.
    void resolved(QString domain, QString iconUrl);

private:
    struct Probe {
        QString       domain;               // requested domain (cache/emit key)
        QString       host;                 // host being probed (domain, then its parent)
        QStringList   queue;                // icon URLs still to try on this host
        QSet<QString> tried;                // URLs already fetched (skip repeats)
        bool          homepageDone = false; // homepage <link> scan ran for this host
        bool          parentTried  = false;
    };

    void startProbe(const QString &domain);
    void step(std::shared_ptr<Probe> p);
    void finish(const QString &domain, const QString &iconUrl);
    // GET `url` capped at maxBytes; done(body, finalUrl) with body empty on
    // failure. keepPartial keeps what arrived when the cap/timeout cuts the
    // transfer (homepage HTML: the <head> is all we need).
    void fetch(
        const QUrl &url, int maxBytes, bool keepPartial, std::function<void(QByteArray, QUrl)> done
    );

    QHash<QString, QString> _cache;    // domain → icon URL ("" = checked, none)
    QSet<QString>           _inflight; // resolving now, or queued behind the cap
    QQueue<QString>         _waiting;  // domains queued behind kMaxConcurrent
    int                     _active = 0;
};

} // namespace imap

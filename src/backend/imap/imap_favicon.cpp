// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_favicon.h"

#include "network/shared_nam.h"

#include <QFutureWatcher>
#include <QImage>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QtConcurrent>

#include <algorithm>

namespace imap {

namespace {
constexpr int kMaxIconBytes      = 512 * 1024; // larger than any sane favicon
constexpr int kMaxHtmlBytes      = 128 * 1024; // <link>s live in <head>; no need for more
constexpr int kMaxHtmlCandidates = 4;          // best-scored homepage icons to probe
// Domains probed at once. A cold 500-message mailbox can hold ~250 distinct
// sender domains at up to ~6 requests (with 10 s timeouts) each — at 4-wide
// that outlived typical sessions, so half the domains never got probed before
// quit. 8-wide halves the wall time; per-domain requests stay sequential.
constexpr int kMaxConcurrent     = 8;
constexpr int kRequestTimeoutMs  = 10000;

QString hostUrl(const QString &host, const char *path) {
    return QStringLiteral("https://") + host + QLatin1String(path);
}
} // namespace

FaviconResolver::FaviconResolver(QObject *parent) : QObject(parent) {}

void FaviconResolver::resolve(const QString &domain) {
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
        return; // a probe is already running/queued; its finish will emit
    _inflight.insert(d);
    if (_active >= kMaxConcurrent) {
        _waiting.enqueue(d);
        return;
    }
    startProbe(d);
}

void FaviconResolver::startProbe(const QString &domain) {
    ++_active;
    auto p    = std::make_shared<Probe>();
    p->domain = domain;
    p->host   = domain;
    p->queue  = {hostUrl(domain, "/apple-touch-icon.png")};
    step(p);
}

void FaviconResolver::step(std::shared_ptr<Probe> p) {
    // 1. Try queued icon URLs, first hit wins.
    while (!p->queue.isEmpty()) {
        const QString url = p->queue.takeFirst();
        if (p->tried.contains(url))
            continue;
        p->tried.insert(url);
        fetch(
            QUrl(url), kMaxIconBytes, false, [this, p, url](const QByteArray &body, const QUrl &) {
                if (body.isEmpty()) {
                    step(p);
                    return;
                }
                // SPA catch-alls answer 200 + HTML for missing icons — the
                // decode check is what rejects those, not the status code.
                // QImage::fromData fully decodes (up to 512 KB), so it runs on
                // a worker thread; only the verdict comes back here.
                auto *w = new QFutureWatcher<bool>(this);
                connect(w, &QFutureWatcher<bool>::finished, this, [this, w, p, url] {
                    const bool ok = w->result();
                    w->deleteLater();
                    if (ok)
                        finish(p->domain, url);
                    else
                        step(p);
                });
                w->setFuture(QtConcurrent::run([body] { return looksLikeImage(body); }));
            }
        );
        return;
    }
    // 2. Scrape the homepage <head> for <link rel=icon> candidates.
    if (!p->homepageDone) {
        p->homepageDone = true;
        const QUrl home(hostUrl(p->host, "/"));
        fetch(
            home,
            kMaxHtmlBytes,
            true,
            [this, p, home](const QByteArray &body, const QUrl &finalUrl) {
                if (body.isEmpty()) {
                    p->queue << hostUrl(p->host, "/favicon.ico");
                    step(p);
                    return;
                }
                // Resolve relative hrefs against where redirects landed
                // (example.com → www.example.com), not where we started. The
                // regex scan of up to 128 KB HTML runs on a worker thread.
                const QUrl base = finalUrl.isEmpty() ? home : finalUrl;
                auto      *w    = new QFutureWatcher<QStringList>(this);
                connect(w, &QFutureWatcher<QStringList>::finished, this, [this, w, p] {
                    p->queue = w->result().mid(0, kMaxHtmlCandidates);
                    w->deleteLater();
                    p->queue << hostUrl(p->host, "/favicon.ico");
                    step(p);
                });
                w->setFuture(QtConcurrent::run([body, base] {
                    return iconCandidatesFromHtml(QString::fromUtf8(body), base);
                }));
            }
        );
        return;
    }
    // 3. Whole chain missed. Newsletters often send from a dedicated subdomain
    // (em.example.com) whose web root serves nothing — hop to the parent domain
    // once and retry the chain there.
    const QString parent = parentDomain(p->host);
    if (!p->parentTried && parent != p->host) {
        p->parentTried  = true;
        p->host         = parent;
        p->homepageDone = false;
        p->queue        = {hostUrl(parent, "/apple-touch-icon.png")};
        step(p);
        return;
    }
    finish(p->domain, {});
}

void FaviconResolver::finish(const QString &domain, const QString &iconUrl) {
    _cache.insert(domain, iconUrl);
    _inflight.remove(domain);
    --_active;
    emit resolved(domain, iconUrl);
    while (_active < kMaxConcurrent && !_waiting.isEmpty())
        startProbe(_waiting.dequeue());
}

void FaviconResolver::fetch(
    const QUrl &url, int maxBytes, bool keepPartial, std::function<void(QByteArray, QUrl)> done
) {
    QNetworkRequest req(url);
    req.setTransferTimeout(kRequestTimeoutMs);
    auto *reply = net::sharedNam()->get(req);
    reply->setParent(this); // die with the resolver, not with the shared NAM
    // Drain as data arrives — abort() closes the device, so anything unread at
    // that point is lost; reading in the finished handler alone would make the
    // over-cap partial-HTML case come back empty.
    auto body = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, reply, [reply, body, maxBytes] {
        *body += reply->readAll();
        if (body->size() > maxBytes)
            reply->abort();
    });
    connect(
        reply, &QNetworkReply::finished, this, [reply, body, keepPartial, done = std::move(done)] {
            reply->deleteLater();
            const int  status  = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool aborted = reply->error() == QNetworkReply::OperationCanceledError;
            const bool ok      = status == 200 &&
                            (reply->error() == QNetworkReply::NoError || (aborted && keepPartial));
            done(ok ? *body : QByteArray{}, reply->url());
        }
    );
}

QStringList FaviconResolver::iconCandidatesFromHtml(const QString &html, const QUrl &baseUrl) {
    struct Cand {
        int     score;
        QString url;
    };
    static const QRegularExpression tagRe(
        QStringLiteral("<link\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression attrRe(
        QStringLiteral(R"re(([\w-]+)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'>]+)))re"),
        QRegularExpression::CaseInsensitiveOption
    );

    QList<Cand> cands;
    auto        tags = tagRe.globalMatch(html);
    while (tags.hasNext()) {
        const QString tag = tags.next().captured(0);
        QString       rel, href, sizes;
        auto          attrs = attrRe.globalMatch(tag);
        while (attrs.hasNext()) {
            const auto    m    = attrs.next();
            // Exactly one of the three value groups participates.
            const QString val  = m.captured(2) + m.captured(3) + m.captured(4);
            const QString name = m.captured(1).toLower();
            if (name == QLatin1String("rel"))
                rel = val.toLower();
            else if (name == QLatin1String("href"))
                href = val.trimmed();
            else if (name == QLatin1String("sizes"))
                sizes = val.toLower();
        }
        const QStringList tokens = rel.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        int               base   = 0;
        if (tokens.contains(QLatin1String("apple-touch-icon")) ||
            tokens.contains(QLatin1String("apple-touch-icon-precomposed")))
            base = 300;
        else if (tokens.contains(QLatin1String("icon"))) // also rel="shortcut icon"
            base = 100;                                  // exact token — mask-icon won't match
        if (base == 0 || href.isEmpty())
            continue;
        int side = 0; // sizes="32x32 64x64" / "any" — bigger is a better avatar
        for (const QString &s : sizes.split(QLatin1Char(' '), Qt::SkipEmptyParts))
            side = qMax(
                side,
                s == QLatin1String("any") ? 256
                                          : qMin(s.section(QLatin1Char('x'), 0, 0).toInt(), 512)
            );
        QUrl u(href);
        if (u.isRelative())
            u = baseUrl.resolved(u);
        if (u.scheme() != QLatin1String("https") && u.scheme() != QLatin1String("http"))
            continue; // data:, chrome-extension:, …
        cands.append({base + side, u.toString()});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
        return a.score > b.score;
    });
    QStringList out;
    for (const Cand &c : cands)
        if (!out.contains(c.url))
            out << c.url;
    return out;
}

QString FaviconResolver::parentDomain(const QString &domain) {
    const QStringList labels = domain.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (labels.size() <= 2)
        return domain;
    return labels.mid(labels.size() - 2).join(QLatin1Char('.'));
}

bool FaviconResolver::looksLikeImage(const QByteArray &bytes) {
    if (bytes.isEmpty())
        return false;
    const QImage img = QImage::fromData(bytes);
    if (!img.isNull())
        return img.width() >= 8 && img.height() >= 8; // 1×1 trackers/placeholders don't count
    // SVG has no QImage handler; ImageCache renders it via its QSvgRenderer
    // fallback (the BIMI-logo path). Sniff content rather than link Qt6::Svg here.
    const QByteArray head = bytes.left(4096).trimmed().toLower();
    return head.startsWith("<svg") || (head.startsWith("<?xml") && head.contains("<svg"));
}

} // namespace imap

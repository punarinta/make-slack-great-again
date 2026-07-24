// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/slack/session_import/session_migrator.h"

#include "backend/slack/session_import/token_deriver.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace slack::session {

namespace {
constexpr int kTransferTimeoutMs = 20000;

// Same rationale as TokenDeriver's jar: cookie must ride via the jar (survives
// redirects; not clobbered by QNAM's automatic cookie handling).
class SeededCookieJar : public QNetworkCookieJar {
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    void seed(const QNetworkCookie &c) { insertCookie(c); }
};
} // namespace

SessionMigrator::SessionMigrator(QObject *parent)
    : QObject(parent), _nam(new QNetworkAccessManager(this)) {}

void SessionMigrator::run(const QString &cookie, const QList<Item> &items) {
    _cookie = cookie;
    _items  = items;
    _idx    = 0;
    _resolved.clear();
    _lastError.clear();

    auto          *jar = new SeededCookieJar(_nam);
    QNetworkCookie c("d", _cookie.toUtf8());
    c.setDomain(QStringLiteral(".slack.com"));
    c.setPath(QStringLiteral("/"));
    jar->seed(c);
    _nam->setCookieJar(jar);

    resolveNext();
}

void SessionMigrator::resolveNext() {
    if (_idx >= _items.size()) {
        if (_resolved.isEmpty()) {
            emit finished({}, _lastError.isEmpty() ? QStringLiteral("no_workspaces") : _lastError);
            return;
        }
        // Domains resolved → derive + validate session tokens against the cookie.
        _deriver = new TokenDeriver(this);
        connect(_deriver, &TokenDeriver::finished, this, &SessionMigrator::finished);
        _deriver->run(_cookie, _resolved);
        return;
    }

    const Item      item = _items.at(_idx);
    QNetworkRequest req{QUrl(QStringLiteral("https://slack.com/api/team.info"))};
    req.setRawHeader("Authorization", QByteArray("Bearer ") + item.currentToken.toUtf8());
    req.setTransferTimeout(kTransferTimeoutMs); // `d` cookie supplied by the jar
    auto *reply = _nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, item]() {
        reply->deleteLater();
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (obj.value(QStringLiteral("ok")).toBool()) {
            const QString domain = obj.value(QStringLiteral("team"))
                                       .toObject()
                                       .value(QStringLiteral("domain"))
                                       .toString();
            if (!domain.isEmpty()) {
                TeamSession t;
                t.workspaceUrl = QStringLiteral("https://") + domain + QStringLiteral(".slack.com");
                t.teamId       = item.teamId;
                t.teamName     = item.teamName;
                t.iconUrl      = item.iconUrl;
                _resolved.append(t);
            } else {
                _lastError = QStringLiteral("no_domain");
            }
        } else {
            _lastError =
                obj.value(QStringLiteral("error")).toString(QStringLiteral("team_info_failed"));
        }
        ++_idx;
        resolveNext();
    });
}

} // namespace slack::session

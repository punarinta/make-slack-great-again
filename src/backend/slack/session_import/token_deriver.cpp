// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/slack/session_import/token_deriver.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

namespace slack::session {

namespace {

constexpr int kTransferTimeoutMs = 20000;

// Cookie jar that lets us seed the `d` cookie directly. The session cookie MUST
// live in the jar, not a per-request raw header: the workspace boot URL 302-chains
// (/ → /messages → /ssb/redirect) and QNetworkAccessManager rebuilds each redirected
// request from the jar, dropping any manually-set Cookie header — so a raw header
// authenticates only the first hop and the token-bearing final page loads logged out.
class SeededCookieJar : public QNetworkCookieJar {
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    void seed(const QNetworkCookie &c) { insertCookie(c); }
};

// The web client boot payload embeds the user's session api_token.
// (The `d` cookie is provided by the jar above, not a per-request header.) Newer Slack
// serves it as "api_token":"xoxc-…"; scrape that, falling back to any xoxc- run.
QString scrapeToken(const QByteArray &html) {
    static const QRegularExpression keyed(QStringLiteral("\"api_token\":\"(xoxc-[A-Za-z0-9-]+)\""));
    const auto                      m = keyed.match(QString::fromUtf8(html));
    if (m.hasMatch())
        return m.captured(1);
    static const QRegularExpression bare(QStringLiteral("xoxc-[A-Za-z0-9-]+"));
    const auto                      m2 = bare.match(QString::fromUtf8(html));
    return m2.hasMatch() ? m2.captured(0) : QString();
}

} // namespace

TokenDeriver::TokenDeriver(QObject *parent)
    : QObject(parent), _nam(new QNetworkAccessManager(this)) {}

void TokenDeriver::run(const QString &cookie, const QList<TeamSession> &candidates) {
    _cookie = cookie;
    _queue  = candidates;
    _idx    = 0;
    _valid.clear();
    _lastError.clear();

    // Seed the `d` cookie for all *.slack.com so it rides every request and every
    // redirect hop (see SeededCookieJar). Value is sent verbatim (already xoxd-…,
    // percent-encoded as Slack stores it).
    auto          *jar = new SeededCookieJar(_nam);
    QNetworkCookie c("d", _cookie.toUtf8());
    c.setDomain(QStringLiteral(".slack.com"));
    c.setPath(QStringLiteral("/"));
    jar->seed(c);
    _nam->setCookieJar(jar); // NAM takes ownership

    processNext();
}

void TokenDeriver::processNext() {
    if (_idx >= _queue.size()) {
        // Success if we validated at least one workspace; otherwise surface the
        // last error so the UI can explain why (e.g. invalid_auth on a stale cookie).
        emit finished(_valid, _valid.isEmpty() ? _lastError : QString());
        return;
    }
    const TeamSession cand = _queue.at(_idx);
    if (!cand.token.isEmpty()) {
        validate(cand, cand.token);
        return;
    }
    deriveToken(cand, [this, cand](QString token, QString reason) {
        if (token.isEmpty()) {
            _lastError = reason;
            ++_idx;
            processNext();
            return;
        }
        validate(cand, token);
    });
}

void TokenDeriver::deriveToken(
    const TeamSession &cand, std::function<void(QString, QString)> onToken
) {
    if (cand.workspaceUrl.isEmpty()) {
        onToken(QString(), QStringLiteral("no_workspace_url"));
        return;
    }
    QNetworkRequest req{QUrl(cand.workspaceUrl)};
    // Cookie comes from the jar (seeded in run()) so it survives the redirect chain.
    req.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );
    req.setTransferTimeout(kTransferTimeoutMs);
    auto *reply = _nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, onToken]() {
        reply->deleteLater();
        // NEVER gate the scrape on reply->error(): Slack serves the *logged-in* boot
        // page of a workspace root with HTTP 403 (Qt: ContentAccessDenied), token and
        // all. Bailing on the status threw away a perfectly good token and reported
        // it as an unverifiable session — which broke every manual cookie sign-in.
        // A body is the only thing that matters; a logged-out page has no xoxc- run
        // in it ("api_token":null), so a stale cookie still fails, just accurately.
        const QByteArray body = reply->readAll();
        if (body.isEmpty()) {
            onToken(QString(), QStringLiteral("network"));
            return;
        }
        const QString token = scrapeToken(body);
        onToken(token, token.isEmpty() ? QStringLiteral("token_not_found") : QString());
    });
}

void TokenDeriver::validate(const TeamSession &cand, const QString &token) {
    QNetworkRequest req{QUrl(_apiBase + QStringLiteral("auth.test"))};
    req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
    req.setTransferTimeout(kTransferTimeoutMs); // `d` cookie supplied by the jar
    auto *reply = _nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cand, token]() {
        reply->deleteLater();
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (!obj.value(QStringLiteral("ok")).toBool()) {
            _lastError = obj.value(QStringLiteral("error")).toString(QStringLiteral("auth_failed"));
            ++_idx;
            processNext();
            return;
        }
        slack::Credentials creds;
        creds.xoxp         = token;
        creds.cookie       = _cookie;
        creds.teamId       = obj.value(QStringLiteral("team_id")).toString(cand.teamId);
        creds.teamName     = obj.value(QStringLiteral("team")).toString(cand.teamName);
        creds.iconUrl      = cand.iconUrl;
        // Store the workspace URL so the token can be re-derived from a future
        // fresh cookie. auth.test's `url` is authoritative; fall back to the input.
        creds.workspaceUrl = obj.value(QStringLiteral("url")).toString(cand.workspaceUrl);
        fetchIconThenCommit(std::move(creds));
    });
}

void TokenDeriver::fetchIconThenCommit(slack::Credentials creds) {
    if (!creds.iconUrl.isEmpty()) {
        commit(std::move(creds));
        return;
    }
    QNetworkRequest req{QUrl(_apiBase + QStringLiteral("team.info"))};
    req.setRawHeader("Authorization", QByteArray("Bearer ") + creds.xoxp.toUtf8());
    req.setTransferTimeout(kTransferTimeoutMs); // `d` cookie supplied by the jar
    auto *reply = _nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, creds]() mutable {
        reply->deleteLater();
        // Icon is best-effort — commit the workspace regardless of team.info.
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (obj.value(QStringLiteral("ok")).toBool()) {
            const auto team = obj.value(QStringLiteral("team")).toObject();
            const auto icon = team.value(QStringLiteral("icon")).toObject();
            creds.iconUrl   = icon.value(QStringLiteral("image_88"))
                                .toString(icon.value(QStringLiteral("image_68")).toString());
            if (creds.teamName.isEmpty())
                creds.teamName = team.value(QStringLiteral("name")).toString();
        }
        commit(std::move(creds));
    });
}

void TokenDeriver::commit(slack::Credentials creds) {
    _valid.append(std::move(creds));
    ++_idx;
    processNext();
}

} // namespace slack::session

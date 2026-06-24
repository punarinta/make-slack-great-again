// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_server_detect.h"

#include <QDnsLookup>
#include <QSslSocket>
#include <QTimer>

namespace imap {

namespace {
constexpr int kProbeTimeoutMs = 3500;
constexpr int kMaxCandidates  = 5;

// Last two labels of a host ("mail.privateemail.com" → "privateemail.com").
QString registrableTail(const QString &host) {
    const QStringList parts = host.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (parts.size() <= 2)
        return host;
    return parts.mid(parts.size() - 2).join(QLatin1Char('.'));
}

// A generic detected server: known host, password auth, guessed SMTP.
ProviderInfo genericServer(const QString &domain, const QString &imapHost, quint16 port = 993) {
    ProviderInfo p;
    p.name     = domain;
    p.imapHost = imapHost;
    p.imapPort = port;
    p.smtpHost = QStringLiteral("smtp.") + domain;
    p.auth     = AuthMethod::Password;
    p.known    = true;
    return p;
}
} // namespace

ServerDetector::ServerDetector(QObject *parent) : QObject(parent) {}

void ServerDetector::resolve(const QString &email) {
    _email  = email.trimmed();
    _domain = _email.section(QLatin1Char('@'), 1).toLower();
    _candidates.clear();
    _probeIdx = 0;
    _done     = false;
    _guess    = detectProvider(_email);
    if (_domain.isEmpty()) {
        emitUnknown();
        return;
    }
    if (_guess.known) { // known domain (e.g. gmail.com) — no DNS needed
        emitInfo(_guess);
        return;
    }
    startSrv();
}

void ServerDetector::startSrv() {
    auto *srv = new QDnsLookup(QDnsLookup::SRV, QStringLiteral("_imaps._tcp.") + _domain, this);
    connect(srv, &QDnsLookup::finished, this, [this, srv] {
        srv->deleteLater();
        if (srv->error() == QDnsLookup::NoError)
            for (const QDnsServiceRecord &r : srv->serviceRecords()) {
                QString target = r.target();
                if (!target.isEmpty() && target != QLatin1String(".")) {
                    if (target.endsWith('.'))
                        target.chop(1);
                    emitInfo(genericServer(_domain, target, r.port() ? r.port() : 993));
                    return;
                }
            }
        startMx();
    });
    srv->lookup();
}

void ServerDetector::addCandidate(const QString &host) {
    if (!host.isEmpty() && !_candidates.contains(host) && _candidates.size() < kMaxCandidates)
        _candidates.append(host);
}

void ServerDetector::startMx() {
    auto *mx = new QDnsLookup(QDnsLookup::MX, _domain, this);
    connect(mx, &QDnsLookup::finished, this, [this, mx] {
        mx->deleteLater();
        if (mx->error() == QDnsLookup::NoError) {
            // First: is the domain hosted on a recognized provider? (MX → Gmail/…)
            for (const QDnsMailExchangeRecord &r : mx->mailExchangeRecords()) {
                const ProviderInfo mp = providerForMxHost(r.exchange());
                if (mp.known) {
                    emitInfo(mp);
                    return;
                }
            }
        }
        // Otherwise probe conventional hosts + the MX provider's "imap.<provider>"
        // (e.g. MX mail.privateemail.com → probe imap.privateemail.com).
        addCandidate(QStringLiteral("imap.") + _domain);
        addCandidate(QStringLiteral("mail.") + _domain);
        if (mx->error() == QDnsLookup::NoError)
            for (const QDnsMailExchangeRecord &r : mx->mailExchangeRecords()) {
                QString t = r.exchange();
                if (t.endsWith('.'))
                    t.chop(1);
                if (!t.isEmpty())
                    addCandidate(QStringLiteral("imap.") + registrableTail(t));
            }
        probeNext();
    });
    mx->lookup();
}

void ServerDetector::probeNext() {
    if (_done)
        return;
    if (_probeIdx >= _candidates.size()) {
        emitUnknown();
        return;
    }
    const QString host = _candidates.at(_probeIdx++);

    _probe   = new QSslSocket(this);
    _timeout = new QTimer(this);
    _timeout->setSingleShot(true);
    _timeout->setInterval(kProbeTimeoutMs);

    auto cleanup = [this] {
        if (_timeout) {
            _timeout->stop();
            _timeout->deleteLater();
            _timeout = nullptr;
        }
        if (_probe) {
            _probe->disconnect(this);
            _probe->abort();
            _probe->deleteLater();
            _probe = nullptr;
        }
    };
    connect(_probe, &QSslSocket::encrypted, this, [this, host, cleanup] {
        cleanup();
        emitInfo(genericServer(_domain, host)); // clean IMAPS handshake → this is it
    });
    connect(
        _probe, &QSslSocket::errorOccurred, this, [this, cleanup](QAbstractSocket::SocketError) {
            cleanup();
            probeNext();
        }
    );
    connect(_timeout, &QTimer::timeout, this, [this, cleanup] {
        cleanup();
        probeNext();
    });
    _timeout->start();
    _probe->connectToHostEncrypted(host, 993);
}

void ServerDetector::emitInfo(const ProviderInfo &info) {
    if (_done)
        return;
    _done = true;
    emit resolved(info);
}

void ServerDetector::emitUnknown() {
    if (_done)
        return;
    _done = true;
    emit resolved(_guess); // known == false → UI prompts for manual entry
}

} // namespace imap

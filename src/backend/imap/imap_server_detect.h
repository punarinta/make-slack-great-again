// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Async account/server discovery for the add-account flow. Resolves a full
// ProviderInfo from an email address by trying, in order: (1) the known-domain
// table (detectProvider); (2) RFC 6186 SRV (_imaps._tcp.<domain>); (3) the
// domain's MX host mapped to a known provider (so a custom domain hosted on
// Gmail/Outlook resolves to imap.gmail.com + the right auth method); (4) common
// candidate hosts probed by a real implicit-TLS connect on 993. Emits a
// ProviderInfo whose `known` is false only if nothing resolved.
#pragma once

#include "backend/imap/imap_providers.h"

#include <QObject>
#include <QString>
#include <QStringList>

class QSslSocket;
class QSslError;
class QTimer;

namespace imap {

class ServerDetector : public QObject {
    Q_OBJECT
public:
    explicit ServerDetector(QObject *parent = nullptr);

    void resolve(const QString &email);

signals:
    void resolved(ProviderInfo info); // info.known == false ⇒ couldn't detect

private:
    void startSrv();
    void startMx(); // recognize provider from MX, else gather + probe candidates
    void probeNext();
    void emitInfo(const ProviderInfo &info);
    void emitUnknown();
    void addCandidate(const QString &host);

    QString      _email;
    QString      _domain;
    ProviderInfo _guess; // detectProvider() result — the unknown fallback
    QStringList  _candidates;
    int          _probeIdx = 0;
    QSslSocket  *_probe    = nullptr;
    QTimer      *_timeout  = nullptr;
    bool         _done     = false;
};

} // namespace imap

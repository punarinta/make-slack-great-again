// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Async SMTP submission client (imap-backend-plan Phase 4) over QSslSocket:
// port 465 = implicit TLS, else STARTTLS on 587. Runs EHLO → [STARTTLS] → AUTH
// LOGIN → MAIL FROM → RCPT TO* → DATA → message → QUIT, reporting ok/err once.
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QSslSocket;
class QSslError;
class QTimer;

namespace imap {

class SmtpClient : public QObject {
    Q_OBJECT
public:
    explicit SmtpClient(QObject *parent = nullptr);
    ~SmtpClient() override;

    void setInsecure(bool v) { _insecure = v; }

    using Done = std::function<void(bool ok, QString error)>;
    // Submit `rawMessage` (RFC 5322, CRLF) from `fromEmail` to `recipients`. When
    // `xoauth2` is true, `secret` is an OAuth bearer access token (AUTH XOAUTH2);
    // otherwise it is the password (AUTH LOGIN).
    void send(
        const QString     &host,
        quint16            port,
        const QString     &user,
        const QString     &secret,
        const QString     &fromEmail,
        const QStringList &recipients,
        const QByteArray  &rawMessage,
        Done               done,
        bool               xoauth2 = false
    );

private:
    enum class State {
        Greeting,
        Ehlo,
        StartTls,
        EhloTls,
        AuthUser,
        AuthPass,
        XOAuth,
        MailFrom,
        Rcpt,
        Data,
        Body,
        Quit
    };

    void onReadyRead();
    void onEncrypted();
    void beginAuth();                   // AUTH LOGIN or AUTH XOAUTH2 depending on _xoauth2
    void write(const QByteArray &line); // appends CRLF
    void advance(int code, const QByteArray &text);
    void finish(bool ok, const QString &err);
    int  takeResponse(QByteArray &out); // -1 if a full response isn't buffered yet

    QSslSocket *_sock  = nullptr;
    QTimer     *_timer = nullptr;
    QByteArray  _buf;
    State       _state = State::Greeting;

    QString     _host, _user, _pass, _from;
    quint16     _port = 587;
    QStringList _rcpts;
    int         _rcptIdx = 0;
    QByteArray  _message;
    Done        _done;
    bool        _insecure    = false;
    bool        _implicitTls = false;
    bool        _xoauth2     = false; // _pass is a bearer token, use AUTH XOAUTH2
    bool        _finished    = false;
};

} // namespace imap

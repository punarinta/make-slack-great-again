// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_smtp.h"

#include <QSslSocket>
#include <QTimer>

namespace imap {

namespace {
// SMTP DATA dot-stuffing: a line starting with '.' is escaped to "..".
QByteArray dotStuff(QByteArray msg) {
    msg.replace("\r\n.", "\r\n..");
    if (msg.startsWith('.'))
        msg.prepend('.');
    if (!msg.endsWith("\r\n"))
        msg += "\r\n";
    return msg;
}
} // namespace

SmtpClient::SmtpClient(QObject *parent) : QObject(parent) {}
SmtpClient::~SmtpClient() = default;

void SmtpClient::send(
    const QString     &host,
    quint16            port,
    const QString     &user,
    const QString     &secret,
    const QString     &fromEmail,
    const QStringList &recipients,
    const QByteArray  &rawMessage,
    Done               done,
    bool               xoauth2
) {
    _xoauth2     = xoauth2;
    _host        = host;
    _port        = port;
    _user        = user;
    _pass        = secret;
    _from        = fromEmail;
    _rcpts       = recipients;
    _rcptIdx     = 0;
    _message     = rawMessage;
    _done        = std::move(done);
    _finished    = false;
    _state       = State::Greeting;
    _implicitTls = (port == 465);

    _sock = new QSslSocket(this);
    connect(_sock, &QSslSocket::readyRead, this, &SmtpClient::onReadyRead);
    connect(_sock, &QSslSocket::encrypted, this, &SmtpClient::onEncrypted);
    connect(_sock, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        finish(false, _sock->errorString());
    });
    connect(_sock, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &) {
        if (_insecure)
            _sock->ignoreSslErrors();
    });

    _timer = new QTimer(this);
    _timer->setSingleShot(true);
    _timer->setInterval(30000);
    connect(_timer, &QTimer::timeout, this, [this] { finish(false, QStringLiteral("timeout")); });
    _timer->start();

    if (_implicitTls)
        _sock->connectToHostEncrypted(host, port);
    else
        _sock->connectToHost(host, port);
}

void SmtpClient::write(const QByteArray &line) {
    if (_sock)
        _sock->write(line + "\r\n");
}

void SmtpClient::onEncrypted() {
    // After STARTTLS negotiation: re-EHLO over the now-encrypted channel.
    if (_state == State::StartTls) {
        _state = State::EhloTls;
        write("EHLO msga");
    }
}

void SmtpClient::beginAuth() {
    if (_xoauth2) {
        // AUTH XOAUTH2 <base64("user=<u>^Aauth=Bearer <tok>^A^A")>
        const QByteArray ir =
            QByteArray(
                "user=" + _user.toUtf8() + "\x01" + "auth=Bearer " + _pass.toUtf8() + "\x01\x01"
            )
                .toBase64();
        _state = State::XOAuth;
        write("AUTH XOAUTH2 " + ir);
    } else {
        _state = State::AuthUser;
        write("AUTH LOGIN");
    }
}

void SmtpClient::onReadyRead() {
    if (!_sock)
        return;
    _buf += _sock->readAll();
    QByteArray text;
    int        code;
    while ((code = takeResponse(text)) >= 0)
        advance(code, text);
}

int SmtpClient::takeResponse(QByteArray &out) {
    // A response is one or more lines; the final line has a space after the code.
    int pos = 0;
    while (true) {
        const int nl = _buf.indexOf("\r\n", pos);
        if (nl < 0)
            return -1; // incomplete
        const QByteArray line = _buf.mid(pos, nl - pos);
        const bool       last = line.size() >= 4 && line[3] == ' ';
        if (last) {
            out            = _buf.left(nl);
            const int code = _buf.left(3).toInt();
            _buf.remove(0, nl + 2);
            return code;
        }
        pos = nl + 2;
    }
}

void SmtpClient::advance(int code, const QByteArray &text) {
    auto fail = [&] { finish(false, QString::fromUtf8(text)); };

    switch (_state) {
    case State::Greeting:
        if (code != 220)
            return fail();
        _state = State::Ehlo;
        write("EHLO msga");
        break;
    case State::Ehlo:
        if (code != 250)
            return fail();
        if (_implicitTls) // already encrypted → straight to auth
            beginAuth();
        else {
            _state = State::StartTls;
            write("STARTTLS");
        }
        break;
    case State::StartTls:
        if (code != 220)
            return fail();
        _sock->startClientEncryption(); // onEncrypted() re-EHLOs
        break;
    case State::EhloTls:
        if (code != 250)
            return fail();
        beginAuth();
        break;
    case State::XOAuth:
        if (code == 235) { // auth success
            _state = State::Rcpt;
            write("MAIL FROM:<" + _from.toUtf8() + ">");
        } else if (code == 334) {
            _sock->write("\r\n"); // send empty response so the server returns 535
        } else {
            return fail();
        }
        break;
    case State::AuthUser:
        if (code != 334)
            return fail();
        _state = State::AuthPass;
        write(_user.toUtf8().toBase64());
        break;
    case State::AuthPass:
        if (code != 334)
            return fail();
        _state = State::MailFrom;
        write(_pass.toUtf8().toBase64());
        break;
    case State::MailFrom:
        if (code != 235) // AUTH success
            return fail();
        _state = State::Rcpt;
        write("MAIL FROM:<" + _from.toUtf8() + ">");
        break;
    case State::Rcpt:
        if (code != 250 && code != 251) // MAIL FROM / prior RCPT accepted
            return fail();
        if (_rcptIdx < _rcpts.size()) {
            const QByteArray r = _rcpts.at(_rcptIdx++).toUtf8();
            write("RCPT TO:<" + r + ">");
            // stay in Rcpt; next 250 either sends the next RCPT or DATA
            if (_rcptIdx >= _rcpts.size())
                _state = State::Data;
        }
        break;
    case State::Data:
        if (code != 250 && code != 251) // last RCPT accepted
            return fail();
        _state = State::Body;
        write("DATA");
        break;
    case State::Body:
        if (code != 354) // ready for data
            return fail();
        _sock->write(dotStuff(_message));
        _sock->write(".\r\n");
        _state = State::Quit;
        break;
    case State::Quit:
        if (code != 250) // message accepted
            return fail();
        write("QUIT");
        finish(true, {});
        break;
    }
}

void SmtpClient::finish(bool ok, const QString &err) {
    if (_finished)
        return;
    _finished = true;
    if (_timer)
        _timer->stop();
    Done d = _done;
    _done  = nullptr;
    if (_sock) {
        _sock->disconnect(this);
        _sock->abort();
        _sock->deleteLater();
        _sock = nullptr;
    }
    if (d)
        d(ok, err);
}

} // namespace imap

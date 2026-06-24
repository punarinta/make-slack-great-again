// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "backend/imap/imap_client.h"

#include <QSslSocket>

namespace imap {

ImapClient::ImapClient(QObject *parent) : QObject(parent) {}

ImapClient::~ImapClient() = default;

void ImapClient::connectToServer(const QString &host, quint16 port) {
    if (_sock)
        close();
    _failed  = false;
    _greeted = false;
    _ready   = false;
    _framer.clear();
    _sock = new QSslSocket(this);
    connect(_sock, &QSslSocket::encrypted, this, &ImapClient::onEncrypted);
    connect(_sock, &QSslSocket::readyRead, this, &ImapClient::onReadyRead);
    connect(_sock, &QSslSocket::errorOccurred, this, &ImapClient::onSocketError);
    connect(_sock, &QSslSocket::sslErrors, this, &ImapClient::onSslErrors);
    connect(_sock, &QSslSocket::disconnected, this, [this] {
        if (!_failed)
            emit disconnected();
    });
    _sock->connectToHostEncrypted(host, port);
}

void ImapClient::close() {
    if (!_sock)
        return;
    _sock->disconnect(this);
    _sock->abort();
    _sock->deleteLater();
    _sock    = nullptr;
    _greeted = false;
    _ready   = false;
    _busy    = false;
    _queue.clear();
    _curDone = nullptr;
    _curUntagged.clear();
    _framer.clear();
}

void ImapClient::onEncrypted() { /* wait for the server greeting before sending */ }

void ImapClient::onSocketError() {
    fail(_sock ? _sock->errorString() : QStringLiteral("socket error"));
}

void ImapClient::onSslErrors(const QList<QSslError> &errs) {
    if (_insecure && _sock) {
        _sock->ignoreSslErrors();
        return;
    }
    QStringList msgs;
    for (const auto &e : errs)
        msgs << e.errorString();
    fail(QStringLiteral("TLS error: ") + msgs.join(QStringLiteral("; ")));
}

void ImapClient::onReadyRead() {
    if (!_sock)
        return;
    _framer.append(_sock->readAll());
    QByteArray line;
    while (_framer.nextLine(line))
        dispatch(line);
}

void ImapClient::dispatch(const QByteArray &line) {
    if (!_greeted) {
        if (line.startsWith("* OK") || line.startsWith("* PREAUTH")) {
            _greeted = true;
            pump();
        } else if (line.startsWith("* BYE") || line.startsWith("* NO")) {
            fail(QStringLiteral("server refused at greeting: ") + QString::fromUtf8(line));
        }
        return;
    }

    if (line.startsWith("+")) {
        // Continuation request. IDLE → "+ idling". AUTHENTICATE XOAUTH2 → a base64
        // error challenge; reply with an empty line so the server returns a tagged
        // NO we can fail on.
        if (_busy && _curIsIdle)
            _idling = true;
        else if (_busy && _curIsAuth && _sock)
            _sock->write("\r\n");
        return;
    }
    if (line.startsWith("* ")) {
        const QByteArray body = line.mid(2);
        // Track capabilities wherever they appear — notably the untagged
        // CAPABILITY a server volunteers during LOGIN (post-auth set).
        if (body.startsWith("CAPABILITY"))
            _caps = body;
        if (_idling) {
            if (_onPush)
                _onPush(line); // server push while idling (EXISTS/EXPUNGE/FETCH)
        } else if (_busy) {
            _curUntagged.append(body);
        } else {
            emit unsolicited(line);
        }
        return;
    }
    if (_busy && line.startsWith(_curTag + " ")) {
        const QByteArray rest = line.mid(_curTag.size() + 1);
        Response         r;
        r.ok       = rest.startsWith("OK");
        r.status   = rest;
        r.untagged = _curUntagged;

        ResponseCb done = _curDone;
        _busy           = false;
        _curIsIdle      = false;
        _curIsAuth      = false;
        _idling         = false;
        _curDone        = nullptr;
        _curUntagged.clear();
        _curTag.clear();
        if (done)
            done(r);
        pump();
    }
}

void ImapClient::enqueue(Cmd c, bool front) {
    if (front)
        _queue.prepend(std::move(c));
    else
        _queue.append(std::move(c));
    pump();
}

void ImapClient::pump() {
    if (_busy || !_greeted || _queue.isEmpty() || !_sock)
        return;
    const Cmd c = _queue.takeFirst();
    _busy       = true;
    _curIsIdle  = c.idle;
    _curIsAuth  = c.auth;
    _curDone    = c.done;
    _curUntagged.clear();
    _curTag               = "A" + QByteArray::number(++_tagN).rightJustified(4, '0');
    const QByteArray wire = _curTag + " " + c.text + "\r\n";
    _sock->write(wire);
}

void ImapClient::fail(const QString &why) {
    if (_failed)
        return;
    _failed = true;
    emit error(why);
}

void ImapClient::sendCommand(const QByteArray &command, ResponseCb done) {
    enqueue({command, std::move(done)});
}

void ImapClient::login(const QString &user, const QString &password, ResponseCb done) {
    const QByteArray cmd =
        "LOGIN " + Proto::quote(user.toUtf8()) + " " + Proto::quote(password.toUtf8());
    enqueue({cmd, [this, done](const Response &r) {
                 if (!r.ok) {
                     fail(QStringLiteral("login failed: ") + QString::fromUtf8(r.status));
                     if (done)
                         done(r);
                     return;
                 }
                 // Phase 0 lesson: refresh CAPABILITY after auth before declaring
                 // ready, so feature gating uses the post-login set.
                 enqueue(
                     {"CAPABILITY",
                      [this, r, done](const Response &) {
                          _ready = true;
                          emit loggedIn();
                          if (done)
                              done(r); // report the LOGIN outcome (ok)
                      }},
                     /*front=*/true
                 );
             }});
}

void ImapClient::loginXOAuth2(const QString &user, const QString &accessToken, ResponseCb done) {
    // SASL XOAUTH2 initial response: base64("user=<u>^Aauth=Bearer <tok>^A^A").
    const QByteArray ir =
        QByteArray(
            "user=" + user.toUtf8() + "\x01" + "auth=Bearer " + accessToken.toUtf8() + "\x01\x01"
        )
            .toBase64();
    Cmd c;
    c.text = "AUTHENTICATE XOAUTH2 " + ir;
    c.auth = true;
    c.done = [this, done](const Response &r) {
        if (!r.ok) {
            fail(QStringLiteral("OAuth sign-in failed: ") + QString::fromUtf8(r.status));
            if (done)
                done(r);
            return;
        }
        enqueue(
            {"CAPABILITY",
             [this, r, done](const Response &) {
                 _ready = true;
                 emit loggedIn();
                 if (done)
                     done(r);
             }},
            /*front=*/true
        );
    };
    enqueue(std::move(c));
}

void ImapClient::list(std::function<void(bool, QList<Mailbox>)> done) {
    sendCommand("LIST \"\" \"*\"", [done](const Response &r) {
        if (done)
            done(r.ok, Proto::parseList(r.untagged));
    });
}

void ImapClient::select(const QString &mailbox, std::function<void(bool, SelectResult)> done) {
    sendCommand("SELECT " + Proto::quote(mailbox.toUtf8()), [done](const Response &r) {
        if (done)
            done(r.ok, Proto::parseSelect(r.untagged, r.status));
    });
}

void ImapClient::uidSearch(
    const QByteArray &criteria, std::function<void(bool, QList<quint32>)> done
) {
    sendCommand("UID SEARCH " + criteria, [done](const Response &r) {
        if (done)
            done(r.ok, Proto::parseSearch(r.untagged));
    });
}

void ImapClient::uidFetch(
    const QByteArray                            &uidSet,
    const QByteArray                            &items,
    std::function<void(bool, QList<QByteArray>)> done
) {
    sendCommand("UID FETCH " + uidSet + " (" + items + ")", [done](const Response &r) {
        QList<QByteArray> fetches;
        for (const QByteArray &u : r.untagged)
            if (u.contains(" FETCH "))
                fetches.append(u);
        if (done)
            done(r.ok, fetches);
    });
}

void ImapClient::uidThread(
    const QByteArray                     &algorithm,
    const QByteArray                     &charset,
    const QByteArray                     &criteria,
    std::function<void(bool, QByteArray)> done
) {
    sendCommand(
        "UID THREAD " + algorithm + " " + charset + " " + criteria, [done](const Response &r) {
            QByteArray thread;
            for (const QByteArray &u : r.untagged)
                if (u.startsWith("THREAD"))
                    thread = u;
            if (done)
                done(r.ok, thread);
        }
    );
}

void ImapClient::startIdle(std::function<void(const QByteArray &)> onPush) {
    _onPush = std::move(onPush);
    Cmd c;
    c.text = "IDLE";
    c.idle = true;
    c.done = [this](const Response &) {
        // IDLE finished (its tagged completion arrived after DONE).
        auto cb        = _onIdleStopped;
        _onIdleStopped = nullptr;
        if (cb)
            cb();
    };
    enqueue(std::move(c));
}

void ImapClient::stopIdle(std::function<void()> onStopped) {
    if (!_idling) {
        if (onStopped)
            onStopped();
        return;
    }
    _onIdleStopped = std::move(onStopped);
    if (_sock)
        _sock->write("DONE\r\n");
}

} // namespace imap

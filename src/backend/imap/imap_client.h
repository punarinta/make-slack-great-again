// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Async IMAP client over QSslSocket — the productionized Phase 0 spike, below the
// Backend seam. Owns the connection, a single-in-flight command queue (keeps the
// caller well-behaved, mirroring net::HttpQueue), and the response framing via
// imap::ResponseFramer. Speaks request/response (CAPABILITY/LOGIN/LIST/SELECT/
// UID SEARCH/UID FETCH/UID THREAD); IDLE push is wired in the realtime phase.
//
// Phase 0 lesson baked in: capabilities are refreshed AFTER login (some servers,
// e.g. Dovecot, only advertise THREAD/etc. post-auth), so hasCapability() always
// reflects the authenticated set by the time loggedIn() fires.
#pragma once

#include "backend/imap/imap_protocol.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class QSslSocket;
class QSslError;

namespace imap {

class ImapClient : public QObject {
    Q_OBJECT
public:
    using ResponseCb = std::function<void(const Response &)>;

    explicit ImapClient(QObject *parent = nullptr);
    ~ImapClient() override;

    // Ignore TLS certificate errors (self-signed dev servers only).
    void setInsecure(bool v) { _insecure = v; }

    void connectToServer(const QString &host, quint16 port = 993);
    void close();

    [[nodiscard]] bool              isLoggedIn() const { return _ready; }
    [[nodiscard]] const QByteArray &capabilities() const { return _caps; }
    [[nodiscard]] bool hasCapability(const QByteArray &c) const { return _caps.contains(c); }

    // Authenticate with LOGIN. `done` fires after the post-login CAPABILITY
    // refresh, so capabilities() is authoritative inside it. Also emits
    // loggedIn() on success / error() on failure.
    void login(const QString &user, const QString &password, ResponseCb done = {});
    // OAuth (XOAUTH2 SASL): authenticate with a bearer access token instead of a
    // password (Gmail/Outlook). Same post-login CAPABILITY refresh + loggedIn().
    void loginXOAuth2(const QString &user, const QString &accessToken, ResponseCb done = {});

    // Generic command (the tag is added automatically). Untagged lines that
    // arrive while it runs are collected into Response::untagged.
    void sendCommand(const QByteArray &command, ResponseCb done = {});

    // Typed convenience wrappers (parse via imap::Proto).
    void list(std::function<void(bool ok, QList<Mailbox>)> done);
    void select(const QString &mailbox, std::function<void(bool ok, SelectResult)> done);
    void uidSearch(const QByteArray &criteria, std::function<void(bool ok, QList<quint32>)> done);
    // FETCH delivers the raw "* n FETCH (...)" lines (literals inlined) for the
    // mapper to interpret; the client stays semantics-free for ENVELOPE/BODYSTRUCTURE.
    void uidFetch(
        const QByteArray                                          &uidSet,
        const QByteArray                                          &items,
        std::function<void(bool ok, QList<QByteArray> fetchLines)> done
    );
    void uidThread(
        const QByteArray                                   &algorithm,
        const QByteArray                                   &charset,
        const QByteArray                                   &criteria,
        std::function<void(bool ok, QByteArray threadLine)> done
    );

    // --- IDLE (RFC 2177) ---
    // Begin idling on the currently-selected mailbox. `onPush` fires for every
    // untagged response the server sends while idling (e.g. "* 5 EXISTS"). The
    // command stays in flight until stopIdle(); no other command runs meanwhile.
    void               startIdle(std::function<void(const QByteArray &push)> onPush);
    // Send DONE; `onStopped` fires once the IDLE command completes (queue free,
    // safe to send commands again). No-op (fires immediately) if not idling.
    void               stopIdle(std::function<void()> onStopped = {});
    [[nodiscard]] bool isIdling() const { return _idling; }

signals:
    void loggedIn();
    void error(QString reason);
    void disconnected();
    // An untagged response arriving outside any command (server push). The
    // realtime phase routes EXISTS/EXPUNGE/FETCH here.
    void unsolicited(QByteArray untaggedLine);

private:
    struct Cmd {
        QByteArray text;
        ResponseCb done;
        bool       idle = false; // an IDLE command: stays in flight until DONE
        bool       auth = false; // an AUTHENTICATE command (SASL continuation)
    };

    void onReadyRead();
    void onEncrypted();
    void onSocketError();
    void onSslErrors(const QList<QSslError> &errs);
    void dispatch(const QByteArray &line);
    void enqueue(Cmd c, bool front = false);
    void pump();
    void fail(const QString &why);

    QSslSocket    *_sock = nullptr;
    ResponseFramer _framer;
    QByteArray     _caps;

    QList<Cmd>        _queue;
    bool              _busy = false;
    QByteArray        _curTag;
    QList<QByteArray> _curUntagged;
    ResponseCb        _curDone;

    int  _tagN      = 0;
    bool _greeted   = false;
    bool _ready     = false;
    bool _insecure  = false;
    bool _failed    = false;
    bool _curIsIdle = false; // the in-flight command is IDLE
    bool _curIsAuth = false; // the in-flight command is AUTHENTICATE (SASL)
    bool _idling    = false; // server acknowledged IDLE (got "+ idling")

    std::function<void(const QByteArray &)> _onPush;        // per-push during IDLE
    std::function<void()>                   _onIdleStopped; // fired when IDLE completes
};

} // namespace imap

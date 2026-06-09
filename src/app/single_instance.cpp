// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "single_instance.h"
#include <QLocalSocket>
#include <QDataStream>
#include <QDir>

SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {}

// Per-user socket name prevents cross-user socket collisions.
QString SingleInstance::socketName() {
    return "msga-" + QDir::home().dirName();
}

bool SingleInstance::init(const QString &urlArg) {
    const QString name = socketName();

    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(300)) {
        if (!urlArg.isEmpty()) {
            QDataStream ds(&probe);
            ds << urlArg;
            probe.flush();
            probe.waitForBytesWritten(500);
        }
        probe.disconnectFromServer();
        return false;
    }

    // No running instance — become the primary.
    QLocalServer::removeServer(name); // clean up any stale socket file
    _server = new QLocalServer(this);
    _server->setSocketOptions(QLocalServer::UserAccessOption);
    if (_server->listen(name))
        connect(_server, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
    // If listen() fails we still run; we just can't receive redirects from future instances.

    if (!urlArg.isEmpty())
        QMetaObject::invokeMethod(
            this, [this, urlArg] { emit uriReceived(QUrl(urlArg)); }, Qt::QueuedConnection
        );

    return true;
}

void SingleInstance::release() {
    if (_server) {
        _server->close();
        QLocalServer::removeServer(socketName());
    }
}

void SingleInstance::onNewConnection() {
    while (_server->hasPendingConnections()) {
        auto *sock = _server->nextPendingConnection();
        // The mere act of a second instance connecting means "please show yourself"
        // (e.g. Ubuntu dock click when window is hidden launches a new process).
        emit  activateRequested();
        connect(sock, &QLocalSocket::readyRead, this, [this, sock] {
            QString     url;
            QDataStream ds(sock);
            ds >> url;
            sock->deleteLater();
            if (!url.isEmpty())
                emit uriReceived(QUrl(url));
        });
    }
}

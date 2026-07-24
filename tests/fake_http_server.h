// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MSGA contributors. See LICENSE for details.
// Minimal local HTTP/1.1 server for WebApiClient tests. Enqueue JSON bodies
// with enqueue(); each incoming request consumes one entry and gets that body
// as the response. Handles both GET and POST (reads until full Content-Length
// received). Records the request line path of every request in requestPaths.
#pragma once

#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>

class FakeHttpServer {
public:
    FakeHttpServer() {
        QObject::connect(&_server, &QTcpServer::newConnection, &_server, [this] {
            onNewConnection();
        });
        _server.listen(QHostAddress::LocalHost);
    }

    QString baseUrl() const { return QString("http://127.0.0.1:%1/").arg(_server.serverPort()); }

    // Queue a normal 200 OK JSON response (consumed by the next request).
    void enqueue(const QByteArray &json) { _pending.append(make200(json)); }

    // Queue an HTTP 429 with a Retry-After header (seconds), to exercise the
    // queue's rate-limit backpressure.
    void enqueue429(int retryAfterSecs) {
        const QByteArray body = R"({"ok":false,"error":"ratelimited"})";
        QByteArray       resp = "HTTP/1.1 429 Too Many Requests\r\n";
        resp += "Retry-After: " + QByteArray::number(retryAfterSecs) + "\r\n";
        resp += "Content-Type: application/json\r\n";
        resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        resp += "Connection: close\r\n\r\n";
        resp += body;
        _pending.append(resp);
    }

    int requestCount    = 0;
    int dropConnections = 0; // close this many requests without responding

    QList<QByteArray> requestPaths;   // "/chat.postMessage" etc., in arrival order
    QList<QByteArray> requestTargets; // full request-line target incl. query string
    QList<QByteArray> requestBodies;  // raw POST bodies ("" for GET), in arrival order
    QList<QByteArray> requestHeaders; // raw header block of each request, in arrival order

private:
    static QByteArray make200(const QByteArray &body) {
        QByteArray resp = "HTTP/1.1 200 OK\r\n";
        resp += "Content-Type: application/json\r\n";
        resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        resp += "Connection: close\r\n\r\n";
        resp += body;
        return resp;
    }

    void onNewConnection() {
        auto *sock = _server.nextPendingConnection();
        auto *buf  = new QByteArray;
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
            buf->append(sock->readAll());

            // Wait for complete headers.
            int headerEnd = buf->indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;

            // Parse Content-Length so we wait for the full POST body.
            QByteArray headers       = buf->left(headerEnd);
            int        contentLength = 0;
            for (const QByteArray &line : headers.split('\n')) {
                if (line.trimmed().toLower().startsWith("content-length:"))
                    contentLength = line.trimmed().mid(15).trimmed().toInt();
            }
            if (buf->size() < headerEnd + 4 + contentLength)
                return; // body not fully received yet

            ++requestCount;
            // Request line: "GET /chat.postMessage?channel=C1 HTTP/1.1"
            const QList<QByteArray> reqLine = headers.left(headers.indexOf("\r\n")).split(' ');
            if (reqLine.size() >= 2) {
                requestPaths.append(reqLine[1].split('?').first());
                requestTargets.append(reqLine[1]);
            }
            requestBodies.append(buf->mid(headerEnd + 4, contentLength));
            requestHeaders.append(headers);

            if (dropConnections > 0) {
                --dropConnections;
                sock->close(); // simulates a stale/killed connection
                return;
            }
            const QByteArray resp = _pending.isEmpty()
                                        ? make200(R"({"ok":false,"error":"no_response_queued"})")
                                        : _pending.takeFirst();
            sock->write(resp);
            sock->flush();
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, [sock, buf] {
            delete buf;
            sock->deleteLater();
        });
    }

    QTcpServer        _server;
    QList<QByteArray> _pending;
};

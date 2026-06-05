// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once
#include <QObject>
#include <QLocalServer>
#include <QUrl>

// Ensures only one MSGA process runs at a time.
// Secondary instances forward their msga:// URL argument to the primary and exit.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject *parent = nullptr);

    // Call once at startup with the msga:// argument from argv (empty string if none).
    // Returns true  → this is the primary instance; server is now listening.
    // Returns false → a primary was already running; URL forwarded; caller must exit(0).
    bool init(const QString &urlArg);

signals:
    void uriReceived(QUrl url);

private:
    void onNewConnection();

    QLocalServer *_server = nullptr;

    static QString socketName();
};

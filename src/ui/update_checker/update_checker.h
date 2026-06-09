// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// Fetches the platform manifest from msga.app, compares against the running
// version, and downloads the new binary/DMG when a newer version is found.
// Linux:  downloads to a .download temp file, renames atomically over the
//         running binary (safe on Linux — kernel holds the old inode), then
//         signals downloadReady so the UI can prompt for a restart.
// macOS:  saves the DMG to ~/Downloads, opened via QDesktopServices on restart.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);

    void    checkInBackground(); // silent; no-ops on failure / no network
    void    checkNow();          // explicit check; always emits a terminal signal
    bool    isChecking() const { return _checking; }
    bool    isReady() const { return _ready; }
    QString downloadedPath() const { return _downloadedPath; }

signals:
    void checkStarted();
    void upToDate();
    void updateAvailable(int newVersion);
    void downloadProgress(int percent);
    void downloadReady();
    void checkFailed(const QString &message);

private:
    void fetch(bool silent);
    void onManifestDone(QNetworkReply *reply, bool silent);
    void startDownload(int newVersion);
    void onDownloadDone(QNetworkReply *reply, int newVersion);

    static QString manifestUrl();
    static QString binaryUrl();
    static QString stagePath();

    QNetworkAccessManager *_nam      = nullptr;
    bool                   _checking = false;
    bool                   _ready    = false;
    QString                _downloadedPath;
};

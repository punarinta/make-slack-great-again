// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// Fetches the platform manifest from msga.app, compares against the running
// version, and downloads the new binary/DMG when a newer version is found.
// Linux:  stages {applicationFilePath()}.new, replaced atomically on restart.
// macOS:  saves the DMG to ~/Downloads, opened via QDesktopServices on restart.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);

    void checkInBackground();   // silent; no-ops on failure / no network
    void checkNow();            // explicit check; always emits a terminal signal
    bool isChecking() const { return _checking; }

    QString stagedPath()    const { return _staged; }
    int     stagedVersion() const { return _stagedVersion; }
    void    clearStaged();

signals:
    void checkStarted();
    void upToDate();
    void updateAvailable(int newVersion);
    void downloadProgress(int percent);
    void downloadReady(const QString &stagedPath);
    void checkFailed(const QString &message);

private:
    void fetch(bool silent);
    void onManifestDone(QNetworkReply *reply, bool silent);
    void startDownload(int newVersion);
    void onDownloadDone(QNetworkReply *reply, int newVersion);

    static QString manifestUrl();
    static QString binaryUrl();
    static QString stagePath();

    QNetworkAccessManager *_nam           = nullptr;
    bool                   _checking      = false;
    int                    _stagedVersion = 0;
    QString                _staged;
};

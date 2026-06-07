// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "update_checker.h"
#include "app_credentials.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QSettings>
#include <QDateTime>

static constexpr char kBase[] = "https://msga.app/download/";

#if defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
static constexpr char kBinary[]   = "msga-linux-x86_64";
static constexpr char kManifest[] = "msga-linux-x86_64.manifest";
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
static constexpr char kBinary[]   = "msga-macos-arm64.dmg";
static constexpr char kManifest[] = "msga-macos-arm64.manifest";
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_X86_64)
static constexpr char kBinary[]   = "msga-macos-x86_64.dmg";
static constexpr char kManifest[] = "msga-macos-x86_64.manifest";
#else
static constexpr char kBinary[]   = "";
static constexpr char kManifest[] = "";
#endif

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , _nam(new QNetworkAccessManager(this))
{
    QSettings s("msga", "msga");
    _staged        = s.value("updates/stagedPath").toString();
    _stagedVersion = s.value("updates/stagedVersion", 0).toInt();
    if (!_staged.isEmpty() && !QFile::exists(_staged)) {
        _staged.clear();
        _stagedVersion = 0;
        s.remove("updates/stagedPath");
        s.remove("updates/stagedVersion");
    }
}

QString UpdateChecker::manifestUrl() {
    return kManifest[0] ? QString(kBase) + kManifest : QString();
}

QString UpdateChecker::binaryUrl() {
    return kBinary[0] ? QString(kBase) + kBinary : QString();
}

QString UpdateChecker::stagePath() {
#if defined(Q_OS_LINUX)
    // Same directory as the running binary → rename(2) is atomic on the same FS.
    return QCoreApplication::applicationFilePath() + ".new";
#else
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
           + "/" + kBinary;
#endif
}

void UpdateChecker::checkInBackground() {
    if (_checking) return;
    // Already have a usable staged update — just surface it again.
    if (_stagedVersion > AppCredentials::version && !_staged.isEmpty()) {
        emit downloadReady(_staged);
        return;
    }
    fetch(/*silent=*/true);
}

void UpdateChecker::checkNow() {
    if (_checking) return;
    emit checkStarted();
    fetch(/*silent=*/false);
}

void UpdateChecker::fetch(bool silent) {
    const QString url = manifestUrl();
    if (url.isEmpty()) {
        if (!silent)
            emit checkFailed(tr("Automatic updates are not supported on this platform."));
        return;
    }
    _checking = true;
    auto *reply = _nam->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, silent] {
        reply->deleteLater();
        _checking = false;
        onManifestDone(reply, silent);
    });
}

void UpdateChecker::onManifestDone(QNetworkReply *reply, bool silent) {
    if (reply->error() != QNetworkReply::NoError) {
        if (!silent) emit checkFailed(reply->errorString());
        return;
    }
    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    const int remote = obj["version"].toInt();
    if (remote <= 0) {
        if (!silent) emit checkFailed(tr("Could not parse version manifest."));
        return;
    }

    QSettings("msga", "msga").setValue("updates/lastChecked",
        QDateTime::currentSecsSinceEpoch());

    if (remote <= AppCredentials::version) {
        emit upToDate();
        return;
    }
    emit updateAvailable(remote);

    if (_stagedVersion == remote && !_staged.isEmpty()) {
        emit downloadReady(_staged);
        return;
    }
    startDownload(remote);
}

void UpdateChecker::startDownload(int newVersion) {
    const QString dest = stagePath();
    QFile::remove(dest);

    auto *file = new QFile(dest, this);
    if (!file->open(QIODevice::WriteOnly)) {
        emit checkFailed(tr("Cannot write update to %1").arg(dest));
        file->deleteLater();
        return;
    }

    auto *reply = _nam->get(QNetworkRequest(QUrl(binaryUrl())));
    connect(reply, &QNetworkReply::downloadProgress,
            this, [this](qint64 recv, qint64 total) {
        if (total > 0)
            emit downloadProgress(int(recv * 100 / total));
    });
    connect(reply, &QNetworkReply::readyRead, file, [reply, file] {
        file->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, file, newVersion] {
        file->flush();
        file->close();
        file->deleteLater();
        reply->deleteLater();
        onDownloadDone(reply, newVersion);
    });
}

void UpdateChecker::onDownloadDone(QNetworkReply *reply, int newVersion) {
    const QString dest = stagePath();
    if (reply->error() != QNetworkReply::NoError) {
        QFile::remove(dest);
        emit checkFailed(tr("Download failed: %1").arg(reply->errorString()));
        return;
    }

#if defined(Q_OS_LINUX)
    QFile::setPermissions(dest,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
        QFile::ReadGroup | QFile::ExeGroup |
        QFile::ReadOther | QFile::ExeOther);
#endif

    _staged        = dest;
    _stagedVersion = newVersion;
    QSettings s("msga", "msga");
    s.setValue("updates/stagedPath",    _staged);
    s.setValue("updates/stagedVersion", _stagedVersion);
    emit downloadReady(_staged);
}

void UpdateChecker::clearStaged() {
    QFile::remove(_staged);
    _staged.clear();
    _stagedVersion = 0;
    QSettings s("msga", "msga");
    s.remove("updates/stagedPath");
    s.remove("updates/stagedVersion");
}

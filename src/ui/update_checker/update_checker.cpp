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

#if defined(Q_OS_LINUX)
#include <cerrno>
#include <cstdio>
#include <cstring>
#endif

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
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
static constexpr char kBinary[]   = "msga-windows-x86_64.exe";
static constexpr char kManifest[] = "msga-windows-x86_64.manifest";
#else
static constexpr char kBinary[]   = "";
static constexpr char kManifest[] = "";
#endif

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), _nam(new QNetworkAccessManager(this)) {
#if defined(Q_OS_WIN)
    // Clean up backup left by the previous update's rename-away step.
    QFile::remove(QCoreApplication::applicationFilePath() + ".old");
#endif
}

QString UpdateChecker::manifestUrl() {
    return kManifest[0] ? QString(kBase) + kManifest : QString();
}

QString UpdateChecker::binaryUrl() {
    return kBinary[0] ? QString(kBase) + kBinary : QString();
}

static QString downloadTempPath() {
#if defined(Q_OS_MACOS)
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + kBinary;
#else
    // Linux and Windows: same directory as the running binary so rename(2)/MoveFileEx
    // stays on the same filesystem and is atomic.
    return QCoreApplication::applicationFilePath() + ".download";
#endif
}

void UpdateChecker::checkInBackground() {
    if (_checking || _ready)
        return;
    fetch(/*silent=*/true);
}

void UpdateChecker::checkNow() {
    if (_checking)
        return;
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
    _checking   = true;
    auto *reply = _nam->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, silent] {
        reply->deleteLater();
        _checking = false;
        onManifestDone(reply, silent);
    });
}

void UpdateChecker::onManifestDone(QNetworkReply *reply, bool silent) {
    if (reply->error() != QNetworkReply::NoError) {
        if (!silent)
            emit checkFailed(reply->errorString());
        return;
    }
    const QJsonObject obj    = QJsonDocument::fromJson(reply->readAll()).object();
    const int         remote = obj["version"].toInt();
    if (remote <= 0) {
        if (!silent)
            emit checkFailed(tr("Could not parse version manifest."));
        return;
    }

    QSettings("msga", "msga").setValue("updates/lastChecked", QDateTime::currentSecsSinceEpoch());

    if (remote <= AppCredentials::version) {
        emit upToDate();
        return;
    }
    emit updateAvailable(remote);
    startDownload(remote);
}

void UpdateChecker::startDownload(int newVersion) {
    const QString dest = downloadTempPath();
    QFile::remove(dest);

    auto *file = new QFile(dest, this);
    if (!file->open(QIODevice::WriteOnly)) {
        emit checkFailed(tr("Cannot write update to %1").arg(dest));
        file->deleteLater();
        return;
    }

    auto *reply = _nam->get(QNetworkRequest(QUrl(binaryUrl())));
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 recv, qint64 total) {
        if (total > 0)
            emit downloadProgress(int(recv * 100 / total));
    });
    connect(reply, &QNetworkReply::readyRead, file, [reply, file] {
        file->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, newVersion] {
        file->flush();
        file->close();
        file->deleteLater();
        reply->deleteLater();
        onDownloadDone(reply, newVersion);
    });
}

void UpdateChecker::onDownloadDone(QNetworkReply *reply, int newVersion) {
    Q_UNUSED(newVersion)
    const QString tmp = downloadTempPath();
    if (reply->error() != QNetworkReply::NoError) {
        QFile::remove(tmp);
        emit checkFailed(tr("Download failed: %1").arg(reply->errorString()));
        return;
    }

#if defined(Q_OS_LINUX)
    QFile::setPermissions(
        tmp,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup |
            QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther
    );
    const QString target = QCoreApplication::applicationFilePath();
    // QFile::rename() refuses to overwrite an existing destination; use POSIX
    // rename() which atomically replaces the directory entry.
    if (::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(target).constData()) != 0) {
        const int e = errno;
        QFile::remove(tmp);
        emit checkFailed(tr("Could not replace binary: %1").arg(QString::fromLocal8Bit(strerror(e)))
        );
        return;
    }
    _ready          = true;
    _downloadedPath = target;
    emit downloadReady();
#elif defined(Q_OS_WIN)
    // Windows locks running EXEs against deletion/replacement, but not against
    // renaming. Rename the current EXE away first, then move the new one in.
    const QString target = QCoreApplication::applicationFilePath();
    const QString backup = target + ".old";
    QFile::remove(backup);
    if (!QFile::rename(target, backup)) {
        QFile::remove(tmp);
        emit checkFailed(
            tr("Could not move current binary — check file permissions on %1").arg(target)
        );
        return;
    }
    if (!QFile::rename(tmp, target)) {
        QFile::rename(backup, target); // best-effort restore
        emit checkFailed(tr("Could not place new binary at %1").arg(target));
        return;
    }
    _ready          = true;
    _downloadedPath = target;
    emit downloadReady();
#else
    // macOS: DMG saved to Downloads; opened by the user or via QDesktopServices.
    _ready          = true;
    _downloadedPath = tmp;
    emit downloadReady();
#endif
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cc_outputs.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QUrl>

namespace claude_code {

namespace {

constexpr int    kMaxFiles     = 10;
constexpr qint64 kMaxFileBytes = 50LL * 1024 * 1024;
// A file counts as the turn's when it was modified from a little before its
// prompt to a little after the answer (clocks, and the answer's record being
// stamped once written).
constexpr qint64 kSlackMicros  = 2'000'000;
constexpr int    kSvgPreviewPx = 1600; // long side of an SVG's rendered preview

const QString kIndex = QStringLiteral("index.json");

bool shownKind(const QString &path) {
    static const QSet<QString> kExts = {
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("gif"),  QStringLiteral("webp"), QStringLiteral("bmp"),
        QStringLiteral("svg"),  QStringLiteral("pdf"),  QStringLiteral("mp3"),
        QStringLiteral("wav"),  QStringLiteral("ogg"),  QStringLiteral("m4a"),
        QStringLiteral("flac"), QStringLiteral("mp4"),  QStringLiteral("webm"),
        QStringLiteral("mov"),  QStringLiteral("html"), QStringLiteral("htm"),
        QStringLiteral("csv"),
    };
    return kExts.contains(QFileInfo(path).suffix().toLower());
}

QString expandHome(const QString &token) {
    if (token.startsWith(QLatin1String("~/")))
        return QDir::homePath() + token.mid(1);
    return token;
}

// A folder for the copies of one answer.
QString messageDir(const OutputContext &ctx) {
    QString key = ctx.messageKey;
    key.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return outputsDir(ctx.convId) + QLatin1Char('/') + key;
}

File fileFor(const QJsonObject &e, const QString &dir, const QString &idPrefix, int n) {
    const QString path    = dir + QLatin1Char('/') + e.value(QLatin1String("file")).toString();
    const QString url     = QUrl::fromLocalFile(path).toString();
    const QString preview = e.value(QLatin1String("preview")).toString();
    File          f;
    f.id                 = idPrefix + QString::number(n);
    f.name               = e.value(QLatin1String("name")).toString();
    f.mimeType           = e.value(QLatin1String("mime")).toString();
    f.fileType           = QFileInfo(f.name).suffix().toLower();
    f.size               = QFileInfo(path).size();
    f.urlPrivate         = url;
    f.urlPrivateDownload = url;
    if (f.mimeType.startsWith(QLatin1String("image/"))) {
        // An SVG is shown by its rendered preview; the download is the SVG.
        const QString shown    = preview.isEmpty() ? path : dir + QLatin1Char('/') + preview;
        const QString shownUrl = QUrl::fromLocalFile(shown).toString();
        const QSize   px       = QImageReader(shown).size();
        f.urlPrivate           = shownUrl;
        f.thumbUrl             = shownUrl;
        f.imageWidth           = e.value(QLatin1String("w")).toInt();
        f.imageHeight          = e.value(QLatin1String("h")).toInt();
        if (px.width() > 0)
            f.thumbs.push_back(FileThumb{px.width(), px.height(), shownUrl});
    }
    return f;
}

// The copy of `src` as file `name` in `dir`, and how it's shown; empty when
// it can't be read.
QJsonObject copyInto(const QString &src, const QString &dir, const QString &name) {
    const QString dest = dir + QLatin1Char('/') + name;
    QFile::remove(dest);
    if (!QFile::copy(src, dest))
        return {};
    QFile(dest).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QJsonObject e{
        {QStringLiteral("name"), QFileInfo(src).fileName()},
        {QStringLiteral("file"), name},
        {QStringLiteral("mime"), QMimeDatabase().mimeTypeForFile(src).name()},
    };
    if (QFileInfo(src).suffix().compare(QLatin1String("svg"), Qt::CaseInsensitive) == 0) {
        QSvgRenderer r(dest);
        if (!r.isValid())
            return e; // not a picture after all: a file card
        QSize logical = r.defaultSize();
        if (logical.isEmpty())
            logical = QSize(512, 512);
        QSize px = logical * 2;
        if (px.width() > kSvgPreviewPx || px.height() > kSvgPreviewPx)
            px.scale(kSvgPreviewPx, kSvgPreviewPx, Qt::KeepAspectRatio);
        QImage img(px, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        {
            QPainter p(&img);
            r.render(&p);
        }
        const QString preview = name + QStringLiteral(".preview.png");
        if (!img.save(dir + QLatin1Char('/') + preview))
            return e;
        e[QStringLiteral("mime")]    = QStringLiteral("image/svg+xml");
        e[QStringLiteral("preview")] = preview;
        e[QStringLiteral("w")]       = logical.width();
        e[QStringLiteral("h")]       = logical.height();
    } else if (e.value(QLatin1String("mime")).toString().startsWith(QLatin1String("image/"))) {
        const QSize sz = QImageReader(dest).size();
        if (sz.isEmpty()) {
            e[QStringLiteral("mime")] = QStringLiteral("application/octet-stream");
            return e; // unreadable: a file card, not a broken picture
        }
        e[QStringLiteral("w")] = sz.width();
        e[QStringLiteral("h")] = sz.height();
    }
    return e;
}

} // namespace

QStringList mentionedFiles(const QString &text, const QString &cwd) {
    static const QRegularExpression kSplit(QStringLiteral("[\\s`'\"<>()\\[\\]{}|*,;]+"));
    QStringList                     absolute, relative, dirs;
    for (QString token : text.split(kSplit, Qt::SkipEmptyParts)) {
        while (!token.isEmpty() && QStringLiteral(".:!?").contains(token.back()))
            token.chop(1);
        if (token.startsWith(QLatin1String("file://")))
            token = QUrl(token).toLocalFile();
        else if (token.contains(QLatin1String("://")))
            continue; // a web link
        token = expandHome(token);
        if (token.isEmpty())
            continue;
        const QFileInfo fi(token);
        if (QDir::isAbsolutePath(token)) {
            if (fi.isDir()) {
                dirs << QDir::cleanPath(token);
            } else if (shownKind(token)) {
                absolute << QDir::cleanPath(token);
                dirs << fi.absolutePath(); // "…/x/a.svg and a.png"
            }
        } else if (shownKind(token)) {
            relative << token;
        }
    }
    if (!cwd.isEmpty())
        dirs << cwd;

    QStringList   out;
    QSet<QString> seen;
    const auto    add = [&](const QString &path) {
        const QFileInfo fi(path);
        if (fi.isFile() && !seen.contains(fi.absoluteFilePath())) {
            seen.insert(fi.absoluteFilePath());
            out << fi.absoluteFilePath();
            return true;
        }
        return false;
    };
    // In the order the text names them: absolute ones first is close enough —
    // answers name a folder, then what's in it.
    for (const QString &p : absolute)
        add(p);
    for (const QString &r : relative)
        for (const QString &d : dirs)
            if (add(QDir(d).filePath(r)))
                break;
    return out;
}

std::vector<File> outputFiles(const QString &text, const OutputContext &ctx) {
    if (ctx.convId.isEmpty() || ctx.messageKey.isEmpty())
        return {};
    const QString dir      = messageDir(ctx);
    const QString idPrefix = QStringLiteral("out-%1-").arg(ctx.messageKey);

    // Copied before: the answer keeps what it had then.
    QJsonArray index;
    if (QFile f(dir + QLatin1Char('/') + kIndex); f.open(QIODevice::ReadOnly)) {
        index = QJsonDocument::fromJson(f.readAll()).array();
    } else {
        const QStringList named = mentionedFiles(text, ctx.cwd);
        QStringList       made;
        for (const QString &path : named) {
            const QFileInfo fi(path);
            const qint64    mtime = fi.lastModified().toMSecsSinceEpoch() * 1000;
            if (mtime < ctx.turnStart - kSlackMicros || mtime > ctx.date + kSlackMicros)
                continue; // made before this turn, or changed since the answer
            if (fi.size() > kMaxFileBytes)
                continue;
            made << path;
            if (made.size() == kMaxFiles)
                break;
        }
        if (made.isEmpty())
            return {};
        QDir().mkpath(dir);
        for (int i = 0; i < made.size(); ++i) {
            const QJsonObject e = copyInto(
                made[i], dir, QStringLiteral("%1-%2").arg(i).arg(QFileInfo(made[i]).fileName())
            );
            if (!e.isEmpty())
                index.append(e);
        }
        QSaveFile out(dir + QLatin1Char('/') + kIndex);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(QJsonDocument(index).toJson(QJsonDocument::Compact));
            out.commit();
        }
    }
    std::vector<File> files;
    for (int i = 0; i < index.size(); ++i) {
        const QJsonObject e = index[i].toObject();
        if (QFileInfo::exists(dir + QLatin1Char('/') + e.value(QLatin1String("file")).toString()))
            files.push_back(fileFor(e, dir, idPrefix, i));
    }
    return files;
}

QString outputsDir(const QString &convId) {
    QString id = convId;
    id.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
           QStringLiteral("/claude-code/files/") + id;
}

void clearOutputs(const QString &convId) {
    if (!convId.isEmpty())
        QDir(outputsDir(convId)).removeRecursively();
}

void pruneOutputs(const QStringList &keep) {
    QSet<QString> kept;
    for (const QString &id : keep)
        kept.insert(QFileInfo(outputsDir(id)).fileName());
    const QString root = QFileInfo(outputsDir(QStringLiteral("x"))).path();
    for (const QString &name : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (!kept.contains(name))
            QDir(root + QLatin1Char('/') + name).removeRecursively();
}

} // namespace claude_code

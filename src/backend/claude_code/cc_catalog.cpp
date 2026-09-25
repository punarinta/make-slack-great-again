// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cc_catalog.h"
#include "cc_roles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace claude_code {
namespace {

constexpr qint64 kEndBytes = 96 * 1024; // read from each end of a transcript

QString oneLine(const QString &s) {
    QString out = s.simplified();
    if (out.size() > 200)
        out = out.left(199) + QStringLiteral("…");
    return out;
}

// What someone typed, from a "user" record — not tool output, not what Claude
// Code tells the model, not a command's terminal output. "" when it's none.
QString typedPrompt(const QJsonObject &o) {
    if (o.value(QLatin1String("isMeta")).toBool() ||
        o.value(QLatin1String("isCompactSummary")).toBool() ||
        o.value(QLatin1String("isSidechain")).toBool())
        return {};
    const QJsonValue origin = o.value(QLatin1String("origin"));
    if (origin.isObject() &&
        origin.toObject().value(QLatin1String("kind")).toString() != QLatin1String("human"))
        return {};
    const QJsonValue content =
        o.value(QLatin1String("message")).toObject().value(QLatin1String("content"));
    QString text;
    if (content.isString()) {
        text = content.toString();
    } else {
        for (const auto &v : content.toArray()) {
            const QJsonObject b = v.toObject();
            const QString     t = b.value(QLatin1String("type")).toString();
            if (t == QLatin1String("tool_result"))
                return {};
            if (t == QLatin1String("text"))
                text += b.value(QLatin1String("text")).toString() + QLatin1Char(' ');
        }
    }
    text = text.trimmed();
    // "<command-name>/compact</command-name>…" is how a slash command is kept.
    static const QLatin1String kCommand("<command-name>");
    if (text.startsWith(kCommand)) {
        const qsizetype end = text.indexOf(QLatin1String("</command-name>"));
        return end > 0 ? text.mid(kCommand.size(), end - kCommand.size()).trimmed() : QString();
    }
    if (text.startsWith(QLatin1Char('<')))
        return {}; // caveats, command output, task notifications
    return oneLine(text);
}

// The lines of `bytes`, less the first when it may have been cut (a tail).
QList<QByteArray> wholeLines(const QByteArray &bytes, bool skipFirst) {
    QList<QByteArray> lines = bytes.split('\n');
    if (skipFirst && !lines.isEmpty())
        lines.removeFirst();
    return lines;
}

} // namespace

bool catalogEntryFrom(const QByteArray &head, const QByteArray &tail, CatalogEntry &e) {
    QString aiTitle, customTitle, summary;
    auto    read = [&](const QJsonObject &o, bool fromHead) {
        const QString type = o.value(QLatin1String("type")).toString();
        if (e.cwd.isEmpty())
            e.cwd = o.value(QLatin1String("cwd")).toString();
        if (type == QLatin1String("user")) {
            const QString p = typedPrompt(o);
            if (p.isEmpty())
                return;
            if (fromHead && e.firstPrompt.isEmpty())
                e.firstPrompt = p;
            if (!fromHead)
                e.lastPrompt = p;
        } else if (type == QLatin1String("custom-title")) {
            customTitle = o.value(QLatin1String("customTitle")).toString().trimmed();
        } else if (type == QLatin1String("ai-title")) {
            aiTitle = o.value(QLatin1String("aiTitle")).toString().trimmed();
        } else if (type == QLatin1String("summary")) {
            summary = o.value(QLatin1String("summary")).toString().trimmed();
        } else if (type == QLatin1String("last-prompt") && !fromHead) {
            if (const QString p = oneLine(o.value(QLatin1String("lastPrompt")).toString());
                !p.isEmpty())
                e.lastPrompt = p;
        }
    };
    for (const QByteArray &l : wholeLines(head, false))
        read(QJsonDocument::fromJson(l).object(), true);
    for (const QByteArray &l : wholeLines(tail, true))
        read(QJsonDocument::fromJson(l).object(), false);
    if (e.firstPrompt.isEmpty() && e.lastPrompt.isEmpty())
        return false;
    // Recorded with the first request, so near the start — a long line may be
    // cut there, hence the raw search.
    RoleMark mark = roleInTranscriptBytes(head);
    if (mark.id.isEmpty())
        mark = roleInTranscriptBytes(tail);
    e.role     = mark.id;
    e.roleName = mark.name;
    e.title    = !customTitle.isEmpty() ? customTitle : !aiTitle.isEmpty() ? aiTitle : summary;
    return true;
}

bool readCatalogEntry(const QString &transcriptPath, CatalogEntry &e) {
    QFile f(transcriptPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const qint64 size = f.size();
    QByteArray   head, tail;
    if (size <= 2 * kEndBytes) {
        head = f.readAll();
        tail = '\n' + head; // both ends are the whole file
    } else {
        head = f.read(kEndBytes); // a cut last line just doesn't parse
        f.seek(size - kEndBytes);
        tail = f.read(kEndBytes);
    }
    const QFileInfo fi(transcriptPath);
    e.sessionId      = fi.completeBaseName();
    e.transcriptPath = fi.absoluteFilePath();
    e.modifiedMs     = fi.lastModified().toMSecsSinceEpoch();
    return catalogEntryFrom(head, tail, e);
}

std::vector<CatalogEntry> scanCatalog(const QString &projectsDir) {
    std::vector<CatalogEntry> out;
    const QDir                projects(projectsDir);
    for (const auto &dir : projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir folder(projects.filePath(dir));
        for (const QFileInfo &fi : folder.entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files)) {
            CatalogEntry e;
            if (readCatalogEntry(fi.absoluteFilePath(), e))
                out.push_back(std::move(e));
        }
    }
    std::sort(out.begin(), out.end(), [](const CatalogEntry &a, const CatalogEntry &b) {
        return a.modifiedMs > b.modifiedMs;
    });
    return out;
}

} // namespace claude_code

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QStringList>
#include <QWidget>
#include <vector>

class QScrollArea;

// File attachment strip: shows chips for pending local files and read-only
// existing files (edit mode). Hides itself when there are no files to show.
class AttachmentStrip : public QWidget {
    Q_OBJECT
public:
    explicit AttachmentStrip(QWidget *parent = nullptr);

    bool hasFiles() const { return !_pending.isEmpty() || !_readOnly.empty(); }
    // Rebuild the displayed chips from the given lists and show/hide accordingly.
    void rebuild(const QStringList &pending, const std::vector<File> &readOnly);

signals:
    void removeRequested(const QString &path);

private:
    void addPendingChip(const QString &path);
    void addReadOnlyChip(const File &file);

    QScrollArea      *_scroll = nullptr;
    QWidget          *_strip  = nullptr;
    QStringList       _pending;
    std::vector<File> _readOnly;
};

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QHash>
#include <QStringList>
#include <QWidget>
#include <vector>

class QFrame;
class QScrollArea;
class PopupTooltip;

// File attachment strip: shows chips for pending local files and read-only
// existing files (edit mode). Image files get a cover-style preview as the
// chip background; text files get their first lines rendered as the background.
// Hides itself when there are no files to show.
class AttachmentStrip : public QWidget {
    Q_OBJECT
public:
    explicit AttachmentStrip(QWidget *parent = nullptr);

    bool hasFiles() const { return !_pending.isEmpty() || !_readOnly.empty(); }
    // Rebuild the displayed chips from the given lists and show/hide accordingly.
    void rebuild(const QStringList &pending, const std::vector<File> &readOnly);

signals:
    void removeRequested(const QString &path);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void applyTheme();
    void addPendingChip(const QString &path);
    void addReadOnlyChip(const File &file);
    void addRemoveButton(QFrame *chip, const QString &path, bool onImage);
    void addOverlayLabels(QFrame *chip, const QString &name, const QString &sub);

    QScrollArea              *_scroll  = nullptr;
    QWidget                  *_strip   = nullptr;
    PopupTooltip             *_tooltip = nullptr;
    QHash<QWidget *, QString> _tooltipBtns;
    QStringList               _pending;
    std::vector<File>         _readOnly;
};

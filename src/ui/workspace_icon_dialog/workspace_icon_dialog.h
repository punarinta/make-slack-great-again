// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/icon_picker_dialog/icon_picker_dialog.h"

// "Workspace icon": pick a picture that only this install shows for a
// workspace (a non-admin cannot change the one Slack serves). The card
// previews the rail bubble with the candidate, or the letter fallback after
// "Use default". Result contract: see IconPickerDialog.
//
//   teamId/name — drive the bubble colour and the letter fallback
//   current     — the icon the rail shows now (null = letter fallback)
//   hasCustom   — whether an override is already installed (enables "Use default")
class WorkspaceIconDialog : public IconPickerDialog {
    Q_OBJECT
public:
    WorkspaceIconDialog(
        const QString &teamId,
        const QString &name,
        const QPixmap &current,
        bool           hasCustom,
        QWidget       *parent = nullptr
    );

protected:
    QImage decodeBytes(const QByteArray &bytes) const override;
    QImage prepareImage(const QImage &img) const override;
    void   paintPreview(QPainter &p, const QRectF &r, const QPixmap &icon) const override;

private:
    QString _teamId;
    QString _name;
};

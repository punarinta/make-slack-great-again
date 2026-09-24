// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/icon_picker_dialog/icon_picker_dialog.h"

class QCheckBox;

// "Tray icon" (Settings → Appearance, GitHub issue #73): the user's own picture
// for the tray, previewed on a panel-like tile with the unread dot the tray
// paints over it. Adds the monochrome option (a white silhouette; on macOS the
// template image the menu bar tints). Result contract: see IconPickerDialog,
// plus monochrome() — which may be the only thing that changed.
class TrayIconDialog : public IconPickerDialog {
    Q_OBJECT
public:
    // Reads the current picture and options from CustomTrayIcon.
    explicit TrayIconDialog(QWidget *parent = nullptr);

    bool monochrome() const;

protected:
    QImage decodeBytes(const QByteArray &bytes) const override;
    QImage prepareImage(const QImage &img) const override;
    void   paintPreview(QPainter &p, const QRectF &r, const QPixmap &icon) const override;
    void   applyTheme() override;

private:
    QCheckBox *_monochrome = nullptr;
};

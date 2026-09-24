// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "workspace_icon_dialog.h"
#include "ui/image_cache.h"
#include "ui/workspace_switcher/workspace_bubble.h"
#include "util/custom_workspace_icon.h"

#include <QPainter>

namespace {
// The preview bubble is bigger than the rail's 40 px so the crop is judgeable;
// the corner radius is scaled to keep the rail's shape.
constexpr int kPreviewRadius = 24;
} // namespace

WorkspaceIconDialog::WorkspaceIconDialog(
    const QString &teamId,
    const QString &name,
    const QPixmap &current,
    bool           hasCustom,
    QWidget       *parent
)
    : IconPickerDialog(
          {.title = tr("Workspace icon"),
           .hint =
               tr("Only you see this icon. The picture is cropped to a square. You "
                  "can also drop an image file onto this window."),
           .chooserTitle = tr("Choose workspace icon")},
          current.toImage(),
          hasCustom,
          parent
      ),
      _teamId(teamId), _name(name) {
    finish();
}

QImage WorkspaceIconDialog::decodeBytes(const QByteArray &bytes) const {
    // The cache decoder handles every format the app shows (raster + SVG) and
    // bounds the decode, so a 20-megapixel photo never lands in memory whole.
    return ImageCache::decodeBoundedImage(bytes, CustomWorkspaceIcon::kStoredSize);
}

QImage WorkspaceIconDialog::prepareImage(const QImage &img) const {
    return CustomWorkspaceIcon::prepare(img);
}

// The rail bubble at preview size, painted through the same routine the rail uses.
void WorkspaceIconDialog::paintPreview(QPainter &p, const QRectF &r, const QPixmap &icon) const {
    // Scaled per paint (cheap at this size) so a DPR change never shows a
    // pixmap baked for the previous screen.
    const QPixmap scaled =
        icon.isNull() ? QPixmap()
                      : Ui::scaleWorkspaceIcon(icon, kPreviewSize, p.device()->devicePixelRatioF());
    Ui::paintWorkspaceBubble(
        p, r, {.teamId = _teamId, .name = _name, .icon = scaled, .radius = kPreviewRadius}, font()
    );
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "tray_icon_dialog.h"
#include "ui/theme.h"
#include "util/custom_tray_icon.h"

#include <QCheckBox>
#include <QPainter>
#include <QSvgRenderer>
#include <QHBoxLayout>

namespace {
constexpr int kTileRadius = 16;
constexpr int kIconSize   = 56; // the icon inside the tile, as a panel would pad it
} // namespace

TrayIconDialog::TrayIconDialog(QWidget *parent)
    : IconPickerDialog(
          {.title = tr("Tray icon"),
           .hint =
               tr("The picture is fitted into a square, and the unread dot is drawn over "
                  "its corner. You can also drop an image file onto this window."),
           .chooserTitle = tr("Choose tray icon")},
          CustomTrayIcon::stored(),
          CustomTrayIcon::hasImage(),
          parent
      ) {
    _monochrome = new QCheckBox(tr("Convert to monochrome"));
    _monochrome->setChecked(CustomTrayIcon::monochrome());
    connect(_monochrome, &QCheckBox::toggled, this, [this] {
        refreshPreview();
        markDirty();
    });
    footerOptions()->addWidget(_monochrome);
    finish();
}

bool TrayIconDialog::monochrome() const {
    return _monochrome->isChecked();
}

QImage TrayIconDialog::decodeBytes(const QByteArray &bytes) const {
    return CustomTrayIcon::decode(bytes);
}

QImage TrayIconDialog::prepareImage(const QImage &img) const {
    return CustomTrayIcon::styled(img, _monochrome && _monochrome->isChecked());
}

void TrayIconDialog::paintPreview(QPainter &p, const QRectF &r, const QPixmap &icon) const {
    const auto &th = Th::c();
    // A dark panel stand-in: most trays are dark, and the built-in icon is white.
    p.setPen(Qt::NoPen);
    p.setBrush(th.nav.bg);
    p.drawRoundedRect(r, kTileRadius, kTileRadius);

    const QRectF iconRect(
        r.center().x() - kIconSize / 2.0, r.center().y() - kIconSize / 2.0, kIconSize, kIconSize
    );
    if (icon.isNull()) {
        QSvgRenderer def(QStringLiteral(":/icon_tray.svg"));
        def.render(&p, iconRect);
    } else {
        p.drawPixmap(iconRect, icon, QRectF(icon.rect()));
    }
    // The unread dot, at the proportions updateTrayIcon() uses (36 of 128).
    const qreal d = kIconSize * 36.0 / CustomTrayIcon::kStoredSize;
    p.setBrush(th.badge.mention);
    p.drawEllipse(QRectF(iconRect.right() - d, iconRect.bottom() - d, d, d));
}

void TrayIconDialog::applyTheme() {
    IconPickerDialog::applyTheme();
    if (_monochrome)
        _monochrome->setStyleSheet(Th::checkBoxQss());
}

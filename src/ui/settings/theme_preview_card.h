// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/theme.h"

#include <QAbstractButton>

// A checkable theme swatch for Settings → Appearance: a miniature mock of the
// app (workspace rail, conv list with a selected row and a mention badge,
// message area with an accent button) painted with the PREVIEWED theme's own
// palette, plus the theme name underneath. Selection chrome (border, label)
// follows the ACTIVE theme. Group cards in a QButtonGroup for exclusivity.
class ThemePreviewCard : public QAbstractButton {
    Q_OBJECT
public:
    ThemePreviewCard(
        QString themeId, QString displayName, const Th::Theme &preview, QWidget *parent = nullptr
    );

    const QString &themeId() const { return _themeId; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    QString          _themeId;
    QString          _name;
    const Th::Theme &_preview;

    static constexpr int kCardW  = 148;
    static constexpr int kMockH  = 92;
    static constexpr int kLabelH = 24;
    static constexpr int kRadius = 8;
};

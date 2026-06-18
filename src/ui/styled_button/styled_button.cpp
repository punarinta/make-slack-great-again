// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "styled_button.h"
#include "ui/control_metrics.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

StyledButton::StyledButton(const QString &text, Variant variant, QWidget *parent)
    : QPushButton(text, parent), _variant(variant) {
    init();
}

StyledButton::StyledButton(QWidget *parent) : QPushButton(parent) {
    init();
}

void StyledButton::init() {
    setCursor(Qt::PointingHandCursor);
    setSize(_size);
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void StyledButton::setVariant(Variant variant) {
    _variant = variant;
    applyTheme();
}

void StyledButton::setSize(Size size) {
    _size = size;
    // Link buttons are inline text: no fixed height, no padding box.
    if (_variant == Variant::Link) {
        setMinimumHeight(0);
        setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    } else {
        setFixedHeight(size == Size::Small ? Ui::kControlHeightSmall : Ui::kControlHeight);
    }
    applyTheme();
}

void StyledButton::applyTheme() {
    const auto &th  = Th::c();
    const int   pad = _size == Size::Small ? 12 : 18;
    const int   fs  = _size == Size::Small ? th.fonts.md : th.fonts.base;

    QString css;
    switch (_variant) {
    case Variant::Primary:
        css = QString(
                  "StyledButton {"
                  "  background: %1; color: %2; border: none;"
                  "  border-radius: %3px; padding: 0 %4px; font-weight: 600; font-size: %5px;"
                  "}"
                  "StyledButton:hover    { background: %6; }"
                  "StyledButton:pressed  { background: %7; }"
                  "StyledButton:disabled { background: %8; color: %9; }"
        )
                  .arg(Th::qss(th.accent.def), Th::qss(th.accent.text))
                  .arg(Ui::kControlRadius)
                  .arg(pad)
                  .arg(fs)
                  .arg(
                      Th::qss(th.accent.hover),
                      Th::qss(th.accent.pressed),
                      Th::qss(th.surface.highlightStrong),
                      Th::qss(th.text.tertiary)
                  );
        break;
    case Variant::Secondary:
        css =
            QString(
                "StyledButton {"
                "  background: %1; color: %2; border: 1px solid %3;"
                "  border-radius: %4px; padding: 0 %5px; font-size: %6px;"
                "}"
                "StyledButton:hover    { background: %7; }"
                "StyledButton:pressed  { background: %8; }"
                "StyledButton:disabled { color: %9; }"
            )
                .arg(
                    Th::qss(th.surface.raised), Th::qss(th.text.primary), Th::qss(th.divider.strong)
                )
                .arg(Ui::kControlRadius)
                .arg(pad)
                .arg(fs)
                .arg(
                    Th::qss(th.surface.sunken),
                    Th::qss(th.surface.highlightStrong),
                    Th::qss(th.text.tertiary)
                );
        break;
    case Variant::Ghost:
        css = QString(
                  "StyledButton {"
                  "  background: %1; color: %2; border: none;"
                  "  border-radius: %3px; padding: 0 %4px; font-size: %5px;"
                  "}"
                  "StyledButton:hover    { background: %6; }"
                  "StyledButton:pressed  { background: %6; }"
                  "StyledButton:disabled { color: %7; }"
        )
                  .arg(Th::qss(th.surface.highlight), Th::qss(th.text.primary))
                  .arg(Ui::kControlRadius)
                  .arg(pad)
                  .arg(fs)
                  .arg(Th::qss(th.surface.highlightStrong), Th::qss(th.text.tertiary));
        break;
    case Variant::Danger:
        css = QString(
                  "StyledButton {"
                  "  background: %1; color: %2; border: none;"
                  "  border-radius: %3px; padding: 0 %4px; font-weight: 600; font-size: %5px;"
                  "}"
                  "StyledButton:hover    { background: %6; }"
                  "StyledButton:pressed  { background: %6; }"
                  "StyledButton:disabled { background: %7; color: %8; }"
        )
                  .arg(Th::qss(th.danger.def), Th::qss(th.accent.text))
                  .arg(Ui::kControlRadius)
                  .arg(pad)
                  .arg(fs)
                  .arg(
                      Th::qss(th.danger.hover),
                      Th::qss(th.surface.highlightStrong),
                      Th::qss(th.text.tertiary)
                  );
        break;
    case Variant::Link:
        css = QString(
                  "StyledButton {"
                  "  background: transparent; border: none; padding: 0;"
                  "  color: %1; font-size: %2px; text-decoration: underline; text-align: left;"
                  "}"
                  "StyledButton:hover { color: %3; }"
        )
                  .arg(Th::qss(th.text.link))
                  .arg(_size == Size::Small ? th.fonts.caption : th.fonts.md)
                  .arg(Th::qss(th.accent.hover));
        break;
    }
    setStyleSheet(css);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QPushButton>

// Reusable themed text button with web-style variants. Replaces the per-dialog
// QPushButton + setStyleSheet duplication: each call site picks a Variant and the
// button paints itself from theme tokens and re-themes on themeChanged.
//
//   Primary   — accent-filled call to action (Save, Create, Forward…)
//   Secondary — outlined neutral button (Cancel, Back, Copy Link…)
//   Ghost     — subtle filled neutral button (Disconnect, Check for updates…)
//   Danger    — destructive accent (Delete, Clear cache…)
//   Link      — borderless text link (underlined)
//
// Default height matches StyledLineEdit (Ui::kControlHeight) so an input and a
// button placed in the same row line up; pass Size::Small for compact rows, or
// Size::XSmall (22px, tighter 4px radius) for banner / inline buttons.
class StyledButton : public QPushButton {
    Q_OBJECT
public:
    enum class Variant { Primary, Secondary, Ghost, Danger, Link };
    enum class Size { Normal, Small, XSmall };

    explicit StyledButton(
        const QString &text, Variant variant = Variant::Primary, QWidget *parent = nullptr
    );
    explicit StyledButton(QWidget *parent = nullptr);

    void setVariant(Variant variant);
    void setSize(Size size);

private:
    void init();
    void applyTheme();

    Variant _variant = Variant::Primary;
    Size    _size    = Size::Normal;
};

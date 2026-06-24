// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QFrame>
#include <QSize>
#include <QString>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class IconButton;

// Reusable themed input field with optional prefix label and character counter.
// Changes border color on focus using composer.border / composer.borderFocus tokens.
class StyledLineEdit : public QFrame {
    Q_OBJECT
public:
    // Height matches StyledButton's matching Size so an input + button in the
    // same row line up: Normal = Ui::kControlHeight, Small = kControlHeightSmall.
    enum class Size { Normal, Small };

    explicit StyledLineEdit(QWidget *parent = nullptr);

    void       setSize(Size size);
    // Drop the frame: no border, no rounded corners, transparent background — for
    // a field embedded in another container (e.g. the composer subject line, which
    // draws its own separator). Idempotent; re-applied across theme changes.
    void       setBorderless(bool on);
    void       setPrefix(const QString &text);
    // Show a tinted SVG glyph at the left edge of the field (e.g. a search
    // magnifier). Re-tinted on theme change. Pass an empty path to remove it.
    void       setLeadingIcon(const QString &svgPath, const QSize &size = QSize(16, 16));
    void       setMaxLength(int max);
    void       setPlaceholderText(const QString &text);
    QString    text() const;
    void       setText(const QString &text);
    void       clear();
    QLineEdit *lineEdit() const { return _edit; }

    // Show a clickable tinted SVG button at the right edge; emits trailingClicked().
    // Lazily created; pass an empty path to hide it. Returns the button so callers
    // can disable it / show a busy state.
    IconButton *setTrailingIcon(const QString &svgPath);
    IconButton *trailingButton() const { return _trailingBtn; }

    // Turn this into a password field with a built-in show/hide eye toggle
    // (reusable across the app). Sets echo mode to Password initially.
    void enablePasswordReveal();

signals:
    void textChanged(const QString &text);
    void returnPressed();
    void trailingClicked();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void applyTheme();
    void updateCounter();
    void updateBorderStyle(bool focused);

    QHBoxLayout *_layout       = nullptr;
    QLabel      *_leadingIcon  = nullptr;
    QLabel      *_prefixLabel  = nullptr;
    QLineEdit   *_edit         = nullptr;
    QLabel      *_counterLabel = nullptr;
    IconButton  *_trailingBtn  = nullptr;
    QString      _leadingIconPath;
    QSize        _leadingIconSize = QSize(16, 16);
    int          _maxLength       = 0;
    bool         _focused         = false;
    bool         _passwordShown   = false;
    bool         _borderless      = false;
};

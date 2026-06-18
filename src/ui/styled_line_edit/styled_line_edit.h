// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QFrame>
#include <QSize>
#include <QString>

class QHBoxLayout;
class QLabel;
class QLineEdit;

// Reusable themed input field with optional prefix label and character counter.
// Changes border color on focus using composer.border / composer.borderFocus tokens.
class StyledLineEdit : public QFrame {
    Q_OBJECT
public:
    explicit StyledLineEdit(QWidget *parent = nullptr);

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

signals:
    void textChanged(const QString &text);
    void returnPressed();

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
    QString      _leadingIconPath;
    QSize        _leadingIconSize = QSize(16, 16);
    int          _maxLength       = 0;
    bool         _focused         = false;
};

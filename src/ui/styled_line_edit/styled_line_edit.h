// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QFrame>

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
    void       setMaxLength(int max);
    void       setPlaceholderText(const QString &text);
    QString    text() const;
    void       setText(const QString &text);
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
    QLabel      *_prefixLabel  = nullptr;
    QLineEdit   *_edit         = nullptr;
    QLabel      *_counterLabel = nullptr;
    int          _maxLength    = 0;
    bool         _focused      = false;
};

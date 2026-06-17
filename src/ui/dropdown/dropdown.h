// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QStringList>
#include <QWidget>

// Reusable themed dropdown (select) — a rounded, bordered field showing the
// current choice plus a chevron, whose popup is our own ContextMenu (so the
// option list matches the app's look instead of the native, off-theme combo
// popup). The current option is marked with a checkmark.
//
// Usage:
//   auto *d = new Dropdown;
//   d->setItems({tr("Off"), tr("On")});
//   d->setCurrentIndex(0);
//   connect(d, &Dropdown::currentIndexChanged, this, [](int i){ ... });
class Dropdown : public QWidget {
    Q_OBJECT
public:
    explicit Dropdown(QWidget *parent = nullptr);

    void    addItem(const QString &text);
    void    setItems(const QStringList &items);
    int     currentIndex() const { return _current; }
    void    setCurrentIndex(int index);
    QString currentText() const;

signals:
    void currentIndexChanged(int index);

protected:
    void  paintEvent(QPaintEvent *) override;
    void  mousePressEvent(QMouseEvent *) override;
    void  enterEvent(QEnterEvent *) override;
    void  leaveEvent(QEvent *) override;
    QSize sizeHint() const override;

private:
    void openMenu();
    void applyStyle(); // rebuild the QSS box (border/background) for the current state

    QStringList _items;
    int         _current = -1;
    bool        _hover   = false;
    bool        _open    = false; // popup currently showing → keep the field highlighted
};

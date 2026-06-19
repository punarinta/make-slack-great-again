// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QList>
#include <QStringList>
#include <QVariant>
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

    // Each item carries optional opaque user data (like QComboBox::itemData), so
    // callers can store a stable id ("system"/"en"/…) independent of the label.
    void     addItem(const QString &text, const QVariant &data = {});
    void     addSeparator(); // a non-selectable divider row in the popup
    void     setItems(const QStringList &items);
    void     clear();
    int      currentIndex() const { return _current; }
    void     setCurrentIndex(int index);
    QString  currentText() const;
    QVariant currentData() const;
    int findData(const QVariant &data) const; // index of the first item whose data == `data`, or -1

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

    QStringList     _items;
    QList<QVariant> _data;  // parallel to _items; opaque per-item user data
    QList<bool>     _isSep; // parallel to _items; true → divider, not selectable
    int             _current = -1;
    bool            _hover   = false;
    bool            _open    = false; // popup currently showing → keep the field highlighted
};

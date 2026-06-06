// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "mention_completer.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QApplication>

static constexpr int kMaxRows = 8;

MentionCompleter::MentionCompleter(QWidget *parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName("mentionCompleter");
    setStyleSheet(
        "QFrame#mentionCompleter {"
        "  background: #FFFFFF;"
        "  border: 1px solid #D1D1D1;"
        "  border-radius: 6px;"
        "}"
    );
    _layout = new QVBoxLayout(this);
    _layout->setContentsMargins(4, 4, 4, 4);
    _layout->setSpacing(1);
    hide();
}

void MentionCompleter::show(const QPoint &globalPos, const QList<Item> &items, Callback cb) {
    _cb = std::move(cb);
    _sel = 0;
    rebuild(items);

    adjustSize();
    // Position above the cursor
    const int popupH = height();
    move(globalPos - QPoint(0, popupH + 4));
    QFrame::show();
    raise();
}

void MentionCompleter::dismiss() {
    hide();
    _items.clear();
    _rows.clear();
}

bool MentionCompleter::handleKey(int key) {
    if (!isVisible()) return false;
    if (key == Qt::Key_Escape) { dismiss(); return true; }
    if (key == Qt::Key_Up) {
        selectRow((_sel - 1 + _rows.size()) % _rows.size());
        return true;
    }
    if (key == Qt::Key_Down) {
        selectRow((_sel + 1) % _rows.size());
        return true;
    }
    if (key == Qt::Key_Tab || key == Qt::Key_Return) {
        confirm();
        return true;
    }
    return false;
}

bool MentionCompleter::isVisible() const {
    return QFrame::isVisible();
}

void MentionCompleter::rebuild(const QList<Item> &items) {
    // Clear
    while (_layout->count())
        delete _layout->takeAt(0)->widget();
    _rows.clear();
    _items = items.mid(0, kMaxRows);

    for (int i = 0; i < _items.size(); ++i) {
        auto *row = new QPushButton(this);
        row->setFlat(true);
        row->setFocusPolicy(Qt::NoFocus);
        row->setCursor(Qt::PointingHandCursor);
        row->setText(_items[i].display);
        row->setStyleSheet(
            "QPushButton {"
            "  text-align: left;"
            "  padding: 4px 10px;"
            "  font-size: 13px;"
            "  color: #1D1C1D;"
            "  background: transparent;"
            "  border: none;"
            "  border-radius: 4px;"
            "}"
            "QPushButton:hover { background: #F0F0F0; }"
        );
        const int idx = i;
        connect(row, &QPushButton::clicked, this, [this, idx] {
            _sel = idx;
            confirm();
        });
        _layout->addWidget(row);
        _rows.append(row);
    }

    selectRow(0);
}

void MentionCompleter::selectRow(int row) {
    if (_rows.isEmpty()) return;
    // Deselect old
    if (_sel >= 0 && _sel < _rows.size()) {
        _rows[_sel]->setStyleSheet(
            "QPushButton {"
            "  text-align: left; padding: 4px 10px;"
            "  font-size: 13px; color: #1D1C1D;"
            "  background: transparent; border: none; border-radius: 4px;"
            "}"
            "QPushButton:hover { background: #F0F0F0; }"
        );
    }
    _sel = row;
    if (_sel >= 0 && _sel < _rows.size()) {
        _rows[_sel]->setStyleSheet(
            "QPushButton {"
            "  text-align: left; padding: 4px 10px;"
            "  font-size: 13px; color: #1D1C1D;"
            "  background: #E8F5F0; border: none; border-radius: 4px;"
            "}"
        );
    }
}

void MentionCompleter::confirm() {
    if (_sel < 0 || _sel >= _items.size()) { dismiss(); return; }
    const QString insert = _items[_sel].insert;
    dismiss();
    if (_cb) _cb(insert);
}

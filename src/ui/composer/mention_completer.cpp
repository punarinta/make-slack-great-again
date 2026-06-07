// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "mention_completer.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QApplication>

static constexpr int kMaxRows = 8;

MentionCompleter::MentionCompleter(QWidget *parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setObjectName("mentionCompleter");
    _layout = new QVBoxLayout(this);
    _layout->setContentsMargins(4, 4, 4, 4);
    _layout->setSpacing(1);
    hide();

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void MentionCompleter::applyTheme() {
    setStyleSheet(QString("QFrame#mentionCompleter {"
                          "  background: %1;"
                          "  border: 1px solid %2;"
                          "  border-radius: 6px;"
                          "}")
                      .arg(Th::qss(Th::c().surface.raised), Th::qss(Th::c().divider.strong)));
}

void MentionCompleter::show(const QPoint &globalPos, const QList<Item> &items, Callback cb) {
    _cb  = std::move(cb);
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
    if (!isVisible())
        return false;
    if (key == Qt::Key_Escape) {
        dismiss();
        return true;
    }
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
            QString("QPushButton {"
                    "  text-align: left;"
                    "  padding: 4px 10px;"
                    "  font-size: %3px;"
                    "  color: %1;"
                    "  background: transparent;"
                    "  border: none;"
                    "  border-radius: 4px;"
                    "}"
                    "QPushButton:hover { background: %2; }")
                .arg(Th::qss(Th::c().text.primary), Th::qss(Th::c().surface.highlight))
                .arg(Th::c().fonts.md)
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
    if (_rows.isEmpty())
        return;
    // Deselect old
    if (_sel >= 0 && _sel < _rows.size()) {
        _rows[_sel]->setStyleSheet(
            QString("QPushButton {"
                    "  text-align: left; padding: 4px 10px;"
                    "  font-size: %3px; color: %1;"
                    "  background: transparent; border: none; border-radius: 4px;"
                    "}"
                    "QPushButton:hover { background: %2; }")
                .arg(Th::qss(Th::c().text.primary), Th::qss(Th::c().surface.highlight))
                .arg(Th::c().fonts.md)
        );
    }
    _sel = row;
    if (_sel >= 0 && _sel < _rows.size()) {
        _rows[_sel]->setStyleSheet(
            QString("QPushButton {"
                    "  text-align: left; padding: 4px 10px;"
                    "  font-size: %3px; color: %1;"
                    "  background: %2; border: none; border-radius: 4px;"
                    "}")
                .arg(Th::qss(Th::c().text.primary), Th::qss(Th::c().accent.subtleBg))
                .arg(Th::c().fonts.md)
        );
    }
}

void MentionCompleter::confirm() {
    if (_sel < 0 || _sel >= _items.size()) {
        dismiss();
        return;
    }
    const QString insert = _items[_sel].insert;
    dismiss();
    if (_cb)
        _cb(insert);
}

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

MentionCompleter::MentionCompleter(QWidget *parent) : QFrame(parent) {
    // Plain child widget of the container, like MentionPopup — a real window
    // (Qt::Tool) cannot be positioned by the client on Wayland and is
    // activated on show by some window managers, which steals the editor's
    // focus and instantly dismisses the completer via FocusOut.
    setObjectName("mentionCompleter");
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::NoFocus);
    _layout = new QVBoxLayout(this);
    _layout->setContentsMargins(4, 4, 4, 4);
    _layout->setSpacing(1);
    hide();

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void MentionCompleter::applyTheme() {
    setStyleSheet(QString(
                      "QFrame#mentionCompleter {"
                      "  background: %1;"
                      "  border: 1px solid %2;"
                      "  border-radius: 6px;"
                      "}"
    )
                      .arg(Th::qss(Th::c().surface.raised), Th::qss(Th::c().divider.strong)));
}

void MentionCompleter::show(const QPoint &globalPos, const QList<Item> &items, Callback cb) {
    _cb  = std::move(cb);
    _sel = 0;
    rebuild(items);

    // height() is stale until the first show — size from the layout instead.
    _layout->activate();
    const QSize sz = sizeHint();
    resize(sz);

    // Bottom edge sits just above the anchor (the trigger character), in the
    // parent's coordinates; keep the popup inside the parent horizontally.
    QPoint pos = globalPos - QPoint(0, sz.height() + 4);
    if (QWidget *par = parentWidget()) {
        const QPoint local = par->mapFromGlobal(globalPos);
        pos                = local - QPoint(0, sz.height() + 4);
        pos.setX(qBound(0, pos.x(), qMax(0, par->width() - sz.width())));
    }
    move(pos);
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
            QString(
                "QPushButton {"
                "  text-align: left;"
                "  padding: 4px 10px;"
                "  font-size: %3px;"
                "  color: %1;"
                "  background: transparent;"
                "  border: none;"
                "  border-radius: 4px;"
                "}"
                "QPushButton:hover { background: %2; }"
            )
                .arg(Th::qss(Th::c().text.primary), Th::qss(Th::c().surface.highlight))
                .arg(Th::c().fonts.md)
        );
        const int idx = i;
        connect(row, &QPushButton::clicked, this, [this, idx] {
            _sel = idx;
            confirm();
        });
        _layout->addWidget(row);
        // Children added to an already-visible parent stay hidden until shown
        // explicitly; without this the popup collapses on rebuild-while-open.
        row->show();
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
            QString(
                "QPushButton {"
                "  text-align: left; padding: 4px 10px;"
                "  font-size: %3px; color: %1;"
                "  background: transparent; border: none; border-radius: 4px;"
                "}"
                "QPushButton:hover { background: %2; }"
            )
                .arg(Th::qss(Th::c().text.primary), Th::qss(Th::c().surface.highlight))
                .arg(Th::c().fonts.md)
        );
    }
    _sel = row;
    if (_sel >= 0 && _sel < _rows.size()) {
        _rows[_sel]->setStyleSheet(
            QString(
                "QPushButton {"
                "  text-align: left; padding: 4px 10px;"
                "  font-size: %3px; color: %1;"
                "  background: %2; border: none; border-radius: 4px;"
                "}"
            )
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

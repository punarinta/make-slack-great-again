// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QTextDocument>
#include <QWidget>

class Session;

// Full-window in-app viewer for table messages ("Open full table"). Mounted
// once per top-level window and reused via open(), like ImageViewerOverlay.
// Dark near-opaque backdrop with a centred card showing the ENTIRE table (the
// inline message caps at kMaxInlineTableRows). Wheel / arrow keys scroll when
// the table is taller than the window; Esc or a backdrop click dismisses.
class TableViewerOverlay : public QWidget {
    Q_OBJECT
public:
    explicit TableViewerOverlay(QWidget *windowParent);

    // Show the viewer for a "table" block. `session` resolves mentions/emoji
    // inside cells and may be null.
    void open(const Block &block, const Session *session);

protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void  rebuildDoc(); // re-renders _block into _doc with current theme colors
    void  relayout();   // cover the parent and clamp the scroll offset
    QRect cardRect() const;
    int   maxScroll() const;

    static constexpr int kMargin  = 48; // breathing room around the card
    static constexpr int kCardPad = 12; // card padding around the table

    Block          _block;
    const Session *_session = nullptr;
    QTextDocument  _doc;
    int            _scroll = 0; // vertical doc offset, 0..maxScroll()
};

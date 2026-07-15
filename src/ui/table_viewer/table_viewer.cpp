// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "table_viewer.h"
#include "ui/message_list/message_render.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

TableViewerOverlay::TableViewerOverlay(QWidget *windowParent) : QWidget(windowParent) {
    setFocusPolicy(Qt::StrongFocus);
    hide();

    _doc.setDocumentMargin(0);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        // The table HTML bakes theme colors (grid lines, shading) — re-render.
        if (!_block.tableRows.empty())
            rebuildDoc();
        update();
    });

    if (windowParent)
        windowParent->installEventFilter(this); // track window resizes while open
}

void TableViewerOverlay::open(const Block &block, const Session *session) {
    _block   = block;
    _session = session;
    _scroll  = 0;
    rebuildDoc();
    relayout();
    show();
    raise();
    setFocus();
    update();
}

void TableViewerOverlay::rebuildDoc() {
    _doc.setDefaultFont(QApplication::font());
    _doc.setDefaultStyleSheet(MsgRender::docStyleSheet());
    _doc.setHtml(MsgRender::tableBlockHtml(_block, _session, /*maxRows=*/-1));
}

void TableViewerOverlay::relayout() {
    if (parentWidget())
        setGeometry(parentWidget()->rect());
    const int availInnerW = std::max(50, width() - 2 * kMargin - 2 * kCardPad);
    _doc.setTextWidth(availInnerW);
    _scroll = std::clamp(_scroll, 0, maxScroll());
}

QRect TableViewerOverlay::cardRect() const {
    const int availW = std::max(50, width() - 2 * kMargin);
    const int availH = std::max(50, height() - 2 * kMargin);
    const int docW   = qCeil(_doc.idealWidth());
    const int docH   = qCeil(_doc.size().height());
    const int w      = std::min(docW + 2 * kCardPad, availW);
    const int h      = std::min(docH + 2 * kCardPad, availH);
    return {(width() - w) / 2, (height() - h) / 2, w, h};
}

int TableViewerOverlay::maxScroll() const {
    const int docH = qCeil(_doc.size().height());
    return std::max(0, docH - (cardRect().height() - 2 * kCardPad));
}

void TableViewerOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), Th::c().surface.viewerBackdrop);

    const QRect card = cardRect();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Th::c().surface.raised);
    p.drawRoundedRect(card, 8, 8);

    p.save();
    p.setClipRect(card.adjusted(kCardPad, kCardPad, -kCardPad, -kCardPad));
    p.translate(card.left() + kCardPad, card.top() + kCardPad - _scroll);
    QAbstractTextDocumentLayout::PaintContext pCtx;
    pCtx.palette.setColor(QPalette::Text, Th::c().text.primary);
    pCtx.clip = QRectF(0, _scroll, _doc.textWidth(), card.height() - 2 * kCardPad);
    _doc.documentLayout()->draw(&p, pCtx);
    p.restore();
}

void TableViewerOverlay::keyPressEvent(QKeyEvent *e) {
    const int page = std::max(40, cardRect().height() - 2 * kCardPad - 24);
    int       d    = 0;
    switch (e->key()) {
    case Qt::Key_Escape:
        hide();
        return;
    case Qt::Key_Up:
        d = -40;
        break;
    case Qt::Key_Down:
        d = 40;
        break;
    case Qt::Key_PageUp:
        d = -page;
        break;
    case Qt::Key_PageDown:
        d = page;
        break;
    case Qt::Key_Home:
        _scroll = 0;
        update();
        return;
    case Qt::Key_End:
        _scroll = maxScroll();
        update();
        return;
    default:
        QWidget::keyPressEvent(e);
        return;
    }
    _scroll = std::clamp(_scroll + d, 0, maxScroll());
    update();
}

void TableViewerOverlay::mousePressEvent(QMouseEvent *e) {
    // Backdrop click dismisses; clicks on the card do not (text selection may
    // come later).
    if (e->button() == Qt::LeftButton && !cardRect().contains(e->pos())) {
        hide();
        return;
    }
    QWidget::mousePressEvent(e);
}

void TableViewerOverlay::wheelEvent(QWheelEvent *e) {
    const int d = e->angleDelta().y();
    if (d != 0) {
        _scroll = std::clamp(_scroll - d / 2, 0, maxScroll());
        update();
    }
    e->accept();
}

bool TableViewerOverlay::eventFilter(QObject *obj, QEvent *ev) {
    if (obj == parentWidget() && ev->type() == QEvent::Resize && isVisible()) {
        relayout();
        update();
    }
    return QWidget::eventFilter(obj, ev);
}

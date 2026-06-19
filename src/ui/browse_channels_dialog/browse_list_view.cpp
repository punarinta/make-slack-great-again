// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "browse_list_view.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/icon_utils.h"
#include "ui/image_cache.h"
#include "ui/user_avatar.h"

#include <QApplication>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <algorithm>

BrowseListView::BrowseListView(ImageCache *imgCache, QWidget *parent)
    : VirtualListWidget(parent), _imgCache(imgCache) {
    viewport()->setCursor(Qt::PointingHandCursor);
    rebuildIcons();
    if (_imgCache)
        connect(_imgCache, &ImageCache::loaded, this, [this] { viewport()->update(); });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        rebuildIcons();
        viewport()->update();
    });
}

void BrowseListView::setItems(std::vector<Item> items) {
    _items = std::move(items);
    applyFilter(_filterText);
}

void BrowseListView::applyFilter(const QString &query) {
    _filterText     = query;
    const QString q = query.trimmed().toLower();
    _filtered.clear();
    _filtered.reserve(_items.size());
    for (int i = 0; i < static_cast<int>(_items.size()); ++i) {
        if (q.isEmpty() || _items[i].searchKey.contains(q))
            _filtered.push_back(i);
    }
    _hovered = -1;
    verticalScrollBar()->setValue(0);
    updateScrollRange();
    viewport()->update();
}

QString BrowseListView::idAt(int visibleRow) const {
    if (visibleRow < 0 || visibleRow >= static_cast<int>(_filtered.size()))
        return {};
    return _items[_filtered[visibleRow]].id;
}

void BrowseListView::rebuildIcons() {
    _hashPx  = svgPixmap(":/ui/hash.svg", QSize(14, 14), Th::c().text.secondary);
    _lockPx  = svgPixmap(":/ui/lock.svg", QSize(14, 14), Th::c().text.secondary);
    _checkPx = svgPixmap(":/ui/check.svg", QSize(13, 13), Th::c().text.secondary);
}

void BrowseListView::updateScrollRange() {
    const int total = static_cast<int>(_filtered.size()) * kRowH;
    const int vh    = viewport()->height();
    verticalScrollBar()->setRange(0, std::max(0, total - vh));
    verticalScrollBar()->setPageStep(vh);
}

int BrowseListView::rowAt(int vpY) const {
    const int row = (vpY + verticalScrollBar()->value()) / kRowH;
    return (row < 0 || row >= static_cast<int>(_filtered.size())) ? -1 : row;
}

void BrowseListView::setHovered(int row) {
    if (row == _hovered)
        return;
    _hovered = row;
    viewport()->update();
}

void BrowseListView::resizeEvent(QResizeEvent *e) {
    QAbstractScrollArea::resizeEvent(e);
    updateScrollRange();
}

void BrowseListView::wheelEvent(QWheelEvent *e) {
    auto        *vsb = verticalScrollBar();
    const QPoint px  = e->pixelDelta();
    if (!px.isNull())
        vsb->setValue(vsb->value() - px.y());
    else
        vsb->setValue(
            vsb->value() - e->angleDelta().y() * QApplication::wheelScrollLines() * kRowH / 120
        );
    e->accept();
}

void BrowseListView::doMouseMove(QMouseEvent *e) {
    const int total = static_cast<int>(_filtered.size()) * kRowH;
    if (_sbDragging) {
        const int vh         = viewport()->height();
        const int thumbH     = std::max(20, total > 0 ? vh * vh / total : vh);
        const int trackRange = vh - thumbH;
        if (trackRange > 0) {
            const int newScroll =
                _sbDragStartScroll + (e->pos().y() - _sbDragStartY) * (total - vh) / trackRange;
            verticalScrollBar()->setValue(std::clamp(newScroll, 0, verticalScrollBar()->maximum()));
        }
        return;
    }
    const int sbHitX = scrollThumbHitX();
    if (e->pos().x() >= sbHitX && isOnScrollThumb(e->pos().y(), total))
        viewport()->setCursor(Qt::SizeVerCursor);
    else
        viewport()->setCursor(Qt::PointingHandCursor);
    setHovered(rowAt(e->pos().y()));
}

void BrowseListView::doMousePress(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton)
        return;
    const int total  = static_cast<int>(_filtered.size()) * kRowH;
    const int sbHitX = scrollThumbHitX();
    if (e->pos().x() >= sbHitX && isOnScrollThumb(e->pos().y(), total)) {
        _sbDragging        = true;
        _sbDragStartY      = e->pos().y();
        _sbDragStartScroll = verticalScrollBar()->value();
        viewport()->setCursor(Qt::SizeVerCursor);
        return;
    }
    const int row = rowAt(e->pos().y());
    if (row >= 0 && onActivated)
        onActivated(_items[_filtered[row]].id);
}

void BrowseListView::doMouseRelease(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && _sbDragging) {
        _sbDragging = false;
        viewport()->setCursor(Qt::PointingHandCursor);
    }
}

void BrowseListView::doMouseLeave() {
    setHovered(-1);
}

void BrowseListView::doPaint(QPaintEvent *) {
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), Th::c().surface.raised);

    const int vh      = viewport()->height();
    const int scrollY = verticalScrollBar()->value();
    const int count   = static_cast<int>(_filtered.size());
    if (count == 0)
        return;

    const int first = std::max(0, scrollY / kRowH);
    const int last  = std::min(count - 1, (scrollY + vh) / kRowH);
    for (int r = first; r <= last; ++r)
        paintRow(p, _items[_filtered[r]], r * kRowH - scrollY, r == _hovered);

    paintScrollThumb(p, count * kRowH, Th::c().nav.scrollThumb);
}

void BrowseListView::paintRow(QPainter &p, const Item &it, int y, bool hovered) {
    const int vw = viewport()->width();
    if (hovered)
        p.fillRect(QRect(0, y, vw, kRowH), Th::c().surface.highlight);

    QFont nameFont = QApplication::font();
    nameFont.setBold(true);
    QFont subFont = QApplication::font();
    subFont.setPointSizeF(subFont.pointSizeF() * 0.88);
    const QFontMetrics fmName(nameFont);
    const QFontMetrics fmSub(subFont);

    int textX      = kRowPadH;
    int rightLimit = vw - kRowPadH;

    if (it.isPerson) {
        const int     avX = kRowPadH;
        const int     avY = y + (kRowH - kAvatarSize) / 2;
        const QPixmap px =
            (_imgCache && !it.avatarUrl.isEmpty()) ? _imgCache->get(it.avatarUrl) : QPixmap{};
        UserAvatar::State st;
        st.showPresence = false;
        UserAvatar::paint(
            p,
            QRect(avX, avY, kAvatarSize, kAvatarSize),
            px,
            it.initial,
            st,
            kAvatarSize / 2,
            p.device()->devicePixelRatioF()
        );
        textX = avX + kAvatarSize + 12;
    } else if (it.isMember) {
        // Reserve room on the right for the "Joined" badge.
        const QString joined = QObject::tr("Joined");
        const int     badgeW = 13 + 4 + fmSub.horizontalAdvance(joined);
        const int     bx     = vw - kRowPadH - badgeW;
        const int     cy     = y + kRowH / 2;
        p.drawPixmap(bx, cy - 13 / 2, _checkPx);
        p.setPen(Th::c().text.secondary);
        p.setFont(subFont);
        p.drawText(bx + 13 + 4, cy - fmSub.height() / 2 + fmSub.ascent(), joined);
        rightLimit = bx - 12;
    }

    // Channel rows carry a leading hash/lock icon on the title line.
    const int iconShift = it.isPerson ? 0 : (14 + 6);

    const bool    twoLine = !it.subtitle.isEmpty();
    const int     availW  = rightLimit - (textX + iconShift);
    const QString title   = fmName.elidedText(it.title, Qt::ElideRight, availW);

    if (twoLine) {
        const int contentH = fmName.height() + 1 + fmSub.height();
        const int top      = y + (kRowH - contentH) / 2;
        const int titleBl  = top + fmName.ascent();
        const int subBl    = top + fmName.height() + 1 + fmSub.ascent();
        if (!it.isPerson)
            p.drawPixmap(textX, top + (fmName.height() - 14) / 2, it.isPrivate ? _lockPx : _hashPx);
        p.setPen(Th::c().text.primary);
        p.setFont(nameFont);
        p.drawText(textX + iconShift, titleBl, title);
        p.setPen(Th::c().text.secondary);
        p.setFont(subFont);
        p.drawText(textX + iconShift, subBl, fmSub.elidedText(it.subtitle, Qt::ElideRight, availW));
    } else {
        const int titleTop = y + (kRowH - fmName.height()) / 2;
        if (!it.isPerson)
            p.drawPixmap(
                textX, titleTop + (fmName.height() - 14) / 2, it.isPrivate ? _lockPx : _hashPx
            );
        p.setPen(Th::c().text.primary);
        p.setFont(nameFont);
        p.drawText(textX + iconShift, titleTop + fmName.ascent(), title);
    }
}

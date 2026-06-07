// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "workspace_switcher.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/image_cache.h"
#include "ui/popup_tooltip/popup_tooltip.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QSvgRenderer>
#include <cmath>

WorkspaceSwitcher::WorkspaceSwitcher(QWidget *parent) : QWidget(parent) {
    setFixedWidth(kW);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
    _tooltip = new PopupTooltip(this);
    connect(
        &ThemeManager::instance(),
        &ThemeManager::themeChanged,
        this,
        QOverload<>::of(&QWidget::update)
    );
}

void WorkspaceSwitcher::setWorkspaces(const std::vector<Entry> &entries) {
    std::vector<EntryPrivate> next;
    next.reserve(entries.size());
    for (const auto &e : entries) {
        QPixmap existing;
        for (const auto &old : _entries)
            if (old.info.teamId == e.teamId) {
                existing = old.icon;
                break;
            }
        next.push_back({e, std::move(existing)});
    }
    _entries = std::move(next);
    loadIcons();
    update();
}

void WorkspaceSwitcher::setActive(const QString &teamId) {
    if (_activeId == teamId)
        return;
    _activeId = teamId;
    update();
}

void WorkspaceSwitcher::setUnread(const QString &teamId, int count) {
    for (auto &ep : _entries) {
        if (ep.info.teamId != teamId)
            continue;
        if (ep.info.unread == count)
            return;
        ep.info.unread = count;
        update();
        return;
    }
}

// ── Icon loading ──────────────────────────────────────────────────────────────

void WorkspaceSwitcher::setImageCache(ImageCache *cache) {
    _imgCache = cache;
    connect(cache, &ImageCache::loaded, this, [this](const QString &url) {
        for (auto &ep : _entries) {
            if (ep.info.iconUrl != url || !ep.icon.isNull())
                continue;
            const QPixmap px = _imgCache->get(url);
            if (!px.isNull())
                ep.icon = scaleIcon(px);
            break;
        }
        update();
    });
}

QPixmap WorkspaceSwitcher::scaleIcon(const QPixmap &src) const {
    const qreal dpr  = devicePixelRatioF();
    const int   phys = qRound(kBubble * dpr);
    QPixmap     px   = src.scaled(phys, phys, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    px.setDevicePixelRatio(dpr);
    return px;
}

void WorkspaceSwitcher::loadIcons() {
    if (!_imgCache)
        return;
    for (auto &ep : _entries) {
        if (ep.info.iconUrl.isEmpty() || !ep.icon.isNull())
            continue;
        const QPixmap px = _imgCache->get(ep.info.iconUrl);
        if (!px.isNull())
            ep.icon = scaleIcon(px);
        // null means in-flight or not yet fetched — loaded() signal will fire
    }
}

// ── Settings icon helper ──────────────────────────────────────────────────────

static void paintSettings(QPainter &p, const QRectF &r, bool hovered) {
    // Circle background (same visual style as + button)
    const QColor fill(255, 255, 255, hovered ? 55 : 22);
    const QColor border(255, 255, 255, hovered ? 180 : 90);
    p.setBrush(fill);
    p.setPen(QPen(border, 1.5));
    const qreal rad = r.height() / 2.0;
    p.drawRoundedRect(QRectF(r).adjusted(0.75, 0.75, -0.75, -0.75), rad, rad);

    static QSvgRenderer renderer(QString(":/ui/settings-2.svg"));
    if (!renderer.isValid())
        return;

    const QColor iconColor = hovered ? QColor(245, 240, 245) : QColor(180, 165, 180);
    const qreal  dim       = r.width() * 0.52;
    const QRectF ir(r.x() + (r.width() - dim) / 2.0, r.y() + (r.height() - dim) / 2.0, dim, dim);

    const int sz = qMax(1, qRound(dim));
    QPixmap   px(sz, sz);
    px.fill(Qt::transparent);
    QPainter pp(&px);
    pp.setRenderHint(QPainter::Antialiasing);
    renderer.render(&pp, QRectF(0, 0, sz, sz));
    pp.setCompositionMode(QPainter::CompositionMode_SourceIn);
    pp.fillRect(0, 0, sz, sz, iconColor);
    pp.end();

    p.drawPixmap(ir, px, QRectF(px.rect()));
}

// ── Geometry ──────────────────────────────────────────────────────────────────

QRect WorkspaceSwitcher::entryRect(int i) const {
    return QRect((kW - kBubble) / 2, kTopPad + i * (kBubble + kGap), kBubble, kBubble);
}

QRect WorkspaceSwitcher::addButtonRect() const {
    const int y = kTopPad + static_cast<int>(_entries.size()) * (kBubble + kGap) + kGap;
    return QRect((kW - kAddSize) / 2, y, kAddSize, kAddSize);
}

QRect WorkspaceSwitcher::gearButtonRect() const {
    return QRect((kW - kGearSize) / 2, height() - kBottomPad - kGearSize, kGearSize, kGearSize);
}

int WorkspaceSwitcher::hitTest(const QPoint &pos) const {
    for (int i = 0; i < static_cast<int>(_entries.size()); ++i)
        if (entryRect(i).contains(pos))
            return i;
    if (addButtonRect().contains(pos))
        return -2;
    if (gearButtonRect().contains(pos))
        return -3;
    return -99;
}

QColor WorkspaceSwitcher::bubbleColor(const QString &teamId) const {
    const int hue = static_cast<int>(qHash(teamId) * 37u) % 360;
    return QColor::fromHsl(hue, Th::c().workspaceHslSaturation, Th::c().workspaceHslLightness);
}

// ── Painting ──────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    p.fillRect(rect(), Th::c().nav.bg);

    for (int i = 0; i < static_cast<int>(_entries.size()); ++i) {
        const auto &ep     = _entries[i];
        const QRect r      = entryRect(i);
        const bool  active = (ep.info.teamId == _activeId);
        const bool  hov    = (_hovered == i);

        // Active indicator: white pill on left edge
        if (active) {
            const int barH = 28;
            const int barY = r.top() + (r.height() - barH) / 2;
            p.setBrush(Qt::white);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(0, barY, kBarW, barH, kBarW / 2.0, kBarW / 2.0);
        }

        if (!ep.icon.isNull()) {
            QColor bg = bubbleColor(ep.info.teamId);
            if (active)
                bg = bg.lighter(125);
            else if (hov)
                bg = bg.lighter(115);
            p.setBrush(bg);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(r, kRadius, kRadius);

            QPainterPath clip;
            clip.addRoundedRect(QRectF(r), kRadius, kRadius);
            p.setClipPath(clip);
            p.drawPixmap(r, ep.icon);
            p.setClipping(false);
        } else {
            // Letter fallback
            QColor bg = bubbleColor(ep.info.teamId);
            if (active)
                bg = bg.lighter(125);
            else if (hov)
                bg = bg.lighter(115);

            p.setBrush(bg);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(r, kRadius, kRadius);

            const QChar ch = ep.info.name.isEmpty() ? QChar('?') : ep.info.name.at(0).toUpper();
            p.setPen(Qt::white);
            QFont f = font();
            f.setPixelSize(17);
            f.setBold(true);
            p.setFont(f);
            p.drawText(r, Qt::AlignCenter, ch);
        }

        // White ring for active / hover
        if (active || hov) {
            p.setPen(QPen(QColor(255, 255, 255, active ? 200 : 100), active ? 2.0 : 1.5));
            p.setBrush(Qt::NoBrush);
            const qreal inset = 0.75;
            p.drawRoundedRect(
                QRectF(r).adjusted(inset, inset, -inset, -inset), kRadius - inset, kRadius - inset
            );
        }

        // Unread notification badge (top-right corner of the avatar bubble)
        if (ep.info.unread > 0) {
            const QString text =
                ep.info.unread > 99 ? QStringLiteral("99+") : QString::number(ep.info.unread);
            QFont bf = font();
            bf.setPixelSize(9);
            bf.setBold(true);
            const QFontMetrics fm(bf);
            const int          badgeH = 14;
            const int          badgeW = qMax(badgeH, fm.horizontalAdvance(text) + 6);
            const int          bx     = r.right() - badgeW + 3;
            const int          by     = r.top() - 3;
            const QRectF       br(bx, by, badgeW, badgeH);

            p.setBrush(Th::c().badge.unread);
            p.setPen(QPen(Th::c().nav.bg, 1.5));
            p.drawRoundedRect(br.adjusted(0.75, 0.75, -0.75, -0.75), badgeH / 2.0, badgeH / 2.0);
            p.setFont(bf);
            p.setPen(Qt::white);
            p.drawText(br.toRect(), Qt::AlignCenter, text);
        }
    }

    // "+" add button
    const QRect  ar     = addButtonRect();
    const bool   addHov = (_hovered == -2);
    const QColor addFill(255, 255, 255, addHov ? 55 : 22);
    const QColor addBorder(255, 255, 255, addHov ? 180 : 90);

    p.setBrush(addFill);
    p.setPen(QPen(addBorder, 1.5));
    const qreal r2 = ar.height() / 2.0;
    p.drawRoundedRect(QRectF(ar).adjusted(0.75, 0.75, -0.75, -0.75), r2, r2);

    // Draw + as two opaque lines — no alpha so the crossing pixel has no artifact.
    // Hover feedback already comes from the circle fill/border above.
    const QPointF c         = QRectF(ar).center();
    const qreal   arm       = 5.0;
    const QColor  plusColor = addHov ? QColor(245, 240, 245) : QColor(180, 165, 180);
    p.setPen(QPen(plusColor, 2.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(c.x() - arm, c.y()), QPointF(c.x() + arm, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - arm), QPointF(c.x(), c.y() + arm));

    // Settings button (bottom of column)
    paintSettings(p, QRectF(gearButtonRect()), _hovered == -3);
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

void WorkspaceSwitcher::mousePressEvent(QMouseEvent *e) {
    _pressed = hitTest(e->pos());
}

void WorkspaceSwitcher::mouseReleaseEvent(QMouseEvent *e) {
    const int hit = hitTest(e->pos());
    if (hit != _pressed) {
        _pressed = -99;
        return;
    }
    _pressed = -99;

    if (e->button() == Qt::LeftButton) {
        if (hit >= 0)
            emit workspaceClicked(_entries[hit].info.teamId);
        else if (hit == -2)
            emit addWorkspaceClicked();
        else if (hit == -3)
            emit settingsClicked();
    } else if (e->button() == Qt::RightButton && hit >= 0) {
        emit workspaceRightClicked(_entries[hit].info.teamId, e->globalPosition().toPoint());
    }
}

void WorkspaceSwitcher::mouseMoveEvent(QMouseEvent *e) {
    const int h = hitTest(e->pos());
    if (h != _hovered) {
        _hovered = h;
        setCursor((h >= -3 && h != -99) ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }

    if (h >= 0 && h < static_cast<int>(_entries.size())) {
        const QRect r = entryRect(h);
        _tooltip->showRightOf(_entries[h].info.name, QRect(mapToGlobal(r.topLeft()), r.size()));
    } else if (h == -2) {
        const QRect r = addButtonRect();
        _tooltip->showAbove(tr("Add workspace"), QRect(mapToGlobal(r.topLeft()), r.size()));
    } else if (h == -3) {
        const QRect r = gearButtonRect();
        _tooltip->showAbove(tr("Settings"), QRect(mapToGlobal(r.topLeft()), r.size()));
    } else {
        _tooltip->hide();
    }
}

void WorkspaceSwitcher::leaveEvent(QEvent *) {
    _tooltip->hide();
    if (_hovered == -99)
        return;
    _hovered = -99;
    update();
}
